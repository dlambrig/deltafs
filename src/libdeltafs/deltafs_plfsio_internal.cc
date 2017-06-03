/*
 * Copyright (c) 2015-2017 Carnegie Mellon University.
 *
 * All rights reserved.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file. See the AUTHORS file for names of contributors.
 */

#include "deltafs_plfsio_internal.h"

#include "pdlfs-common/crc32c.h"
#include "pdlfs-common/hash.h"
#include "pdlfs-common/logging.h"
#include "pdlfs-common/mutexlock.h"
#include "pdlfs-common/strutil.h"

#include <assert.h>
#include <math.h>
#include <algorithm>

namespace pdlfs {
extern const char* GetLengthPrefixedSlice(const char* p, const char* limit,
                                          Slice* result);
namespace plfsio {

static inline uint32_t BloomHash(const Slice& key) {
  return Hash(key.data(), key.size(), 0xbc9f1d34);  // Magic
}

static bool BloomKeyMayMatch(const Slice& key, const Slice& input) {
  const size_t len = input.size();
  if (len < 2) {
    return true;  // Consider it a match
  }
  const uint32_t bits = static_cast<uint32_t>((len - 1) * 8);

  const char* array = input.data();
  // Use the encoded k so that we can read filters generated by
  // bloom filters created using different parameters.
  const uint32_t k = static_cast<unsigned char>(array[len - 1]);
  if (k > 30) {
    // Reserved for potentially new encodings for short bloom filters.
    // Consider it a match.
    return true;
  }

  uint32_t h = BloomHash(key);
  const uint32_t delta = (h >> 17) | (h << 15);  // Rotate right 17 bits
  for (size_t j = 0; j < k; j++) {
    const uint32_t b = h % bits;
    if ((array[b / 8] & (1 << (b % 8))) == 0) {
      return false;
    }
    h += delta;
  }

  return true;
}

// A simple bloom filter implementation
class BloomBlock {
 public:
  BloomBlock(size_t bits_per_key, size_t bytes)
      : bits_per_key_(bits_per_key), bytes_(bytes) {
    // Reserve memory space
    const size_t trailer_size = kBlockTrailerSize;
    space_.reserve(bytes_ + 1 + trailer_size);
    Reset();
  }

  ~BloomBlock() {}

  void Reset() {
    finished_ = false;
    space_.clear();
    space_.resize(bytes_, 0);
    // Round down to reduce probing cost a little bit
    k_ = static_cast<uint32_t>(bits_per_key_ * 0.69);  // 0.69 =~ ln(2)
    if (k_ < 1) k_ = 1;
    if (k_ > 30) k_ = 30;
    // Remember # of probes in filter
    space_.push_back(static_cast<char>(k_));
    bits_ = static_cast<uint32_t>(8 * bytes_);
  }

  void AddKey(const Slice& key) {
    assert(!finished_);  // Finish() has not been called
    // Use double-hashing to generate a sequence of hash values.
    uint32_t h = BloomHash(key);
    const uint32_t delta = (h >> 17) | (h << 15);  // Rotate right 17 bits
    for (size_t j = 0; j < k_; j++) {
      const uint32_t b = h % bits_;
      space_[b / 8] |= (1 << (b % 8));
      h += delta;
    }
  }

  Slice Finish() {
    assert(!finished_);
    finished_ = true;
    return space_;
  }

  Slice Finalize(bool crc32c = true) {
    assert(finished_);
    Slice contents = space_;  // Contents without the trailer
    char trailer[kBlockTrailerSize];
    trailer[0] = kNoCompression;
    if (crc32c) {
      uint32_t crc = crc32c::Value(contents.data(), contents.size());
      crc = crc32c::Extend(crc, trailer, 1);  // Extend crc to cover block type
      EncodeFixed32(trailer + 1, crc32c::Mask(crc));
    } else {
      EncodeFixed32(trailer + 1, 0);
    }
    space_.append(trailer, sizeof(trailer));
    return space_;
  }

  std::string* buffer_store() { return &space_; }

 private:
  // No copying allowed
  void operator=(const BloomBlock&);
  BloomBlock(const BloomBlock&);
  const size_t bits_per_key_;  // Number of bits for each key
  const size_t bytes_;         // Total filter size

  bool finished_;
  std::string space_;
  uint32_t bits_;
  uint32_t k_;
};

class WriteBuffer::Iter : public Iterator {
 public:
  explicit Iter(const WriteBuffer* write_buffer)
      : cursor_(-1),
        offsets_(&write_buffer->offsets_[0]),
        num_entries_(write_buffer->num_entries_),
        buffer_(write_buffer->buffer_) {}

  virtual ~Iter() {}
  virtual void Next() { cursor_++; }
  virtual void Prev() { cursor_--; }
  virtual Status status() const { return Status::OK(); }
  virtual bool Valid() const { return cursor_ >= 0 && cursor_ < num_entries_; }
  virtual void SeekToFirst() { cursor_ = 0; }
  virtual void SeekToLast() { cursor_ = num_entries_ - 1; }
  virtual void Seek(const Slice& target) {
    // Not supported
  }

  virtual Slice key() const {
    assert(Valid());
    Slice result;
    const char* p = &buffer_[offsets_[cursor_]];
    Slice input = buffer_;
    assert(p - buffer_.data() >= 0);
    input.remove_prefix(p - buffer_.data());
    if (GetLengthPrefixedSlice(&input, &result)) {
      return result;
    } else {
      assert(false);
      result = Slice();
      return result;
    }
  }

  virtual Slice value() const {
    assert(Valid());
    Slice result;
    const char* p = &buffer_[offsets_[cursor_]];
    Slice input = buffer_;
    assert(p - buffer_.data() >= 0);
    input.remove_prefix(p - buffer_.data());
    if (GetLengthPrefixedSlice(&input, &result) &&
        GetLengthPrefixedSlice(&input, &result)) {
      return result;
    } else {
      assert(false);
      result = Slice();
      return result;
    }
  }

 private:
  int cursor_;
  const uint32_t* offsets_;
  int num_entries_;
  Slice buffer_;
};

Iterator* WriteBuffer::NewIterator() const {
  assert(finished_);
  return new Iter(this);
}

struct WriteBuffer::STLLessThan {
  Slice buffer_;

  explicit STLLessThan(const Slice& buffer) : buffer_(buffer) {}

  bool operator()(uint32_t a, uint32_t b) {
    Slice key_a = GetKey(a);
    Slice key_b = GetKey(b);
    assert(!key_a.empty() && !key_b.empty());
    return key_a < key_b;
  }

  Slice GetKey(uint32_t offset) {
    Slice result;
    const char* p = GetLengthPrefixedSlice(
        buffer_.data() + offset, buffer_.data() + buffer_.size(), &result);
    if (p != NULL) {
      return result;
    } else {
      assert(false);
      return result;
    }
  }
};

void WriteBuffer::FinishAndSort() {
  // Sort entries
  assert(!finished_);
  std::vector<uint32_t>::iterator begin = offsets_.begin();
  std::vector<uint32_t>::iterator end = offsets_.end();
  std::sort(begin, end, STLLessThan(buffer_));
  finished_ = true;
}

void WriteBuffer::Reset() {
  num_entries_ = 0;
  finished_ = false;
  offsets_.clear();
  buffer_.clear();
}

void WriteBuffer::Reserve(uint32_t num_entries, size_t buffer_size) {
  buffer_.reserve(buffer_size);
  offsets_.reserve(num_entries);
}

void WriteBuffer::Add(const Slice& key, const Slice& value) {
  assert(!finished_);       // Finish() has not been called
  assert(key.size() != 0);  // Key cannot be empty
  const size_t offset = buffer_.size();
  PutLengthPrefixedSlice(&buffer_, key);
  PutLengthPrefixedSlice(&buffer_, value);
  offsets_.push_back(static_cast<uint32_t>(offset));
  num_entries_++;
}

size_t WriteBuffer::memory_usage() const {
  size_t result = 0;
  result += sizeof(uint32_t) * offsets_.capacity();
  result += buffer_.capacity();
  return result;
}

OutputStats::OutputStats()
    : footer_size(0),
      final_data_size(0),
      data_size(0),
      final_meta_size(0),
      meta_size(0),
      final_index_size(0),
      index_size(0),
      final_filter_size(0),
      filter_size(0),
      value_size(0),
      key_size(0) {}

static size_t TotalIndexSize(const OutputStats& stats) {
  return stats.filter_size + stats.index_size + stats.meta_size +
         stats.footer_size;
}

static size_t TotalDataSize(const OutputStats& stats) {
  return stats.data_size;
}

TableLogger::TableLogger(const DirOptions& options, LogSink* data,
                         LogSink* indx)
    : options_(options),
      num_uncommitted_index_(0),
      num_uncommitted_data_(0),
      data_block_(16),
      index_block_(1),
      meta_block_(1),
      pending_index_entry_(false),
      pending_meta_entry_(false),
      num_tables_(0),
      num_epochs_(0),
      data_sink_(data),
      indx_sink_(indx),
      finished_(false) {
  // Sanity checks
  assert(indx_sink_ != NULL && data_sink_ != NULL);

  indx_sink_->Ref();
  data_sink_->Ref();

  // Allocate memory
  const size_t estimated_index_size_per_table = 4 << 10;
  index_block_.Reserve(estimated_index_size_per_table);
  const size_t estimated_meta_size = 4 << 10;
  meta_block_.Reserve(estimated_meta_size);

  uncommitted_indexes_.reserve(1 << 10);
  data_block_.buffer_store()->reserve(options_.block_buffer);
  data_block_.buffer_store()->clear();
  data_block_.SwitchBuffer(NULL);
  data_block_.Reset();
}

TableLogger::~TableLogger() {
  indx_sink_->Unref();
  data_sink_->Unref();
}

void TableLogger::MakeEpoch() {
  assert(!finished_);  // Finish() has not been called
  EndTable(static_cast<BloomBlock*>(NULL));
  if (!ok()) {
    return;  // Abort
  } else if (num_tables_ == 0) {
    return;  // Empty epoch
  } else if (num_epochs_ >= kMaxEpoches) {
    status_ = Status::AssertionFailed("Too many epochs");
  } else {
    num_tables_ = 0;
    num_epochs_++;
  }
}

template <typename T>
void TableLogger::EndTable(T* filter_block) {
  assert(!finished_);  // Finish() has not been called

  EndBlock();
  if (!ok()) {
    return;
  } else if (pending_index_entry_) {
    BytewiseComparator()->FindShortSuccessor(&last_key_);
    PutLengthPrefixedSlice(&uncommitted_indexes_, last_key_);
    pending_index_handle_.EncodeTo(&uncommitted_indexes_);
    pending_index_entry_ = false;
    num_uncommitted_index_++;
  }

  Commit();
  if (!ok()) {
    return;
  } else if (index_block_.empty()) {
    return;  // Empty table
  }

  Slice index_contents = index_block_.Finish();
  const size_t index_size = index_contents.size();
  Slice final_index_contents =  // No zero padding necessary for index blocks
      index_block_.Finalize(!options_.skip_checksums);
  const size_t final_index_size = final_index_contents.size();
  const uint64_t index_offset = indx_sink_->Ltell();
  status_ = indx_sink_->Lwrite(final_index_contents);
  output_stats_.final_index_size += final_index_size;
  output_stats_.index_size += index_size;
  if (!ok()) return;  // Abort

  size_t filter_size = 0;
  const uint64_t filter_offset = indx_sink_->Ltell();
  Slice final_filter_contents;

  if (filter_block != NULL) {
    Slice filer_contents = filter_block->Finish();
    filter_size = filer_contents.size();
    final_filter_contents = filter_block->Finalize(!options_.skip_checksums);
    const size_t final_filter_size = final_filter_contents.size();
    status_ = indx_sink_->Lwrite(final_filter_contents);
    output_stats_.final_filter_size += final_filter_size;
    output_stats_.filter_size += filter_size;
  } else {
    // No filter configured
  }

  if (ok()) {
    index_block_.Reset();
    pending_meta_handle_.set_filter_offset(filter_offset);
    pending_meta_handle_.set_filter_size(filter_size);
    pending_meta_handle_.set_offset(index_offset);
    pending_meta_handle_.set_size(index_size);
    assert(!pending_meta_entry_);
    pending_meta_entry_ = true;
  } else {
    return;  // Abort
  }

  if (num_tables_ >= kMaxTablesPerEpoch) {
    status_ = Status::AssertionFailed("Too many tables");
  } else if (pending_meta_entry_) {
    pending_meta_handle_.set_smallest_key(smallest_key_);
    BytewiseComparator()->FindShortSuccessor(&largest_key_);
    pending_meta_handle_.set_largest_key(largest_key_);
    std::string handle_encoding;
    pending_meta_handle_.EncodeTo(&handle_encoding);
    meta_block_.Add(EpochKey(num_epochs_, num_tables_), handle_encoding);
    pending_meta_entry_ = false;
  }

  if (ok()) {
    smallest_key_.clear();
    largest_key_.clear();
    last_key_.clear();
    num_tables_++;
  }
}

void TableLogger::Commit() {
  assert(!finished_);  // Finish() has not been called
  if (data_block_.buffer_store()->empty()) return;  // Empty commit
  if (!ok()) return;                                // Abort

  data_sink_->Lock();
  assert(num_uncommitted_data_ == num_uncommitted_index_);
  const size_t offset = data_sink_->Ltell();
  status_ = data_sink_->Lwrite(*data_block_.buffer_store());
  data_sink_->Unlock();
  if (!ok()) return;  // Abort

  Slice key;
  int num_index_committed = 0;
  Slice input = uncommitted_indexes_;
  std::string handle_encoding;
  BlockHandle handle;
  while (!input.empty()) {
    if (GetLengthPrefixedSlice(&input, &key)) {
      handle.DecodeFrom(&input);
      handle.set_offset(offset + handle.offset());
      handle.EncodeTo(&handle_encoding);
      index_block_.Add(key, handle_encoding);
      num_index_committed++;
    } else {
      break;
    }
  }

  assert(num_index_committed == num_uncommitted_index_);
  num_uncommitted_data_ = num_uncommitted_index_ = 0;
  uncommitted_indexes_.clear();
  data_block_.buffer_store()->clear();
  data_block_.SwitchBuffer(NULL);
  data_block_.Reset();
}

void TableLogger::EndBlock() {
  assert(!finished_);               // Finish() has not been called
  if (data_block_.empty()) return;  // Empty block
  if (!ok()) return;                // Abort

  Slice block_contents = data_block_.Finish();
  const size_t block_size = block_contents.size();
  Slice final_block_contents;
  if (options_.block_padding) {
    final_block_contents = data_block_.Finalize(
        !options_.skip_checksums, static_cast<uint32_t>(options_.block_size));
  } else {
    final_block_contents = data_block_.Finalize(!options_.skip_checksums);
  }

  const size_t final_block_size = final_block_contents.size();
  const uint64_t block_offset =
      data_block_.buffer_store()->size() - final_block_size;
  output_stats_.final_data_size += final_block_size;
  output_stats_.data_size += block_size;

  if (ok()) {
    data_block_.SwitchBuffer(NULL);
    data_block_.Reset();
    pending_index_handle_.set_size(block_size);
    pending_index_handle_.set_offset(block_offset);
    assert(!pending_index_entry_);
    pending_index_entry_ = true;
    num_uncommitted_data_++;
  }
}

void TableLogger::Add(const Slice& key, const Slice& value) {
  assert(!finished_);       // Finish() has not been called
  assert(key.size() != 0);  // Key cannot be empty
  if (!ok()) return;        // Abort

  if (!last_key_.empty()) {
    // Keys within a single table are expected to be added in a sorted order.
    assert(key.compare(last_key_) >= 0);
    if (options_.unique_keys) {
      // Duplicated keys are not allowed
      assert(key.compare(last_key_) != 0);
    }
  }
  if (smallest_key_.empty()) {
    smallest_key_ = key.ToString();
  }
  largest_key_ = key.ToString();

  // Add an index entry if there is one pending insertion
  if (pending_index_entry_) {
    BytewiseComparator()->FindShortestSeparator(&last_key_, key);
    PutLengthPrefixedSlice(&uncommitted_indexes_, last_key_);
    pending_index_handle_.EncodeTo(&uncommitted_indexes_);
    pending_index_entry_ = false;
    num_uncommitted_index_++;
  }

  // Flush block buffer if it is about to full
  if (data_block_.buffer_store()->size() + options_.block_size >
      options_.block_buffer) {
    Commit();
  }

  last_key_ = key.ToString();
  output_stats_.value_size += value.size();
  output_stats_.key_size += key.size();

  data_block_.Add(key, value);
  if (data_block_.CurrentSizeEstimate() + kBlockTrailerSize >=
      static_cast<size_t>(options_.block_size * options_.block_util)) {
    EndBlock();
  }
}

Status TableLogger::Finish() {
  assert(!finished_);  // Finish() has not been called
  MakeEpoch();
  finished_ = true;
  if (!ok()) return status_;
  BlockHandle epoch_index_handle;
  std::string footer_buf;
  Footer footer;

  assert(!pending_meta_entry_);
  Slice meta_contents = meta_block_.Finish();
  const size_t meta_size = meta_contents.size();
  Slice final_meta_contents =  // No padding is needed for the root meta block
      meta_block_.Finalize(!options_.skip_checksums);
  const size_t final_meta_size = final_meta_contents.size();
  const uint64_t meta_offset = indx_sink_->Ltell();
  status_ = indx_sink_->Lwrite(final_meta_contents);
  output_stats_.final_meta_size += final_meta_size;
  output_stats_.meta_size += meta_size;
  if (!ok()) return status_;

  epoch_index_handle.set_size(meta_size);
  epoch_index_handle.set_offset(meta_offset);
  footer.set_epoch_index_handle(epoch_index_handle);
  footer.set_num_epoches(num_epochs_);
  footer.EncodeTo(&footer_buf);

  const size_t footer_size = footer_buf.size();

  if (options_.tail_padding) {
    // Add enough padding to ensure the final size of the index log
    // is some multiple of the physical write size.
    const uint64_t total_size = indx_sink_->Ltell() + footer_size;
    const size_t overflow = total_size % options_.index_buffer;
    if (overflow != 0) {
      const size_t n = options_.index_buffer - overflow;
      status_ = indx_sink_->Lwrite(std::string(n, 0));
    } else {
      // No need to pad
    }
  }

  if (ok()) {
    status_ = indx_sink_->Lwrite(footer_buf);
    output_stats_.footer_size += footer_size;
    return status_;
  } else {
    return status_;
  }
}

DirLogger::DirLogger(const DirOptions& options, port::Mutex* mu,
                     port::CondVar* cv, LogSink* indx, LogSink* data,
                     CompactionStats* stats)
    : options_(options),
      bg_cv_(cv),
      mu_(mu),
      data_(data),
      indx_(indx),
      compaction_stats_(stats),
      num_flush_requested_(0),
      num_flush_completed_(0),
      has_bg_compaction_(false),
      table_logger_(new TableLogger(options, data, indx)),
      filter_(NULL),
      mem_buf_(NULL),
      imm_buf_(NULL),
      imm_buf_is_epoch_flush_(false),
      imm_buf_is_final_(false) {
  // Sanity checks
  assert(mu != NULL && cv != NULL);
  assert(data_ != NULL && indx_ != NULL);

  data_->Ref();
  indx_->Ref();

  // Determine the right table size and bloom filter size.
  // Works best when the key and value sizes are fixed.
  //
  // Otherwise, if the estimated key or value sizes are greater
  // than the real average, filter will be allocated with less bytes
  // and there will be higher false positive rate.
  //
  // On the other hand, if the estimated sizes are less than
  // the real, filter will waste memory and each
  // write buffer will be allocated with
  // less memory.
  size_t overhead_per_entry = static_cast<size_t>(
      VarintLength(options_.key_size) + VarintLength(options_.value_size) +
      sizeof(uint32_t)  // Offset of an entry in buffer
      );
  size_t bytes_per_entry =
      options_.key_size + options_.value_size + overhead_per_entry;

  size_t bits_per_entry = 8 * bytes_per_entry;

  size_t total_bits_per_entry =
      options_.bf_bits_per_key + 2 * bits_per_entry;  // Due to double buffering

  size_t table_buffer =  // Total write buffer for each memtable partition
      options_.memtable_buffer / static_cast<uint32_t>(1 << options_.lg_parts) -
      options_.block_buffer;  // Reserved for compaction

  // Estimated amount of entries per table
  entries_per_tb_ = static_cast<uint32_t>(
      ceil(8.0 * double(table_buffer) / double(total_bits_per_entry)));

  tb_bytes_ = entries_per_tb_ * (bytes_per_entry - sizeof(uint32_t));
  // Compute bloom filter size (in both bits and bytes)
  bf_bits_ = entries_per_tb_ * options_.bf_bits_per_key;
  // For small n, we can see a very high false positive rate.
  // Fix it by enforcing a minimum bloom filter length.
  if (bf_bits_ > 0 && bf_bits_ < 64) {
    bf_bits_ = 64;
  }

  bf_bytes_ = (bf_bits_ + 7) / 8;
  bf_bits_ = bf_bytes_ * 8;

#if VERBOSE >= 2
  Verbose(__LOG_ARGS__, 2, "OPT: plfsdir.memtable.tb_size -> %d x %s",
          2 * (1 << options_.lg_parts), PrettySize(tb_bytes_).c_str());
  Verbose(__LOG_ARGS__, 2, "OPT: plfsdir.memtable.bf_size -> %d x %s",
          2 * (1 << options_.lg_parts), PrettySize(bf_bytes_).c_str());
#endif

  // Allocate memory
  buf0_.Reserve(entries_per_tb_, tb_bytes_);
  buf1_.Reserve(entries_per_tb_, tb_bytes_);

  if (options_.bf_bits_per_key != 0) {
    filter_ = new BloomBlock(options_.bf_bits_per_key, bf_bytes_);
  }

  mem_buf_ = &buf0_;
}

DirLogger::~DirLogger() {
  mu_->AssertHeld();
  while (has_bg_compaction_) {
    bg_cv_->Wait();
  }
  delete table_logger_;
  BloomBlock* bf = static_cast<BloomBlock*>(filter_);
  if (bf != NULL) {
    delete bf;
  }
  data_->Unref();
  indx_->Unref();
}

// Block until compaction finishes and return the
// latest compaction status.
Status DirLogger::Wait() {
  mu_->AssertHeld();
  Status status;
  while (table_logger_->ok() && has_bg_compaction_) {
    bg_cv_->Wait();
  }
  if (!table_logger_->ok()) {
    status = table_logger_->status();
  }
  return status;
}

// Pre-close all linked log files.
// Usually, log files are reference counted and are closed when
// de-referenced by the last opener. Optionally, caller may force the
// fsync and closing of all log files.
Status DirLogger::PreClose() {
  mu_->AssertHeld();
  const bool sync = true;
  data_->Lock();
  Status status = data_->Lclose(sync);
  data_->Unlock();
  if (status.ok()) {
    status = indx_->Lclose(sync);
  }
  return status;
}

// If dry_run has been set, simply perform status checks and no compaction
// jobs will be scheduled or waited for. Return immediately, and return OK if
// compaction may be scheduled immediately without waiting, or return a special
// status if compaction cannot be scheduled immediately due to lack of buffer
// space, or directly return a status that indicates an I/O error.
// Otherwise, **wait** until a compaction is scheduled unless
// options_.non_blocking is set. After a compaction has been scheduled,
// **wait** until it finishes unless no_wait has been set.
Status DirLogger::Flush(const FlushOptions& flush_options) {
  mu_->AssertHeld();
  // Wait for buffer space
  while (imm_buf_ != NULL) {
    if (flush_options.dry_run || options_.non_blocking) {
      return Status::BufferFull(Slice());
    } else {
      bg_cv_->Wait();
    }
  }

  Status status;
  if (flush_options.dry_run) {
    status = table_logger_->status();  // Status check only
  } else {
    num_flush_requested_++;
    const uint32_t thres = num_flush_requested_;
    const bool force = true;
    status = Prepare(force, flush_options.epoch_flush, flush_options.finalize);
    if (status.ok()) {
      if (!flush_options.no_wait) {
        while (num_flush_completed_ < thres) {
          bg_cv_->Wait();
        }
      }
    }
  }

  return status;
}

Status DirLogger::Add(const Slice& key, const Slice& value) {
  mu_->AssertHeld();
  Status status = Prepare();
  if (status.ok()) mem_buf_->Add(key, value);
  return status;
}

Status DirLogger::Prepare(bool force, bool epoch_flush, bool finalize) {
  mu_->AssertHeld();
  Status status;
  assert(mem_buf_ != NULL);
  while (true) {
    if (!table_logger_->ok()) {
      status = table_logger_->status();
      break;
    } else if (!force &&
               mem_buf_->CurrentBufferSize() <
                   static_cast<size_t>(tb_bytes_ * options_.memtable_util)) {
      // There is room in current write buffer
      break;
    } else if (imm_buf_ != NULL) {
      if (options_.non_blocking) {
        status = Status::BufferFull(Slice());
        break;
      } else {
        bg_cv_->Wait();
      }
    } else {
      // Attempt to switch to a new write buffer
      force = false;
      assert(imm_buf_ == NULL);
      imm_buf_ = mem_buf_;
      if (epoch_flush) imm_buf_is_epoch_flush_ = true;
      epoch_flush = false;
      if (finalize) imm_buf_is_final_ = true;
      finalize = false;
      WriteBuffer* const current_buf = mem_buf_;
      MaybeScheduleCompaction();
      if (current_buf == &buf0_) {
        mem_buf_ = &buf1_;
      } else {
        mem_buf_ = &buf0_;
      }
    }
  }

  return status;
}

void DirLogger::MaybeScheduleCompaction() {
  mu_->AssertHeld();

  if (has_bg_compaction_) return;  // Skip if there is one already scheduled
  if (imm_buf_ == NULL) return;    // Nothing to be scheduled

  has_bg_compaction_ = true;

  if (options_.compaction_pool != NULL) {
    options_.compaction_pool->Schedule(DirLogger::BGWork, this);
  } else if (options_.allow_env_threads) {
    Env::Default()->Schedule(DirLogger::BGWork, this);
  } else {
    DoCompaction();
  }
}

void DirLogger::BGWork(void* arg) {
  DirLogger* ins = reinterpret_cast<DirLogger*>(arg);
  MutexLock ml(ins->mu_);
  ins->DoCompaction();
}

void DirLogger::DoCompaction() {
  mu_->AssertHeld();
  assert(has_bg_compaction_);
  assert(imm_buf_ != NULL);
  CompactMemtable();
  imm_buf_->Reset();
  imm_buf_is_epoch_flush_ = false;
  imm_buf_is_final_ = false;
  imm_buf_ = NULL;
  has_bg_compaction_ = false;
  MaybeScheduleCompaction();
  bg_cv_->SignalAll();
}

void DirLogger::CompactMemtable() {
  mu_->AssertHeld();
  WriteBuffer* const buffer = imm_buf_;
  assert(buffer != NULL);
  const bool is_final = imm_buf_is_final_;
  const bool is_epoch_flush = imm_buf_is_epoch_flush_;
  TableLogger* const tb = table_logger_;
  BloomBlock* const bf = static_cast<BloomBlock*>(filter_);
  mu_->Unlock();
  const OutputStats start_stats = tb->output_stats_;
  uint64_t start = Env::Default()->NowMicros();
#if VERBOSE >= 3
  Verbose(__LOG_ARGS__, 3, "Compacting memtable: (%d/%d Bytes) ...",
          static_cast<int>(buffer->CurrentBufferSize()),
          static_cast<int>(tb_bytes_));
#endif
#ifndef NDEBUG
  uint32_t num_keys = 0;
#endif
  if (bf != NULL) bf->Reset();
  buffer->FinishAndSort();
  Iterator* const iter = buffer->NewIterator();
  iter->SeekToFirst();
  for (; iter->Valid(); iter->Next()) {
#ifndef NDEBUG
    num_keys++;
#endif
    if (bf != NULL) {
      bf->AddKey(iter->key());
    }
    tb->Add(iter->key(), iter->value());
    if (!tb->ok()) {
      break;
    }
  }

  if (tb->ok()) {
    // Paranoid checks
    assert(num_keys == buffer->NumEntries());
    tb->EndTable(bf);  // Inject the filter into the table

    if (is_epoch_flush) {
      tb->MakeEpoch();
    }
    if (is_final) {
      tb->Finish();
    }
  }

  const OutputStats end_stats = tb->output_stats_;
  uint64_t end = Env::Default()->NowMicros();

#if VERBOSE >= 3
  Verbose(__LOG_ARGS__, 3, "Compaction done: %d entries (%d us)",
          static_cast<int>(buffer->NumEntries()),
          static_cast<int>(end - start));
#endif

  delete iter;
  mu_->Lock();
  compaction_stats_->index_size +=
      TotalIndexSize(end_stats) - TotalIndexSize(start_stats);
  compaction_stats_->data_size +=
      TotalDataSize(end_stats) - TotalDataSize(start_stats);

  num_flush_completed_++;
}

size_t DirLogger::memory_usage() const {
  mu_->AssertHeld();
  size_t result = 0;
  result += buf0_.memory_usage();
  result += buf1_.memory_usage();
  std::vector<std::string*> stores;
  stores.push_back(table_logger_->meta_block_.buffer_store());
  stores.push_back(table_logger_->data_block_.buffer_store());
  if (filter_ != NULL) {
    stores.push_back(static_cast<BloomBlock*>(filter_)->buffer_store());
  }
  stores.push_back(table_logger_->index_block_.buffer_store());
  for (size_t i = 0; i < stores.size(); i++) {
    result += stores[i]->capacity();
  }
  return result;
}

template <typename T>
static Status ReadBlock(LogSource* source, const DirOptions& options,
                        const T& handle, BlockContents* result) {
  result->data = Slice();
  result->heap_allocated = false;
  result->cachable = false;

  assert(source != NULL);
  size_t n = static_cast<size_t>(handle.size());
  size_t m = n;
  if (!options.skip_checksums) {
    m += kBlockTrailerSize;
  }
  char* buf = new char[m];
  Slice contents;
  Status status = source->Read(handle.offset(), m, &contents, buf);
  if (status.ok()) {
    if (contents.size() != m) {
      status = Status::Corruption("Truncated block read");
    }
  }
  if (!status.ok()) {
    delete[] buf;
    return status;
  }

  // CRC checks
  const char* data = contents.data();  // Pointer to where read put the data
  if (!options.skip_checksums && options.verify_checksums) {
    const uint32_t crc = crc32c::Unmask(DecodeFixed32(data + n + 1));
    const uint32_t actual = crc32c::Value(data, n + 1);
    if (actual != crc) {
      delete[] buf;
      status = Status::Corruption("Block checksum mismatch");
      return status;
    }
  }

  if (data != buf) {
    // File implementation has given us pointer to some other data.
    // Use it directly under the assumption that it will be live
    // while the file is open.
    delete[] buf;
    result->data = Slice(data, n);
    result->heap_allocated = false;
    result->cachable = false;  // Avoid double cache
  } else {
    result->data = Slice(buf, n);
    result->heap_allocated = true;
    result->cachable = true;
  }

  return status;
}

// Retrieve value to a specific key from a given block and call "saver"
// using the value found. In addition, set *exhausted to true if a larger key
// has been observed so there is no need to check further.
// Return OK on success and a non-OK status on errors.
Status Dir::Fetch(const Slice& key, const BlockHandle& handle, Saver saver,
                  void* arg, bool* exhausted) {
  *exhausted = false;
  Status status;
  BlockContents contents;
  status = ReadBlock(data_, options_, handle, &contents);
  if (!status.ok()) {
    return status;
  }

  Block* block = new Block(contents);
  Iterator* const iter = block->NewIterator(BytewiseComparator());
  if (options_.unique_keys) {
    iter->Seek(key);  // Binary search
  } else {
    iter->SeekToFirst();
    while (iter->Valid() && key.compare(iter->key()) > 0) {
      iter->Next();
    }
  }

  for (; iter->Valid(); iter->Next()) {
    if (iter->key() == key) {
      saver(arg, key, iter->value());
      // If keys are unique, we are done
      if (options_.unique_keys) {
        *exhausted = true;
        break;
      }
    } else {
      assert(iter->key() > key);
      *exhausted = true;
      break;
    }
  }

  status = iter->status();

  delete iter;
  delete block;
  return status;
}

// Check if a specific key may or must not exist in one or more blocks
// indexed by the given filter.
bool Dir::KeyMayMatch(const Slice& key, const BlockHandle& handle) {
  Status status;
  BlockContents contents;
  status = ReadBlock(indx_, options_, handle, &contents);
  if (status.ok()) {
    bool r = BloomKeyMayMatch(key, contents.data);
    if (contents.heap_allocated) {
      delete[] contents.data.data();
    }
    return r;
  } else {
    return true;
  }
}

// Retrieve value to a specific key from a given table and call "saver" using
// the value found. Use filter to reduce block reads if available.
// Return OK on success and a non-OK status on errors.
Status Dir::Fetch(const Slice& key, const TableHandle& handle, Saver saver,
                  void* arg) {
  Status status;
  // Check key range and filter
  if (key.compare(handle.smallest_key()) < 0 ||
      key.compare(handle.largest_key()) > 0) {
    return status;
  } else {
    BlockHandle filter_handle;
    filter_handle.set_offset(handle.filter_offset());
    filter_handle.set_size(handle.filter_size());
    if (filter_handle.size() != 0 && !KeyMayMatch(key, filter_handle)) {
      return status;
    }
  }

  // Load the index block
  BlockContents contents;
  status = ReadBlock(indx_, options_, handle, &contents);
  if (!status.ok()) {
    return status;
  }

  Block* block = new Block(contents);
  Iterator* const iter = block->NewIterator(BytewiseComparator());
  if (options_.unique_keys) {
    iter->Seek(key);
  } else {
    iter->SeekToFirst();
    while (iter->Valid() && key.compare(iter->key()) > 0) {
      iter->Next();
    }
  }

  bool exhausted = false;
  for (; iter->Valid(); iter->Next()) {
    BlockHandle h;
    Slice input = iter->value();
    status = h.DecodeFrom(&input);
    if (status.ok()) {
      status = Fetch(key, h, saver, arg, &exhausted);
    }

    if (!status.ok()) {
      break;
    } else if (exhausted) {
      break;
    }
  }

  if (status.ok()) {
    status = iter->status();
  }

  delete iter;
  delete block;
  return status;
}

namespace {
struct SaverState {
  std::string* dst;
  bool found;
};

static void SaveValue(void* arg, const Slice& key, const Slice& value) {
  SaverState* state = reinterpret_cast<SaverState*>(arg);
  state->dst->append(value.data(), value.size());
  state->found = true;
}

struct ParaSaverState : public SaverState {
  uint32_t epoch;
  std::vector<uint32_t>* offsets;
  std::string* buffer;
  port::Mutex* mu;
};

static void ParaSaveValue(void* arg, const Slice& key, const Slice& value) {
  ParaSaverState* state = reinterpret_cast<ParaSaverState*>(arg);
  MutexLock ml(state->mu);
  state->offsets->push_back(static_cast<uint32_t>(state->buffer->size()));
  PutVarint32(state->buffer, state->epoch);
  PutLengthPrefixedSlice(state->buffer, value);
  state->found = true;
}

static inline Iterator* NewEpochIterator(Block* epoch_index) {
  return epoch_index->NewIterator(BytewiseComparator());
}

}  // namespace

void Dir::Get(const Slice& key, uint32_t epoch, GetContext* ctx) {
  mutex_.AssertHeld();
  if (!ctx->status->ok()) {
    return;
  }
  Iterator* epoch_iter = ctx->epoch_iter;
  if (epoch_iter == NULL) {
    epoch_iter = NewEpochIterator(epochs_);
  }
  mutex_.Unlock();
  Status status;
  std::string epoch_key;
  uint32_t table = 0;
  for (; status.ok(); table++) {
    epoch_key = EpochKey(epoch, table);
    // Try reusing current iterator position if possible
    if (!epoch_iter->Valid() || epoch_iter->key() != epoch_key) {
      epoch_iter->Seek(epoch_key);
      if (!epoch_iter->Valid()) {
        break;  // EOF
      } else if (epoch_iter->key() != epoch_key) {
        break;  // No such table
      }
    }
    ParaSaverState state;
    state.epoch = epoch;
    state.offsets = ctx->offsets;
    state.buffer = ctx->buffer;
    state.mu = &mutex_;
    state.dst = ctx->dst;
    state.found = false;
    TableHandle h;
    Slice handle_encoding = epoch_iter->value();
    status = h.DecodeFrom(&handle_encoding);
    epoch_iter->Next();
    if (status.ok()) {
      if (options_.parallel_reads) {
        status = Fetch(key, h, ParaSaveValue, &state);
      } else {
        status = Fetch(key, h, SaveValue, &state);
      }
      if (status.ok() && state.found) {
        if (options_.unique_keys) {
          break;
        }
      }
    }
  }

  if (status.ok()) {
    status = epoch_iter->status();
  }

  mutex_.Lock();
  if (epoch_iter != ctx->epoch_iter) {
    delete epoch_iter;
  }
  assert(ctx->num_open_reads > 0);
  ctx->num_open_reads--;
  cond_var_.SignalAll();
  if (ctx->status->ok()) {
    *ctx->status = status;
  }
}

struct Dir::STLLessThan {
  Slice buffer_;

  explicit STLLessThan(const Slice& buffer) : buffer_(buffer) {}

  bool operator()(uint32_t a, uint32_t b) {
    const uint32_t epoch_a = GetEpoch(a);
    const uint32_t epoch_b = GetEpoch(b);
    return epoch_a < epoch_b;
  }

  uint32_t GetEpoch(uint32_t off) {
    uint32_t e = 0;
    const char* p = GetVarint32Ptr(  // Decode epoch number
        buffer_.data() + off, buffer_.data() + buffer_.size(), &e);
    if (p != NULL) {
      return e;
    } else {
      assert(false);
      return e;
    }
  }
};

void Dir::Merge(GetContext* ctx) {
  std::vector<uint32_t>::iterator begin = ctx->offsets->begin();
  std::vector<uint32_t>::iterator end = ctx->offsets->end();
  std::sort(begin, end, STLLessThan(*ctx->buffer));

  uint32_t ignored;
  Slice value;
  std::vector<uint32_t>::const_iterator it;
  for (it = ctx->offsets->begin(); it != ctx->offsets->end(); ++it) {
    Slice input = *ctx->buffer;
    input.remove_prefix(*it);
    GetVarint32(&input, &ignored);
    GetLengthPrefixedSlice(&input, &value);
    ctx->dst->append(value.data(), value.size());
  }
}

Status Dir::Read(const Slice& key, std::string* dst) {
  Status status;
  assert(epochs_ != NULL);
  std::vector<uint32_t> offsets;
  std::string buffer;

  MutexLock ml(&mutex_);
  num_bg_reads_++;
  GetContext ctx;
  ctx.num_open_reads = 0;  // Number of outstanding epoch reads
  ctx.status = &status;
  ctx.offsets = &offsets;
  ctx.buffer = &buffer;
  if (!options_.parallel_reads) {
    // Pre-create the epoch iterator for serial reads
    ctx.epoch_iter = NewEpochIterator(epochs_);
  } else {
    ctx.epoch_iter = NULL;
  }
  ctx.dst = dst;
  if (num_epoches_ != 0) {
    uint32_t epoch = 0;
    for (; epoch < num_epoches_; epoch++) {
      ctx.num_open_reads++;
      BGItem item;
      item.epoch = epoch;
      item.dir = this;
      item.ctx = &ctx;
      item.key = key;
      if (!options_.parallel_reads) {
        Get(item.key, item.epoch, item.ctx);
      } else if (options_.reader_pool != NULL) {
        options_.reader_pool->Schedule(Dir::BGWork, &item);
      } else if (options_.allow_env_threads) {
        Env::Default()->Schedule(Dir::BGWork, &item);
      } else {
        Get(item.key, item.epoch, item.ctx);
      }
      if (!status.ok()) {
        break;
      }
    }
  }

  // Wait for all outstanding read operations to conclude
  while (ctx.num_open_reads > 0) {
    cond_var_.Wait();
  }

  delete ctx.epoch_iter;
  // Merge read results
  if (status.ok()) {
    if (options_.parallel_reads) {
      Merge(&ctx);
    }
  }

  assert(num_bg_reads_ > 0);
  num_bg_reads_--;
  cond_var_.SignalAll();
  return status;
}

void Dir::BGWork(void* arg) {
  BGItem* item = reinterpret_cast<BGItem*>(arg);
  MutexLock ml(&item->dir->mutex_);
  item->dir->Get(item->key, item->epoch, item->ctx);
}

Dir::Dir(const DirOptions& options, LogSource* data, LogSource* indx)
    : options_(options),
      num_epoches_(0),
      indx_(indx),
      data_(data),
      cond_var_(&mutex_),
      num_bg_reads_(0),
      epochs_(NULL) {
  assert(indx_ != NULL && data_ != NULL);
  indx_->Ref();
  data_->Ref();
}

Dir::~Dir() {
  // Wait for all on-going reads to finish
  mutex_.Lock();
  while (num_bg_reads_ != 0) {
    cond_var_.Wait();
  }
  mutex_.Unlock();

  delete epochs_;
  indx_->Unref();
  data_->Unref();
}

Status Dir::Open(const DirOptions& options, LogSource* data, LogSource* indx,
                 Dir** dirptr) {
  *dirptr = NULL;
  Status status;
  char space[Footer::kEncodeLength];
  Slice input;
  if (indx->Size() >= sizeof(space)) {
    status =
        indx->Read(indx->Size() - sizeof(space), sizeof(space), &input, space);
  } else {
    status = Status::Corruption("Dir index too short to be valid");
  }

  if (!status.ok()) {
    return status;
  }

  Footer footer;
  status = footer.DecodeFrom(&input);
  if (!status.ok()) {
    return status;
  }

  BlockContents contents;
  const BlockHandle& handle = footer.epoch_index_handle();
  status = ReadBlock(indx, options, handle, &contents);
  if (!status.ok()) {
    return status;
  }

  Dir* dir = new Dir(options, data, indx);
  dir->num_epoches_ = footer.num_epoches();
  Block* block = new Block(contents);
  dir->epochs_ = block;

  *dirptr = dir;
  return status;
}

}  // namespace plfsio
}  // namespace pdlfs
