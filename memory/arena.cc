//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "memory/arena.h"

#include <algorithm>

#include "logging/logging.h"
#include "port/malloc.h"
#include "port/port.h"
#include "rocksdb/env.h"
#include "test_util/sync_point.h"
#include "util/string_util.h"

namespace ROCKSDB_NAMESPACE {

size_t Arena::OptimizeBlockSize(size_t block_size) {
  // Make sure block_size is in optimal range
  block_size = std::max(Arena::kMinBlockSize, block_size);
  block_size = std::min(Arena::kMaxBlockSize, block_size);

  // make sure block_size is the multiple of kAlignUnit
  // 维持block_size是kAlignUnit的倍数，方便内部分配的时候进行对齐
  if (block_size % kAlignUnit != 0) {
    block_size = (1 + block_size / kAlignUnit) * kAlignUnit;
  }

  return block_size;
}

 // ！！！
// 上层调用方务必保证传递的huge page size是系统默认的huge page size，
// 且已经向系统预留了足够的huge page物理内存
Arena::Arena(size_t block_size, AllocTracker* tracker, size_t huge_page_size)
    : kBlockSize(OptimizeBlockSize(block_size)), tracker_(tracker) {
  assert(kBlockSize >= kMinBlockSize && kBlockSize <= kMaxBlockSize &&
         kBlockSize % kAlignUnit == 0);
  TEST_SYNC_POINT_CALLBACK("Arena::Arena:0", const_cast<size_t*>(&kBlockSize));
  alloc_bytes_remaining_ = sizeof(inline_block_);
  blocks_memory_ += alloc_bytes_remaining_;
  aligned_alloc_ptr_ = inline_block_;
  unaligned_alloc_ptr_ = inline_block_ + alloc_bytes_remaining_;
  if (MemMapping::kHugePageSupported) {
    hugetlb_size_ = huge_page_size;
    // ！！！
    // 使hugetlb_size_ >= kBlockSize且为系统默认huge page size的整数倍。
    if (hugetlb_size_ && kBlockSize > hugetlb_size_) {
      hugetlb_size_ = ((kBlockSize - 1U) / hugetlb_size_ + 1U) * hugetlb_size_;
    }
  }
  if (tracker_ != nullptr) {
    tracker_->Allocate(kInlineSize);
  }
}

Arena::~Arena() {
  if (tracker_ != nullptr) {
    assert(tracker_->is_freed());
    tracker_->FreeMem();
  }
}

// active block空间不足时，Arena调用该方法申请新的active block，
// 然后马上从新active block分配目标size空间给上层。
// ！！！
// 分配的新active block不可以进行任何形式的整块初始化/触页，因为这可能导致：
//   1. 触发整个block所有page的“缺页异常->进入内核态分配物理页->建立页表物理映射->初始化CPU开销”，
//      将所有开销都积压到了上层当前这次内存申请上，造成延迟尖峰。
//   2. block其他page还没被使用就先占用了物理内存，提前增加RSS。
char* Arena::AllocateFallback(size_t bytes, bool aligned) {
  // 上层申请的空间过大，直接为其单独分配一块irregular block
  if (bytes > kBlockSize / 4) {
    ++irregular_block_num;
    // Object is more than a quarter of our block size.  Allocate it separately
    // to avoid wasting too much space in leftover bytes.
    // new关键字本身保证了分配的内存首地址是至少std::max_align_t对齐的，所以不需要检查aligned了。
    return AllocateNewBlock(bytes);
  }

  // We waste the remaining space in the current block.
  size_t size = 0;
  char* block_head = nullptr;
  // 1. 在允许huge page分配的情况下，优先使用以优化TLB。memtable属于高频插入场景，
  //    首次触页分配的huge page物理内存会很快被充分利用。
  if (MemMapping::kHugePageSupported && hugetlb_size_ > 0) {
    size = hugetlb_size_;
    block_head = AllocateFromHugePage(size);
  }
  // 2. huge page分配不允许/失败情况下，fallback回normal block分配。
  if (!block_head) {
    size = kBlockSize;
    block_head = AllocateNewBlock(size);
  }
  // 3. 到这里的时候，block_head已经保证是std::max_align_t对齐了的。

  // 4. 处理当前上层内存请求：从新分配的active block中分配空间。
  alloc_bytes_remaining_ = size - bytes;

  if (aligned) {
    aligned_alloc_ptr_ = block_head + bytes;
    unaligned_alloc_ptr_ = block_head + size;
    return block_head;
  } else {
    aligned_alloc_ptr_ = block_head;
    unaligned_alloc_ptr_ = block_head + size - bytes;
    return unaligned_alloc_ptr_;
  }
}

// 使用mmap系统调用直接向系统申请huge active block。
// ！！！
// 调用者必须保证bytes参数是系统默认huge page size的整数倍，避免munmap失败。
char* Arena::AllocateFromHugePage(size_t bytes) {
  // mmap系统调用保证了返回地址本身已经按照huge page对齐了，也就必然对齐std::max_align_t了。
  MemMapping mm = MemMapping::AllocateHuge(bytes);
  auto addr = static_cast<char*>(mm.Get());
  if (addr) {
    huge_blocks_.push_back(std::move(mm));
    // 在上层已经对齐bytes到系统默认huge page size的情况下，系统实际分配的虚拟内存大小就是bytes值。
    blocks_memory_ += bytes;
    if (tracker_ != nullptr) {
      tracker_->Allocate(bytes);
    }
  }
  return addr;
}

// 1. 按照固定kAlignUnit对齐分配bytes大小的内存区域，由于只是针对bytes大小进行
//    内存分配，不会感知上层实际需要构造的类型，所以不会处理上层类型导致的over-aligned问题。
//    从active block的低地址到高地址分配。
// 2. huge_page_size非0且支持huge page情况下，会单独向系统申请huge page粒度的空间。主要用于
//    “较大区域的频繁随机访问”的场景，例如Bloom filter位数组、hash桶数组。“较大区域”意味着需要脱离
//    active block独立分配，“频繁随机访问”意味着normal page的大量TLB entry会加剧TLB miss，
//    所以这类场景下需要独立huge page分配区域，减少TLB entry数目以缓解TLB miss。
char* Arena::AllocateAligned(size_t bytes, size_t huge_page_size,
                             Logger* logger) {
  // 1. 针对“较大区域的频繁随机访问”独立分配huge page粒度区域（自动对齐了std::max_align_t）
  if (MemMapping::kHugePageSupported && hugetlb_size_ > 0 &&
      // 上层同样需要保证传入的huge_page_size等于系统默认huge page size
      huge_page_size > 0 && bytes > 0) {
    // Allocate from a huge page TLB table.
    // 将申请空间bytes对齐到huge page size的整数倍，防止munmap失败
    size_t reserved_size =
        ((bytes - 1U) / huge_page_size + 1U) * huge_page_size;
    assert(reserved_size >= bytes);

    char* addr = AllocateFromHugePage(reserved_size);
    if (addr == nullptr) {
      ROCKS_LOG_WARN(logger,
                     "AllocateAligned fail to allocate huge TLB pages: %s",
                     errnoStr(errno).c_str());
      // fail back to malloc
    } else {
      return addr;
    }
  }

  // 2. 常规模式下直接从active block中顺序获取首地址对齐std::max_align_t的区域
  size_t current_mod =
      reinterpret_cast<uintptr_t>(aligned_alloc_ptr_) & (kAlignUnit - 1);
  size_t slop = (current_mod == 0 ? 0 : kAlignUnit - current_mod);
  // 实际分配的空间还要加上对齐std::max_align_t后的额外空间
  size_t needed = bytes + slop;
  char* result;
  if (needed <= alloc_bytes_remaining_) {
    result = aligned_alloc_ptr_ + slop;
    aligned_alloc_ptr_ += needed;
    alloc_bytes_remaining_ -= needed;
  } else {
    // AllocateFallback always returns aligned memory
    result = AllocateFallback(bytes, true /* aligned */);
  }
  assert((reinterpret_cast<uintptr_t>(result) & (kAlignUnit - 1)) == 0);
  return result;
}

// 使用new关键字（malloc）内存分配器申请新的normal active block,
// 相当于在上层与brk/mmap系统调用之间隔了一层ptmalloc用户态内存分配器，减少系统调用的开销。
char* Arena::AllocateNewBlock(size_t block_bytes) {
  // NOTE: std::make_unique zero-initializes the block so is not appropriate
  // here
  // new关键字本身通过malloc分配器保证了首地址至少按照std::max_align_t对齐了
  char* block = new char[block_bytes];
  // 由于std::make_unique会对分配区域进行全0初始化，导致提前整block触页，所以不可以使用。
  blocks_.push_back(std::unique_ptr<char[]>(block));

  // allocated_size只用于Arena占用虚拟内存记账统计。
  size_t allocated_size;
#ifdef ROCKSDB_MALLOC_USABLE_SIZE
  // malloc内存分配器分配给上层的小块中除了上层要求的block_bytes之外，
  // 还包括固定分块size开销，通过malloc_usable_size
  // 能够更精确的获取本次申请实际占用的allocated_size。
  allocated_size = malloc_usable_size(block);
#ifndef NDEBUG
  // It's hard to predict what malloc_usable_size() returns.
  // A callback can allow users to change the costed size.
  std::pair<size_t*, size_t*> pair(&allocated_size, &block_bytes);
  TEST_SYNC_POINT_CALLBACK("Arena::AllocateNewBlock:0", &pair);
#endif  // NDEBUG
#else
  allocated_size = block_bytes;
#endif  // ROCKSDB_MALLOC_USABLE_SIZE
  blocks_memory_ += allocated_size;
  if (tracker_ != nullptr) {
    tracker_->Allocate(allocated_size);
  }
  return block;
}

}  // namespace ROCKSDB_NAMESPACE
