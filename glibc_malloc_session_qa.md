# 当前 Session 问答记录

记录范围：截至本次记录请求，当前会话中的用户提问和助手正式回答。正文保留原文；角色标签用于区分发言，不包含工具调用、工具输出及过程更新。

## 用户（1）

glibc的malloc内存分配器为什么要设置mmap阈值对于大于该值的请求直接mmap，为什么不直接全部走堆main arena分配？你必须从源码实现逻辑给出根本原因，禁止泛泛而谈

## 助手（1）

根本原因是：**普通堆块的释放受“相邻块是否空闲、能否合并到堆尾”约束；独立 `mmap` 块的释放不依赖其他对象的生命周期。阈值用于决定何时值得付出独立映射的成本，换取这种独立回收能力。**

下面按你当前环境的 **glibc 2.39** 实现解释。

先纠正前提：**超过阈值，不一定直接 `mmap`。** `_int_malloc()` 会先尝试已有空闲块，再尝试切分 `av->top`；这些都无法满足请求，才进入 `sysmalloc()`。后者才检查规格化后的大小 `nb` 是否达到 `mp_.mmap_threshold`、独立映射数量是否低于上限，并尝试映射；失败仍可继续其他分配路径。因此阈值控制的是“需要向系统申请新内存时的选择”。[分配路径源码](https://github.com/bminor/glibc/blob/glibc-2.39/malloc/malloc.c#L4427-L4488)、[阈值判断](https://github.com/bminor/glibc/blob/glibc-2.39/malloc/malloc.c#L2554-L2587)

**为什么全部从 `brk` 堆分配会出问题？看释放时究竟执行什么。**

main arena 常规扩堆路径调用 `MORECORE`，它最终调用 `__sbrk(increment)`：正数扩展堆尾，负数收缩堆尾。这个接口没有“释放堆中间某一块”的参数。[morecore.c](https://github.com/bminor/glibc/blob/glibc-2.39/malloc/morecore.c#L17-L30)

对普通大块，释放路径是：

```text
_int_free
  → _int_free_merge_chunk
      → 合并前面的相邻空闲块
      → _int_free_create_chunk
          → 后面是普通块：尝试向后合并，结果放入 unsorted bin
          → 后面是 top：合并并更新 av->top
      → _int_free_maybe_consolidate
          → 满足条件时尝试 systrim
```

这里**只合并地址相邻的空闲块**。分散的空闲块不会因为总量很大就变成一个连续块。[释放与合并源码](https://github.com/bminor/glibc/blob/glibc-2.39/malloc/malloc.c#L4663-L4798)

`systrim()` 更直接地暴露了限制：它根据 **`av->top` 的大小**计算可释放量，保留必要空间并向下按页取整；确认当前 program break 就在 top 末端后，才用负数 `MORECORE` 收缩堆。**它不根据所有 bins 的空闲总量缩堆。**[systrim 源码](https://github.com/bminor/glibc/blob/glibc-2.39/malloc/malloc.c#L2957-L3019)

由这些分支，可以推导一个具体例子。假设全部走 `brk` 堆，形成：

```text
低地址                                            高地址
[ A：存活 ][ B：64 MiB ][ C：存活，32 B ][ top ] | brk
```

释放 B 后：

```text
[ A：存活 ][ B：空闲，64 MiB ][ C：存活，32 B ][ top ] | brk
```

B 会进入空闲链表，**top 不会因此增加 64 MiB**。如果把 break 降到 B 前面，会连仍在使用的 C 一起截掉；而分配器又不能擅自移动 C，因为程序持有指向它的指针。

于是：

- B 可以被后续请求复用，但不能通过缩堆独立释放。
- 后续申请一个大于 B、且现有其他连续空闲块也无法满足的对象，仍需要新内存。
- 如果 B 被后续小对象切分，其中一些长期存活，原先这 64 MiB 又会成为多个不能跨越存活对象合并的空洞。

这就是这里“碎片”的具体形成机制：**普通堆把不同对象的回收条件耦合在同一条地址相邻关系上。**

**独立 `mmap` 如何解除这个约束？**

`sysmalloc_mmap()` 为这次分配取得按页取整的映射，并在 chunk 头中设置 `IS_MMAPPED`；同时记录从 chunk 起点回到映射起点所需的偏移。

`__libc_free()` 发现这个标志，就走 `munmap_chunk()`。后者用偏移和 chunk 大小还原完整映射区间，直接调用 `__munmap()`，无需检查前后对象是否空闲，也不需要先合并到 top。[映射构造](https://github.com/bminor/glibc/blob/glibc-2.39/malloc/malloc.c#L2400-L2483)、[free 分流](https://github.com/bminor/glibc/blob/glibc-2.39/malloc/malloc.c#L3358-L3406)、[munmap_chunk](https://github.com/bminor/glibc/blob/glibc-2.39/malloc/malloc.c#L3023-L3051)

因此，如果上述 B 是独立映射，释放 B 就能解除它的映射，C 的存活不会阻挡它。

需要精确区分：**“堆中间不能通过 `brk` 释放”不等于“其中的物理页永远不能回收”。** glibc 2.39 的显式 `malloc_trim()` 会通过 `mtrim()` 扫描空闲 bins，对块内部合适的完整页执行 `MADV_DONTNEED`。它可以丢弃物理页内容，但保留虚拟地址范围及分配器管理关系；普通 main arena 大块 `free()` 的自动收缩路径调用的是前面的 `systrim()`。[mtrim 源码](https://github.com/bminor/glibc/blob/glibc-2.39/malloc/malloc.c#L5149-L5227)

**为什么还要阈值，不把所有请求都独立映射？**

从上述实现可以直接比较成本：arena 中已有块的复用主要是链表操作和切分；独立映射则需要映射、解除映射，并且每次分配按页取整。小对象若各自独立映射，这些成本占比很高；大对象一旦被其他存活对象挡住，滞留的内存量又更大。**阈值是这两种成本的策略分界，并非 main arena 能分配的最大尺寸。**

这个策略甚至会随运行调整：`__libc_free()` 在释放符合条件的独立映射块时，会提高 `mmap_threshold`，同时把 `trim_threshold` 设置为其两倍。按源码的设计意图，这是把“某个尺寸的对象已经开始释放”作为它可能属于临时、反复分配对象的信号，使之后同类请求更倾向于堆内复用。[动态调整实现](https://github.com/bminor/glibc/blob/glibc-2.39/malloc/malloc.c#L3374-L3388)

最后，`main_arena` 是分配器管理结构，不能与 `brk` 完全画等号：它在扩堆失败时也有 mmap 后备路径；非主 arena 的 heap 本来就通过 mmap 建立。这里真正决定能否单独释放的，是**该对象是否作为 `IS_MMAPPED` 独立块管理**，而非底层内存是否曾经由 mmap 获得。[main arena 后备路径](https://github.com/bminor/glibc/blob/glibc-2.39/malloc/malloc.c#L2715-L2742)、[arena heap 定义](https://github.com/bminor/glibc/blob/glibc-2.39/malloc/arena.c#L55-L69)

## 用户（2）

第一个问题，你的意思是大块的请求堵在了堆中间导致前面空闲的没办法合到top中释放，但是即使没有这个大块，前面的空闲块也会由于中间其他的used块而没办法合并到top啊，退一步讲，即使前面的空闲块没办法合并到top又有什么问题呢，他照样可以和前后空闲合并啊，照样可以单独释放MADV_DONTNEED物理页啊？第二个问题，unsorted bin、fast bin、tcache这些结构的管理方式是怎样的，glibc分配器是不是有一个数组，小下标的桶每个桶负责单独一个小尺寸的空闲块链表，然后大下标的桶每个桶负责一个区间的尺寸的空闲块链表，这个又是什么东西，和前面那些结构的关系是什么，除此之外还有哪些主要结构？

## 助手（2）

你的反驳成立。**“不能合到 top”本身不能证明必须使用 mmap；如果允许及时对堆内空闲页执行 `MADV_DONTNEED`，物理内存确实可以回收。**我上一条需要把“源码当前采用的策略”和“理论上做不到”区分得更清楚。

下面仍以 **glibc 2.39、普通 x86-64 配置**为准。

**先回答第一个问题。**

你说的三件事都成立：

- 任何存活块都可能阻止前面的空闲块并入 top，小块也一样。
- 不能并入 top 的空闲块，仍然可以和地址相邻的空闲块合并、被重新分配。
- 其中符合条件的完整页，可以通过 `MADV_DONTNEED` 回收物理页。

所以，大块使用 mmap 的理由，并不是“大块有一种小块没有的阻挡能力”。更准确地说，是**避免一个已经释放的大对象，其回收方式仍然依赖堆内其他对象以及额外的回收操作**。

对比源码中的实际行为：

| 操作 | 普通 main arena 堆块 | 独立 mmap 块 |
|---|---|---|
| `free()` | 合并相邻空闲块，进入 bin 或 top | 直接进入 `munmap_chunk()` |
| 自动向系统回收 | 满足条件时，`systrim()` 收缩 top | 解除该块覆盖的映射 |
| 中间空闲块的物理页 | 显式 `malloc_trim()` 可以扫描并丢弃完整页 | 释放映射时处理 |
| 回收后虚拟地址区间 | `MADV_DONTNEED` 后仍留在堆中 | 不再由该分配占有 |

这里最实在的差异是：**glibc 普通大块 `free()` 的 main arena 路径没有对这个中间块调用 `madvise`；它调用的自动收缩逻辑只检查 top。**扫描 bins 并执行 `MADV_DONTNEED` 的逻辑在另一条 `malloc_trim() → mtrim()` 路径中。[free 后的收缩判断](https://github.com/bminor/glibc/blob/glibc-2.39/malloc/malloc.c#L4769-L4798)、[mtrim 实现](https://github.com/bminor/glibc/blob/glibc-2.39/malloc/malloc.c#L5149-L5227)

假设你进一步问：“那我修改分配器，在释放大堆块时直接对它的内部完整页做 `madvise`，可以吗？”

**可以。这是一种可行的分配器设计，不存在必须使用独立 mmap 的逻辑定理。**但它保留了下面这些不同：

1. **虚拟地址范围仍由堆保留。**两个被存活对象隔开的空闲区间，即使物理页都丢弃了，也不能直接拼成一个更大的连续对象。
2. **块还在 arena 的切分、合并和链表管理体系中。**需要保留相关元数据；不能丢弃和元数据、存活对象共享的页。
3. **回收策略需要自己决定。**每次都丢弃页，会让后续复用重新承担页面建立和初始化成本；延迟丢弃，又会暂时保留物理内存。独立 mmap 同样有系统调用和再次分配的成本，并非没有代价。

因此，**如果你足够及时地调用 `malloc_trim()`，且主要关注 RSS，那么大块独立 mmap 的部分优势确实会缩小。**glibc 的阈值是在“保留块供复用”和“让这次分配独立归还系统”之间做启发式选择。不是因为 main arena 无法处理大块，也不是因为所有堆中间空闲内存都无法回收。

---

**再回答第二个问题：你描述的数组确实存在，但它只是其中一套。**

需要先分清三种不同关系：

- **地址相邻关系**：用于判断能否合并 chunk。
- **空闲链表关系**：用于找到能满足请求的 chunk。
- **管理归属关系**：这个 chunk 属于哪个 arena，或者暂存在哪个线程的 tcache。

**链表中的前后节点，通常不是地址相邻的 chunk。**

glibc 利用 chunk 的大小计算地址上的下一块，利用 `prev_size` 找到前一块；`fd/bk` 则连接空闲链表。合并操作依靠前者，分桶查找依靠后者。[chunk 与相邻块操作](https://github.com/bminor/glibc/blob/glibc-2.39/malloc/malloc.c#L1136-L1147)、[边界标记操作](https://github.com/bminor/glibc/blob/glibc-2.39/malloc/malloc.c#L1354-L1442)

整体关系可以画成：

```text
每个线程
└── tcache
    ├── entries[]：各尺寸单链表的头
    └── counts[]：各链表的节点数量

每个 arena：一个 malloc_state
├── mutex
├── fastbinsY[]：另一套按精确尺寸分桶的单链表
├── bins[]：存放各普通 bin 的双向链表头
│   ├── unsorted bin
│   ├── small bins
│   └── large bins
├── binmap：辅助跳过空 bin
├── top：当前顶部空闲块
├── last_remainder：特定切分余块的指针
└── next 等：连接其他 arena、维护状态

独立 mmap chunk
└── 通过自身头部记录释放信息，不进入上述空闲 bins
```

`tcache` 是每线程的；`fastbinsY`、`bins`、`top` 是每 arena 的。多个线程可以共享一个 arena，不能把 arena 理解成固定的一线程一份。[malloc_state 定义](https://github.com/bminor/glibc/blob/glibc-2.39/malloc/malloc.c#L1812-L1856)、[tcache 定义](https://github.com/bminor/glibc/blob/glibc-2.39/malloc/malloc.c#L3104-L3128)、[arena 获取逻辑](https://github.com/bminor/glibc/blob/glibc-2.39/malloc/arena.c#L108-L138)

**你说的“小下标对应一个尺寸、大下标对应尺寸区间”，具体是 `bins[]`。**

下面所有尺寸均指**经过对齐和开销处理后的 chunk size**，不是直接指 `malloc(n)` 的 `n`。

`bins[]` 实际存储的是成对的 `fd/bk` 指针。每对指针充当一个循环双向链表的哨兵，`bin_at(av, i)` 将逻辑 bin 编号映射到这对指针。

在上述配置下：

| 逻辑 bin 编号 | 类别 | 收纳的 chunk size |
|---|---|---|
| 1 | unsorted bin | 混合尺寸，不按尺寸排序 |
| 2～63 | small bins | 每桶一个精确尺寸：32、48、64……1008 字节 |
| 64～126 | large bins | 每桶一个尺寸区间，从 1024 字节开始 |
| 0、127 | — | 0 不存在；127 不被正常尺寸索引使用 |

`NBINS` 是 128，底层 `bins` 数组有 254 个指针槽，并不是 128 个 chunk 指针各自表示一条链表。[bin 头与尺寸索引源码](https://github.com/bminor/glibc/blob/glibc-2.39/malloc/malloc.c#L1520-L1607)

small bin 的索引，在这里就是 chunk size 除以 16。例如 64 字节 chunk 对应逻辑 bin 4。

large bin 的区间逐步变宽，由 `largebin_index_64()` 的分段移位表达式计算。例如：

```text
bin 64：1024、1040、1056、1072
bin 65：1088、1104、1120、1136
……
后面的桶覆盖更宽区间
```

注意：**large bin 的“large”从约 1 KiB 开始，与 mmap 的“大块阈值”不是一回事。**

**large bin 不是把区间内的块随便串起来。**

它同时使用：

- `fd/bk`：连接桶内所有块，按大小组织，相同大小的块相邻。
- `fd_nextsize/bk_nextsize`：连接不同尺寸的代表节点，辅助跳过相同尺寸的重复块。

因为一个桶包含多种尺寸，分配时还要在桶内找“足够大且尽量小”的块。找到之后可以切分，多余部分重新进入 unsorted bin。[large bin 插入与选择](https://github.com/bminor/glibc/blob/glibc-2.39/malloc/malloc.c#L4167-L4318)

**unsorted bin 则是暂缓尺寸分类的中转链表。**

普通 chunk 释放并完成相邻合并后，如果没有并入 top，通常先进入 unsorted bin。某些切分剩余块、fastbin 整理产生的块也会进入这里。

之后 `_int_malloc()` 处理 unsorted 时：

```text
遇到能立即利用的块
    → 分配，或者暂存进 tcache

未在该步骤利用的块
    → 按尺寸转入 small bin / large bin
```

这里并不是“任何够大的 unsorted 块都立即切分”：一般优先处理精确匹配，另外还有针对 `last_remainder` 的特殊切分路径。其目的，是给近期释放的块一次直接复用机会，避免马上做完整的尺寸分类。[unsorted 处理源码](https://github.com/bminor/glibc/blob/glibc-2.39/malloc/malloc.c#L4044-L4169)

**fastbin 是另一套数组，不是 small bin 的别名。**

`av->fastbinsY[]` 中，每个桶也是一个精确 chunk size，但采用**单链表、后进先出**。上述配置分配了 10 个数组槽，默认启用的尺寸范围是 32～128 字节。

与 small bin 最关键的区别是：

> **进入 fastbin 时，不立即将块交给相邻空闲块合并逻辑。**

源码中，块进入 fastbin 后，其占用状态仍然保留。更精确地说，表示“这个块占用”的位，位于**后一个 chunk 的 `PREV_INUSE`** 中。

这样下次同尺寸分配可以直接取出；代价是这些块暂时不能参与合并。需要整理时，`malloc_consolidate()` 批量取出 fastbins 的块，合并后送入 unsorted 或 top。[fastbin 定义](https://github.com/bminor/glibc/blob/glibc-2.39/malloc/malloc.c#L1711-L1736)、[批量合并实现](https://github.com/bminor/glibc/blob/glibc-2.39/malloc/malloc.c#L4800-L4908)

**tcache 又是一套每线程的精确尺寸缓存。**

glibc 2.39 默认：

- 64 个桶，chunk size 为 32、48……1040 字节。
- 每桶最多缓存 7 个 chunk，可调。
- 普通 `malloc/free` 使用单链表头部插入、头部取出。
- 链表节点信息放在已释放块原来的用户数据区。

tcache 命中可以避免进入 arena 的常规分配路径。进入 tcache 的块，同样暂时保留占用状态，不参与相邻合并。

**tcache 满了，不是把整个桶倾倒进 fastbin；而是这次新释放的块继续走后续路径。**已经缓存的那些块仍留在 tcache 中。[tcache 插入和取出](https://github.com/bminor/glibc/blob/glibc-2.39/malloc/malloc.c#L3153-L3200)、[free 中的容量判断](https://github.com/bminor/glibc/blob/glibc-2.39/malloc/malloc.c#L4510-L4569)

所以这几套结构的尺寸范围会重叠。以 **64 字节 chunk** 为例：

```text
tcache 对应桶：index 2
fastbin 对应桶：index 2
small bin 对应桶：逻辑编号 4
```

这表示同尺寸的不同块可以在不同管理阶段进入不同结构；**同一个空闲块不会同时挂在这些链表里**。

普通 `free()` 的主要分流可以概括为：

```text
独立 mmap 块？
  是 → munmap

普通 arena 块：
  tcache 尺寸适用且对应桶有空间？
    是 → tcache，返回

  fastbin 尺寸适用？
    是 → fastbin，返回

  与相邻空闲块合并
    → 到达 top：并入 top
    → 否则：进入 unsorted
```

分配时的主要路线则是：

```text
tcache
  → 对应 fastbin（尺寸适用时）
  → 对应 small bin（小尺寸请求）
  → 处理 unsorted
  → large bin / 借助 binmap 查找更大的桶
  → top
  → sysmalloc
```

这不是每次都完整走一遍：大请求可能先触发 fastbin 合并；从 arena 取块时也可能顺便把其他同尺寸块补入 tcache。[分配快速路径](https://github.com/bminor/glibc/blob/glibc-2.39/malloc/malloc.c#L3906-L4045)

其余几个主要结构的作用是：

| 结构 | 作用 |
|---|---|
| `top` | 特殊顶部空闲块，不挂在普通 bin 中；用于切分、扩展和顶部收缩 |
| `last_remainder` | 指向特定的小请求切分余块；是一个指针，不是独立内存池 |
| `binmap` | 位图索引，帮助跳过已知为空的普通尺寸桶 |
| `malloc_state` | 一个 arena 的管理状态，包括锁、上述链表头、统计和 arena 链接 |
| `heap_info` | 非主 arena 的 heap 区域头；一个 arena 可以管理多个这样的区域 |
| `malloc_par` / `mp_` | 全局策略参数与统计，例如 mmap 阈值、trim 阈值、tcache 容量 |

[arena 管理字段](https://github.com/bminor/glibc/blob/glibc-2.39/malloc/malloc.c#L1812-L1899)、[heap_info 定义](https://github.com/bminor/glibc/blob/glibc-2.39/malloc/arena.c#L55-L69)

这也补充了你第一个问题的边界：**应用已经 `free()`，并不意味着该块已经成为可合并、可由 `mtrim()` 扫描的普通空闲块。**`malloc_trim()` 会整理 fastbins，但不会清空所有线程的 tcache；仍在 tcache 中的块，依然可能阻挡地址相邻的空闲块合并。

## 用户（3）

第一个问题，所以意思就是，大块分配直接mmap，可以再free的时候直接munmap释放物理内存了，而如果是走堆的话，可能会由于被后面存活对象堵住无法进入top被trim，而进入空闲回收复用结构的话，又会由于复用需要降低后续复用时的缺页成本而不会选择马上MADV_DONTNEED释放物理页，就导致大请求占据的物理内存可能使得在上层free后RSS短时间内仍然较高，那MADV_DONTNEED操作是在malloc/free链路的哪个步骤？第二个问题，chunk在被free后进行合并的时候怎么找到前一个chunk的，为什么一个chunk的used标志位是在后一个chunk的prev size空间中，后一个chunk的这8B的空间不是可以给用户用的吗？第三个问题，unsorted bin的容量是多大，他是怎么回smallbin和largebin的？第四个问题，chunk合并的时候是包括fastbin、unsortedbin、smallbin、largebin、top这些一起参与吗？

## 助手（3）

你的第一个理解基本正确，但要把“RSS 短时间内仍较高”改成：**RSS 可能持续较高，glibc 没有保证过一段时间就自动回收这些堆中间空闲页。**

以下继续按 **glibc 2.39、普通 x86-64 配置**解释。

**1．`MADV_DONTNEED` 到底在哪条调用链上？**

需要区分三条路径。

**main arena 的普通 `free()` 自动回收路径：**

```text
free
 → _int_free
 → _int_free_merge_chunk
 → _int_free_maybe_consolidate
 → systrim
 → 负数 MORECORE，也就是收缩 brk
```

这里没有针对堆中间空闲块的 `MADV_DONTNEED`。只有满足条件时才尝试缩 top；不能并入 top 的块留在空闲管理结构中。

保留已经驻留的页，确实能让后续复用避免重新建立页面的成本。但从代码执行上说，**并不是每次 free 都动态评估一次“要不要 madvise”，而是这条路径根本没有这样的操作。**[自动回收源码](https://github.com/bminor/glibc/blob/glibc-2.39/malloc/malloc.c#L4769-L4798)

**显式调用 `malloc_trim()` 的路径：**

```text
malloc_trim
 → __malloc_trim
 → 逐个锁住 arena，调用 mtrim
     → malloc_consolidate：整理该 arena 的 fastbins
     → 扫描 unsorted 和可能包含足够大块的普通尺寸 bins
     → 对空闲块内部适合回收的完整页执行 MADV_DONTNEED
     → main arena 另外尝试 systrim
```

`mtrim()` 会跳过块头和链表元数据，并把地址、长度调整到完整页范围。它保留 chunk 和链表关系，因此以后仍可以复用这段虚拟地址。[mtrim 源码](https://github.com/bminor/glibc/blob/glibc-2.39/malloc/malloc.c#L5149-L5227)

**非主 arena 的普通 `free()` 还有另一条路径：**

```text
_int_free_maybe_consolidate
 → heap_trim
 → shrink_heap
 → MADV_DONTNEED
```

这里处理的是该 heap 的**尾部空间**。`shrink_heap()` 另有条件分支，可能用固定地址的 `PROT_NONE` 映射替换尾部；整个后续 heap 可以移除时，`heap_trim()` 还会直接 `munmap`。[非主 arena 收缩源码](https://github.com/bminor/glibc/blob/glibc-2.39/malloc/arena.c#L506-L610)

所以不能概括成“malloc/free 内完全没有 `MADV_DONTNEED`”。准确说法是：**普通 main arena 的中间空闲块，不会因为这次 `free()` 就直接执行它；显式 `malloc_trim()` 可以处理这种情况。**

---

**2．怎样找到前一个 chunk？借用的 8 字节为什么不会覆盖占用标志？**

你把两个字段混在了一起：

| 当前 chunk 的字段 | 含义 |
|---|---|
| `prev_size`，8 字节 | 前一个 chunk 的大小，但只在前块处于可合并的空闲状态时有效 |
| `size`，8 字节 | 当前 chunk 大小，以及低位的标志 |
| `size` 中的 `PREV_INUSE` | **前一个** chunk 是否按占用状态处理 |

**标志位在 `size` 中，不在 `prev_size` 中。**

设当前块是 Q，前一块是 P。寻找 P 的算法是：

```text
检查 Q.size 的 PREV_INUSE：

为 1：
    前块按占用处理，不向前合并，也不读取 Q.prev_size

为 0：
    前块大小 = Q.prev_size
    P 的地址 = Q 的地址 - Q.prev_size
    将 P 从其空闲链表摘除，然后合并
```

对应 `_int_free_merge_chunk()` 中的 `prev_inuse()`、`prev_size()`、地址减法和 `unlink_chunk()`。[向前合并源码](https://github.com/bminor/glibc/blob/glibc-2.39/malloc/malloc.c#L4688-L4697)

用具体地址看最清楚。假设 P 的 chunk size 是 64 字节：

```text
地址          字段
0x1000        P.prev_size
0x1008        P.size，低位描述 P 前面的块

0x1010        P 用户区开始
  ...
0x103f

0x1040        Q.prev_size ── P 占用时，这 8B 可以借给 P 用户使用
0x1048        Q.size      ── 这里的 PREV_INUSE 才描述 P 是否占用
0x1050        Q 用户区开始
```

因此 P 的可用区域可以是：

```text
0x1010 ～ 0x1047，共 56 字节
```

它包含 `Q.prev_size`，**没有包含 `Q.size`**。这里被借用的 8 字节是给**前一块 P**使用的，不是给 Q 使用的。[布局和可用尺寸计算](https://github.com/bminor/glibc/blob/glibc-2.39/malloc/malloc.c#L1190-L1255)、[memsize 定义](https://github.com/bminor/glibc/blob/glibc-2.39/malloc/malloc.c#L1447-L1453)

假设 P 释放时进入普通合并路径，而非 tcache/fastbin，并且 Q 仍存活：

```text
Q.size 的 PREV_INUSE 被清零
Q.prev_size 被写成 P 的大小
```

此时 P 的用户已经放弃这块内存，glibc 可以重新把那 8 字节用作元数据。之后释放 Q，就可以利用它向前找到 P。如果 P 与前面的空闲块合并了，记录的就是合并后块的大小。

这就是 **boundary tag（边界标记）**：

- 当前块的头部记录自己的大小。
- 普通空闲块的尾部也记录大小，而这个尾部恰好表示为下一块的 `prev_size`。
- `PREV_INUSE` 指明这个尾部记录是否有效。

这样无需维护所有已分配块的双向链表，也能直接找到可合并的前块；占用期间又能复用不需要的尾部大小字段。

**如果 P 被放入 tcache/fastbin，则不会完成上述“清标志、写空闲边界”的转换；它暂时仍按占用块处理。**

---

**3．unsorted bin 容量多大？什么时候转入 small/large bin？**

**每个 arena 一个 unsorted bin，没有类似 tcache“每桶默认 7 块”的固定容量限制。**

它是一个循环双向链表，节点就嵌在空闲 chunk 里，所以不存在预先分配 N 个节点的容量。释放普通块时，完成合并后通常从链表头插入；分配时从尾部取出处理。[unsorted 定义](https://github.com/bminor/glibc/blob/glibc-2.39/malloc/malloc.c#L1649-L1662)、[插入实现](https://github.com/bminor/glibc/blob/glibc-2.39/malloc/malloc.c#L4732-L4747)

**转入 small/large bin 的触发点是 `_int_malloc()` 扫描 unsorted，和“装满了”无关。**

其主要逻辑可以写成以下伪代码：

```text
从 unsorted 尾部取一个 chunk：

若命中特定 last_remainder 切分条件：
    切分并返回，余块留在 unsorted

否则将该 chunk 从 unsorted 摘除：

    若 chunk 大小恰好匹配请求：
        返回给用户，或先放进 tcache

    否则：
        按这个 chunk 自己的大小计算桶编号
        小尺寸 → 插入 small bin
        大尺寸 → 有序插入 large bin
        设置对应 binmap 位
```

所以，一个比请求更大的普通 unsorted 块，可能先被归入 large bin，再在后面的 large bin 查找阶段被选中、切分。[unsorted 分类源码](https://github.com/bminor/glibc/blob/glibc-2.39/malloc/malloc.c#L4070-L4242)

源码中的两个限制不要误认为容量：

- `MAX_ITERS = 10000`：限制这一轮 unsorted 扫描迭代次数。
- `tcache_unsorted_limit`：控制填充 tcache 时的一部分扫描工作，默认 0 表示不施加该项限制。

它们都不是 unsorted 最多可以存多少块。如果分配一直在 tcache 等前面的路径命中，unsorted 也不一定马上被扫描、分类。

---

**4．合并时，这些结构里的块能不能一起参与？**

**能不能合并，首先取决于地址是否相邻、边界标记是否显示为空闲；不是取决于两个块在哪个桶。**

假设当前 P 已经进入普通合并路径：

| 相邻块所在位置 | 能否直接与 P 合并 |
|---|---|
| unsorted bin | 可以，先从原链表摘除 |
| small bin | 可以，先从原链表摘除 |
| large bin | 可以，摘除时同时维护尺寸辅助链 |
| top | 可以，走特殊分支，合并结果成为新 top |
| fastbin | 暂时不可以；先由 `malloc_consolidate()` 取出并整理 |
| tcache | 暂时不可以；`malloc_consolidate()` 不负责清空 tcache |
| 用户仍占用的块 | 不可以 |
| 独立 mmap 块 | 不参与这种 arena chunk 合并 |

例如下面是**地址顺序**：

```text
[ A：small bin ][ P：正在释放 ][ B：large bin ][ C：存活 ]
```

释放 P 时，可以直接：

```text
从 small bin 摘除 A
从 large bin 摘除 B
合成 [ A + P + B ]
把结果放入 unsorted
```

**不需要先把 A、B 都迁移到 unsorted 才能合并，也不需要遍历所有 bins 去寻找它们。**地址和大小字段已经定位到相邻块；随后 `unlink_chunk()` 用块内的链表指针将它摘除。[合并源码](https://github.com/bminor/glibc/blob/glibc-2.39/malloc/malloc.c#L4663-L4760)、[unlink_chunk](https://github.com/bminor/glibc/blob/glibc-2.39/malloc/malloc.c#L1608-L1647)

如果地址布局是：

```text
[ A：普通空闲 ][ F：fastbin ][ B：普通空闲 ]
```

F 暂时保留占用标志，因此 A、B 不能跨过它合并。等 `malloc_consolidate()` 取出 F，才会把三者合并。一次较大的 `free()` 可能在后续整理步骤中触发这个过程，所以 **fastbin 可以在同一次 free 调用引发的批量整理中参与，但不等于普通相邻合并直接识别 fastbin 节点**。[fastbin 整理源码](https://github.com/bminor/glibc/blob/glibc-2.39/malloc/malloc.c#L4800-L4908)

所有这些合并都受所属 heap 区域的边界约束；不同 arena、被边界标记隔开的区域，不会仅因地址接近就被合并。

## 用户（4）

第一个问题，那显式 malloc_trim() 发生在什么时候，是上层通过什么方法调吗？第二个问题，那chunk的第二个8字节size字段的标志位中是不是还得有一个标志位是显示当前chunk是否空闲可合并的，否则的话前一个chunk在free的时候想和他合并的时候怎么知道能不能合？第三个问题，那unsorted bin的插入chunk可以是fastbin整理合并后插入、largebin切分后剩余插入和free chunk合并后插入三种，而取出chunk是在malloc路径上走到smallbin和largebin时都没找到时会循环若干次每次从unsortedbin中取出一个用于分配/放回smallbin和largebin？

## 助手（4）

**第一个问题：是，上层直接调用 `malloc_trim()`，它是 glibc 提供的公开函数。**

C/C++ 程序包含 `<malloc.h>` 后即可调用：

```c
#include <malloc.h>
#include <stdlib.h>

void release_buffer(void *buffer)
{
    free(buffer);
    malloc_trim(0);
}
```

这只是演示调用方法。上层可以在一批任务结束、释放大量临时对象之后调用，也可以由使用的库或运行时调用。**调用时机由调用方决定，glibc 没有定时执行 `malloc_trim()` 的后台线程。**

参数 `0` 表示不要求为主堆顶部额外保留余量，但仍须保留分配器必要空间并满足页对齐。它不是“释放全部空闲内存”的保证，也不只处理刚刚 `free` 的那个指针，而是尝试回收整个进程各 arena 中符合条件的空闲空间。[接口说明](https://man7.org/linux/man-pages/man3/malloc_trim.3.html)

需要继续区分：

```text
上层主动调用 malloc_trim()
    → 扫描各 arena，回收适合的空闲页

普通 free() 内部满足条件
    → 调用 systrim / heap_trim，尝试顶部回收
```

后者并不是内部再调用一遍公开的 `malloc_trim()`。[实现入口](https://github.com/bminor/glibc/blob/glibc-2.39/malloc/malloc.c#L5207-L5227)

---

**第二个问题：不需要额外的“当前块是否占用”标志；向后合并时，再往后看一个 chunk。**

对于地址连续的三个普通 chunk：

```text
[ A ][ B ][ C ]
```

标志含义如下：

| 字段 | 描述谁 |
|---|---|
| `A.size` 的 `PREV_INUSE` | A 前面的块 |
| `B.size` 的 `PREV_INUSE` | A |
| `C.size` 的 `PREV_INUSE` | B |

所以 **free(A) 想判断能不能合并 B，读取的是 `C.size` 中的 `PREV_INUSE`。**

它不需要猜测 C 的地址，因为 B 自己的大小就在 B 的头部：

```text
B 的地址 = A 的地址 + chunksize(A)
C 的地址 = B 的地址 + chunksize(B)

B 是否按占用处理 = C.size & PREV_INUSE
```

glibc 的 `inuse(p)` 宏做的正是这个操作：**根据当前块大小找到下一块，再读取下一块的 `PREV_INUSE`。**[inuse 宏](https://github.com/bminor/glibc/blob/glibc-2.39/malloc/malloc.c#L1411-L1414)

落实到 `_int_free_create_chunk()`：

```text
nextchunk = B
nextsize  = chunksize(B)

如果 B 就是 top：
    直接合并到 top
否则：
    读取地址 B + nextsize 处的 size 字段低位
    也就是读取 C 的 PREV_INUSE

    如果为 0：
        B 可合并，将 B 摘链并合并
    如果为 1：
        不合并 B
```

这对应源码中的 `inuse_bit_at_offset(nextchunk, nextsize)`。[向后合并实现](https://github.com/bminor/glibc/blob/glibc-2.39/malloc/malloc.c#L4711-L4727)

因此，每个普通 chunk 的第二个 8 字节中只有这三个已使用标志：

- `PREV_INUSE`：前块按占用处理。
- `IS_MMAPPED`：当前块是独立映射块。
- `NON_MAIN_ARENA`：相关 arena 归属标志。

没有另一个 `CURRENT_INUSE`。**当前块的占用信息已经由下一块保存，不需要重复保存。**top 和独立映射块有特殊处理，不能套用普通相邻块读取逻辑。[标志定义](https://github.com/bminor/glibc/blob/glibc-2.39/malloc/malloc.c#L1353-L1380)

---

**第三个问题：你对 unsorted 来源的理解基本正确，但查找顺序需要改。**

你列出的三类确实是主要来源，前提是结果没有并入 top：

- fastbin 整理、合并后的普通空闲块。
- 从普通 bin 取块并切分后的余块。
- 普通 free 路径完成合并后的块。

第二项不应仅限于“large bin 切分”：小请求也可能从更大的普通 bin 取块并切分，余块同样进入 unsorted。**切分 top 的余块则继续作为 top，不进入 unsorted。**

关于取出顺序，**不是“small bin 和 large bin 都没找到，再查 unsorted”。**在 tcache 等前置快速路径没有返回的情况下，主要顺序是：

```text
小尺寸请求：
    对应 fastbin（尺寸适用时）
      → 对应精确尺寸 small bin
      → 处理 unsorted
      → 借助 binmap 搜索更大的普通 bins
      → top
      → sysmalloc

大尺寸请求：
    必要时整理 fastbins
      → 处理 unsorted
      → 搜索当前尺寸所属的 large bin
      → 借助 binmap 搜索更大的 bins
      → top
      → sysmalloc
```

**对应 small bin 在 unsorted 前查；对应 large bin 在 unsorted 后查。**源码还专门解释了原因：small bin 中都是精确匹配，可以先直接取；large bin 要选尽量合适的块，因此先把近期释放的 unsorted 块纳入考虑。[前置 small bin 路径](https://github.com/bminor/glibc/blob/glibc-2.39/malloc/malloc.c#L3971-L4045)、[unsorted 之后的 large bin 路径](https://github.com/bminor/glibc/blob/glibc-2.39/malloc/malloc.c#L4251-L4278)

你说的“循环从 unsorted 取一个，用于分配或放入尺寸桶”是对的，具体是：

```text
从 unsorted 尾部取一个块
    → 满足特定 last_remainder 条件：切分使用
    → 精确匹配：返回，或先缓存进 tcache
    → 否则：按该块的大小归入 small bin / large bin
然后继续处理下一个
```

例如当前请求的规格化大小是 2000 字节，而 unsorted 中有一个 3008 字节块：

```text
扫描 unsorted：
    3008 != 2000
    → 先把这个块放入对应 large bin

随后搜索 large bin：
    这个块可能成为合适候选
    → 取出、切分、返回
```

也就是说，**同一次 malloc 可以把一个块从 unsorted 分类到 large bin，紧接着又从 large bin 取出来使用。**这两个阶段分别负责近期空闲块分类和按尺寸选择，并不要求等待下一次 malloc。[分类与后续选择源码](https://github.com/bminor/glibc/blob/glibc-2.39/malloc/malloc.c#L4125-L4318)

## 用户（5）

帮我把当前session的所有问答对话记录原封不动的记录在当前目录新建的一个md中。
