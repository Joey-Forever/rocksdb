// Copyright (c) 2011-present, Facebook, Inc. All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#pragma once

#include <array>
#include <atomic>

#include "rocksdb/slice.h"
#include "table/multiget_context.h"
#include "util/atomic.h"
#include "util/hash.h"

namespace ROCKSDB_NAMESPACE {

class Slice;
class Allocator;
class Logger;

// A Bloom filter intended only to be used in memory, never serialized in a way
// that could lead to schema incompatibility. Supports opt-in lock-free
// concurrent access.
//
// This implementation is also intended for applications generally preferring
// speed vs. maximum accuracy: roughly 0.9x BF op latency for 1.1x FP rate.
// For 1% FP rate, that means that the latency of a look-up triggered by an FP
// should be less than roughly 100x the cost of a Bloom filter op.
//
// For simplicity and performance, the current implementation requires
// num_probes to be a multiple of two and <= 10.
//
// JOEY_TODO: 看到这里
// 问题：huge page是什么？和tlb有什么关系？什么情况下用这个东西，有什么优点？
//
// 回答：
// Huge page（大页）就是比普通虚拟内存页更大的页。它的主要价值不是减少 CPU cache miss，
// 而是减少 TLB miss 和页表开销。
//
// 典型 Linux/x86-64：
//
// 普通页：4 KiB
// 大页：  2 MiB
// 超大页：1 GiB
// CPU cache line：通常64 B
//
// 注意这四者不是同一个层级。
//
// 虚拟地址为什么需要页表
//
// 进程看到的是虚拟地址 virtual address，内存实际使用的是物理地址 physical address。
// CPU 访问 value = *ptr 时，需要完成“虚拟地址 -> 地址翻译 -> 物理地址 -> 访问
// cache/memory”。操作系统通过多级页表记录“虚拟页号 -> 物理页框号”。
//
// 例如 4 KiB page 下，虚拟地址可以粗略拆成“虚拟页号 + 页内偏移”。页内偏移为
// 12 bit，因为 4 KiB = 2^12。只要找到虚拟页对应的物理页框，低 12 bit 的页内
// 偏移保持不变。
//
// TLB 是页表翻译的 CPU cache
//
// 每次内存访问都遍历多级页表会非常慢，所以 CPU 内部有 TLB（Translation Lookaside
// Buffer）。它缓存最近使用的“虚拟页 -> 物理页”映射。
//
// 访问过程大致是：
//
// 虚拟地址
//   -> 查询TLB
//      -> TLB hit：立即得到物理地址
//      -> TLB miss：page-table walk，然后填入TLB
//
// TLB hit 很快；TLB miss 需要 page-table walk，可能访问多级页表。页表项本身如果也
// 不在 CPU cache 中，还会进一步访问内存。因此 TLB miss 会带来多次额外内存访问、
// CPU pipeline stall、页表项污染 CPU cache 和更高的访问延迟。
//
// Huge page 为什么减少 TLB miss
//
// 假设程序访问 1 GiB 内存。使用 4 KiB 普通页需要：
//
// 1 GiB / 4 KiB = 262,144 个页
//
// 使用 2 MiB huge page 只需要：
//
// 1 GiB / 2 MiB = 512 个页
//
// 减少了 512 倍。一个 TLB entry 如果映射普通页，只覆盖 4 KiB；如果映射 2 MiB
// huge page，则覆盖 2 MiB。所以相同数量的 TLB entries 能覆盖更多内存，这称为
// TLB reach。
//
// 例如纯粹假设有 512 个对应 TLB entries：
//
// 4 KiB page：512 * 4 KiB = 2 MiB
// 2 MiB page：512 * 2 MiB = 1 GiB
//
// 实际 CPU 通常为不同页大小设置不同数量和层次的 TLB entries，但核心关系不变：页越大，
// 每个 TLB entry 覆盖的地址范围越大，大型工作集产生的 TLB miss 通常越少。
//
// Huge page 也能减少页表内存
//
// 4 KiB page 管理 1 GiB 内存需要 262,144 个末级页表项，而 2 MiB page 只需要
// 512 个大页表项。因此 huge page 还能减少页表自身占用、page-table walk 的层级或工作量、
// 页表项对 CPU cache 的污染以及内核管理大量小页的元数据开销。
//
// Huge page 不改变 cache line
//
// 即使使用 2 MiB huge page，CPU cache line 仍然通常是 64 B。Huge page 优化的是
// 虚拟地址翻译，cache line 优化的是 CPU cache 与内存之间的数据传输和一致性。
// alignas(64) 主要针对 cache line/false sharing，2 MiB huge page 主要针对
// TLB/page table。二者可以同时存在，但解决的是不同问题。
//
// Linux 中常见的两种 huge page
//
// 1. 显式 HugeTLB
//
// 程序通过 mmap(..., MAP_HUGETLB, ...) 明确申请大页。通常需要提前预留，例如：
//
// sysctl -w vm.nr_hugepages=20
//
// 其特点是页大小明确、行为更可预测、通常需要预留 huge-page pool、申请不到时可能直接
// 失败、大页资源独立管理、内存碎片和预留配置需要运维关注。RocksDB 当前
// MemMapping::AllocateHuge() 在 Linux 上使用的正是 MAP_PRIVATE | MAP_ANONYMOUS |
// MAP_HUGETLB。
//
// 2. Transparent Huge Pages，THP
//
// THP 由内核自动尝试把普通匿名内存合并或直接分配成大页，应用不一定显式使用
// MAP_HUGETLB。程序也可以通过 madvise(ptr, size, MADV_HUGEPAGE) 表达偏好。
// 优点是应用接入简单；缺点是行为没有显式 HugeTLB 那么确定，内核可能为了整理或合并大页
// 产生 memory compaction、后台 khugepaged 工作、分配延迟抖动以及大页拆分和合并开销。
// 当前 RocksDB 这条 Arena 路径主要是显式 HugeTLB，不是仅仅依赖 THP。
//
// RocksDB Arena 如何使用 huge page
//
// 构造 Arena 时传入 Arena(block_size, tracker, huge_page_size)。如果
// huge_page_size > 0 并且平台支持 MAP_HUGETLB，Arena 会记录希望使用的大页大小。
//
// 假设 kBlockSize = 5 MiB，huge_page_size = 2 MiB，Arena 会将实际 huge-page
// block 大小向上取整为 ceil(5 MiB / 2 MiB) * 2 MiB = 6 MiB，因为显式
// huge-page mapping 必须按大页粒度申请。
//
// 当 inline block 或当前 active block 不足时，Arena 会尝试
// MemMapping::AllocateHuge(size)。在 Linux 上最终进入带有 MAP_PRIVATE、
// MAP_ANONYMOUS 和 MAP_HUGETLB 的 mmap。如果 huge-page 分配失败，Arena 会
// fallback 到普通 block allocation，因此启用它通常不会直接导致 RocksDB 无法继续分配，
// 但会记录 warning。
//
// 什么情况下适合使用
//
// 1. 大型、长期驻留、频繁访问的内存区域，例如很大的 MemTable、大型内存索引、大型
// hash table、大型 allocator arena、数据库 buffer/cache 元数据、大规模图或内存数据库，
// 以及随机访问的大 working set。这些数据结构会触及大量不同的 4 KiB page，容易产生
// TLB miss。使用 2 MiB huge page 后，一个 TLB entry 可以覆盖更大范围。
//
// 2. 工作集明显超过 TLB coverage。小数据结构全部落在几十个普通页中时，TLB 已经能覆盖，
// huge page 很难提供收益。如果是数 GiB 的随机访问结构，huge page 才更有可能明显降低
// TLB miss。
//
// 3. Arena 式整块分配。Arena 一次申请大 block，在 block 内切割小对象，小对象生命周期
// 相同，最终整块释放，很少产生单独 free，能比较充分地利用整个 huge page。如果每次只申请
// 几个很小且短命的对象，显式申请 2 MiB huge page 会造成严重浪费。
//
// 主要优点
//
// 1. 减少 TLB miss。这是最核心的收益，尤其是大型随机访问数据结构。
// 2. 减少 page-table walk。
// 3. 减少页表内存，一个大页表项可以代替大量普通页表项。
// 4. 降低 page-table walk 对 CPU cache 的污染。
// 5. 对于内存密集、TLB miss 明显的 workload，可能提高吞吐并降低 CPU cycles/op。
//
// 主要缺点
//
// 1. 内部碎片。只使用 100 KiB，却分配一个 2 MiB huge page，剩余部分可能被浪费。
// 2. 需要预留和运维配置。预留太少会申请失败，预留太多会占住普通应用可用的物理内存。
// 3. 物理内存碎片。大页需要更大粒度的物理内存资源，通常适合在启动早期预留。
// 4. 内存回收灵活性降低。HugeTLB 页面不像普通匿名页一样灵活地参与常规内存管理。
// 5. NUMA 影响可能更明显。绑定或 first-touch 策略不合适时，收益可能被远程 NUMA
// 访问延迟抵消。
// 6. 不保证一定更快。如果瓶颈是 CPU cache miss、branch misprediction、锁竞争、WAL I/O、
// compaction I/O、comparator 或内存带宽，而不是 TLB miss，收益可能很小甚至为负。
//
// 如何判断是否值得开启
//
// 应通过 workload 压测和硬件计数器判断，例如 Linux perf 中关注 dTLB-loads、
// dTLB-load-misses、iTLB-load-misses、page-faults、cycles、instructions 和
// cache-misses。比较普通页和 Huge page 下的 QPS、p99、cycles/op、dTLB miss/op 和
// 内存占用。如果 dTLB miss/op、cycles/op 明显下降，吞吐或尾延迟改善，且额外内存占用
// 可接受，才说明适合实际 workload。
//
// 一句话概括：Huge page 用更大的页映射粒度扩大 TLB coverage，从而减少大型内存工作集的
// 地址翻译和页表开销；它最适合长期驻留、规模大、访问频繁的 Arena/索引结构，但会付出内部
// 碎片、预留内存和更低内存管理灵活性的代价。
//
// 问题：将key本身拆成索引分量的为什么没有取代跳表、平衡树这些索引结构
//
// 回答：
// 因为“把 key 拆成几段直接索引”并不是没有代价。它实际上就是 Trie / Radix Tree（基数树）。它在合适场景里确实会取代平衡树，但不适合所有索引问题。
//
// 核心差别是：Radix Tree 的成本主要取决于 key 的长度；平衡树、跳表的成本主要取决于元素数量。
//
// 假设有 64 位整数 key，每次取 8 位：
//
// ```text
// key = [8位][8位][8位][8位][8位][8位][8位][8位]
//         ↓    ↓    ↓    ↓    ↓    ↓    ↓    ↓
//        逐级直接索引
// ```
//
// 最多走 8 层，不需要比较。但每个节点可能有 256 个子指针。数据稀疏时，大量位置为空，空间浪费严重。
//
// ### 1. 分支数与树高存在权衡
//
// 每次取更多位：
//
// ```text
// 每级 4 位：16 个槽，层数多
// 每级 8 位：256 个槽，层数少
// 每级 16 位：65536 个槽，层数很少
// ```
//
// 层数越少，每个节点的指针数组越大；层数越多，指针跳转和缓存未命中越多。
//
// 页表也存在这个问题，只不过页表场景非常特殊：
//
// - key 固定为虚拟页号
// - key 长度固定
// - 每个页表刚好放进一个物理页
// - 页表项大小固定
// - CPU 硬件直接支持遍历
// - TLB 可以缓存结果
//
// 因此它的参数可以由硬件精心设计。
//
// ### 2. 普通 key 经常不是固定长度整数
//
// 数据库中的 key 可能是：
//
// ```text
// "alice@example.com"
// "北京市/海淀区/..."
// 复合键：（时间, 用户ID, 类型）
// 任意长度的二进制字符串
// ```
//
// Trie 当然也能处理，但长公共前缀或随机字符串可能产生很多层。平衡树只需要一个比较函数，因此更通用：
//
// ```text
// compare(key1, key2)
// ```
//
// 只要 key 可以排序，树结构本身不需要理解 key 的内部表示。
//
// ### 3. “直接索引”仍然会产生随机内存访问
//
// 理论上每一级是 `O(1)` 数组索引，但每一级通常要读取一个新节点：
//
// ```text
// 根节点 → 子节点 → 孙节点 → ...
// ```
//
// 这些节点可能不连续，会产生多次 CPU cache miss。现代系统中，内存访问延迟往往比几次整数比较贵得多。
//
// B-tree 的一个节点可以保存很多有序 key。读取一个缓存行或磁盘页后，可以在节点内部完成多次比较，因此特别适合存储系统：
//
// ```text
// 一次磁盘/缓存页读取
//        ↓
// 获得数百个 key 和子节点范围
// ```
//
// 所以虽然 B-tree 有比较过程，却可能需要更少的昂贵随机访问。
//
// ### 4. 稀疏数据会让普通 Radix Tree 浪费空间
//
// 例如只保存两个 64 位 key：
//
// ```text
// 0x0000000000000001
// 0xFFFFFFFFFFFFFFFF
// ```
//
// 如果每级都使用完整数组，就可能为了两个元素分配许多大型节点。
//
// 可以通过路径压缩、稀疏节点等方式改善，于是就产生了：
//
// - Patricia Trie
// - Crit-bit Tree
// - Adaptive Radix Tree（ART）
// - Linux 内核的 XArray / radix tree 类结构
//
// ART 会根据实际子节点数量切换节点规格，例如只存 4、16、48 或 256 个分支。这说明这种思想不仅存在，而且已经广泛使用，只是实现比页表更复杂。
//
// ### 5. 不同结构擅长的操作不同
//
// | 结构 | 主要优势 |
// |---|---|
// | Hash Table | 精确查找平均 `O(1)` |
// | Radix Tree / Trie | 固定长度 key、前缀查询、避免完整 key 比较 |
// | B-tree/B+tree | 范围扫描、磁盘和缓存友好、高扇出 |
// | 平衡二叉树 | 通用有序映射、稳定的最坏情况复杂度 |
// | 跳表 | 实现简单，范围遍历方便，并发实现相对自然 |
// | 页表式 Radix Tree | 固定整数地址、硬件遍历、稀疏地址空间 |
//
// 例如要查询：
//
// ```text
// 找出 [1000, 2000] 内的所有记录
// ```
//
// B+tree 找到 `1000` 后，可以沿叶子节点顺序扫描。普通 radix tree 虽然也能做有序遍历，但实现和存储布局未必像 B+tree 那样适合批量扫描。
//
// ### 关键结论
//
// 它没有全面取代其他索引，是因为不存在脱离工作负载的“最优索引”。
//
// 页表采用这种结构，是因为它面对的是一个极其受限、规则的问题：固定长度整数 key、固定页面大小、单点地址翻译、硬件协助。普通索引则还要考虑变长 key、空间利用率、范围查询、缓存与磁盘局部性、并发更新等。
//
// 所以准确地说，这种方案并非没有取代树结构，而是以 Trie、Radix Tree、ART 等形式，在适合的场景中与 B-tree、跳表、哈希表并存。

// ART原理
class DynamicBloom {
 public:
  // allocator: pass allocator to bloom filter, hence trace the usage of memory
  // total_bits: fixed total bits for the bloom
  // num_probes: number of hash probes for a single key
  // hash_func:  customized hash function
  // huge_page_tlb_size:  if >0, try to allocate bloom bytes from huge page TLB
  //                      within this page size. Need to reserve huge pages for
  //                      it to be allocated, like:
  //                         sysctl -w vm.nr_hugepages=20
  //                     See linux doc Documentation/vm/hugetlbpage.txt
  explicit DynamicBloom(Allocator* allocator, uint32_t total_bits,
                        uint32_t num_probes = 6, size_t huge_page_tlb_size = 0,
                        Logger* logger = nullptr);

  ~DynamicBloom() {}

  // Assuming single thread adding to the DynamicBloom
  void Add(const Slice& key);

  // Like Add, but may be called concurrently with other functions. Does not
  // establish happens-before relationship with other functions so requires some
  // external mechanism to ensure other threads can see the change.
  void AddConcurrently(const Slice& key);

  // Assuming single threaded access to this function.
  void AddHash(uint32_t hash);

  // Like AddHash, but may be called concurrently with other functions. Does not
  // establish happens-before relationship with other functions so requires some
  // external mechanism to ensure other threads can see the change.
  void AddHashConcurrently(uint32_t hash);

  // Multithreaded access to this function is OK
  bool MayContain(const Slice& key) const;

  void MayContain(int num_keys, Slice* keys, bool* may_match) const;

  // Multithreaded access to this function is OK
  bool MayContainHash(uint32_t hash) const;

  void Prefetch(uint32_t h);

 private:
  // Length of the structure, in 64-bit words. For this structure, "word"
  // will always refer to 64-bit words.
  uint32_t kLen;
  // We make the k probes in pairs, two for each 64-bit read/write. Thus,
  // this stores k/2, the number of words to double-probe.
  const uint32_t kNumDoubleProbes;

  RelaxedAtomic<uint64_t>* data_;

  // or_func(ptr, mask) should effect *ptr |= mask with the appropriate
  // concurrency safety, working with bytes.
  template <typename OrFunc>
  void AddHash(uint32_t hash, const OrFunc& or_func);

  bool DoubleProbe(uint32_t h32, size_t a) const;
};

inline void DynamicBloom::Add(const Slice& key) { AddHash(BloomHash(key)); }

inline void DynamicBloom::AddConcurrently(const Slice& key) {
  AddHashConcurrently(BloomHash(key));
}

inline void DynamicBloom::AddHash(uint32_t hash) {
  AddHash(hash, [](RelaxedAtomic<uint64_t>* ptr, uint64_t mask) {
    ptr->StoreRelaxed(ptr->LoadRelaxed() | mask);
  });
}

inline void DynamicBloom::AddHashConcurrently(uint32_t hash) {
  AddHash(hash, [](RelaxedAtomic<uint64_t>* ptr, uint64_t mask) {
    // Happens-before between AddHash and MaybeContains is handled by
    // access to versions_->LastSequence(), so all we have to do here is
    // avoid races (so we don't give the compiler a license to mess up
    // our code) and not lose bits.  std::memory_order_relaxed is enough
    // for that.
    if ((mask & ptr->LoadRelaxed()) != mask) {
      ptr->FetchOrRelaxed(mask);
    }
  });
}

inline bool DynamicBloom::MayContain(const Slice& key) const {
  return (MayContainHash(BloomHash(key)));
}

inline void DynamicBloom::MayContain(int num_keys, Slice* keys,
                                     bool* may_match) const {
  std::array<uint32_t, MultiGetContext::MAX_BATCH_SIZE> hashes;
  std::array<size_t, MultiGetContext::MAX_BATCH_SIZE> byte_offsets;
  for (int i = 0; i < num_keys; ++i) {
    hashes[i] = BloomHash(keys[i]);
    size_t a = FastRange32(hashes[i], kLen);
    PREFETCH(data_ + a, 0, 3);
    byte_offsets[i] = a;
  }

  for (int i = 0; i < num_keys; i++) {
    may_match[i] = DoubleProbe(hashes[i], byte_offsets[i]);
  }
}

#if defined(_MSC_VER)
#pragma warning(push)
// local variable is initialized but not referenced
#pragma warning(disable : 4189)
#endif
inline void DynamicBloom::Prefetch(uint32_t h32) {
  size_t a = FastRange32(h32, kLen);
  PREFETCH(data_ + a, 0, 3);
}
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

// Speed hacks in this implementation:
// * Uses fastrange instead of %
// * Minimum logic to determine first (and all) probed memory addresses.
//   (Uses constant bit-xor offsets from the starting probe address.)
// * (Major) Two probes per 64-bit memory fetch/write.
//   Code simplification / optimization: only allow even number of probes.
// * Very fast and effective (murmur-like) hash expansion/re-mixing. (At
// least on recent CPUs, integer multiplication is very cheap. Each 64-bit
// remix provides five pairs of bit addresses within a uint64_t.)
//   Code simplification / optimization: only allow up to 10 probes, from a
//   single 64-bit remix.
//
// The FP rate penalty for this implementation, vs. standard Bloom filter, is
// roughly 1.12x on top of the 1.15x penalty for a 512-bit cache-local Bloom.
// This implementation does not explicitly use the cache line size, but is
// effectively cache-local (up to 16 probes) because of the bit-xor offsetting.
//
// NB: could easily be upgraded to support a 64-bit hash and
// total_bits > 2^32 (512MB). (The latter is a bad idea without the former,
// because of false positives.)

inline bool DynamicBloom::MayContainHash(uint32_t h32) const {
  size_t a = FastRange32(h32, kLen);
  PREFETCH(data_ + a, 0, 3);
  return DoubleProbe(h32, a);
}

inline bool DynamicBloom::DoubleProbe(uint32_t h32, size_t byte_offset) const {
  // Expand/remix with 64-bit golden ratio
  uint64_t h = 0x9e3779b97f4a7c13ULL * h32;
  for (unsigned i = 0;; ++i) {
    // Two bit probes per uint64_t probe
    uint64_t mask =
        ((uint64_t)1 << (h & 63)) | ((uint64_t)1 << ((h >> 6) & 63));
    uint64_t val = data_[byte_offset ^ i].LoadRelaxed();
    if (i + 1 >= kNumDoubleProbes) {
      return (val & mask) == mask;
    } else if ((val & mask) != mask) {
      return false;
    }
    h = (h >> 12) | (h << 52);
  }
}

template <typename OrFunc>
inline void DynamicBloom::AddHash(uint32_t h32, const OrFunc& or_func) {
  size_t a = FastRange32(h32, kLen);
  PREFETCH(data_ + a, 0, 3);
  // Expand/remix with 64-bit golden ratio
  uint64_t h = 0x9e3779b97f4a7c13ULL * h32;
  for (unsigned i = 0;; ++i) {
    // Two bit probes per uint64_t probe
    uint64_t mask =
        ((uint64_t)1 << (h & 63)) | ((uint64_t)1 << ((h >> 6) & 63));
    or_func(&data_[a ^ i], mask);
    if (i + 1 >= kNumDoubleProbes) {
      return;
    }
    h = (h >> 12) | (h << 52);
  }
}

}  // namespace ROCKSDB_NAMESPACE
