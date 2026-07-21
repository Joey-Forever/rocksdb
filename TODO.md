# 单 RocksDB + Per-Region MemTable 技术框架

## 核心目标

整体沿用 TiKV Raftstore v1：一个 TiKV Store 只维护一个 RocksDB 实例，所有
Region 共享全局 LSM Tree、VersionSet、MANIFEST、Block Cache 和后台线程池。

主要变化是将 RocksDB 中一个 CF 只有一个 mutable MemTable 的结构，改成每个
Region 在各数据 CF 中分别维护自己的 MemTable：

```text
One RocksDB
├── Global Sequence / VersionSet / MANIFEST
├── Shared Block Cache / Table Cache / Background Pools
├── default CF
│   ├── Region A mutable + immutable[]
│   ├── Region B mutable + immutable[]
│   └── Region C mutable + immutable[]
├── write CF
│   ├── Region A mutable + immutable[]
│   └── ...
└── lock CF
    ├── Region A mutable + immutable[]
    └── ...
```

RocksDB WAL 关闭，Raft Log 作为崩溃恢复日志。Region 维度主要用于 MemTable
生命周期、Flush、L0 Compaction Debt 和写入流控，不作为 SST 的永久所有权边界。

## Region MemTable 结构

每个 CF 内部维护一个 `RegionMemTableSet`，通过 Region ID 找到该 Region 当前的
mutable MemTable、immutable MemTable 队列和 Flush 状态。

```text
RegionMemTableSet
└── Region ID
    ├── current mutable generation
    ├── immutable generations
    ├── accumulated bytes
    ├── accumulated Raft Log count / Apply Index range
    ├── oldest unflushed timestamp
    ├── L0 file count / L0 bytes / compaction debt
    ├── durable index
    └── flush / compaction / throttle state
```

MemTable 按需创建。没有写入的 Region 只保留少量元数据，不预分配完整 Write
Buffer。所有 Region MemTable 的内存仍由全局 WriteBufferManager 统一限制。

## Apply 和 WriteBatch

Apply Worker 继续采用 v1 的批处理模型，一轮可以处理多个 Region 的一批 committed
Raft Log，并把修改加入同一个逻辑 WriteBatch。

关闭 WAL 后仍然保留 WriteBatch，因为它继续承担以下职责：

- 跨 Region 和跨 CF 合并写入。
- 统一分配 RocksDB Sequence Number。
- 保证同一 Raft Entry 中多项修改的原子可见性。
- 减少 WriteThread、锁和函数调用开销。
- 支持 SavePoint，以及单条 Raft Entry 失败时的局部回滚。

写入流程：

```text
Apply Worker selects ready Region FSMs
    -> append operations to one logical WriteBatch
    -> allocate sequence numbers without writing RocksDB WAL
    -> route operations by Region ID and CF
    -> insert into corresponding Region mutable MemTables
    -> publish Apply result and client callback
```

WriteBatch 中的操作携带 Region 上下文，或者以连续 Region 操作组的方式编码 Region
上下文。写入时不依赖当前 Region Range 重新推断归属，避免与 Split/Merge 并发时
发生路由错误。

## Region Flush 策略

每个 Region 独立维护三个主要 Flush 指标，任意指标达到阈值即可触发该 Region 的
MemTable 切换：

1. 尚未 Flush 的累计数据量。
2. 尚未 Flush 的累计 Raft Log 数量或 Apply Index 距离。
3. 最老未 Flush 数据的累计存活时间。

触发后，Apply Worker 只完成内存状态切换：

```text
Region A mutable generation N
    -> freeze as immutable generation N
    -> create mutable generation N + 1
    -> enqueue background flush task
```

实际 SST 构建和 VersionEdit 安装交给后台 Flush Pool，Apply Worker 不等待磁盘 I/O，
新日志可以继续写入下一代 mutable MemTable。

后台 Flush Scheduler 可以同时选择多个达到阈值的 Region：

- 热点 Region 优先独立 Flush，隔离其 L0 Compaction Debt。
- 相邻的低流量 Region 可以合并到一次 Flush Job，减少小 SST 和文件数量。
- 不相邻的 Key Range 不写入同一个 SST，避免 SST 的最小/最大 Key 覆盖中间范围。

一个旧 immutable MemTable 或 SST 不要求永久只属于一个 Region。Region 独立 Flush
是一种降低 L0 读写放大的调度策略，而不是持久化格式中的所有权语义。

## L0 和 Compaction

Region 隔离主要作用于 L0。每个 Region 分别统计：

- L0 文件数量。
- L0 数据量。
- Pending Compaction Bytes。
- Compaction 消耗速度和债务增长速度。

Compaction Picker 根据 Region Debt 选择 L0 Compaction，热点 Region 的 L0 文件不会
因为全局文件计数直接阻塞其他 Region。进入 L1 以后，SST 可以继续使用 RocksDB
原有的全局 Level Compaction 规则，按 Key Range 保持不重叠，不要求 Region 隔离。

如果一个 L0 SST 包含多个 Region，需要在 Table Properties 或关联元数据中记录各
Region 的 Key Range、字节贡献和最大 Apply Index，以便正确计算 Region Debt 和
Flush 持久化进度。

Compaction Pool 全局共享，但调度时需要提供 Region 公平性：热点 Region 获得足够
资源偿还债务，同时不能长期占满全部后台 Compaction Slot。

## 两级写入流控

### Region 级流控

当某个 Region 的 L0 文件数、L0 字节数或 Compaction Debt 超过阈值时，只限制该
Region 的新写请求：

```text
Region A overloaded -> throttle Region A proposals
Region B healthy    -> continue serving Region B
```

流控反馈从 RocksDB 传到 TiKV Scheduler/Proposal 入口。已经 committed 的 Raft Log
继续 Apply，避免产生无法收敛的 Apply Lag 和 Raft Log 堆积。

### 全局流控

Region 隔离不能消除物理资源共享，因此以下资源达到硬限制时仍执行 Store/DB 级
保护：

- 所有 Region MemTable 的总内存。
- immutable MemTable 和 L0 文件总数。
- 全局 Pending Compaction Bytes。
- 磁盘空间和后台 I/O 饱和度。
- 文件句柄、Table Cache 和 VersionSet 元数据。

Region 级流控负责业务隔离，全局流控负责防止整个实例资源耗尽。

## WAL-less 持久化与恢复

关闭 RocksDB WAL 后，MemTable 中未 Flush 的数据通过 Raft Log 恢复。每个 Region
维护独立的持久化边界：

```text
durable_index(region) = min(
    default CF durable index,
    write CF durable index,
    lock CF durable index,
    region/apply metadata durable index)
```

持久化顺序：

```text
freeze Region MemTable generations
    -> build and install all required SSTs
    -> persist Region durable_index
    -> allow Raft Log GC up to durable_index
```

SST 或 VersionEdit 安装失败时不推进 `durable_index`。`durable_index` 没有可靠持久化
前，不允许删除对应 Raft Log。崩溃发生在 SST 安装成功、durable index 更新完成
之前时，恢复流程使用旧边界保守重放日志。

Region 的 durable index 可以在所有相关 CF Flush 完成后写入 Raft Engine 中的专用
元数据记录。Raft Log GC 只依赖这个已经持久化的边界，不依赖内存中的 applied
index。

某个 CF 在一段 Apply 区间内没有实际数据修改时，也需要推进该 CF 的逻辑 Flush
进度，避免 `durable_index` 被无修改的 CF 永久卡住。低流量 Region 通过时间或日志
量阈值定期 Flush，避免长期保留大量 Raft Log。

## Raft State Machine Snapshot

Snapshot 采用 TiKV Raftstore v1 的精确 Range Snapshot 思路，不采用 v2 的异步整库
Checkpoint。前台只负责固定一致性边界，Region 数据的遍历、整理和文件生成全部放到
后台完成。

同一个 Region 的 Raft Entry 只允许串行 Apply。处理 Snapshot 请求时，Apply Worker 在
该 Region 的两个 Entry 之间建立边界：

```text
apply entries through index I
    -> commit the pending RocksDB WriteBatch
    -> record Region applied_index = I and applied_term = T
    -> acquire one RocksDB Snapshot
    -> record its global sequence_number = S
    -> capture Region range / epoch / peer configuration
    -> enqueue background Range Snapshot task
    -> immediately continue applying later entries
```

`(I, T)` 和 RocksDB Snapshot 必须在同一个 Region Apply 顺序点上绑定，保证 Snapshot
视图严格包含 `I` 及以前的修改，不包含该 Region 在 `I` 之后的修改。其他 Region 可以
并发写入并推进 RocksDB 的全局 Sequence Number；后台只扫描目标 Region 的 Key Range，
因此这些无关写入不会进入最终文件。

前台除了记录 `sequence_number`，还必须一直持有对应的 RocksDB Snapshot Handle。只
记录数字不足以固定历史版本，Compaction 可能删除该 Sequence 所需要的数据。获取
Snapshot 不要求把 MemTable Flush 成 SST，也不等待磁盘 I/O。

后台任务对 default、write、lock 等数据 CF 使用同一个 RocksDB Snapshot 和目标
Region Range 进行遍历，将该一致性视图重新编码成有序、紧凑的 Snapshot SST：

```text
ReadOptions.snapshot = snapshot_at_sequence_S
    -> scan every Region data CF in [start_key, end_key)
    -> merge MemTable and all LSM levels by RocksDB visibility rules
    -> emit only versions visible at sequence S
    -> build compact per-CF Snapshot SST files
    -> attach index I / term T / Region metadata / checksums
```

Snapshot 文件不携带目标 Sequence 之后的物理写入，也不携带普通 L0 重叠、已经淘汰
的 Internal Key 版本和无效 Tombstone。接收方安装后直接把状态机边界设置为 `(I, T)`，
只需要继续 Apply `I + 1` 之后的 Raft Log，不会重复执行 Snapshot 中超前的操作。

Snapshot Scheduler 根据机器负载控制任务：

- 根据磁盘带宽、CPU、后台 Compaction 压力和 Block Cache 压力决定是否启动。
- 限制每台 Store 同时生成 Snapshot 的数量，并为任务设置独立 I/O Rate Limiter。
- 高负载时降低扫描速度，但为已经持有 RocksDB Snapshot 的任务保留最低完成带宽，
  避免长期阻止旧版本回收。
- 超过最大持有时间仍无法完成时取消任务、释放 Snapshot Handle，等待负载下降后重新
  获取新的 `(applied_index, term, sequence_number)` 边界。
- Region Epoch、Split/Merge 状态或 Snapshot 请求已经失效时取消旧任务，不发送过期
  文件。

长时间持有 RocksDB Snapshot 会阻止 Compaction 删除其仍然可见的历史版本，增加磁盘
空间和读放大。因此负载控制的目标不是无限期暂停任务，而是控制启动并发并让已启动
任务以稳定的最低速率完成。Snapshot 扫描仍会与共享 RocksDB 中的其他 Region 竞争
CPU、磁盘和 Cache，但这种资源影响可以限速、观测和取消，不改变 Raft 状态机的正确性
语义。

### 不采用 TiKV v2 异步 Checkpoint 的原因

TiKV v2 先在 Apply 线程记录 `applied_index = I`，再把仍然接收后续写入的 Region
Tablet 交给后台线程创建 RocksDB Checkpoint，因此可能出现边界错位：

```text
Raft Snapshot metadata: applied_index = I
Checkpoint data:         may already contain entries I + 1 ... J
```

其最致命的问题是接收方会在已经包含 `I + 1 ... J` 数据的状态上再次重放这些日志，
从而要求现在以及未来的所有 Apply Command 都必须可以安全重复执行。计数器增量、
Read-Modify-Write、依赖时间或随机数的写入以及外部副作用都可能因此产生静默数据错误。

如果改成在整个 Checkpoint 生成期间停止该 Region Apply，虽然可以得到精确边界，但
Checkpoint 触发的 MemTable Flush、文件枚举和磁盘 I/O 会直接进入前台延迟。在磁盘
繁忙时暂停可能达到秒级，此时 committed Raft Log 持续堆积，写请求无法返回，需要等待
更高 Apply Index 的线性一致读也会阻塞，同时增加 Raft Log 保留量和 Apply Lag。因此
本设计不采用 v2 的方案。

## Split 和 Merge

Split/Merge 只改变 Region 到 MemTable generation 的读取和写入映射，不要求立即
重写旧 immutable MemTable 或 SST。

### Split

```text
Parent Region A [a, z)
    -> freeze current parent generations
    -> child A1 [a, m) writes to new A1 generations
    -> child A2 [m, z) writes to new A2 generations
    -> children read old parent generations by their own Key Range
    -> old generations become ordinary global LSM data after Flush
```

### Merge

```text
Region A generations + Region B generations
    -> remain as readable old generations
    -> merged Region writes to a new generation
    -> old generations retire after Flush and Snapshot references are released
```

Region generation 负责区分新旧写入归属，Region Epoch 继续负责请求合法性。旧
Snapshot 和 Iterator 持有对应 generation 引用，直到其生命周期结束后再回收内存。

## 读取路径

读取某个 Region 时，只查询与该 Region 当前 Range 相关的内存 generations，然后
继续查询全局 LSM：

```text
Region mutable generation
    -> Region immutable generations
    -> inherited parent/source generations from Split/Merge
    -> global L0/L1/... SSTs
```

所有来源仍按 RocksDB Internal Key 和 Sequence Number 合并，保持 Snapshot、Delete、
Range Tombstone 和 Iterator 的原有可见性规则。

SuperVersion 需要持有 Region MemTable Set 的一致视图。MemTable 切换、Split/Merge
映射变化和 SST 安装时发布新的版本，旧版本由正在执行的读请求继续引用。

## RocksDB 核心改造

该设计需要对 RocksDB 做以下结构性修改：

- 在一个 CF 中支持多个 Region mutable MemTable 和 immutable 队列。
- WriteBatch Insert 路径根据 Region 上下文路由到对应 MemTable。
- SuperVersion 保存 Region MemTable Set 及 generation 映射。
- Get、MultiGet、Iterator 和 Snapshot 合并目标 Region 的内存数据与全局 LSM。
- Flush Picker 按 Region 的大小、日志量和未 Flush 时间选择 immutable MemTable。
- Flush Job 支持一次处理一个或多个相邻 Region generations。
- VersionSet 和 Table Properties 保存 Region L0 统计及 Flush 结果。
- Compaction Picker 按 Region L0 Debt 调度，同时服从全局资源限制。
- Write Controller 输出 Region 级流控信号和全局硬限制信号。
- Flush 完成后向 TiKV 报告各 CF 的 Region durable index。

最终形成的边界是：

```text
TiKV owns:
    Raft ordering, Region lifecycle, durable index, proposal flow control

RocksDB owns:
    Region MemTable routing, sequence visibility, Flush, LSM, Compaction

Shared contract:
    Region context, generation, Flush result, L0 debt, durable progress
```
