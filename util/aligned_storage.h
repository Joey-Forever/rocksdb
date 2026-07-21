//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
#pragma once

#include <cstddef>

#include "rocksdb/rocksdb_namespace.h"

namespace ROCKSDB_NAMESPACE {

template <typename T, std::size_t Align = alignof(T)>
struct aligned_storage {
  // type类本质上是一个已对齐的空字节容器，创建type类实例时只是占位了一块对齐的内存区域，没有进行T类的实际构造。
  // 但是提供的只是一块raw storage，没有对T对象进行可选构造、析构的生命周期管理（std::optional的功能）。
  //  1. 对char数组进行alignas(Align)保证字节容器首地址按照alignof(T)对齐了
  //  2. 将char数组封装到struct中，使得字节容器实际占据的内存大小为alignof(T)的整数倍
  struct type {
    alignas(Align) unsigned char data[sizeof(T)];
  };
};

}  // namespace ROCKSDB_NAMESPACE
