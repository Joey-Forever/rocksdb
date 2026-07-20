//  Copyright (c) Meta Platforms, Inc. and affiliates.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

// A compact demonstration of the local RocksDB read/write path.
//
// It intentionally contains no distributed or transactional protocol logic.
// The demo shows these RocksDB building blocks:
//   * Put/Get through the default column family
//   * a snapshot reading an older sequence
//   * an atomic WriteBatch containing Put and Delete operations
//   * an Iterator scanning a key prefix
//   * Flush moving MemTable contents into SST files
//   * closing and reopening the DB to read persistent data

#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>

#include "rocksdb/db.h"
#include "rocksdb/iterator.h"
#include "rocksdb/options.h"
#include "rocksdb/snapshot.h"
#include "rocksdb/status.h"
#include "rocksdb/write_batch.h"

namespace {

using ROCKSDB_NAMESPACE::DB;
using ROCKSDB_NAMESPACE::FlushOptions;
using ROCKSDB_NAMESPACE::Iterator;
using ROCKSDB_NAMESPACE::Options;
using ROCKSDB_NAMESPACE::ReadOptions;
using ROCKSDB_NAMESPACE::Slice;
using ROCKSDB_NAMESPACE::Snapshot;
using ROCKSDB_NAMESPACE::Status;
using ROCKSDB_NAMESPACE::WriteBatch;
using ROCKSDB_NAMESPACE::WriteOptions;

// Keep runtime data with the other generated files under build/.
const char* kDBPath = "build/rocksdb_tikv_transaction_demo";

class SnapshotGuard {
 public:
  explicit SnapshotGuard(DB* db) : db_(db), snapshot_(db_->GetSnapshot()) {}
  ~SnapshotGuard() { db_->ReleaseSnapshot(snapshot_); }

  SnapshotGuard(const SnapshotGuard&) = delete;
  SnapshotGuard& operator=(const SnapshotGuard&) = delete;

  const Snapshot* get() const { return snapshot_; }

 private:
  DB* db_;
  const Snapshot* snapshot_;
};

Status PrintGet(DB* db, const ReadOptions& read_options,
                const std::string& key, const char* view_name) {
  std::string value;
  const Status status = db->Get(read_options, key, &value);
  std::cout << "  " << view_name << " get(" << std::quoted(key) << "): ";
  if (status.ok()) {
    std::cout << std::quoted(value) << '\n';
    return Status::OK();
  }
  if (status.IsNotFound()) {
    std::cout << "<not found>\n";
    return Status::OK();
  }
  std::cout << status.ToString() << '\n';
  return status;
}

Status PrintAccountRange(DB* db) {
  std::cout << "\n4. Iterate over the account/ key range\n";
  std::unique_ptr<Iterator> iterator(db->NewIterator(ReadOptions()));
  const Slice prefix("account/");
  for (iterator->Seek(prefix); iterator->Valid() &&
                               iterator->key().starts_with(prefix);
       iterator->Next()) {
    std::cout << "  " << std::quoted(iterator->key().ToString()) << " -> "
              << std::quoted(iterator->value().ToString()) << '\n';
  }
  return iterator->status();
}

Status RunDemo() {
  Options options;
  options.create_if_missing = true;

  // Start each run from a deterministic empty database. The final database is
  // deliberately kept on disk so its WAL, MANIFEST, and SST files can be
  // inspected after the program exits.
  Status status = ROCKSDB_NAMESPACE::DestroyDB(kDBPath, options);
  if (!status.ok()) {
    return status;
  }

  std::unique_ptr<DB> db;
  status = DB::Open(options, kDBPath, &db);
  if (!status.ok()) {
    return status;
  }

  // WAL stays enabled so unflushed local writes can be recovered. sync=true
  // waits for the WAL sync before Put/Write returns.
  WriteOptions write_options;
  write_options.sync = true;

  std::cout << "1. Write individual keys with DB::Put\n";
  status = db->Put(write_options, "account/alice", "100");
  if (!status.ok()) {
    return status;
  }
  status = db->Put(write_options, "account/bob", "50");
  if (!status.ok()) {
    return status;
  }
  status = db->Put(write_options, "account/obsolete", "remove-me");
  if (!status.ok()) {
    return status;
  }
  status = PrintGet(db.get(), ReadOptions(), "account/alice", "latest");
  if (!status.ok()) {
    return status;
  }

  {
    SnapshotGuard snapshot(db.get());
    ReadOptions snapshot_read;
    snapshot_read.snapshot = snapshot.get();
    const uint64_t sequence_before_batch = db->GetLatestSequenceNumber();

    std::cout << "\n2. Apply three operations with one DB::Write batch\n";
    WriteBatch batch;
    batch.Put("account/alice", "80");
    batch.Put("account/bob", "70");
    batch.Delete("account/obsolete");
    status = db->Write(write_options, &batch);
    if (!status.ok()) {
      return status;
    }

    std::cout << "  latest sequence: " << sequence_before_batch << " -> "
              << db->GetLatestSequenceNumber() << '\n';

    std::cout << "\n3. Compare the old snapshot with the latest view\n";
    status = PrintGet(db.get(), snapshot_read, "account/alice", "snapshot");
    if (!status.ok()) {
      return status;
    }
    status = PrintGet(db.get(), ReadOptions(), "account/alice", "latest");
    if (!status.ok()) {
      return status;
    }
    status =
        PrintGet(db.get(), snapshot_read, "account/obsolete", "snapshot");
    if (!status.ok()) {
      return status;
    }
    status =
        PrintGet(db.get(), ReadOptions(), "account/obsolete", "latest");
    if (!status.ok()) {
      return status;
    }
  }

  status = PrintAccountRange(db.get());
  if (!status.ok()) {
    return status;
  }

  std::cout << "\n5. Flush the MemTable into an SST\n";
  FlushOptions flush_options;
  flush_options.wait = true;
  status = db->Flush(flush_options);
  if (!status.ok()) {
    return status;
  }
  std::string l0_files;
  if (db->GetProperty("rocksdb.num-files-at-level0", &l0_files)) {
    std::cout << "  L0 SST files: " << l0_files << '\n';
  }

  std::cout << "\n6. Close and reopen the database\n";
  db.reset();
  status = DB::Open(options, kDBPath, &db);
  if (!status.ok()) {
    return status;
  }
  status = PrintGet(db.get(), ReadOptions(), "account/alice", "reopened");
  if (!status.ok()) {
    return status;
  }
  status = PrintGet(db.get(), ReadOptions(), "account/bob", "reopened");
  if (!status.ok()) {
    return status;
  }

  db.reset();
  std::cout << "\nPersistent demo data kept at " << kDBPath << '\n';
  return Status::OK();
}

}  // namespace

int main() {
  const Status status = RunDemo();
  if (!status.ok()) {
    std::cerr << "demo failed: " << status.ToString() << '\n';
    return 1;
  }
  return 0;
}
