# jemalloc 内存管理讨论总结

更新日期：2026-10-10。已整合原总结、末尾完整问答和本次 session，按主题去重；后续讨论以本文中的条件和修正为准。

## 1. 范围与核心认识

- 本地源码：`/workspaces/joey_project/jemalloc`，核对提交 `a47e019e`。旧路径 `Joey-Project-For-Linux` 不再使用。
- 主要讨论普通 PAC、slab 和 dirty/muzzy/retained；HPA、SEC、pinned、guarded、特殊对齐、自定义 hooks 等可能改变路径。
- 数字示例默认 64 位、4 KiB 页、16 B quantum、`SC_LG_NGROUP=2`，忽略额外 padding。
- MyRocks 实测使用 jemalloc 5.2.1、glibc 2.35；本地 glibc 对照为 `/tmp/glibc-2.39-malloc.c`。不能把本地 master 的实现细节直接套到实测版本。
- 本次没有运行 benchmark。源码事实、机制推断、第三方实测应分别表述，不能用机制推断替代因果验证。

```text
小对象请求 -> tcache -> arena 的 size-class bin/shard -> slab 中的 region
large 请求 / 新 slab -> 页分配层 -> 空闲 extent 复用或取得新映射
对象指针 -> 全局 emap / radix tree -> 外置 edata -> extent、arena、bin
```

**最重要的边界：** 对象 free 不等于 slab 已空；slab 排空不等于立即降低 RSS；地址相邻不等于能合并；retained 不等于物理页必定已归还；跨 arena 不能直接复用 extent 不等于必须长期保留其物理页。

## 2. 小对象 size class、slab 与内部碎片

典型配置有 **36 个小对象 class**，从 8 B 到 14 KiB，最小 large class 为 16 KiB。class 预先确定，不随请求大小种类增加；对象变多增加的是 slab。每个 class 可以有多个 bin shard，实际 `bin_t` 数是各 class 的 shard 数之和，随 arena 创建时确定。

完整的小对象 class 序列（单位 B）：

```text
8, 16, 32, 48, 64
80, 96, 112, 128
160, 192, 224, 256
320, 384, 448, 512
640, 768, 896, 1024
1280, 1536, 1792, 2048
2560, 3072, 3584, 4096
5120, 6144, 7168, 8192
10240, 12288, 14336
```

常规分组在 `(B, 2B]` 中设四档，间距 `B/4`。请求向上取整：65 B 占 80 B，1025 B 占 1280 B。常规分组中浪费占 region 大小小于 20%，相对请求小于 25%；tiny/初始分组不受此统一上限约束，例如 17 B 占 32 B。

这些比例只计算对象取整，不含半空 slab、slab 尾部余量和元数据。也不能笼统说 jemalloc 小请求比 glibc 浪费：典型 64 位 glibc 的 1 B 请求占一个 32 B chunk，jemalloc 可以使用 8 B region，另加摊销元数据。

一个 64 B bin 管的是提供 64 B region 的 slab，并非 64 B 大小的 slab。同一 bin 的 region 大小、slab 大小和 region 数固定；60 B 请求不会因为其他 class 有老 slab，就改用那个 class。

要区分三种大小：对象 size class、承载对象的 slab 总大小、下层空闲 extent 的分桶区间。上层按对象 class 定位 bin，只有需要新 slab 时才按 slab 总大小向页分配器申请。运行时可用 `arenas.nbins`、`arenas.bin.<i>.size`、`arenas.bin.<i>.slab_size` 核对实际配置。

| bin 内结构 | 用途 |
|---|---|
| `slabcur` | 当前分配 slab，通常直接操作其位图 |
| `slabs_nonfull` | 其他非满 slab，按 `(sn, addr)` 排序的 pairing heap |
| `slabs_full` | 满 slab 链表；当前自动 arena 跳过维护，手动 arena 为 reset 等操作维护 |

`slabcur` 独立于另两个集合。链表和 heap 的链接字段位于外置 `edata_t` 中，相关字段共用 union，不需要在用户 extent 内放 header。

## 3. sn、集中复用与 pairing heap

**sn 是来源年龄，不是释放时间或全局唯一 ID。** 普通 PAC 新取得 extent 时分配递增 sn；拆分双方继承 sn，合并保留较小 sn，普通释放和复用不重新取号。`(sn, addr)` 先比较 sn，相同再比较地址；不同 arena 不共享全局顺序，跨 mmap 的 sn 顺序也不等于地址顺序。

优先补入较老、较低地址的部分 slab，可以让其他 slab 少接收新对象，随着旧对象释放而排空。收益可能包括更少活跃页、更集中访问、更多整块复用或回收机会。它不移动存活对象，一个长期存活对象仍可能阻止 slab 排空；低地址数值本身没有硬件速度优势。

LIFO 也可能让底部 slab 排空，并有近期访问局部性，但选择受“最近进入 nonfull”的释放事件影响；稳定排序使老 slab 再出现空位后仍恢复原优先级。不能据此认定一种策略在所有负载上更优。需要支持动态取最小、插入及任意删除，pairing heap 是一种实现选择，并非唯一结构。

**全空 slab 的删除：** region 真正归还时，在 bin 锁内更新 bitmap、`nfree`；达到 `nregs` 后，从 `slabcur` 或 nonfull heap 脱离，随后归还整个 slab。单 region slab 有特殊分支。仅进入 tcache 不会立即增加 slab 的 `nfree`。

对应调用为 `bin_dalloc_locked_handle_newly_empty()` -> `bin_dissociate_slab()`；nonfull 分支调用 `edata_heap_remove()`，普通直接释放路径在解开 bin 锁后才调用 `arena_slab_dalloc()`，不把整块回收工作一直放在 bin 锁内。

pairing heap 不是数组二叉堆，不维护节点下标。`edata` 内嵌 `prev/next/lchild`：分别表达父节点或前兄弟、后兄弟、首个孩子。拿到 `edata *` 就定位了节点；删除时合并其孩子子树并重接指针，根删除还处理辅助链表。无需搜索待删节点，但子树合并使删除不保证 O(1)。

## 4. edata、全局 emap 与 free 的同步

`edata_t` 是外置 extent 描述符，由元数据分配器和 `edata_cache` 管理；radix tree 叶项只存其指针及 class/slab/state 等摘要，多个叶项可指向同一个 edata。

外置的机制优势：整段用户 extent 可以 purge/decommit 而保留描述符；页级布局和切分不必预留完整 header。内置 slab header 也不能让任意对象通过减固定偏移找到 slab 起点，仍需映射或额外对齐约束。这些是机制分析，不是已证实的唯一历史设计动机。

edata 并非唯一保存地址的结构：`heap_min` 缓存 `(sn, addr)`，tcache 保存对象裸指针，emap 索引编码虚拟地址，hooks 直接接收地址和长度。

**查询方向是先全局 emap，后 arena：**

```text
free(ptr)
  -> arena_emap_global 的叶项
  -> 快路径：读 class/slab 摘要，放入线程 tcache
  -> 真正归还时：通过 edata 找原 arena/bin/shard，更新 slab
```

有效非空指针的普通 `free` 需要查询分配信息，但线程本地叶节点缓存可减少树遍历，小对象快路径不必解引用 edata。`free(NULL)` 无需查询；`sdallocx/free_sized` 等提供大小的接口在符合条件的快路径可以跳过查询。归还 region 时，用对象与 slab 基址之差计算编号，实现使用 `div_compute()`。

**emap 查询不需要 arena 共享锁，原因有三层：**

1. 树的子节点指针和叶项使用原子读写；紧凑叶项以一个机器字读写。非紧凑叶项有多个原子字段，一致性仍依赖调用协议，不能泛化为任意并发快照。
2. 缺失树节点由 `rtree->init_lock` 保护初始化，再以 release 发布；读取已有节点不取此锁。普通 extent 释放不会销毁对应树节点数组。
3. 合法对象的生命周期保证定位信息有效：对象尚未真正归还 slab 时，其他线程释放同 slab 的对象不能让该 slab 全空并被回收。

针对有效分配指针的 `dependent` read 可用 relaxed 原子读：映射登记早于分配返回，跨线程交接指针必须由应用正确同步。任意查询不能套用该保证；双重 free、free 与同对象 realloc 并发也不在合法前提内。

查询无锁不意味着 edata 可任意无锁修改：bitmap、空闲计数及 bin 集合仍受 bin 锁保护；extent 拆分、合并、回收有页分配层同步。

## 5. radix tree 成本、登记范围与邻居查找

树宽度由有效虚拟地址位决定，`RTREE_NSB = LG_VADDR - LG_PAGE`。4 KiB 页下：

| LG_VADDR | 页号位数 | 层数与索引位数 |
|---:|---:|---|
| 48 | 36 | 2 层：18 + 18 |
| 57 | 45 | 3 层：15 + 15 + 15 |
| 64 | 52 | 3 层：17 + 17 + 18 |

48 位地址、8 B 叶项时，一个叶数组的虚拟大小是 2 MiB，但子节点按需创建，元数据利用 demand-zero，RSS 取决于实际触及的元数据页。一个 4 KiB 元数据页可放 512 个叶项，覆盖 2 MiB 用户虚拟地址；集中地址通常比随机散落更节省元数据物理页。

密集映射的叶项成本约为用户页大小的 0.195%（8 B/4 KiB），非紧凑 16 B 叶项约 0.39%；不含 edata、上层节点及稀疏浪费。叶项清空不会自动释放承载它的元数据页。

| extent 类型 | 普通 emap 登记范围 |
|---|---|
| active slab | 首尾和所有中间页 |
| active large | 通常仅首尾边界 |
| 整体空闲 extent | 首页和末页 |

对 `[base, base + size)`，查前邻居用 `base - PAGE`，后邻居用 `base + size`；不需要有序树前驱/后继查找。找到后仍需检查 arena、状态、页分配器、commit 状态及 hooks 兼容性。dirty 与 retained 不会只因相邻就直接合并。

仅登记边界减少写入和触页，不会把叶数组缩成两个元素。大映射切分可集中虚拟地址并共享元数据页，但不要求物理页连续，也不能认定其唯一目的就是优化 radix tree。

## 6. 空闲 extent 的分桶、选块与拆分限制

普通 ecache 的 eset 包含非空桶 bitmap、每桶按 `(sn, addr)` 排序的 heap、堆顶摘要和 LRU。典型配置 `SC_NPSIZES=199`，桶数组长 200；与 36 个小对象 class、glibc 的 128 个 bins 不是同一概念。

- 插入按 extent 实际大小向下量化，普通搜索起点向上量化。从安全起点及之后选，保证容量足够；对齐另行处理。
- 桶内实际尺寸可不同：`[80,96) KiB` 桶可有 80、84、88、92 KiB。关闭 large size classes 后，前一桶可能混有够大和不够大的块，需有界枚举，不能盲取堆顶。
- 普通 `eset_first_fit()` 用 bitmap 跳过空桶，比较允许范围内各桶 `heap_min` 摘要，通常不因精确匹配而停止。成本与候选非空桶数相关，不是固定读取 200 个 edata，也不是扫描全部 extent。
- **不是 best-fit：** 请求 80 KiB 时，92 KiB/sn=10 可能胜过 80 KiB/sn=20，拆出 12 KiB 余量；跨桶也可能选择较老的大块。4 KiB 页下 `[8,12) KiB` 桶内只有 8 KiB，但 8 KiB 请求仍可能跨桶拆 16 KiB。

拆分余量继承 sn，因而可能连续胜出，把老大块逐步切小。只要其中部分仍活跃，就不能恢复原大块；风险是损害后续大请求复用，而不是所有拆分都有害。

即使拆出的部分后来全部释放，dirty 延迟合并也可能让原大块暂时无法恢复。“释放大块 -> 穿插寿命不同的小请求 -> 再请求大块”是值得关注的负载模式；高频小对象 malloc 本身不代表每次都会进入 extent 拆分路径。

`lg_extent_max_active_fit=k` 主要约束普通 dirty/延迟合并路径，名义候选/请求比例约为 `2^k`，默认 k=6（64 倍）。实现按桶代表大小判断：

```cpp
if ((sz_pind2sz(i) >> lg_max_fit) > size) { break; }
```

它不是拆分次数或缓存容量限制，也不是逐 extent 的精确除法上限。普通 muzzy/retained 不使用同样限制；当前较大 dirty extent 的提前合并逻辑也引用此参数。

调小可保护大块，却可能让小请求无法使用已有空闲内存；调大增加复用机会，也可能切碎大块。两者都不保证降低 RSS 或提高性能，发生频率尚未测量。

## 7. dirty、muzzy、retained 与两种排序

| 状态 | 主要含义 | 常规行为 |
|---|---|---|
| dirty | 空闲、尚未 purge，可能保留先前物理页 | 优先复用；延迟合并，淘汰时补做；部分大块提前合并 |
| muzzy | lazy purge 后，内容可由 OS 丢弃 | 可复用，也有自身 decay；通常立即尝试合并 |
| retained | 分配器保有虚拟地址 | 可复用、拆分、合并；不做普通 decay |

retained 可以来自从未交给应用的新映射余量，也可以来自 decommit/purge 后的区域；hooks 或回收失败可能留下物理页，因此它不是“RSS 必为零”的证明。

立即尝试合并不意味着 retained 最终只剩一个大块：非相邻区域、夹在中间的活跃 extent、状态或 hooks 不兼容都会阻止合并。合并减少的是 extent 元素数，不会缩小固定的大小桶数组。

`extent_grow_retained()` 取得大映射、切出当前请求后，前后余量各由独立 edata 描述，普通路径进入 retained。例如 2 MiB 切出 64 KiB，余下 1984 KiB 已是受管理的空闲 extent，而非未登记的原始地址。

两种顺序各有用途：

- `(sn, addr)` 决定候选复用优先级。
- LRU 近似记录本次空闲入队顺序：入尾、复用时移除、再释放重新入尾；合并会调整位置。它不记录应用读写，也不是创建时间顺序。

回收加入空闲时间维度，所以可以淘汰 sn 小的老块。直接淘汰最大 sn 是另一种可设计策略，但 sn 不反映近期使用和尺寸适配性。条件相近时，优先复用小 sn 会使其不断离开 LRU，较少复用的大 sn 则逐渐靠前；这是倾向，不是保证。

retained 复用也使用 heap；其 LRU 主要复用统一管理和销毁遍历，并无日常按闲置时间淘汰需求。arena/PAC 销毁会清理 retained，默认 destroy hook 可解除映射。常见 64 位 Linux 的 `retain=true` 普通回收保留映射；`retain=false` 或自定义 hooks 可改变行为。合并、释放物理页、解除虚拟映射是三个不同动作。

## 8. dirty 命中具体节省什么

两种命中共享 `extent_recycle()`：锁 ecache、选块、移出 heap/LRU、更新 active 状态、必要时拆分并放回余量，最后检查 commit/zero。**retained 命中通常也不需要新 mmap；dirty 不会省掉所有元数据操作。**

| dirty 相对 retained 的收益 | 限定条件 |
|---|---|
| 提前结束查找，跳过后续 muzzy/retained | 普通分配顺序 |
| 少一次 `pac->grow_mtx` 加锁/解锁 | retained 命中也经过此锁；双方仍各取 ecache 锁 |
| 候选桶搜索范围可能更小 | dirty 比例限制，不保证每次实际扫描更少 |
| 无需重新 commit | retained 只有 decommitted 时才需要；Linux 常见 purge 后仍 committed |
| 可能免去合并后再拆分 | dirty 保留原边界且确实选中原尺寸块时 |
| 少部分统计/profiling 记账 | 取决于构建及运行配置 |

拆分可能需要取得并初始化新 edata、准备/提交 emap 边界、执行 split hook、修改大小、余量重新入 heap/LRU 及更新统计；此前合并也有类似管理成本。通常不复制用户数据。保留两个相邻 8 KiB 块，只是给直接复用提供机会，并非精确命中保证。

从完整生命周期看，短期保留 dirty 可避免 purge/decommit、页表撤销和相关 TLB 失效，以及再次触页时的缺页、物理页准备、清零和页表建立。后半段可能发生在应用访问阶段，不在 malloc 返回前。若排除这些物理页成本，主要剩下上述锁、查找和条件性管理操作。

`committed=true` 不等于物理页驻留。Linux 常见 overcommit 配置下，`MADV_DONTNEED` 后仍 committed，retained 复用无需 commit hook。清零请求也不保证 dirty 更便宜：dirty 可能需要 forced purge 或 memset，已知为零的 retained 可跳过清零。

## 9. decay 策略、purge 操作与触发入口

**decay 决定何时保留/回收多少空闲页；purge 执行内容丢弃或物理页回收。**

- Linux lazy purge 常用 `MADV_FREE`，RSS 不一定立即下降，之后可进入 muzzy。
- forced purge 常用 `MADV_DONTNEED`，通常及时移除已驻留页，保留映射和后续零页语义。
- 完整回收还可能走 decommit/unmap，并非每次都调用同一种 madvise，也非 dirty 必经 muzzy 再进 retained。

常规 decay 根据时间和历史空闲页数量计算总体保留上限，超出时从 LRU 按 extent 回收，并可能补做合并；不为每个 extent 设置精确倒计时。

**当前触发机制：**

1. **应用活动。** arena 分配/释放、tcache 批量 flush 等推进线程本地 ticker。当前几何分布 ticker 平均约每 1000 个相关 tick 提供检查机会；不是每 1000 次 malloc，更不是每次都 purge。检查使用 decay trylock，可因竞争跳过。
2. **时间判断。** 正数 decay 时间分为 200 个区间，期限带随机扰动；例如 10 秒对应基本区间 50 ms，但不保证每 50 ms 执行。检查时推进 epoch、更新 backlog、计算目标，再判断是否回收。
3. **可选后台线程。** 默认关闭。启用后按所负责 arena 的待处理工作计算等待时间，也可被提前唤醒；正数 decay 下，应用线程推进状态/通知，常规 purge 主要交给后台。
4. **显式及生命周期入口。** `arena.<i>.decay` 按策略处理，可不回收或延后；`arena.<i>.purge` 请求主动回收空闲页。修改 decay 配置、线程退出清理、arena 清理也可能触发检查或强制回收。

默认 `dirty_decay_ms=10000`、`muzzy_decay_ms=0`。0 表示页进入相应可回收路径后采用立即回收策略；-1 禁止该类页的自动 decay。无后台、无显式调用且相关分配活动停止时，时间流逝本身不会执行回收代码。

decay 不自动清空所有线程 tcache，也不能回收仍有存活对象的 slab。5.2.1 的主要触发类别相同，但当时使用线程/arena 对应的普通 ticker，不能套用当前几何分布实现。

## 10. arena 默认数量、创建条件、锁粒度与设计动机

默认自动 arena 上限：检测到 N>1 个 CPU 时为 `4*N`，只有 1 个时为 1。本地 `narenas_ratio=4`，5.2.1 也采用该规则；Linux 常见路径按 affinity mask 计可用逻辑 CPU，不必等于物理核心数或容器 CPU quota。

**上限不等于启动时全部创建。** 默认非 percpu、未显式指定 arena 时，新线程需要绑定 arena 才进入 `arena_choose_hard()`：

```text
找已有自动 arena 中关联线程数最少者
  -> 若其计数为 0：复用该 arena
  -> 若自动 arena 名额已用完：共享该最少者
  -> 否则：创建新 arena 并绑定
```

也就是说，**所有已创建自动 arena 都至少有 1 个关联线程，并且仍有名额时，下一个需要绑定的线程就触发创建**，不等每个 arena 关联多个线程。已经绑定的线程通常直接用本地记录，不随每次 malloc 重新比较负载；线程退出空出的 arena 可再次绑定。应用分配与内部元数据分配的关联计数分别维护，不是瞬时并发数或锁等待数。`narenas_auto=1` 时直接共用 arena 0。

手动 arena、oversize 专用 arena 另计。48 个可用 CPU 且无配置覆盖时默认自动上限为 192，但不能据此认定 MyRocks 实测创建了 192 个。查询配置用 `opt.narenas`，具体实例可查 `arena.<i>.initialized`；不要把 arena 索引范围直接当已初始化数量。

**decay 不能让无限增加 arena 变得免费：**

- arena 没有关联线程或近期没有分配活动，不代表其对象已经全部释放；退出线程分配的对象仍可能被其他线程持有。
- 半空 slab 的空位不能直接被其他 arena 的普通 bin 分配使用；少量长期存活对象可阻止 slab 回收，`dirty_decay_ms=0` 也无效。
- 全空 extent 可以 purge，但其他 arena 再使用物理页要付出回收、缺页和页表操作成本；同 arena 内直接复用 dirty 可以避免这轮周转。
- 实际创建的 arena 增加 bin、锁、统计和页分配状态等元数据，普通 decay 不销毁这些结构。
- 跨线程释放最终仍要回原 slab 的 arena/bin/shard；给释放线程新增 arena 不会迁移已有 slab。

不同 class 本就有独立 slab 集合，为它们分别加 bin 锁，可在较少 arena 内并发操作，同时共享该 arena 页级空闲池。当前补充新 slab 时会先释放 bin 锁。热点同 class 还可增加 bin shard，但这也进一步分散 slab 空位。

| 机制 | 解决的主要问题 |
|---|---|
| tcache | 普通对象操作暂不访问共享 bin，批量往返 |
| 按 class 的 bin 锁 | 不同 class 的独立操作可并发 |
| 同 class 的 bin shards | 分散热点 class 竞争，增加局部碎片风险 |
| 多 arena | 分散更广泛竞争，包括页分配层，扩大内存隔离范围 |
| decay | 回收整体空闲页，缓解驻留内存积累 |

若每线程独占 arena、对象由本线程释放且可接受内存成本，bin 锁细分的额外并发收益可能较小。其价值更多体现在 arena 数受限、线程数超过上限和跨线程释放等共享场景。

**按线程隔离分配模式，确实可能改善回收，但取决于负载：**

- 最典型的是**同一 size class、不同寿命**：A 分配少量长期对象，B 成批分配短期对象。共享 bin 时若混入同一 slab，A 的对象可能阻止整 slab 回收；分属 arena 时，B 的对象真正归还 slab 后，更容易形成全空 slab，供复用或 decay。
- 不同 size class 原本就使用不同 slab，额外 arena 隔离的作用更多落在页/extent 层。线程边界也不一定对应寿命边界：单线程可能混合多种寿命，多线程也可能具有相同模式。
- 因而这是**隔离寿命干扰与共享空闲空间之间的权衡**，不能推导出 arena 越多 RSS 越低。上述收益是机制推断，需要实际负载验证；它不等于作者明确记录的首要设计动机。

**作者明确记录的设计动机与演进：**

1. **多 arena 首先服务于多核扩展性。** Jason Evans 的 2006 年论文强调降低锁竞争，也指出仅细分空闲链表锁仍可能因共享元数据的 cache line 在 CPU 间迁移（cache sloshing）而扩展不佳。独立 arena 同时减少这类迁移，并缓解不同线程所用对象的 false sharing。多 CPU 默认采用 `4*N` arena，是为降低同时使用同一 arena 的概率；论文当时描述的是轮转绑定，不能直接当作当前分配算法。[作者论文](https://people.freebsd.org/~jasone/jemalloc/bsdcan2006/jemalloc.pdf)
2. **优先选择关联线程少的 arena，是后来的绑定策略改进。** 作者在 2011 年提交中明确将盲目轮转改为选择“关联线程最少、同数时编号最低”的 arena。当前代码据关联计数选择、复用或新建 arena，没有测量线程的对象大小分布、寿命或访问模式；不能把它解释成自动识别负载并隔离。[原始提交](https://github.com/jemalloc/jemalloc/commit/597632be188d2bcc135dad2145cc46ef44897aad)
3. **多 arena 与细粒度锁是互补改进。** 2011 年工程文章记录：早期每 arena 一把锁，慢速页操作会串行化同 arena 的分配，后来引入每个 small size class 的锁，并缩小页操作期间的持锁范围。文章也明确承认，多线程构建长期数据后转为单线程运行，可能留下利用率低、碎片较多的 arena；更多 arena 并非无条件更优。[工程说明](https://engineering.fb.com/2011/01/03/core-infra/scalable-memory-allocation-using-jemalloc/)

## 11. glibc 对比与 MyRocks RSS 的解释

**glibc 的选块不是随机算法。** tcache/fastbin 有近期释放优先行为，普通 bin 有尺寸/队列规则，largebin 按实际 chunk 大小支持 best-fit，另有 `last_remainder` 路径。不能把 largebin 行为概括为全部 malloc 路径。

largebin 桶中可同时有小于和大于请求的块，因此大小排序既帮助查找，也有利于保留大块。若改为从下界足够大的桶取块：把实际分配一并取整会增加内部碎片；只抬高搜索起点则可能错过前一桶的精确匹配。glibc 桶边界是分类边界，不天然等于请求分配粒度。

glibc 2.35/2.39 降低 RSS 不只靠 top：独立 mmap chunk 可解除映射，`malloc_trim()` 可扫描 bin，回收空闲 chunk 内部整页。但普通非 mmap free 的自动回收主要围绕 top/heap 尾部，不等于 jemalloc 的持续 decay。

**MyRocks 公开实测：** 2025 年、10G block cache、48 核且关闭 SMT，block 读取分配、淘汰释放，寿命差异很大。原文单位 G：

| 分配器 | 峰值 VSZ | 峰值 RSS |
|---|---:|---:|
| jemalloc 5.2.1 | 45.6 | 12.2 |
| tcmalloc 2.9.1 | 15.3 | 13.1 |
| glibc 2.35 | 46.1 | 36.2 |

这是各指标独立峰值，不能相减当作同一时刻 retained。[原始测试](https://smalldatum.blogspot.com/2025/04/battle-of-mallocators.html)

统计口径也要分开：请求/已分配对象字节、active 页、dirty/muzzy 页、retained 虚拟地址、元数据和进程 RSS/VSZ 不是同一个量；不能把其中某一差值未经分析就全部归为“碎片”。

后续 glibc `MALLOC_ARENA_MAX` 设为 1/8/48/96 时，峰值 RSS 分别为 14.1/27.7/29.2/32.5G；1 个 arena 大幅缓解膨胀，但性能严重下降。[后续测试](https://smalldatum.blogspot.com/2025/04/battle-of-mallocators-part-2.html)

jemalloc 同样存在 arena 隔离。低 RSS 与此不矛盾：A 的空闲 extent 不能直接交给 B，但 A 可 purge 物理页、保留虚拟地址；B 在自己的地址范围触页，由 OS 提供物理页。隔离仍在，物理页不必永久重复占用。

可能共同贡献优势的机制包括尺寸分类和 slab 布局、稳定复用方向、arena/tcache 管理、页级回收。对象取整浪费只是 RSS 的一项，可能被其他项的改善抵消。独立全空 extent 已能 purge，无需先合并；更少 arena 也会改变布局、请求混合和竞争，不只是改变空闲块归属。

**公开测试没有隔离各机制贡献，不能认定 `(sn, addr)` 或 decay 单独是首要原因。** 要验证需同版本、同负载做控制变量实验，观测 active/半空 slab、dirty/muzzy、元数据、缺页和 RSS 等；不同策略对大块保护也可能相反。

## 12. 为什么 glibc 没有采用同样的 decay

技术上可行，现有 `malloc_trim()` 已具备回收内部整页的机制；chunk header 不是绝对障碍。未找到足以证明维护者“不采用 jemalloc 式 decay”的统一官方解释，不能猜测作者动机。

可确认的历史事实：2015 年 glibc 合入为非主 arena heap 应用 `trim_threshold` 的修改，减少反复 free/purge/重新触页带来的系统调用、页表、TLB、缺页及清零成本，特定测试显著提速。[已合入提交](https://sourceware.org/pipermail/glibc-cvs/2015q2/058210.html)

这证明回收过勤有代价，**不证明 decay 不值得做**；延迟、批量回收本来就在缓解这一问题。从实现推断，完整 decay 还需设计候选/时间历史、拆分合并后的状态维护、重复 madvise 抑制、锁内扫描预算、闲置期驱动和默认参数；若用后台线程，还需处理初始化、退出及 fork 协调。

2026 年 4 月 glibc RFC 提议在普通释放中回收内部整页、用累计阈值限流。作者报告特定复现 RSS 从 1247 MB 降至 296 MB，耗时从 0.34 s 增至 0.50 s；这是提案测试，非时间 decay，也不能据此认定已正式合入。[RFC](https://sourceware.org/pipermail/libc-alpha/2026-April/176852.html)

## 13. 源码索引

以下路径相对 `/workspaces/joey_project/jemalloc`；后续以对应版本源码为准，不依赖旧行号。

| 主题 | 主要入口 |
|---|---|
| size class / slab 配置 | `include/jemalloc/internal/sc.h`、`src/bin_info.c` |
| bin / slab 切换及释放 | `src/bin.c`、`src/arena.c`、`include/jemalloc/internal/bin_inlines.h` |
| edata / sn / heap | `include/jemalloc/internal/edata.h`、`ph.h`；`src/base.c` |
| emap 与原子查询 | `src/emap.c`、`src/rtree.c` 及对应内部头文件 |
| extent 分桶选块 | `src/eset.c:eset_first_fit()`；`src/sz.c` |
| 拆分、合并、retained 复用 | `src/extent.c:extent_recycle/extent_split_impl/extent_record/extent_alloc_retained/extent_grow_retained` |
| 页分配与 decay | `src/pac.c:pac_alloc_real/pac_maybe_decay_purge/pac_do_deferred_work`；`src/decay.c`、`src/pa.c` |
| decay 触发与后台线程 | `include/jemalloc/internal/arena_inlines.h:arena_decay_ticks()`；`src/background_thread.c`、`src/ctl.c` |
| purge / commit / mmap | `src/ehooks.c`、`src/pages.c`、`src/extent_mmap.c` |
| arena 数量与绑定 | `src/jemalloc_init.c:malloc_narenas_default()`；`src/arenas_management.c:arena_choose_hard()` |

补充参考：[当前手册](https://jemalloc.net/jemalloc.3.html)、[5.2.1 文档](https://raw.githubusercontent.com/jemalloc/jemalloc/5.2.1/doc/jemalloc.xml.in)、[glibc 2.39 源码](https://github.com/bminor/glibc/blob/glibc-2.39/malloc/malloc.c)、[低地址复用的历史讨论](https://lists.freebsd.org/pipermail/freebsd-current/2007-March/070303.html)。手册可能随版本变化，历史经验也不能代替当前负载测量。
