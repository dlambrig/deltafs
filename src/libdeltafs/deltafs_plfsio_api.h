#pragma once

/*
 * Copyright (c) 2015-2016 Carnegie Mellon University.
 *
 * All rights reserved.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file. See the AUTHORS file for names of contributors.
 */

#include <stddef.h>

#include "pdlfs-common/env.h"

namespace pdlfs {
namespace plfsio {

struct Options {
  // Approximate size of user data packed per block.
  // This usually corresponds to the size of each I/O request
  // sent to the underlying storage.
  // Default: 128K
  size_t block_size;

  // Approximate size of user data packed per table.
  // This corresponds to the size of the in-memory write buffer
  // we must allocate for each log stream.
  // Default: 2M
  size_t table_size;

  // Thread pool used to run background compaction jobs.
  // Set to NULL to disable background jobs so all compactions will
  // run as foreground jobs.
  // Default: NULL
  ThreadPool* compaction_pool;

  // True if write operations should be performed in a non-blocking manner,
  // in which case a special status is returned instead of blocking the
  // writer to wait for buffer space.
  // Default: True
  bool non_blocking;

  // Number of microseconds to slowdown if a writer cannot make progress
  // because the system has run out of its buffer space.
  // Default: 0
  uint64_t slowdown_micros;

  // Number of partitions to divide the data. Specified in logarithmic
  // number so each x will give 2**x partitions.
  // Default: 0
  int lg_parts;

  // Env instance used to access raw files stored in the underlying
  // storage system. If NULL, Env::Default() will be used.
  // Default: NULL
  Env* env;
};

// Abstraction for a non-thread-safe un-buffered
// append-only log file.
class LogSink {
 public:
  LogSink(WritableFile* f, uint64_t s) : file_(f), offset_(s) {}
  LogSink(WritableFile* f) : file_(f), offset_(0) {}

  // XXX: Keep the file open
  ~LogSink() {}

  uint64_t Ltell() const { return offset_; }

  Status Lwrite(const Slice& data) {
    Status result = file_->Append(data);
    if (result.ok()) {
      result = file_->Flush();
      if (result.ok()) {
        offset_ += data.size();
      }
    }
    return result;
  }

 private:
  WritableFile* file_;
  uint64_t offset_;
};

class Writer {
 public:
  Writer() {}
  virtual ~Writer();

  virtual Status Append(const Slice& fname, const Slice& data) = 0;
  virtual Status MakeEpoch() = 0;

 private:
  // No copying allowed
  void operator=(const Writer&);
  Writer(const Writer&);
};

}  // namespace plfsio
}  // namespace pdlfs
