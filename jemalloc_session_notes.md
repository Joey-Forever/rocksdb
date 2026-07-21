# jemalloc 内存管理讨论记录

## 前几轮讨论总结（不含最后一轮问答）

本文总结当前 session 中关于 jemalloc 的前两轮技术讨论；末尾附上最后一轮问题和回答的完整原文。源码分析基于 `/workspaces/Joey-Project-For-Linux/jemalloc`，第二轮核对时本地 `master` 提交为 `a47e019e`。

### 1. bin 的 slabs_nonfull 为什么优先复用较老、较低地址的 slab

核心意图是给复用提供稳定的集中方向：优先向一部分 slab 填入新对象，让其他 slab 少接收新对象，从而有机会随着原有对象释放而完全排空。这样可以减少长期半空的 slab、降低活跃页数量，提高整体内存利用率。

这是一种分配选择策略，不会搬迁已分配对象，也不保证任意工作负载下都能排空。低地址数值本身没有硬件速度优势；可能的局部性收益来自对象更集中、访问的页更少。

本地实现并非单纯比较地址，而是按 `(sn, addr)` 排序：先比较 extent 序列号，序列号相同时再比较地址，因此源码注释使用 `oldest/lowest`。`slabcur` 保存当前服务分配的 slab，普通对象分配不需要每次操作堆；`slabs_nonfull` 用于管理其他非满 slab，并支持选取、插入、删除。`bin_lower_slab()` 会根据优先级调整 `slabcur`。

slab 完全空闲后可以退出 bin，交还底层页分配器；这不意味着立即归还操作系统或立即减少 RSS，后续还受缓存、purge 和 decay 策略影响。

### 2. free 如何找到所属 slab

jemalloc 使用全局 `arena_emap_global`，将虚拟页映射到 `edata_t` 及相关元数据。`edata_t` 描述 extent；extent 用作 slab 时，它也描述该 slab。

逻辑路径如下：

```text
对象指针 ptr
  → 所在的虚拟页
  → emap 底层 radix tree
  → edata_t
  → slab、arena、bin、bin shard
```

active slab 的首尾页和所有中间页都登记到同一个 `edata_t`，因此合法对象指针无论落在哪一页，都能定位 slab。找到 slab 后，依据 `(ptr - slab_base) / region_size` 计算对象编号；实现使用 `div_compute()` 避免可变除数的直接除法，再更新分配位图和 `nfree`。

radix tree 按地址位段直接索引，深度由地址配置决定，代码支持 1～3 层；每线程还缓存叶节点，减少树遍历。

`free()` 与真正归还 slab 需要区分：常见快路径只读取大小类别、slab 标记等信息，将指针放入 tcache；flush 时才批量查找 `edata` 并更新 slab。直接归还路径可从 `arena_dalloc_small()` 跟踪。

### 3. ecache、eset、pairing heap 和 LRU

`ecache_dirty`、`ecache_muzzy`、`ecache_retained` 使用的 `eset` 不是红黑树，其主要结构是：

```text
ecache
  ├─ eset
  │   ├─ bitmap：标识非空大小桶
  │   ├─ bins[pind]
  │   │   ├─ pairing heap：桶内的 extent
  │   │   └─ heap_min：堆顶的 (sn, addr) 摘要
  │   └─ lru：空闲 extent 的入队顺序
  └─ guarded_eset：带 guard page 的 extent，使用同样的结构
```

插入时，extent 的实际大小经 `sz_psz_quantize_floor()` 和 `sz_psz2ind()` 转换为桶编号。一个桶可以包含多个 extent，实际大小也可能不同。桶内 heap 按 `(sn, addr)` 排序。

普通 dirty/retained 分配并不总是找到最小合适桶就返回：`eset_first_fit()` 会借助 bitmap 查找允许范围内的候选桶，并比较各桶堆顶的 `(sn, addr)`，优先选最老、其次地址最低的可用 extent。dirty 还有限制复用块与请求大小比例的机制，避免为了小请求切分过大的 extent。

heap 与 LRU 管理同一批空闲 extent，但用途不同：

| 结构 | 用途 |
|---|---|
| 大小桶与 pairing heap | 分配时选择复用的 extent |
| LRU 链表 | 回收缓存页时选择先处理的 extent |

`eset_insert()` 将 extent 加入 heap 和 LRU 尾部，`eset_remove()` 同时从二者删除。extent 被复用后离开空闲集合，下次释放再入队。因此这里的 LRU 更接近空闲入队顺序，不跟踪应用对内存的每次访问；它也不等于 heap 中的序列号顺序。

### 4. dirty/muzzy 与 retained 的回收差异

- dirty/muzzy 参与常规 decay。decay 根据时间和历史空闲页数量计算保留页数，超过目标时，经 `pac_stash_decayed()` 调用 `ecache_evict()`，从 LRU 队头按整块 extent 回收。这不是为每个 extent 设置独立、精确的过期时间。
- retained 不参与常规 decay 淘汰。它保留地址空间供后续复用；虽然也有 LRU 结构，但不会仅因存放时间长而自动淘汰。`pac_destroy()` 销毁时通过 `ecache_evict(..., 0)` 遍历并调用 destroy hook。
- extent 离开 dirty 后，可能经 lazy purge 进入 muzzy，也可能经 decommit/purge 进入 retained，或成功解除映射；不能把离开 dirty 等同于 `munmap()`。

### 5. extent 合并如何通过 emap 查找邻居

`emap_t` 内部是 `rtree_t`，按虚拟页号索引，不是仅以空闲 extent 首地址为 key 的红黑树。它既记录 active extent，也记录 jemalloc 保有的空闲 extent。

| 类型 | 需要登记的页 |
|---|---|
| 普通空闲 extent | 首页和末页 |
| active slab | 首尾页及所有中间页 |
| 普通 active large extent | 首尾边界，通常不登记所有中间页 |

对 extent `[base, base + size)`：

```text
前邻居：查询 base - PAGE，命中前一个 extent 的末页
后邻居：查询 base + size，命中后一个 extent 的首页
```

因此只需查询两个已知页地址，不需要做有序树的前驱、后继搜索。末页地址为 `base + size - PAGE`，与 extent 之后的地址不同。

查到邻居后，还需要检查状态、arena、页分配器、commit 状态等是否兼容，再从 eset 移除邻居、执行合并并更新 emap 的边界映射。仅仅地址相邻并不足以合并，例如 dirty 和 retained 不会因此直接合并。

retained/muzzy 通常在放回缓存时立即尝试合并；dirty 配置延迟合并，在淘汰时补做，但这版 master 对较大的 dirty extent 也会提前尝试有限度合并。

### 6. 主要源码入口

源码位置均相对于 `/workspaces/Joey-Project-For-Linux/jemalloc`：

| 主题 | 文件与符号 |
|---|---|
| slab 选择 | `src/bin.c`：`bin_lower_slab()` |
| extent 比较规则 | `include/jemalloc/internal/edata.h`：`edata_snad_comp()` |
| 直接释放小对象 | `src/arena.c`：`arena_dalloc_small()` |
| slab 对象编号与位图 | `include/jemalloc/internal/bin_inlines.h` |
| eset 结构与选块 | `include/jemalloc/internal/eset.h`、`src/eset.c` |
| 页映射与邻居查找 | `include/jemalloc/internal/emap.h`、`src/emap.c` |
| radix tree 与叶节点缓存 | `include/jemalloc/internal/rtree.h` |
| extent 淘汰与合并 | `src/extent.c`：`ecache_evict()`、`extent_record()` |
| decay 与销毁 | `src/pac.c`：`pac_maybe_decay_purge()`、`pac_destroy()` |

---

## 最后一轮问答原文

### 问题原文

第一个问题，那为什么slabs-nonfull不直接采用头插指针栈方式，后插入的先取出来复用，先插入的直到后插入的被复用满之后才会被复用到，这样不也可以让部分slab更有机会排空吗？第二个问题，edata_t是不是就是在每个extent开头作为header进行记录的？第三个问题，按照地址是64位算的话，去掉4kb的页内偏移的12位，还有52位，如果emap的Radix Tree只有一层的话，岂不是一个节点要开2^52个slot，即使是三层，每个节点也要开至少2^17个slot，这是不是空间浪费过于严重了，还有就是每个extent其实是有多个4kb page的，那即使是末级节点也是多个slot对应一个extent，这会不会也有比较严重的空间浪费？第四个问题，所以就是eset其实还是和glibc分配器的bins一样的分桶数组，他的数组大小也是128个桶吗，然后对于负责一个区间大小的桶来说，桶内部为什么就不需要像glibc的largebin那样维护有序了，为什么可以直接选堆顶的extent然后再切割，为什么glibc的largebin必须要维护内部多size有序，而且既然ecache_dirty也是选一个能够容纳的然后再切割的话为什么还要延迟合并，全部一起合并成一个大的还能减小分桶的管理成本呢？

### 回答原文

继续按本地这版 `master` 来看。你这次提出了两个很关键的取舍：**LIFO 也可能促进排空；radix tree 的宽节点也确实有空间成本。** 不能把现有设计理解为唯一可行方案。

**1. LIFO 也能让部分 slab 排空，为什么还要按 `(sn, addr)` 排序？**

是的，你提出的栈方案能产生这种效果。我前面仅用“集中复用”解释，确实还不足以区分这两种策略。

区别在于：**LIFO 的复用对象由最近的释放事件决定；`(sn, addr)` 给 slab 一个相对稳定的优先顺序。**

假设在需要选择下一个 slab 时，有两个候选：

```text
A：较老、低地址，已有多个空位，早已进入 nonfull
C：较新、高地址，刚从全满变成空出一个位置
```

两种策略会选择：

| 策略 | 选择 | 后续倾向 |
|---|---|---|
| LIFO | C | 刚释放出来的空位马上被填回 |
| `(sn, addr)` | A | 优先填 A，给 C 留出继续释放的时间 |

如果多个 slab 不断发生“满 → 释放一个对象 → 非满”，LIFO 容易追着这些刚出现的空位复用。它可能让栈底的 slab 排空，但**哪些 slab 被保留、哪些被冷落，会受释放时序影响**。

稳定排序则让 A、B、C 的相对优先级基本不随这种释放事件变化。即使高优先级 slab 曾经全满，后来重新出现空位，仍能恢复优先复用；当前 [bin_lower_slab()](/workspaces/Joey-Project-For-Linux/jemalloc/src/bin.c:212) 就会据此替换 `slabcur`。这有利于让分配长期向同一批区域集中，也使相邻区域更有机会一起腾空。

不过，**这不是“地址优先在所有工作负载下都比 LIFO 好”的证明**。LIFO 操作简单，也可能有较好的近期访问局部性。jemalloc 作者曾明确比较过 MRU 复用与低地址优先，并报告其测试中后者有较好的布局效果，切换开销也没有造成显著性能下降。这属于经验支持的设计选择。[作者说明](https://lists.freebsd.org/pipermail/freebsd-current/2007-March/070303.html)

此外，`slabs_nonfull` 还要支持删除中间某个已经完全空闲的 slab。单向指针栈不能高效完成任意删除，至少还需要额外链接或其他机制。

**2. `edata_t` 不在它所描述的 extent 开头。**

对于这里讨论的普通用户 extent，`edata_t` 是**单独分配的外置元数据**。

[extent_alloc_wrapper()](/workspaces/Joey-Project-For-Linux/jemalloc/src/extent.c:1143) 的逻辑很清楚：

```cpp
edata = edata_cache_get(...);  // 取得元数据对象
addr  = ehooks_alloc(...);     // 取得 extent 的内存
edata_init(edata, ..., addr, size, ...);
```

`edata_cache` 不够用时，调用 [base_alloc_edata()](/workspaces/Joey-Project-For-Linux/jemalloc/src/base.c:630)，由内部元数据分配器分配 `sizeof(edata_t)`。

因此布局是：

```text
元数据区： [ edata_t：记录 addr、size、状态、链接等 ]
                         │
                         ▼
extent：   [         可用于对象或 slab 的空间         ]
```

这也解释了为什么需要 `emap`：从用户指针不能通过减去固定 header 长度得到 `edata_t`，而要通过页映射查找。

extent 拆分时，同样会另外取得一个 `edata_t` 描述后半段，并不是直接在后半段起始地址写入 header。见 [extent_split_impl()](/workspaces/Joey-Project-For-Linux/jemalloc/src/extent.c:1342)。

**3. radix tree 的节点确实很宽，但要区分有效地址位、虚拟空间与物理内存。**

首先，**64 位指针不代表一定使用 64 位有效虚拟地址**。代码使用：

```cpp
RTREE_NSB = LG_VADDR - LG_PAGE;
```

`LG_VADDR` 由配置指定或探测；层数也不是任意选择：

```text
有效页号位数 ≤ 10：1 层
有效页号位数 ≤ 36：2 层
有效页号位数 ≤ 52：3 层
```

因此不会出现“52 位页号却只开一层”的情况。按照 4 KiB 页计算：

| `LG_VADDR` | 页号有效位数 | 层数 | 各层索引位数 |
|---:|---:|---:|---|
| 48 | 36 | 2 | 18 + 18 |
| 57 | 45 | 3 | 15 + 15 + 15 |
| 64 | 52 | 3 | 17 + 17 + 18 |

这些规则就在 [rtree.h](/workspaces/Joey-Project-For-Linux/jemalloc/include/jemalloc/internal/rtree.h:20)。

你的另一半判断仍然成立：**即使这样，每个节点仍然可能很大。** 例如 48 位有效地址配置下，一个叶节点有 `2^18` 个 slot；采用 8 字节紧凑叶元素时，一个叶数组就是 **2 MiB**。

jemalloc 用两个办法控制成本：

- **子节点按需分配。** 没有触及的地址范围，不会提前创建对应的整棵子树。见 [rtree_leaf_init()](/workspaces/Joey-Project-For-Linux/jemalloc/src/rtree.c:67)。
- **元数据内存利用 demand-zero。** 给数组保留了 2 MiB 地址空间，不等于已经为全部数组实际占用 2 MiB 私有物理内存。[base_alloc() 的注释](/workspaces/Joey-Project-For-Linux/jemalloc/src/base.c:616) 明确说明：auto arena 使用按需置零，目的就是让 radix tree 这类稀疏、多页结构节省物理内存。

所以不能把所有已分配节点的完整数组大小直接当成 RSS；但也不能说它“没有浪费”。**小堆、地址分布稀疏、元数据页利用率低时，开销比例确实可能较高。** 宽节点是在用地址空间和部分元数据空间换取更浅、更直接的查找。

至于“多个 slot 指向同一个 extent”，它重复存放的是小型映射项，**不是多份 `edata_t`**。

以紧凑叶项为例，密集映射下：

```text
每 4 KiB 用户地址范围对应 8 字节叶项
叶项开销比例 = 8 / 4096 ≈ 0.195%
```

非紧凑配置下，常见 64 位布局的叶项为 16 字节，对应约 `0.39%`。这些仅是密集映射时的叶项比例，不包含上层节点、`edata` 和稀疏浪费。

还要修正一个容易误解的地方：**只有 active slab 需要每页登记。普通 large/free extent 通常只登记首尾页。** 不过，只登记两个 slot 并不会把承载它们的叶数组变成两个元素；它主要减少写入和需要触及的元数据页。

**4. `eset` 确实属于按大小分桶的结构，但桶数、选块策略和合并策略与 glibc 不同。**

先回答桶数：**64 位、4 KiB 页、当前 `SC_LG_NGROUP=2` 的配置下，是 200 个数组元素，不是 128。**

源码定义：

```cpp
eset_bin_t bins[SC_NPSIZES + 1];
```

按 [sc.h 的公式](/workspaces/Joey-Project-For-Linux/jemalloc/include/jemalloc/internal/sc.h:225)：

```text
SC_NGROUP      = 4
SC_LG_BASE_MAX = 64 - 2 = 62
SC_NPSIZES     = 4 + (62 - (12 + 2)) × 4 + 4 - 1
              = 199

数组长度 = 199 + 1 = 200
```

glibc 普通 bins 的 `NBINS=128` 是另一套配置，不能套用到 jemalloc。[glibc 源码](https://github.com/bminor/glibc/blob/master/malloc/malloc.c)

接着看为什么 jemalloc 桶内可以不按 size 排序。

**关键是：插入向下量化，搜索起点向上量化。**

假设某个桶对应：

```text
[80 KiB, 96 KiB)
```

里面可能有：

```text
80 KiB、84 KiB、92 KiB
```

若请求是 `80 KiB`，那么这个桶里**每一块都足够大**，堆顶无论是哪块都能满足请求。jemalloc 可以在这些候选中按 `(sn, addr)` 选择，再按需切分。

普通 size-class 模式下，请求已被向上取整到 size class；例如 `72 KiB` 的大对象请求会按 `80 KiB` 处理，从 `80 KiB` 的桶开始搜索，而不会从可能包含不够大 extent 的 `64 KiB` 桶开始。

这并不意味着所有 extent 的实际大小都等于桶下界。合并、拆分后，桶内仍可以存在不同大小；只是**搜索起点保证了候选桶内的元素都能容纳请求**。

当前 master 还有一个很能说明问题的例外：关闭 large size classes 后，请求可能只向上取整到页。这时前一个桶中会同时存在“够大”和“不够大”的 extent，代码就增加了**有上限的桶内枚举搜索**，而不能盲取堆顶。见 [eset_first_fit()](/workspaces/Joey-Project-For-Linux/jemalloc/src/eset.c:292)。

glibc largebin 按大小排序，是为了高效找到 **最小的足够大的 chunk，即 best-fit**。同一 largebin 内可能既有小于请求的 chunk，也有大于请求的 chunk，所以需要按实际大小区分。源码直接说明了排序用于 best-fit，并沿大小链接寻找最小可用块。[glibc largebin 实现](https://github.com/bminor/glibc/blob/master/malloc/malloc.c)

因此，glibc 并非在理论上“必须”这样组织；它是在实现自己选定的策略。jemalloc 的 heap 也有序，只是**按 `(sn, addr)` 有序，不按实际 size 有序**。

最后，为什么 dirty 不全部立即合并？

因为**“找不到合适大小时允许切分”与“每次都先合并再切分”是两回事**。dirty 中的 extent 很可能很快被相似大小的请求复用。

例如两个相邻的 8 KiB extent：

```text
保留边界：
free 8K + free 8K → 两个 8K → 后续两个 8K 请求直接复用

立即合并：
free 8K + free 8K → 合成 16K → 后续 8K 请求又要拆成 8K + 8K
```

反复合并、拆分需要：

- 修改 heap、LRU 和统计；
- 修改 `emap` 的边界映射；
- 取得或归还 `edata_t`；
- 执行 split/merge hook，并增加相关锁内工作。

这些并不涉及复制整个 extent 的用户数据，但元数据操作本身也有成本。[pac_init() 的注释](/workspaces/Joey-Project-For-Linux/jemalloc/src/pac.c:54) 明确给出的理由，就是 dirty extent 很可能很快复用，而 merge/split 成本不可忽略。

你的方案确实可以减少空闲 extent 数量、改善大块可用性；代价是可能制造大量“刚合并又拆开”的工作。而且 `bins` 是固定数组，**合并不会缩小分桶数组本身**，只会减少桶内元素。

所以当前实现采取折中：dirty 配置延迟合并，淘汰时补做；较大的 dirty extent 又会提前进行有限度合并；muzzy/retained 则通常立即合并。具体分支见 [extent_record()](/workspaces/Joey-Project-For-Linux/jemalloc/src/extent.c:1031)。

