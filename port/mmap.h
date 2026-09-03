//  Copyright (c) Meta Platforms, Inc. and affiliates.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#pragma once

#ifdef OS_WIN
#include "port/win/port_win.h"
// ^^^ For proper/safe inclusion of windows.h. Must come first.
#include <memoryapi.h>
#else
#include <sys/mman.h>
#endif  // OS_WIN

#include <cstdint>
#include <utility>

#include "rocksdb/rocksdb_namespace.h"
#include "rocksdb/slice.h"

namespace ROCKSDB_NAMESPACE {

// An RAII wrapper for mmaped memory
// 1. mmap原理：
//  1） mmap如果调用时有传addr，并且flag也有MAP_FIXED，那么addr必须按照normal page/huge page对齐，如果flag没有MAP_FIXED，
//     那么addr只是一个参考，当没对齐时，内核会自行分配一个符合对齐的新addr。
//  2） MAP_FIXED的使用场景一般是为了能将某一批数据的虚拟地址addr紧凑的布局在一个huge page中，但是如果直接使用的话容易把已存在的
//      VMA进行覆盖。所以一般是先通过非指定addr的匿名huge page mmap获取一块大内存，然后再在这块区域内按需执行指定addr MAP_FIXED mmap。
//  3） mmap不要求len对齐，len可以是任意size，但是mmap内部会对len进行向上的normal page/huge page对齐。
// 2. munmap原理：
//  1） munmap内部首先会要求addr按照normal page对齐，然后将len按照normal page向上对齐，然后得到一个[start, end]的unmap区间，
//      随后就会查找Maple Tree获得所有被unmap区间覆盖的VMA，对于中间完全被覆盖的VMA，直接加入pending unmap，而对于首尾两个不完全覆盖
//      的VMA，会执行split VMA操作，在split操作中，再对unmap区间start/end边界按照首/尾VMA的normal page/huge page类型进行对齐检测，
//      检测失败就直接抛弃整个munmap操作。
//  2） 如果最后一个VMA的split操作失败，那第一个VMA的split操作并不会被回滚，因为这并不影响任何内存管理，只是原来的一个VMA变成了两个语义相同
//      的相邻VMA，后续这两个可能又会被merge。
//  3） 因此huge page有个不对称点是，mmap的时候如果len没有对齐huge page，内核会自动对齐到huge page，但是对同一个addr/len进行munmap的时
//      候，只会将len对齐到normal page，然后split VMA发现不对齐huge page边界，会触发munmap失败。因此huge page mmap必须由调用者保证
//      len是huge page对齐的。
class MemMapping {
 public:
  static constexpr bool kHugePageSupported =
#if defined(MAP_HUGETLB) || defined(FILE_MAP_LARGE_PAGES)
      true;
#else
      false;
#endif

  // Allocate memory requesting to be backed by huge pages
  static MemMapping AllocateHuge(size_t length);

  // Allocate memory that is only lazily mapped to resident memory and
  // guaranteed to be zero-initialized. Note that some platforms like
  // Linux allow memory over-commit, where only the used portion of memory
  // matters, while other platforms require enough swap space (page file) to
  // back the full mapping.
  static MemMapping AllocateLazyZeroed(size_t length);

  // No copies
  MemMapping(const MemMapping&) = delete;
  MemMapping& operator=(const MemMapping&) = delete;
  // Move
  MemMapping(MemMapping&&) noexcept;
  MemMapping& operator=(MemMapping&&) noexcept;

  // Releases the mapping
  ~MemMapping();

  inline void* Get() const { return addr_; }
  // The requested length of the mapping, which may be smaller than the
  // actual usable length
  inline size_t Length() const { return length_; }

  // Return the mapping as a Slice (zero-copy view of the mapped memory)
  inline Slice AsSlice() const {
    return Slice(static_cast<const char*>(addr_), length_);
  }

 private:
  MemMapping() {}

  // The mapped memory, or nullptr on failure / not supported
  void* addr_ = nullptr;
  // The known usable number of bytes starting at that address
  size_t length_ = 0;

#ifdef OS_WIN
  HANDLE page_file_handle_ = NULL;
#endif  // OS_WIN

  static MemMapping AllocateAnonymous(size_t length, bool huge);
};

// Simple MemMapping wrapper that presents the memory as an array of T.
// For example,
//  TypedMemMapping<uint64_t> arr = MemMapping::AllocateLazyZeroed(num_bytes);
template <typename T>
class TypedMemMapping : public MemMapping {
 public:
  /*implicit*/ TypedMemMapping(MemMapping&& v) noexcept
      : MemMapping(std::move(v)) {}
  TypedMemMapping& operator=(MemMapping&& v) noexcept {
    MemMapping& base = *this;
    base = std::move(v);
  }

  inline T* Get() const { return static_cast<T*>(MemMapping::Get()); }
  inline size_t Count() const { return MemMapping::Length() / sizeof(T); }

  inline T& operator[](size_t index) const { return Get()[index]; }
};

}  // namespace ROCKSDB_NAMESPACE
