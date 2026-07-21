//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#include "db/flush_scheduler.h"

#include <cassert>

#include "db/column_family.h"

namespace ROCKSDB_NAMESPACE {

// 将传入的cfd使用头插法插入flush scheduler的pending flush list中，也就是生产者
void FlushScheduler::ScheduleWork(ColumnFamilyData* cfd) {
#ifndef NDEBUG
  {
    std::lock_guard<std::mutex> lock(checking_mutex_);
    // debug模式下使用这个东西检测同一个cfd是否重复进pending flush list
    assert(checking_set_.count(cfd) == 0);
    checking_set_.insert(cfd);
  }
#endif  // NDEBUG
  // ！！！
  // 由于pending flush list需要hold住cfd，保证他在被消费之前，数据有效，所以需要对cfd进行ref防止被销毁
  cfd->Ref();
// Suppress false positive clang analyzer warnings.
// 这个宏是在静态分析器的时候才会定义，gcc构建的时候都不会定义的，所以不影响构建运行，只是为了让静态分析器看不到这段代码
#ifndef __clang_analyzer__
  // ！！！
  // 使用头插法将当前cfd插入pending flush list中，避免尾插法的特判定head、额外维护tail、生产者修改已有Node。
  // 最关键是可以直接通过while cas循环重试，每次重试时都可以直接继续cas判定由上一次失败cas自动指向了的新head。
  // 这为并发插入带来一个巨大优势：插入过程不需要访问其他线程创建插入的Node，只需要访问head_这一个atomic变量
  //                          以及自己私有创建的node，避免了复杂的并发处理。并且由于head_ cas无论成功还是失败，
  //                          都不涉及其他需要happens-before关系内容的发布/读取，所以只需要relaxed即可。
  Node* node = new Node{cfd, head_.load(std::memory_order_relaxed)};
  // compare_exchange_weak在compare_exchange_strong基础上允许cpu由于缓存的原因导致cas伪失败，
  // 一般适合用于while cas中，因为伪失败无非就是多循环一次。只是这里没必要，因为compare_exchange_strong未必更慢。
  while (!head_.compare_exchange_strong(
      // 第一个内存序是cas成功的内存序，相当于load+store，可以有release语义
      // 第二个内存序是cas失败的内存序，相当于只load，所以第二个不能使用release，因为他没有东西可发布，最多只有acquire语义
      node->next, node, std::memory_order_relaxed, std::memory_order_relaxed)) {
    // ！！！
    // 由于上层的WriteGroup已经有了一个强release-acquire约束，保证了这里的生产和TakeNextColumnFamily
    // 的消费是符合内容访问的happens-before规则的，所以不需要再在这里的直接生产端重复release，TakeNextColumnFamily的
    // 直接消费端也不需要再重复acquire。
    // 事实上，如果考虑消费-生产同步的话，这里可能甚至都不需要atomic，这里的cas主要是为了给多生产者并发同步的。
    // failing CAS updates the first param, so we are already set for
    // retry.  TakeNextColumnFamily won't happen until after another
    // inter-thread synchronization, so we don't even need release
    // semantics for this CAS
  }
#endif  // __clang_analyzer__
}

ColumnFamilyData* FlushScheduler::TakeNextColumnFamily() {
  while (true) {
    if (head_.load(std::memory_order_relaxed) == nullptr) {
      return nullptr;
    }

    // dequeue the head
    Node* node = head_.load(std::memory_order_relaxed);
    head_.store(node->next, std::memory_order_relaxed);
    ColumnFamilyData* cfd = node->column_family;
    delete node;

#ifndef NDEBUG
    {
      std::lock_guard<std::mutex> lock(checking_mutex_);
      auto iter = checking_set_.find(cfd);
      assert(iter != checking_set_.end());
      checking_set_.erase(iter);
    }
#endif  // NDEBUG

    if (!cfd->IsDropped()) {
      // success
      return cfd;
    }

    // no longer relevant, retry
    cfd->UnrefAndTryDelete();
  }
}

bool FlushScheduler::Empty() {
  auto rv = head_.load(std::memory_order_relaxed) == nullptr;
#ifndef NDEBUG
  std::lock_guard<std::mutex> lock(checking_mutex_);
  // Empty is allowed to be called concurrnetly with ScheduleFlush. It would
  // only miss the recent schedules.
  assert((rv == checking_set_.empty()) || rv);
#endif  // NDEBUG
  return rv;
}

void FlushScheduler::Clear() {
  ColumnFamilyData* cfd;
  while ((cfd = TakeNextColumnFamily()) != nullptr) {
    cfd->UnrefAndTryDelete();
  }
  assert(head_.load(std::memory_order_relaxed) == nullptr);
}

}  // namespace ROCKSDB_NAMESPACE
