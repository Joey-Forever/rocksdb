//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

// Arena is an implementation of Allocator class. For a request of small size,
// it allocates a block with pre-defined block size. For a request of big
// size, it uses malloc to directly get the requested size.

#pragma once

#include <cstddef>
#include <deque>

#include "memory/allocator.h"
#include "port/mmap.h"
#include "rocksdb/env.h"

namespace ROCKSDB_NAMESPACE {

// 操作系统虚拟内存页表：
//  1. 页表本质上是一个Radix Tree，用于将虚拟地址空间内存页映射到物理内存页，CPU进行虚拟地址转换时需要直接读取。
//  2. 使用Radix Tree好处：
//    1）结构简单，不需要像其他数据结构一样为了性能需要进行复杂的结构调整，适合硬件进行优化。
//    2）搜索算法简单高效，虚拟地址本身可以拆为固定数目分量负责页表的各级索引，因此搜索路径固定且非常短，
//       而且每级索引可以直接使用对应分量值进行数组下标索引，不需要key compare。
//  3. 使用Radix Tree唯一缺点就是页表节点固定大小会导致内存浪费。优化方式：
//    1）页表只对真正需要分配物理内存的虚拟内存页建立下级页表节点。
//    2）进程虚拟地址的申请和分配基本都有一定连续性，不会完全随机，连续的虚拟页可以共享高层页表节点和末级节点，因此
//       应用场景的work load本身决定了页表各级节点的内存浪费不会过于严重。
//    3）TLB可以对"虚拟页号->物理页号"的地址转换结果进行缓存，避免每次转换都搜索多级页表。
//    4）2MB和1GB的huge page大页申请机制，让用户态能够申请大块连续虚拟地址空间，让多级页表搜索不需要总到
//       末级节点也可确定物理页，避免频繁零散申请4KB page导致的多级页表节点内存浪费，更重要的是可以减少相同
//       大小虚拟内存区域所需要的TLB entry数目，大幅提高TLB cache hit的概率。
//  4. VMA（Virtual Memory Area）是用于描述一段已分配虚拟地址空间的具体语义的结构，例如读写权限、内存映射关系、大页机制等。
//     是操作系统对已分配虚拟内存空间进行管理的核心结构。他通过page fault缺页异常和页表进行联系起来。
//     1）运行时上层申请虚拟地址空间主要有brk和mmap两种方式（主线程自动扩栈例外），brk是只能对堆末端VMA进行边界扩展/缩小，拓展的部分的语义和原堆VMA一致。
//        而mmap则更灵活，可以自由创建一个具有独立语义的VMA，也支持对某个VMA区域进行独立munmap删除解物理映射，比如线程库会为每个线程
//        单独mmap创建线程栈、Arena通过mmap创建huge page映射等。
//     2）上层申请一块虚拟内存返回成功后，该虚拟内存的区间范围和具体语义会记录在某个VMA中，但是不会立刻分配物理内存。
//     3）当CPU想要访问某个虚拟地址时，首先去访问TLB或者页表，如果发现页表中不存在该“虚拟页->物理页”的映射关系的时候，会触发page fault缺页异常。
//     4）随后内核去找该虚拟地址是否存在VMA，如果不存在则抛出SIGSEGV。如果存在，则根据VMA的具体语义，分配空闲物理页或到page cache中获取
// 　　   共享物理页后再到页表中建立“虚拟页->物理页”映射关系。
//     5）后续CPU再访问相关虚拟地址时就可以从TLB/页表中获得虚拟页到物理页的映射了。
//     6）主线程栈也是一个VMA，他和pthread线程栈mmap VMA都是在线程创建的时候就分配到一块VMA区域，cpu寄存器记录线程的栈指针，在进入一个新函数时
//        只需要移动栈指针预留出栈帧空间，栈局部变量的内存分配也不需要动态内存分配的用户态内存分配器管理以及可能的brk和mmap系统调用，局部变量只有在
//        读写时根据栈帧内固定偏移进行栈内存的访问。不同的是在实际访问栈内存虚拟地址时，对于pthread线程来说，VMA不可自动扩展，超出VMA时直接爆栈；主线程的
//        VMA可以自动扩展，当内核处理cpu page fault时如果发现地址越界没有落在任何一个VMA内，但是处于主线程VMA地址下方且满足扩栈条件时，就会触发主线程VMA的扩展。
//     7）所有的VMA中，只有主线程栈VMA具有越界访问时自动扩展的能力，普通mmap和传统栈都不行，传统栈只能通过brk主动扩展。
//  5. 现代内存分配器常见的“purge/release”机制，会利用虚拟内存机制对虚拟内存和物理内存进行分开释放管理：
//     1）对某个mmap出来的大块虚拟内存区域，如果整块区域都空闲了，可以直接munmap同时删除VMA区域，删除页表映射和释放物理内存页，
//        但是后续上层需要再次分配虚拟内存时可能需要重新mmap系统调用建立VMA。
//     2）更多时候是，该大块区域只有某些虚拟页空闲了，其他页还在使用，这时候可以使用madvise只是将free page的页表映射和物理页
//        释放，但是仍然保留该free page的VMA以及存在于内存分配器的free list中，后续再次分配虚拟内存给上层使用时，不需要重新
//        mmap，可以直接将free page分配出去，上层使用时再触发page fault重新分配清零物理页并建立页表映射。madvice会为此提供两种
//        策略：MADV_FREE，只是告诉内核该虚拟内存区域的物理内存页在内存紧张时可以回收但是允许短时间内保留，适合短时间就能复用的场景，
//        减少缺页成本；MADV_DONTNEED，告诉内核马上将物理内存页释放并取消页表映射，适合长时间空闲页面和需要积极降低物理内存（RSS）的场景。
//     3）但是如果某个虚拟页中有大量空闲区域，只有少量正在使用的区域，内存分配器不会为了整理碎片而移动存活对象，因为这会改变对象的地址，而上层
//        访问时是直接是使用保存的虚拟地址的，这会导致垂悬指针问题。内存分配器只能合并相邻空闲块为大空闲块。
//  6. 进程VMA索引结构不使用Radix Tree：
//     VMA本身作为进程虚拟地址空间分配区域的语义管理结构，他在虚拟地址空间中也具有聚簇性和簇内连续性，但是他并不是多区间稀疏数组，因为
//     每个VMA的key并不是单一的页偏移值，而是一个区间，比如多个VMA连续分布，VMA_1[100, 1000), VMA_2[1001, 2000)，如果将这两个
//     VMA区间的左右边界插入朴素Radix Tree中，那Radix Tree视角下的key在末级节点中就会表现为极度稀疏分布，空间极大浪费。因此，VMA索引
//     结构还是需要使用基于key compare的数据结构了，例如平衡树（查找性能稳定和子树不可逆聚合值增强，btree（TLB/CPU cache line缓存局部
//     性和矮树高）、rbtree（稳定迭代器和intrusive无额外节点分配））、skiplist（高并发写和高效hint）。
//  7. 进程虚拟地址空间基础布局：
//     low_addr                                                                                                          high_addr
//       |<---------------------------------------user space---------------------------------------------->|<----kernel space---->|
//        指令区       静  态  存  储  区       堆区                内  存  映  射  区                主线程栈
//     --|------|----------|-------|------|========>|----------------------------------------|<============|-----------------------
//        .text   .rodata    .data   .bss    heap                     mmap                      main stack
//     1） 虚拟地址空间的所有虚拟地址区间都通过VMA在Maple Tree上进行管理。
//     2） 每个进程的虚拟地址空间都分为低地址的user space和高地址的kernel space，其中kernel space是所有进程共享的。
//     3） .text、.rodata、.data、.bss这几个段是可执行程序装载之后被VMA映射到进程虚拟地址空间的，.text映射的是代码指令，.rodata映射的是只读的initiaized静态变量
//         和无法被直接编译进指令的字面量，.data映射的是可写的非0 initialized静态变量，.bss映射的是可写的uninitialized静态变量和可写的0 initailized静态变量。
//         还有一种常见情况时，即使静态变量有初始化值，但是该初始化值必须在运行时才能计算出来，那该值也会存在.bss等到运行时的初始化代码中才初始化。而使用constexpr修饰的静态变量
//         必须能够在编译期确定const值。constexpr修饰的函数可以嵌套非constexpr函数，而如果一个变量的初始化值调用到该constexpr函数而且所有调用路径都是constexpr函数，
//         就可以在编译期直接生成该变量初始化值，否则的话对于constexpr变量直接编译错误，对于非constexpr变量则等到运行时再计算。
//     4） .text、.rodata直接PROT_READ只读映射page cache中的可执行文件对应页，而.data则通过PROT_WRITE｜PROT_READ｜MAP_PRIVATE读写私有映射page cache中
//         可执行文件对应页，当需要写.data中的内容时，会触发COW重新映射到进程私有的anonymous page，修改不会被同步回可执行文件。
//     5） 和前几个段不同的是，.bss段的实际对象内容不会存储到可执行文件中（因为全0），可执行文件只保留基础的.bss描述信息用于程序装载时进行映射。因此在访问.bss段地址处
//         理page fault时，只需要映射共享只读0页或者直接映射私有的清零anonymous page，不再需要可执行文件在page cache中的内容。
//  8. 进程页表格局：
//         进程 A 的页表根                                进程 B 的页表根
//      低地址表项 → A 的user_space页表              低地址表项 → B 的user_space页表
//      高地址表项 ──────────┐         ┌─────────── 高地址表项
//                         ↓         ↓
//                   共享的kernel_space下级页表 ==> 非KPTI情况下，这个始终包含内核空间的完整映射，
//                              ↓                 KPTI情况下，用户态时这里只映射少量内核入口代码
//                          内核物理页面
//      用户态 -> 内核态的切换逻辑：
//      1） CPU在进程用户态执行过程中碰到"syscall/中断/异常"就会触发陷入内核态。
//      2） CPU硬件层面完成内核态权限转换、执行指令地址转到内核入口代码等一整套受控操作。
//      3） CPU已经处于内核态，开始执行内核入口代码，此时使用的页表和用户态时仍为同一个。
//      4） KPTI情况下，执行内核入口代码时会将页表切换到具有完整内核映射的页表。
//      5） 执行内核具体处理逻辑，此时使用的是当前已经具有完整内核映射的页表。
//  9. 进程页表KPTI优化：
//    1）必要性：传统的进程页表布局方式中，用户空间和内核空间的完整映射都在同一个页表中，共享内核映射的TLB entry也会开启global优化以
//             实现多进程共享。但是这有个致命问题是，用户态和内核内存资源映射之间只有一层脆弱的权限保护，会被Meltdown漏洞突破。
//    2）实现手段：用户态使用的用户页表只有少量的内核入口代码映射，真正进入内核态切换到内核页表后，才有完整的内核空间映射。禁用共享内核
//               映射的global TLB entry优化（针对没有同时映射到两套页表的内核页面），避免用户态切回用户页表后仍能通过global TLB
//               entry获取内核虚拟页映射。通过这两个手段实现了用户态和内核映射之间真正的物理隔离。
//    3）PCID机制：KPTI的开启时，每次切换用户态内核态都要切换页表，即使前后两个页表的下级页表可能相同，“虚拟内存->物理内存，权限”的
//               映射关系也相同，但是在CPU看来该进程切换了页表后会使旧页表的TLB cache全部失效。所以需要使用PCID机制为两个页表分别
//               保留CPU TLB cache，切到内核页表时，用户页表TLB仍然保留不主动失效，降低切回用户态后的TLB miss。
//    4）代价：KPTI+PCID机制下，内核态访问用户空间虚拟页时会导致TLB中存在两条重复（不同PCID）的TLB entry；KPTI会禁用共享内核映射的
//           global TLB entry优化，这就导致了不同进程需要有独立的内核虚拟页映射TLB entry。两个因素叠加之下，加剧了TLB cache容量竞争。

// JOEY_TODO: 实现ART
// 朴素Radix Tree（！！和基于key comapre的索引结构比，不需要单独存key了！！）：
// 当work load满足以下条件时，通过朴素Radix Tree即可获得一个内存开销合理、结构固定、读写简单稳定高效、且不需要为了性能而做复杂的结构调整操作的有序索引结构。
//   1） key类型是固定长度且较短的整数（如uint32、uint64），保证搜索路径稳定且足够短。
//   2） key分布整体上是一个多区间稀疏数组，且每一区间数组足够大且内部key连续（除了最低位分量外其他高位分量全部为公共前缀），保证tree的上层节点扇出足够小，使得末层节点数目足够少且空间利用率足够高。
//   3） 只要求内存开销合理不浪费严重，不要求进一步极限压缩节省空间。
//   4） 典型如虚拟内存页表、page cache XArray。
// ART（Adaptive Radix Tree）：
// 朴素Radix Tree依赖work load是多区间稀疏数组（大量key拥有高度公共前缀），以降低上层节点扇出率以及下层节点数量，从而来摊薄固定size大节点的内存开销，避免下层节点空间占用率过低。而ART通过以下方式避免了对work load的依赖。
//   1） 引入多类型节点，对于低利用率的下层节点使用小size节点，直接避免固定size节点的空slot空间浪费。（高层节点的高扇出不会导致高层空间浪费，他只是导致下层节点过多，间接引发下层空间浪费）。
//   2） 叶子压缩（Lazy Expansion），当某个前缀下只有一个键时，直接将这个唯一键后缀压缩到同一个节点中，避免扩展唯一键后缀导致大量空间浪费（即使使用最小size节点也会浪费）。
//   3） 内部单slot占用节点路径压缩（Path Compression），如果某些key在结构上存在公共前缀，那可能会产生许多只有一个占用slot的内部节点，这些内部节点唯一占用slot的内容可以直接下压进唯一child节点的节点级prefix字段中
//      （prefix字段是所有类型Node都有，包括Node256），从而进一步压缩整体内存开销。而在随机work load下，内部节点的扇出率本身比较高，这种单slot占用节点出现的概率就非常小了，路径压缩的优化空间就很小了。
//   4） 前两个方法已经能够基本解决朴素Radix Tree在随机work load下的空间浪费，第三个方法更多是针对公共前缀较多的key分布做的进一步空间极限压缩。


// Arena是一个针对memtable场景的一个支持std::max_align_t默认对齐（不处理over-aligned）的内存分配器，他的特点是生命周期和
// memtable绑定，而且memtable是一个对元素只add不delete的数据结构，Arena分配给memtable的所有内存只会随着memtable的销毁整体释放，
// 也就意味着Arena不需要处理小块的回收和复用问题，因此：
//   1） 分配小块只需要在active block下一个分配位置直接取align后的目标addr，不需要为零散free的小块维护多个固定size的free list便于复用。
//   2） 返回给上层申请者的小块不需要在返回地址前维护所属小块在free list中的所属size、list ptr、各种管理标志位等信息。
//   3） PS：glibc的malloc分配器支持std::max_align_t对齐，但是由于他不需要处理任意的对齐，所以不需要在每个小块header处预留
//          ”align - 1 + raw_ptr size + raw size”的超额空间，假设std::max_align_t是16，他保证每个小块首地址和size都
//          对齐到16，然后小块分配给上层时返回的是“首地址+16”的addr，然后小块header的16字节用于记录小块的raw size和一些flag bits。
//          free时通过“上层ptr-16”就找到了块头。相当于通过固定的16对齐避免了额外的“align - 1 + raw_ptr_size”的超额空间。
class Arena : public Allocator {
 public:
  // No copying allowed
  Arena(const Arena&) = delete;
  void operator=(const Arena&) = delete;

  static constexpr size_t kInlineSize = 2048;
  static constexpr size_t kMinBlockSize = 4096;
  static constexpr size_t kMaxBlockSize = 2u << 30;

  // ！！！
  // AllocateAligned是固定按照kAlignUnit来对齐每个内存分配区域首地址的，不会感知上层需要的类型精确Alignment，
  // 所以如果上层能够确定自己需要的对齐小于这个数的话，可以直接AllocateAligned就可以使用，否则的话就会发生over-aligned，
  // 必须要上层自己在Allocate超额空间（至少“align - 1 + raw_ptr size”）后手动对齐。（像语言和编译器层面的直接对类型本
  // 身进行栈堆分配这种能够明确感知类型精确Alignment的才可以内置就解决了这种over-aligned）
  static constexpr unsigned kAlignUnit = alignof(std::max_align_t);
  // 对齐值必须是2的幂次方，方便位运算
  static_assert((kAlignUnit & (kAlignUnit - 1)) == 0,
                "Pointer size should be power of 2");

  // huge_page_size: if 0, don't use huge page TLB. If > 0 (should set to the
  // supported hugepage size of the system), block allocation will try huge
  // page TLB first. If allocation fails, will fall back to normal case.
  explicit Arena(size_t block_size = kMinBlockSize,
                 AllocTracker* tracker = nullptr, size_t huge_page_size = 0);
  ~Arena();

  char* Allocate(size_t bytes) override;

  // huge_page_size: if >0, will try to allocate from huage page TLB.
  // The argument will be the size of the page size for huge page TLB. Bytes
  // will be rounded up to multiple of the page size to allocate through mmap
  // anonymous option with huge page on. The extra  space allocated will be
  // wasted. If allocation fails, will fall back to normal case. To enable it,
  // need to reserve huge pages for it to be allocated, like:
  //     sysctl -w vm.nr_hugepages=20
  // See linux doc Documentation/vm/hugetlbpage.txt for details.
  // huge page allocation can fail. In this case it will fail back to
  // normal cases. The messages will be logged to logger. So when calling with
  // huge_page_tlb_size > 0, we highly recommend a logger is passed in.
  // Otherwise, the error message will be printed out to stderr directly.
  char* AllocateAligned(size_t bytes, size_t huge_page_size = 0,
                        Logger* logger = nullptr) override;

  // Returns an estimate of the total memory usage of data allocated
  // by the arena (exclude the space allocated but not yet used for future
  // allocations).
  size_t ApproximateMemoryUsage() const {
    return blocks_memory_ + blocks_.size() * sizeof(char*) -
           alloc_bytes_remaining_;
  }

  size_t MemoryAllocatedBytes() const { return blocks_memory_; }

  size_t AllocatedAndUnused() const { return alloc_bytes_remaining_; }

  // If an allocation is too big, we'll allocate an irregular block with the
  // same size of that allocation.
  size_t IrregularBlockNum() const { return irregular_block_num; }

  size_t BlockSize() const override { return kBlockSize; }

  bool IsInInlineBlock() const {
    return blocks_.empty() && huge_blocks_.empty();
  }

  // check and adjust the block_size so that the return value is
  //  1. in the range of [kMinBlockSize, kMaxBlockSize].
  //  2. the multiple of align unit.
  static size_t OptimizeBlockSize(size_t block_size);

 private:
  // Arena在自身实例中内嵌一个对齐的2kb初始block用于小Arena的使用。
  // 只有在这个内嵌block用完后才会去堆上申请新block，避免小Arena频繁到堆上申请内存。
  // 需要满足按照std::max_align_t对齐
  alignas(std::max_align_t) char inline_block_[kInlineSize];
  // Number of bytes allocated in one block
  const size_t kBlockSize;
  // Allocated memory blocks
  // normal block通过new关键字（本质是malloc）分配，本身就保证了首地址至少按照std::max_align_t对齐了。
  std::deque<std::unique_ptr<char[]>> blocks_;
  // Huge page allocations
  // huge block通过mmap系统调用直接映射分配，保证首地址是按照系统默认huge page size对齐了，也就是自然
  // 按照std::max_align_t对齐了。
  std::deque<MemMapping> huge_blocks_;
  size_t irregular_block_num = 0;

  // Stats for current active block.
  // For each block, we allocate aligned memory chucks from one end and
  // allocate unaligned memory chucks from the other end. Otherwise the
  // memory waste for alignment will be higher if we allocate both types of
  // memory from one direction.
  // 将AllocateAligned和不要求对齐的Allocate分别从active block的两个方向开始分配。
  // 避免混合分配时导致AllocateAligned造成过多内存浪费。缺点是，如果某一边只有很少的
  // 分配请求，可能额外提前占用4kb/huge page的物理内存。
  char* unaligned_alloc_ptr_ = nullptr;
  char* aligned_alloc_ptr_ = nullptr;
  // How many bytes left in currently active block?
  size_t alloc_bytes_remaining_ = 0;

  // 当active block没有足够空间分配的时候，AllocateFallback会优先考虑向系统mmap申请
  // huge page粒度的新active block。hugetlb_size_就是每次申请该类型active block时的size。
  // 因此该值需要满足两个条件：
  //  1） 必须大于等于kBlockSize
  //  2） 必须是系统默认huge page size的整数倍（避免munmap失败）
  size_t hugetlb_size_ = 0;

  char* AllocateFromHugePage(size_t bytes);
  char* AllocateFallback(size_t bytes, bool aligned);
  char* AllocateNewBlock(size_t block_bytes);

  // Bytes of memory in blocks allocated so far
  size_t blocks_memory_ = 0;
  // Non-owned
  AllocTracker* tracker_;
};

// 分配一块没有任何对齐要求的bytes大小内存区域。
// 从active block的高地址到低地址分配。
inline char* Arena::Allocate(size_t bytes) {
  // The semantics of what to return are a bit messy if we allow
  // 0-byte allocations, so we disallow them here (we don't need
  // them for our internal use).
  assert(bytes > 0);
  // 1. active block中有剩余空间，直接分配
  if (bytes <= alloc_bytes_remaining_) {
    unaligned_alloc_ptr_ -= bytes;
    alloc_bytes_remaining_ -= bytes;
    return unaligned_alloc_ptr_;
  }
  // 2. active block空间不足，需要申请新active block再获取分配空间
  return AllocateFallback(bytes, false /* unaligned */);
}

// Like std::destroy_at but a callable type
template <typename T>
struct Destroyer {
  void operator()(T* ptr) { ptr->~T(); }
};

// Like std::unique_ptr but only placement-deletes the object (for
// objects allocated on an arena).
template <typename T>
using ScopedArenaPtr = std::unique_ptr<T, Destroyer<T>>;

}  // namespace ROCKSDB_NAMESPACE
