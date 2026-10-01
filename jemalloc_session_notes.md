# jemalloc 内存管理讨论总结

更新日期：2026-09-30。

本文整合旧文档前面的总结、末尾完整问答，以及后续 session 的全部技术讨论，按主题重新组织，不再单独保留问答原文。后续补充的条件和修正已融入正文，可用作继续讨论的上下文。

## 1. 分析范围与整体结构

- jemalloc 源码版本：本地 master，核对时提交为 `a47e019e`。
- 当前源码目录：`/workspaces/joey_project/jemalloc`。旧记录中的 `/workspaces/Joey-Project-For-Linux/jemalloc` 是之前环境的路径。
- 主要讨论普通 PAC 页分配路径、小对象 slab，以及 dirty/muzzy/retained ecache。HPA、guarded、pinned、特殊对齐、自定义 extent hooks 等可能走不同分支，不应直接套用普通路径的结论。
- 数字示例通常假设 64 位、4 KiB 页、16 B quantum、`SC_LG_NGROUP=2`。涉及 extent 大小的示例忽略额外 padding，除非另有说明。
- glibc 对比参考本地 `/tmp/glibc-2.39-malloc.c`，主要比较 largebin 的大小排序，不把它概括成 glibc 所有分配路径的行为。
- 本次讨论没有运行 benchmark；有关布局、局部性和碎片的收益需要区分源码事实、机制推断与实测结论。

```text
应用请求
  ├─ 小对象：tcache / arena 的对象 size-class bin
  │             └─ slab：切成固定大小的对象 region
  │                  └─ 需要新 slab 时向页分配器申请 extent
  └─ large allocation：通过相应路径取得 extent

普通 PAC 页分配层
  └─ 空闲 extent 缓存：dirty / muzzy / retained
       └─ eset：按 extent 大小分桶 + 桶内 (sn, addr) 堆 + LRU

定位与管理
  对象指针 → emap / radix tree → 外置 edata_t → extent / slab
```

小对象每次 malloc/free 不一定进入 extent 分配或回收路径，tcache、已有 slab，以及其他缓存都可能在更上层满足操作。

## 2. sn、稳定复用方向与 slab 排空

### 2.1 sn 的含义

`sn` 是 serial number，保存在 `edata_t::e_sn`。在所讨论的 PAC 路径中：

- 新取得的 extent 从所属 PAC 的递增计数器获得 sn。
- 拆分后的两部分继承相同的 sn。
- 合并后保留两个 extent 中较小的 sn。
- 普通释放、进入空闲集合、再次复用，不会因此重新取号。

因此，sn 不是最近释放时间，也不是全局唯一 extent ID，更不是所有 arena 共享的全局时间戳。它更接近保留下来的来源年龄；拆分和合并后，不能把它理解成每块当前 extent 的精确创建时间。

`(sn, addr)` 是字典序：先比较 sn，相同再比较地址。同一来源拆出的 extent 可以共享 sn，由地址区分。

### 2.2 优先较老、较低地址 slab 的作用

给复用建立稳定方向：优先向部分 slab 填入新对象，其他 slab 少接收新对象，就有机会随着原有对象释放而排空。这可能减少长期半空 slab、降低活跃页数量，并改善访问局部性。

它不会搬迁存活对象，不保证任意负载下都能排空。低地址数值本身没有硬件速度优势；局部性收益来自分配更集中、访问页数可能更少。

同一映射内、sn 相同、属于同一个 bin 的相邻 slab，可能形成：

```text
低地址                                      高地址
[A：优先补入对象][B：优先补入对象][C：逐渐排空][D：逐渐排空]
                                           └─有机会合并─┘
```

这是布局倾向，不是保证。跨 mmap 区域时，sn 顺序不等于地址顺序；后取得的映射可能在更低地址。不同 bin、shard、arena 也不会统一执行全局排序。高地址 slab 中一个长期存活对象就足以阻止它完全释放。

slab 完全空闲后交给页分配层，不等于立即 munmap 或立即降低 RSS。slab 排空与 ecache 选块处于不同阶段：前者尚有存活对象，后者管理整体空闲的 extent。

### 2.3 与头插栈 / LIFO 的区别

LIFO 也可能让栈底 slab 排空，并有简单操作、近期访问局部性等优势。但它优先选择最近进入 nonfull 的 slab，复用方向受释放事件影响。

例如 A 较老、早已有多个空位，C 较新、刚从全满变为有一个空位：LIFO 可能先填 C；`(sn, addr)` 则先填 A，给 C 继续排空的机会。

稳定排序使高优先级 slab 在重新出现空位后仍恢复原来的优先级。`bin_lower_slab()` 据此调整 slabcur。LIFO 没有同样的地址集中机制，因此空闲区域可能更分散，但不能据此断言它在所有负载上都更差。

此外，nonfull 集合需要删除任意一个已经完全空闲的 slab；单向头插栈本身不能高效完成任意删除。

历史背景：[jemalloc 作者关于 MRU 与低地址复用的讨论](https://lists.freebsd.org/pipermail/freebsd-current/2007-March/070303.html)。这属于特定设计经验，不能代替当前版本、实际负载的测量。

### 2.4 是否只能用堆

需求是动态维护最小候选，同时支持插入和删除。pairing heap 不是唯一实现：平衡树、有序链表、选择时扫描的无序集合都能实现这一语义，只是成本分布不同。

若“优先队列”指抽象功能，可以这样理解；若指必须采用堆，则不成立。jemalloc 在这里选择了 pairing heap。

## 3. 小对象 slab bin：桶数、size class 与内部结构

### 3.1 桶数不会随请求尺寸种类增加

小对象 size class 是预定义的有限集合，`SC_NBINS` 由构建配置决定。任意字节数的请求映射到既有 class，不会为每个新请求大小创建新 bin。例如 49～64 B 请求可映射到 64 B class。

在上述典型配置下：

```text
SC_NBINS = 1 + 4 + 4 × (12 + 2 - 6) - 1 = 36
最小小对象 class：8 B
最大小对象 class：14 KiB
最小 large class：16 KiB
```

size class 间距随尺寸增长，不是每个字节一个 class。超过小对象范围的请求走 large 路径，不继续增加 slab bin。当前代码用 uint8_t 编码小对象查找索引，并在 SC_NBINS > 256 时编译报错。

要区分逻辑桶数和实际 bin_t 数量：每个 class 可以配置多个 shard，arena 的实际 bin 数是各 class 的 shard 数之和。当前版本的 all_bins 是随 arena 创建、按 shard 设置确定长度的动态数组；创建后不随请求尺寸种类扩容。

- 对象数量增加：增加 slab，放入已有 bin。
- 请求尺寸种类增加：映射到已有 class。
- shard 增多：同一个 class 拥有更多 bin 实例，用于降低锁竞争。

运行时可通过 `arenas.nbins`、`arenas.bin.<i>.size`、`arenas.bin.<i>.slab_size` 查询实际配置。

### 3.2 每个 bin 管什么

```text
对象 size class → bin shard
                    ├─ slabcur：当前服务分配的 slab
                    ├─ slabs_nonfull：其他非满 slab，按 (sn, addr) 的堆
                    └─ slabs_full：满 slab 链表
```

slabcur 独立于另外两个集合。普通对象分配通常直接从其位图取 region，不必每次操作 heap。

一个 64 B bin 保存的是提供 64 B 对象的 slab，不是 64 B 大小的 slab。同一 bin 的 reg_size、slab_size 和 nregs 由配置确定。

### 3.3 与 ecache 分桶的区别

| 比较项 | 小对象 slab bin | ecache 中的 eset bin |
|---|---|---|
| 分类依据 | 对象 size class | 整个空闲 extent 的大小区间 |
| 管理对象 | 用于小对象分配的 slab | 整体空闲的 extent |
| 桶内大小 | 对象大小固定，slab 大小由该类配置决定 | extent 实际大小可能不同 |
| 分配动作 | 从 slab 取一个 region | 取一个 extent，必要时拆分 |
| 排序 | nonfull slab 按 (sn, addr) | 各桶 extent 按 (sn, addr) |
| 跨大小桶选择 | 通常直接定位对象所属 class | 可以比较多个足够大的桶 |

60 B 请求映射到 64 B bin 后，不会因为 80 B bin 有更老的 slab 就跨过去。只有需要新 slab 时，才按 slab 总大小向下层申请 extent。

## 4. edata_t、emap 与对象反向定位

### 4.1 edata_t 是外置元数据，叶项只存指针和摘要

普通用户 extent 的 edata_t 不放在 extent 开头，也不是 radix tree 叶 slot 内嵌的整个结构体。

```text
radix tree 叶 slot
  ├─ 指向 edata_t 的指针
  └─ size class、slab 标记、状态等少量信息
                ↓
外部元数据区中的 edata_t
  └─ 地址、大小、sn、状态、位图/链接等
                ↓
实际 extent 内存
```

紧凑叶项会将指针与标志编码到一个机器字。多个 slot 可以指向同一个 edata_t，并不是保存多份 edata。

`extent_alloc_wrapper()` 先从 edata_cache 取元数据，再调用分配 hook 取得 extent，最后关联二者；不足时由 `base_alloc_edata()` 分配元数据。拆分也会另取一个 edata 描述后半段。

### 4.2 为什么不采用 extent 内置 header

内置 header 可以设计，但外置有以下可从实现推导的优势，不应把这些收益误写成已经证实的唯一历史设计动机：

- extent 整段可以被 purge/decommit，而管理信息继续存活。内置 header 可能被清零或不可访问；若保留 header 页，又会阻碍整段物理页回收，或需要迁移元数据。
- 页级切分、slab 布局和对齐无需在每个 extent 开头预留整个 edata。元数据成本仍然存在，只是集中放在别处。
- 一个 slab 有多个对象；即使 slab 开头有 header，任意对象指针减去固定 header 长度，也不能得到 slab 起点。仍需地址映射，或对 slab 大小、对齐引入其他约束。

glibc 常见的每个 chunk 各自带 header，与每个包含许多对象的 slab/extent 只带一个 header，不是同一层次的定位问题。

### 4.3 free 的定位过程

```text
对象指针 ptr
  → 所在虚拟页
  → arena_emap_global 内的 radix tree
  → edata_t
  → slab、arena、bin、bin shard
```

active slab 的首尾和中间所有页都登记到同一个 edata。找到 slab 后，按 `(ptr - slab_base) / region_size` 得到对象编号；实现使用 `div_compute()`，然后更新位图、nfree 等。

但常见 free 快路径只是读取 class/slab 等信息，把指针放入 tcache；flush 或直接释放路径才真正归还 region、更新 slab。不能把调用 free 和该 region 已归还 slab 位图画等号；它可能先从 tcache 被复用。

## 5. radix tree 的宽度、稀疏性与物理内存成本

### 5.1 层数由有效虚拟地址位决定

64 位指针不代表使用全部 64 位虚拟地址。代码采用：

```text
RTREE_NSB = LG_VADDR - LG_PAGE
有效页号位数 ≤ 10：1 层
有效页号位数 ≤ 36：2 层
有效页号位数 ≤ 52：3 层
```

在 4 KiB 页下：

| LG_VADDR | 有效页号位数 | 层数 | 各层索引位数 |
|---:|---:|---:|---|
| 48 | 36 | 2 | 18 + 18 |
| 57 | 45 | 3 | 15 + 15 + 15 |
| 64 | 52 | 3 | 17 + 17 + 18 |

不会出现 52 位页号任意配置成单层数组的情况。每线程还缓存叶节点，减少树遍历。

### 5.2 节点确实可能很大，但虚拟大小不等于 RSS

48 位有效地址、8 B 紧凑叶项时，一个叶数组有 2^18 个 slot，虚拟大小为 2 MiB。控制成本的机制包括：

1. 子节点按需创建，不预先分配整棵树。
2. 元数据使用 demand-zero；base_alloc() 注释明确说明 auto arena 利用它降低 radix tree 等稀疏多页结构的物理内存成本。
3. 有效 slot 集中在少数元数据页时，页内利用率更高。

以普通 4 KiB 物理页、8 B 叶项计：一个元数据页放 512 个 slot，覆盖 2 MiB 用户虚拟地址范围。忽略边界等开销，写入 512 个相邻 slot 可能只触及一个元数据页；分散到 512 个不同元数据页则可能触及 2 MiB 元数据。

所以“多个集中区间的稀疏分布”通常比全地址空间随机散落更有利。实际成本取决于触及多少元数据页，不仅是当前非空 slot 数；也不能认为 slot 清空就自动释放承载它的元数据页。小堆、分布稀疏、元数据页利用率低时，开销比例仍可能较高。

密集映射时，叶项本身比例约为：

- 8 B / 4 KiB ≈ 0.195%。
- 常见非紧凑 16 B 叶项：约 0.39%。

这些不是总元数据比例，不含 edata、上层节点和稀疏浪费。

### 5.3 大块映射后切分与地址集中

`extent_grow_retained()` 会按增长策略取得较大的连续虚拟区域，拆出当前请求，保留余量。同一映射内切出的 extent 处于连续虚拟地址范围，有利于共享 radix tree 节点和元数据页。

但不是每次分配都 mmap 大块；通常先尝试复用，行为也受 retain、hooks 和路径影响。大块映射同时服务于映射管理和后续复用，不能仅从效果推断它专门为 radix tree 而设计。

连续的是虚拟地址，不要求物理页连续；连续地址范围也不意味着所有叶 slot 都需要登记。

## 6. emap 登记范围与相邻 extent 合并

emap 既管理 active extent，也管理 jemalloc 保有的空闲 extent，不是仅以空闲块首地址为 key 的红黑树。

| 类型 | 普通路径需要登记的页 |
|---|---|
| active slab | 首尾和所有中间页 |
| 普通 active large extent | 首尾边界，通常不登记所有中间页 |
| 普通空闲 extent | 首页和末页 |

仅登记两个 slot 不会把叶数组缩成两个元素，主要减少写入和需触及的元数据页。

对于 `[base, base + size)`：

```text
前邻居查询：base - PAGE          → 前一个 extent 的末页
后邻居查询：base + size          → 后一个 extent 的首页
本 extent 的末页：base + size - PAGE
```

无需有序树的前驱、后继查找。找到邻居后，还要检查状态、arena、页分配器、commit 状态等兼容性，执行 hooks，并更新 eset 和 emap。地址相邻不代表一定能合并；dirty 与 retained 不会因此直接合并。

## 7. ecache / eset 的分桶与选块

```text
ecache
  ├─ eset
  │   ├─ bitmap：哪些大小桶非空
  │   ├─ bins[pind]
  │   │   ├─ pairing heap：按 (sn, addr) 排序的空闲 extent
  │   │   └─ heap_min：堆顶比较摘要
  │   └─ lru：空闲入队顺序
  └─ guarded_eset：带 guard page 的 extent，采用同类结构
```

普通 eset 使用 `bins[SC_NPSIZES + 1]`。64 位、4 KiB 页、SC_LG_NGROUP=2 时：

```text
SC_NPSIZES = 4 + (62 - (12 + 2)) × 4 + 4 - 1 = 199
数组长度 = 200
```

这与 36 个小对象 bin、glibc 的 NBINS=128 都是不同概念。

### 7.1 为什么桶内不按 size 排序仍能分配

插入按实际大小向下量化：`sz_psz_quantize_floor()`；普通搜索起点向上量化：`sz_psz_quantize_ceil()`。从安全起点及之后的桶选候选，可保证容量足够；对齐等条件另行处理。

例如 `[80 KiB, 96 KiB)` 桶可含 80、84、88、92 KiB extent。请求 80 KiB 时，堆顶无论是哪一个都足够。普通 large size-class 模式下，72 KiB 对象需求取整到 80 KiB；实际 extent 需求还可能受 padding、对齐影响。

关闭 large size classes 后，请求可能只取整到页，前一个桶里可能混有够大和不够大的 extent。当前代码增加有上限的桶内枚举搜索，不能盲取该桶堆顶；特殊对齐也有补充查找路径。

### 7.2 普通选块不是 best-fit

`eset_first_fit()` 的普通路径比较允许范围内多个桶的堆顶，按 (sn, addr) 选择，不保证遇到最小合适桶就停止。桶内也不保证取到 size 最接近请求的块。

例如：

```text
同一个 [80 KiB, 96 KiB) 桶：
A：80 KiB，sn = 20
B：92 KiB，sn = 10

请求 80 KiB → 可能选 B，拆成 80 + 12 KiB，留下 A
```

跨桶也可能跳过较新的精确匹配，去拆较老的大块。若拆出的部分仍在使用，原大块就不能恢复，用于后续大请求的能力可能受损。

“候选都够大”只保证满足当前容量需求，不保证碎片效果相同。即便桶内改为按 size 排序，只要跨桶仍优先 sn，也不保证全局 best-fit。

8 KiB 是一个需特别说明的例子：4 KiB 页、无额外 padding 时，`[8 KiB, 12 KiB)` 桶内只可能有 8 KiB extent，没有 9/10 KiB extent。但 8 KiB 请求仍可能跨桶选择较老的 16 KiB extent 并拆分。

## 8. 与 glibc largebin 的比较

glibc largebin 按实际 chunk 大小维护顺序，以便在包含请求的桶中找到最小的足够大的 chunk。桶内可能同时存在小于和大于请求的块；大小排序既支持有效查找，也有利于减少不必要的拆分、保留较大空闲块。

这是一种策略选择，不是唯一可行结构。glibc 也可以改成“从下界不小于需求的桶开始，任取一块再切分”，但要承担代价。

示意桶 A 为 [1024, 1088)，桶 B 为 [1088, 1152)，已处理头部和对齐后的需求为 1040：

- 实际分配大小也上取整为 1088：可直接选择 B，但增加取整造成的内部碎片。
- 只把搜索起点上移至 B，仍按 1040 切分：不会产生同样的取整开销，但可能错过 A 中恰好 1040 的空闲块，反而切碎更大块。

glibc 的 largebin 边界是空闲块分类边界，并不天然就是请求分配粒度。jemalloc 的对象 size class 则还规定了请求采用的分配大小。

两者都存在碎片取舍：size best-fit 倾向保留大块；稳定的年龄/地址优先倾向集中复用。这两个目标可能冲突，不能仅从其中一个目标证明整体效果优劣。

## 9. 大 extent 被小请求切分：风险、保护与参数

### 9.1 风险真实存在，但发生频率未测量

要区分：

1. 大 extent 被拆分。
2. 明明有尺寸更合适的候选，仍选择较老的大块。
3. 这种选择使后续大请求无法复用原来的大块。

第 1 件事本身是正常分配机制，不一定有害；讨论担心的是第 2 件事造成第 3 件事。

拆分余量继承 sn，可能被连续优先切分：

```text
A：1 MiB，sn = 10；B：64 KiB，sn = 20
连续请求 64 KiB，且比例等限制允许：
A → 64 KiB 使用中 + 960 KiB 空闲，余量 sn 仍为 10
  → 64 KiB 使用中 + 64 KiB 使用中 + 896 KiB 空闲
B 可能仍然空闲
```

如果之后需要 1 MiB，而小分配还存活，就无法恢复 A。即使小分配释放，dirty 延迟合并也可能暂时阻碍恢复；当前版本某些较大 dirty extent 会提前合并，不能一概认为释放后绝不合并。

“释放大块 → 穿插小请求 → 再请求大块”、大小混用且寿命不同的负载，更可能暴露这一风险。这是机制推断；本次讨论没有运行 benchmark，不能断言真实应用中频繁或罕见。小对象 malloc 很频繁也不意味着每次都进入这层拆分路径。

### 9.2 lg_extent_max_active_fit

该参数主要限制普通 dirty/延迟合并路径中可参与选块的大小范围，不是拆分次数上限，也不是缓存容量上限。令参数为 k，请求大小为 R，直观含义为候选与请求的比例约不超过 2^k。

| k | 名义比例 | R = 64 KiB 时的名义候选大小上限 |
|---:|---:|---:|
| 2 | 4 倍 | 256 KiB |
| 4 | 16 倍 | 1 MiB |
| 6（默认） | 64 倍 | 4 MiB |

具体实现按桶筛选：

```cpp
if ((sz_pind2sz(i) >> lg_max_fit) > size) {
    break;
}
```

超过阈值后不再搜索该桶及更大桶；范围内仍按 (sn, addr) 选择。因为判断针对桶代表大小、采用整数移位，而桶内实际大小可能不同，不能把文档比例理解成逐 extent 的精确除法上限。特殊对齐请求还会调整搜索大小并采用补充路径。

| 调整方向 | 可能收益 | 可能代价 |
|---|---|---|
| 调大 | 更容易使用已有 dirty 内存，减少从其他路径取得内存的需要 | 更容易把大块切小，损害后续大请求复用 |
| 调小 | 更保护完整大块，减少悬殊大小的拆分 | 大块闲着，小请求却要另找内存，可能增加活跃 extent、RSS 或分配成本 |

例如 1 MiB 给 64 KiB 请求拆分，比例为 16 倍：k=6 时允许，k=2 时该块不参与普通 dirty 选块。

调小不保证降低 RSS，调大不保证提高性能。参数不知道未来负载；若大请求很快回来，保留大块可能划算，若之后一直只要小块，限制过严可能降低利用率。

普通 retained/muzzy 选块不使用同样的 dirty 限制；pinned、guarded 等还有各自例外。该版本 extent_record() 的较大 dirty extent 提前合并逻辑也引用这个参数设置合并大小限制，不能把调参影响理解成只有一处选块判断。

结论：jemalloc 允许拆分，同时对部分路径提供额外保护；不是不考虑大块碎片，也不是保证大块不会被切碎。

## 10. dirty 延迟合并的取舍

dirty extent 可能很快被相似大小的请求复用。立即合并可能制造“刚合并又拆分”的操作：

```text
保留边界：两个相邻 8 KiB → 有机会被两个 8 KiB 请求直接复用
立即合并：8 + 8 KiB → 16 KiB → 后续请求又拆成 8 + 8 KiB
```

这里的“有机会”不能省略：原来的例子不是精确匹配保证。即使保留了 8 KiB，普通跨桶选块也可能选择别的较老大块。

合并/拆分通常不复制整个用户数据，但需要操作 heap、LRU、统计、emap、edata 和 hooks，并增加相关锁内工作。延迟合并先省掉当下操作，同时保留直接复用原尺寸块的机会。

当前折中：

- dirty 配置延迟合并，淘汰时补做；较大的 dirty extent 也可能在放回时进行有限度合并。
- muzzy/retained 通常放回时立即尝试合并。
- 合并可减少 extent 元素数，但不会缩小固定的分桶数组。

## 11. LRU、复用优先级与 decay

### 11.1 两种顺序分别做什么

| 顺序 | 含义与用途 |
|---|---|
| (sn, addr) | 在适合当前请求的候选中确定复用方向 |
| LRU 空闲队列 | 近似反映本次空闲入队的先后，用于回收选择 |

普通 extent 插入 eset 时进入 heap 和 LRU 尾部；复用时同时移除，下次释放重新入队。它不跟踪应用对内存的每次读写，不是创建时间队列。合并也会调整队列位置；延迟合并后的结果可能放在邻居的位置。pinned 等特殊路径存在不进入 decay LRU 的例外。

dirty/muzzy 的 decay 根据时间和历史空闲页数量计算保留目标，超过目标时从 LRU 头部按 extent 处理，并可能补做合并。它不是每块 extent 的精确过期计时器。

### 11.2 为什么不直接淘汰最大的 (sn, addr)

这是可行的另一种策略：若所有候选尺寸、对齐等条件相同，淘汰复用顺序靠后的块，有利于保留下一次更可能选中的区域。

但 sn 反映来源年龄，不代表近期使用情况；一个老块可能闲置很久，一个新块可能刚释放、很快再用。候选是否适合请求也重要，例如老的 1 MiB 块受到比例限制，不能服务反复出现的 64 KiB 请求，而新释放的 64 KiB 块可以。

因此，LRU 的作用是给回收加入空闲时间这一维度。它确实可能淘汰 sn 小的块；这不等于策略自相矛盾，也不证明它在所有负载下优于按 sn 逆序淘汰。过早 purge 最近释放的块，也可能增加紧接着复用时重新建立物理页的成本。

### 11.3 两种顺序可能形成一致的整体倾向

在尺寸、对齐等条件相近且持续分配释放时：

```text
(sn, addr) 小 → 更常被复用 → 离开 LRU
                         → 再释放时回到尾部
(sn, addr) 大 → 较少被复用 → 持续空闲，逐渐靠近头部 → 更容易被淘汰
```

所以旧区域反复复用、新区域闲置后回收，是合理的反馈机制。但老 extent 正在使用时根本不在 LRU，不能简单说老块总在尾部。尺寸不匹配、寿命差异、合并等也会破坏这一相关性，不能无测量地认定“长期空闲的主要都是 sn 大的”。

## 12. retained 的复用、LRU 与虚拟地址生命周期

### 12.1 仍然分桶、选择、拆分

普通 retained 复用仍按大小定位候选，并偏向 (sn, addr) 小的 extent，必要时拆分；通常不使用 dirty 的拆分比例限制。

立即合并不意味着只剩一个大 extent：不相邻的映射、被活跃区域隔开的空闲块、不兼容的状态或 hooks 都会阻止合并。合并后按新大小重新入桶；拆分余量继续保存。

### 12.2 不做普通 decay，LRU 用于统一管理和销毁遍历

retained 不会仅因闲置久而被常规 decay 淘汰，因此其 LRU 不承担 dirty/muzzy 那种日常淘汰职责。按时间排序对于普通 retained 复用不是必要条件。

当前实现复用统一 eset 操作，并在 pac_destroy() 中通过 ecache_evict(retained, 0) 遍历，再调用 destroy hook。因此链表并非完全没有用途；销毁本身只需要遍历，并不依赖严格 LRU 顺序，理论上可以采用其他结构。

### 12.3 是否归还虚拟地址空间取决于配置和阶段

| 情况 | 行为 |
|---|---|
| 默认 mmap hooks，retain=true | 普通回收保留映射，尝试回收物理页，留待复用 |
| 默认 mmap hooks，retain=false | 普通回收可以解除映射，归还虚拟地址空间 |
| 自定义 hooks | 由 hooks 对 dalloc、purge、decommit、destroy 等的实现决定 |
| arena/PAC 销毁 | 遍历清理 retained；默认 mmap destroy 路径解除映射 |

常见 64 位 Linux 默认启用 retain；不能将此默认值套用到全部平台。默认 mmap 普通释放在 !opt_retain 时调用 pages_unmap()；默认 mmap destroy 则是另一条解除映射路径。

因此，常见配置下“释放物理页、保留虚拟地址”基本正确，但不能说所有映射永远不会解除，也不能说 extent 最终只会停在 retained：它可以反复被复用、释放、拆分和合并。

retained 的核心是分配器仍保留这段虚拟地址，不严格保证所有物理页已经归还。有些区域是从未触及的映射余量；有些被 purge/decommit；hooks 行为或回收失败可能留下物理页。

extent 离开 dirty 也不等于进入 retained：可能 lazy purge 后进入 muzzy，可能直接 decommit/purge 后 retained，也可能成功解除映射。合并、释放物理页、解除虚拟映射是三种不同动作。

## 13. 继续讨论时应保留的关键边界

- 区分对象 size class、slab 总大小、extent 大小桶；不要混用三者的数量和取整规则。
- 区分仍有存活对象的 slab 与 ecache 中整体空闲的 extent；前者谈排空，后者谈重新占用和回收。
- sn、虚拟地址大小、这次空闲入队时间是不同维度。
- edata_t 本体在外部；radix tree 叶项保存指针和少量信息。
- 候选容量足够不等于 best-fit，也不等于没有碎片代价。
- 稳定复用方向、精确尺寸匹配、延迟合并、及时回收可能互相取舍，不能把一种策略说成普遍最优。
- 大块被切碎是已识别风险，限制和合并只能缓解；本次没有实际负载 benchmark，不能给出发生频率或性能优劣结论。
- 元数据虚拟空间、实际物理页、用户 active 页、retained 地址空间不是同一个统计口径。
- 先前“两块 8 KiB 直接复用”的例子应始终理解为可能路径，而非实现保证。

## 14. 主要源码入口与参考

以下路径相对于 `/workspaces/joey_project/jemalloc`，具体行号可能随版本变化：

| 主题 | 文件与符号 |
|---|---|
| size class 和 bin 数量 | `include/jemalloc/internal/sc.h` |
| bin/slab 配置 | `include/jemalloc/internal/bin_info.h`、`src/bin_info.c` |
| arena bin 数组与分片 | `include/jemalloc/internal/arena.h`、`include/jemalloc/internal/bin.h` |
| slab 选择和切换 | `src/bin.c`：`bin_lower_slab()` |
| sn 和比较 | `include/jemalloc/internal/edata.h`：`edata_snad_comp()`；`src/extent.c`：`extent_sn_next()` |
| 元数据分配、demand-zero | `src/base.c`：`base_alloc()`、`base_alloc_edata()` |
| 小对象直接释放 | `src/arena.c`：`arena_dalloc_small()`；`include/jemalloc/internal/bin_inlines.h` |
| eset 分桶、堆、LRU | `include/jemalloc/internal/eset.h`、`src/eset.c`：`eset_insert()`、`eset_remove()`、`eset_first_fit()` |
| 页大小量化 | `src/sz.c`：`sz_psz_quantize_floor()`、`sz_psz_quantize_ceil()` |
| 页映射与邻居 | `include/jemalloc/internal/emap.h`、`src/emap.c` |
| radix tree 和缓存 | `include/jemalloc/internal/rtree.h`、`src/rtree.c` |
| 分配、拆分和合并 | `src/extent.c`：`extent_alloc_wrapper()`、`extent_split_impl()`、`extent_merge_impl()`、`extent_record()` |
| retained 扩展和复用 | `src/extent.c`：`extent_grow_retained()`、`extent_alloc_retained()` |
| 淘汰、物理页回收 | `src/extent.c`：`ecache_evict()`、`extent_dalloc_wrapper()` |
| PAC 分配、decay 和销毁 | `src/pac.c`：`pac_alloc_real()`、`pac_stash_decayed()`、`pac_maybe_decay_purge()`、`pac_destroy()` |
| mmap retain 与 unmap | `src/extent_mmap.c`、`src/ehooks.c` |

官方文档与本地版本可能存在差异，具体实现以对应提交为准：

- [jemalloc 手册](https://jemalloc.net/jemalloc.3.html)
- [拆分比例参数](https://jemalloc.net/jemalloc.3.html#opt.lg_extent_max_active_fit)
- [retain 配置](https://jemalloc.net/jemalloc.3.html#opt.retain)
- [extent hooks](https://jemalloc.net/jemalloc.3.html#arena.i.extent_hooks)
- [bin 数量查询](https://jemalloc.net/jemalloc.3.html#arenas.nbins)
- [glibc 2.39 malloc 源码](https://github.com/bminor/glibc/blob/glibc-2.39/malloc/malloc.c)


## 15. 本次 session 新增问答原文（追加于 2026-10-08）

### 用户

第一个问题，slab bins数组中相邻两个桶的size class相差多少字节，会不会出现某个分配给某个小请求的内存块的内部碎片过多的情况？第二个问题，slabs_full的链表指针存在哪里，extent本身没有header吧？第三个问题，ecache_retained在复用的时候，他没有ecache_dirty的比例限制，那是遍历取200个桶各自堆top的(sn,addr)然后比较吗？第四个问题，所以每次free至少都需要访问一次emap获取size class信息是吗？第五个问题，那在mmap一块大虚拟内存之后，在划分一块当前请求的extent之后，剩余的未划分区域会进入ecache_retained吗？

### 助手

核对了本地 `a47e019e` 的源码。下面沿用笔记中的配置：**64 位、4 KiB 页、16 B quantum，普通 PAC 路径**。

**1．相邻小对象 bin 的 size class 差多少？内部碎片会不会很大？**

**间距不是固定的，尺寸越大，间距也越大。** 这套配置下的 36 个小对象 size class 是：

| size class，单位 B | 这一组的步长 |
|---|---:|
| 8、16 | 8 |
| 32、48、64 | 16 |
| 80、96、112、128 | 16 |
| 160、192、224、256 | 32 |
| 320、384、448、512 | 64 |
| 640、768、896、1024 | 128 |
| 1280、1536、1792、2048 | 256 |
| 2560、3072、3584、4096 | 512 |
| 5120、6144、7168、8192 | 1024 |
| 10240、12288、14336 | 2048 |

规则是：进入常规分组后，在 `(B, 2B]` 这个区间安排四档，间距为 `B / 4`。例如 `(128, 256]` 对应 160、192、224、256。[源码：sc.h](/workspaces/Joey-Project-For-Linux/jemalloc/include/jemalloc/internal/sc.h:8)

请求会向上取整到最近的 class，因此**确实会产生对象内部碎片**：

| 请求 | 分配的 region | 多出的字节 | 多出部分占 region 的比例 |
|---:|---:|---:|---:|
| 1 B | 8 B | 7 B | 87.5% |
| 17 B | 32 B | 15 B | 46.9% |
| 65 B | 80 B | 15 B | 18.75% |
| 1025 B | 1280 B | 255 B | 19.92% |

这里必须区分比例的分母：

- **常规分组中**，取整浪费占实际分配大小小于 20%；相对于请求大小，额外开销小于 25%。
- 最前面的 tiny class 和初始分组不满足这个统一上限。极小请求的浪费比例可能很高，只是单个对象浪费的绝对字节数较小。

所以你担心的情况存在：**大量 17 B 对象，每个占 32 B，累计浪费可以很明显。**

不过，65 B 请求不会因为某个更大的 bin 有空闲对象，就直接拿一个 256 B region；它固定映射到 80 B class。下层为了提供 slab 而拆分大 extent，是另一层操作。

上述比例也只计算“对象请求大小到 region 大小”的取整浪费，**不包含半空 slab、slab 尾部余量和外置元数据成本**。

**2．`slabs_full` 的链表指针存在哪里？**

**存放在外置的 `edata_t` 中，链表串起来的是 `edata_t`，不需要在 extent 内放 header。**

对应字段是：

```cpp
ql_elm(edata_t) ql_link_active;
```

展开后包含两个指针：

```cpp
edata_t *qre_next;
edata_t *qre_prev;
```

逻辑结构是：

```text
bin.slabs_full
      ↓
   edata A ⇄ edata B ⇄ edata C
      ↓         ↓         ↓
   slab A    slab B    slab C
```

这些链表字段还与 pairing heap 的链接字段共用一个 union：进入 `slabs_full` 时使用链表字段，进入 `slabs_nonfull` 时使用堆字段。[源码：edata.h](/workspaces/Joey-Project-For-Linux/jemalloc/include/jemalloc/internal/edata.h:264)

**这里还发现笔记需要补充一个条件：当前版本的自动 arena 实际跳过 `slabs_full` 链表维护。** 手动创建的 arena 才维护它，以支持 `arena_reset`；自动 arena 不支持 reset，因此省去这部分链接操作。不能把“所有满 slab 都挂在 `slabs_full` 上”当成统一行为。[源码：bin.c](/workspaces/Joey-Project-For-Linux/jemalloc/src/bin.c:168)

**3．retained 没有 dirty 的比例限制，是比较 200 个桶各自堆顶吗？**

**核心理解正确，但不是每次固定访问全部 200 个堆顶。**

普通 retained 搜索大致是：

```text
根据需求大小，计算起始桶 pind
    ↓
使用 bitmap 找到 pind 及之后的非空桶
    ↓
比较这些桶缓存的堆顶 (sn, addr) 摘要
    ↓
选出其中最小的候选
    ↓
移出该 extent，必要时拆分
```

三个细节：

- **小于起始桶的桶不参加普通搜索。**
- **空桶通过 bitmap 跳过。**
- 每个桶都有 `heap_min` 摘要；先比较摘要，只有候选胜出时才取得该桶的堆顶 `edata`。并非先解引用所有堆顶的元数据。

因此，可以把搜索成本理解成与“**起始桶及之后的非空桶数量**”相关，而不是与全部空闲 extent 数量相关。它一般会扫完这个候选桶范围，**不会因为已经找到精确尺寸匹配就停止**。[源码：eset_first_fit()](/workspaces/Joey-Project-For-Linux/jemalloc/src/eset.c:292)

所以，如果较大的 retained extent 更老，它仍然可能胜过较新的精确匹配块。

这里说的是普通分配；原地扩容、只能精确匹配的路径，以及特殊对齐等情况有其他处理。

**4．每次 free 至少都要查一次 emap 获取 size class 吗？**

**对于有效、非空指针的普通 `free(ptr)`，是的。即使只是放回 tcache，也需要知道该放入哪个 size class 的缓存。**

小对象快路径大致是：

```text
free(ptr)
  → emap 查询得到 szind、slab 标记
  → 定位 tcache.bins[szind]
  → 将 ptr 放入缓存
```

但这不意味着每次都需要“从 radix tree 根节点一路查下去，再访问 `edata_t`”：

- 每线程有 radix tree 叶节点缓存，可以直接定位叶 slot。
- 叶 slot 已保存 size class 和 slab 标记；普通小对象快路径可以直接取这些摘要，**无需解引用外置 `edata_t`**。

对应代码是 [free_fastpath()](/workspaces/Joey-Project-For-Linux/jemalloc/include/jemalloc/internal/jemalloc_internal_inlines_c.h:467) 和 [emap_alloc_ctx_try_lookup_fast()](/workspaces/Joey-Project-For-Linux/jemalloc/include/jemalloc/internal/emap.h:317)。

两个例外需要保留：

- `free(NULL)` 不需要取得任何分配信息。
- `sdallocx(ptr, size, ...)`、`free_sized(ptr, size)` 等接口提供了大小，**符合条件的小对象快路径可以直接计算 class，跳过这次 emap 查询**；检查配置和慢路径仍可能查询。传入大小正是这类接口提供优化机会的原因。[官方手册](https://jemalloc.net/jemalloc.3.html)

**5．mmap 大区域后，切出当前请求，剩余部分会进入 retained 吗？**

**是的，在普通 `extent_grow_retained()` 路径中，切分后的余量会进入 `ecache_retained`。**

例如取得 2 MiB 映射，当前需要 64 KiB，忽略额外对齐要求：

```text
新映射的 2 MiB
┌──────────────┬─────────────────────────┐
│ 当前使用     │ 剩余空闲 extent         │
│ 64 KiB       │ 1984 KiB                │
└──────────────┴─────────────────────────┘
      ↓                     ↓
交给上层使用          ecache_retained
```

更准确地说，余量此时已经成为**有独立 `edata_t` 描述的空闲 extent**，并不是一段尚未纳入管理的地址。

如果需要更大的地址对齐，还可能产生前、后两段余量：

```text
[前部余量 lead][满足对齐的当前 extent][后部余量 trail]
       ↓                                  ↓
    retained                           retained
```

源码中 `extent_split_interior()` 产生 `lead`、`trail`，随后通过 `pac_record_grown()` 把普通余量放入 retained；pinned 余量则进入专门的 pinned 缓存。[切分代码](/workspaces/Joey-Project-For-Linux/jemalloc/src/extent.c:785)、[余量入缓存代码](/workspaces/Joey-Project-For-Linux/jemalloc/include/jemalloc/internal/pac.h:206)

**这也说明 retained 中的内存不一定经历过“应用使用 → free → purge”**：它也可以是新映射中从未交给应用、甚至从未触及物理页的余量。

### 用户

第一个问题，即使jemalloc的小分配的内部碎片看似比gblic malloc更严重，但是在例如MyRocks的实测对比中，jemalloc的峰值RSS却比glibc要好得多（你可以上网查），其中最主要的原因是不是就是jemalloc他的复用是按照（sn，addr）小的方向优先，使得另一个方向更有机会排空并合并成大extent服务其他更多样请求或者释放RSS，而glibc在复用chunk的地址分布上则更偏向随机不利于合并成大chunk服务更多样请求而导致复用率下降或者不利于回top释放RSS？第二个问题，slabs_nonfull堆中，如果一个slab变成全空了，怎么将其从堆中删除，详细逻辑是怎样的，或者说edata_t有没有实时记录更新该slab在堆数组中的位置？第三个问题，那是不是jemalloc所有结构中只有edata_t知道某个extent的裸地址，其他结构都只是指向对应的edata_t？第四个问题，jemalloc中decay和purge有什么区别？

### 助手

**第一个问题：稳定的 `(sn, addr)` 复用方向确实有助于减少碎片，但公开的 MyRocks 测试不足以证明它是 RSS 优势的最主要原因。** 你的推理中，“集中复用让其他区域更容易排空”是成立的机制；“glibc 随机复用”和“必须回到 top 才能降低 RSS”则需要修正。

下面分别展开。

**1．为什么 jemalloc 的取整浪费可能更大，实际 RSS 却更低？**

先确认实测结果。Mark Callaghan 在 2025 年的测试中，使用 10G MyRocks block cache，得到：

| 分配器 | 峰值 RSS |
|---|---:|
| jemalloc 5.2.1 | 12.2G |
| tcmalloc 2.9.1 | 13.1G |
| glibc 2.35 | 36.2G |

这是特定 sysbench 负载下的结果。作者指出，RocksDB 读取 block 时分配、淘汰时释放，而 block 的寿命差异很大，会给分配器造成很大压力。[原始测试](https://smalldatum.blogspot.com/2025/04/battle-of-mallocators.html)

关键在于：**对象的取整浪费，只是 RSS 的一个来源。** 还有空闲但无法满足当前请求的空间、被少量存活对象占住的页、缓存中尚未归还的对象、尚未 purge 的空闲页，以及元数据等。较少的对象内部碎片，完全可能被更多的这些成本抵消。

而且，“jemalloc 小分配更浪费”也不能一概而论：glibc 还需要 chunk 元数据和最小 chunk 大小。典型 64 位 glibc 中，1 B 请求也要占一个 32 B chunk；jemalloc 则可以使用 8 B region，另加摊销的外置元数据。

你提出的机制，可以这样理解：

```text
优先向一部分 slab 补入对象
       ↓
其他 slab 较少接收新对象
       ↓
随着旧对象释放，更有机会全空
       ↓
回到页分配层
       ├─ 供其他 size class / large 请求复用
       ├─ 条件允许时与相邻 extent 合并
       └─ purge，降低物理内存占用
```

这是合理的机制解释，但有四个重要边界。

- **排空后不必先合并成大 extent，才能降低 RSS。** 一个独立的、整页空闲的 extent 就可以 purge。合并主要增加服务更大请求的能力，也可能减少管理开销。
- **glibc 的选块不是随机算法。** tcache/fastbin 有近期释放优先的行为，普通 bin 有尺寸和队列规则，largebin 支持 best-fit，还有偏向连续小请求局部性的 `last_remainder` 路径。准确说法是：它没有统一采用 jemalloc 这种来源年龄/地址优先顺序，而不能直接断言它的地址分布随机。[glibc 源码](/tmp/glibc-2.39-malloc.c:4090)
- **glibc 降低 RSS 不只依靠 top 收缩。** 独立 mmap 的 chunk 可以解除映射；`malloc_trim()` 还可以对 bin 中空闲 chunk 内部的整页执行 `MADV_DONTNEED`。不过，“支持显式 trim”不等于正常 free 时总会及时回收这些内部页。[mtrim 实现](/tmp/glibc-2.39-malloc.c:5149)
- **arena 之间的复用限制也是实测支持的重要因素。** 后续测试中，把 glibc arena 数降到 1，RSS 膨胀基本解决了，但性能严重下降；8、48、96 个 arena 仍存在膨胀。这说明 arena 配置及其引起的分配布局、复用范围变化，对该负载影响很大。jemalloc 自己也有多 arena，不能据此简化成“多 arena 一定不好”。[后续测试](https://smalldatum.blogspot.com/2025/04/battle-of-mallocators-part-2.html)

因此，更有依据的解释是：**jemalloc 的尺寸分类与 slab 布局、稳定复用方向、arena/tcache 管理，以及页级回收机制共同影响结果。** 其中 `(sn, addr)` 是有利机制之一；但 extent 层优先旧块还可能切碎较大的空闲块，不能只计算它有利的一面。

要认定它是“最主要原因”，需要在同一版本、同一负载下只改变这一选择策略，测量 RSS、半空 slab 和 dirty 页等变化。上述公开测试没有完成这种因果隔离。

**2．全空 slab 如何从 `slabs_nonfull` 删除？有没有堆数组下标？**

**没有这个下标，因为这里不是数组实现的二叉堆，而是通过指针连接的 pairing heap。**

`edata_t` 内嵌的 `heap_link` 包含三个字段：

```cpp
void *prev;
void *next;
void *lchild;
```

它们描述节点在树中的连接关系：

- `lchild`：第一个孩子。
- `next`：下一个兄弟。
- `prev`：对第一个孩子通常指向父节点，对后续孩子指向前一个兄弟；辅助链表也使用这些链接。

堆本身保存 `root` 等信息。所以拿到 slab 的 `edata_t *`，就已经拿到了要删除的堆节点，无需搜索数组。[节点定义](/workspaces/Joey-Project-For-Linux/jemalloc/include/jemalloc/internal/ph.h:58)

完整过程分两层。

首先，归还最后一个 region 时，在 bin 锁保护下：

```text
根据 ptr 计算 region 编号
    ↓
更新 slab bitmap，将该 region 标记为空闲
    ↓
edata.nfree++
    ↓
nfree == bin_info.nregs，确认 slab 全空
    ↓
bin_dalloc_locked_handle_newly_empty()
    ↓
bin_dissociate_slab()
```

`bin_dissociate_slab()` 再区分：

- 如果是 `slabcur`，将 `slabcur` 清空。
- 如果是你问的 nonfull slab，调用 `edata_heap_remove(&bin->slabs_nonfull, slab)`。
- 只有一个 region 的 slab 有特殊处理，因为它不会经过 nonfull 状态。

移出 bin 后，再由上层调用 `arena_slab_dalloc()` 归还整个 slab；普通直接释放路径是在释放 bin 锁之后做这一步。[region 归还逻辑](/workspaces/Joey-Project-For-Linux/jemalloc/include/jemalloc/internal/bin_inlines.h:77)、[移出 bin](/workspaces/Joey-Project-For-Linux/jemalloc/src/bin.c:190)、[归还 slab](/workspaces/Joey-Project-For-Linux/jemalloc/src/arena.c:945)

然后看堆内部的删除，设目标节点为 `X`：

1. **X 是根节点**：先合并辅助链表，再把 X 的孩子子树合并成一个堆，以合并结果作为新根。
2. **X 不是根节点**：读取 `X.prev`、`X.next`，将 X 的孩子子树合并成替代节点 `R`，然后让 R 接替 X 原来的位置；没有孩子时就直接摘除 X。

示意：

```text
删除前：

P → X → N
    │
    └─ C1、C2、C3 等孩子子树

将孩子子树合并为 R，删除后：

P → R → N
```

代码通过判断 `prev.lchild == X`，决定修改的是父节点的 `lchild`，还是前一个兄弟的 `next`，同时修正反向链接。[ph_remove()](/workspaces/Joey-Project-For-Linux/jemalloc/include/jemalloc/internal/ph.h:391)

孩子子树的合并，会比较根节点的 `(sn, addr)`，让较小的根成为另一个根的父节点，反复处理直到剩一棵树；当前实现采用队列式的多轮配对合并。[合并实现](/workspaces/Joey-Project-For-Linux/jemalloc/include/jemalloc/internal/ph.h:167)

所以，**定位待删除节点不需要遍历，但整个删除操作不保证 O(1)**，因为可能要合并多个孩子子树。实时维护的是连接指针，不是数组下标。

另外，这发生在 region 真正归还 slab 时；应用 `free()` 如果只是把指针放进 tcache，还不会让 slab 的 `nfree` 增加。

**3．是不是只有 `edata_t` 知道 extent 的裸地址？**

**不是。`edata_t` 是 extent 的核心描述符，但不是唯一保存地址的地方。**

对于我们讨论的管理结构，你的理解基本成立：

```text
slabcur / slabs_nonfull / slabs_full / ecache heap / LRU
                          ↓
                       edata_t
                          ↓
                 extent 的地址、大小、状态
```

但“其他结构都只保存 `edata_t *`”太绝对。最直接的反例，就是上一轮提到的堆顶摘要：

```cpp
struct edata_cmp_summary_s {
    uint64_t  sn;
    uintptr_t addr;
};
```

`eset->bins[i].heap_min` **直接缓存了 extent 地址数值**，这样跨桶比较时不必先访问每个 `edata_t`。[摘要定义](/workspaces/Joey-Project-For-Linux/jemalloc/include/jemalloc/internal/edata.h:93)

此外：

- tcache 保存的是用户对象的裸指针，不是每个对象对应的 `edata_t *`。
- emap 以用户虚拟地址作为查询依据；地址信息也体现在 radix tree 的索引路径中。
- 与 OS 和 extent hooks 交互时，也会直接传递地址和长度。

因此，应理解为：**extent 的完整管理信息集中在外置 `edata_t` 中，其他结构可以引用它，也可以缓存地址或直接保存对象指针。**

**4．decay 和 purge 有什么区别？**

**decay 是决定何时、回收多少空闲页的策略；purge 是对选中的页实际执行的回收操作。**

| | decay | purge |
|---|---|---|
| 解决的问题 | 当前应该保留多少空闲页，回收多少？ | 怎样让这些页的物理内存可以被系统回收？ |
| 主要依据 | 时间、历史空闲页数量、配置 | 目标地址范围、OS 能力、extent hooks |
| 是否必然执行系统调用 | 不一定，可能判断暂不需要回收 | 实际调用 purge hook，默认实现可能使用 `madvise` |
| 是否必然马上降低 RSS | 不保证 | lazy purge 也不保证 |

普通流程可以概括成：

```text
extent 空闲，进入 dirty
        ↓
decay 根据时间和历史页数计算保留目标
        ↓
需要减少空闲页时，从 LRU 选择 extent
        ↓
执行回收
        ├─ lazy purge 成功 → muzzy
        └─ 更彻底回收 → retained，或解除映射
```

muzzy 也有自己的 decay。**不是每块 extent 分别启动一个精确倒计时，到期就 purge**；decay 主要计算总体页数目标，再按 extent 选择回收对象。[PAC 实现](/workspaces/Joey-Project-For-Linux/jemalloc/src/pac.c:538)

purge 本身还分两类：

- **Lazy purge**：告诉 OS 这些内容可以丢弃。典型 Linux 实现使用 `MADV_FREE`；系统可能暂时保留物理页，所以 RSS 不一定立即下降。
- **Forced purge**：要求丢弃原内容，后续访问获得零页语义。典型 Linux 匿名私有映射使用 `MADV_DONTNEED`，通常能让已驻留的页及时退出 RSS，但虚拟地址映射仍然保留。

具体回收流程还可能选择 decommit 或 unmap，因此不能把每次 decay 回收都等同于某一种 purge 系统调用。[回收分支](/workspaces/Joey-Project-For-Linux/jemalloc/src/extent.c:1206)

对用户接口而言也有这个区分：`arena.<i>.decay` 根据当前 decay 状态触发回收，可能只回收一部分；`arena.<i>.purge` 则请求 purge 所有未使用的 dirty 页。两者都不能把仍有存活对象的 slab 强行清空，也不等于自动清空所有线程的 tcache。[官方接口说明](https://jemalloc.net/jemalloc.3.html#arena.i.decay)

### 用户（尚未回答）

第一个问题，那在那个MyRocks测试中，jemalloc的RSS优势来源于哪里，如果他也开了多arena的话，应该也会存在glibc那样跨arena无法复用的问题啊，为什么他的RSS就可以这么低？
