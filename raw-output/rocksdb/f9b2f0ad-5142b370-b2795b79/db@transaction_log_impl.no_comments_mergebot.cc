#include "db/transaction_log_impl.h"
#include "db/write_batch_internal.h"
namespace rocksdb {
TransactionLogIteratorImpl::TransactionLogIteratorImpl(const std::string& dir, const Options* options, const TransactionLogIterator::ReadOptions& read_options, const DBOptions* options, const EnvOptions& soptions, const SequenceNumber seq, std::unique_ptr<VectorLogPtr> files, DBImpl const* const dbimpl, DBImpl const * const dbimpl): dir_(dir), options_(options), read_options_(read_options), soptions_(soptions), startingSequenceNumber_(seq), files_(std::move(files)), started_(false), isValid_(false), currentFileIndex_(0), currentBatchSeq_(0), currentLastSeq_(0), dbimpl_(dbimpl) {
  assert(files_ != nullptr);
  assert(dbimpl_ != nullptr);
  reporter_.env = options_->env;
  reporter_.info_log = options_->info_log.get();
  SeekToStartSequence();
}
Status TransactionLogIteratorImpl::OpenLogFile(
    const LogFile* logFile,
    unique_ptr<SequentialFile>* file) {
  Env* env = options_->env;
  if (logFile->Type() == kArchivedLogFile) {
    std::string fname = ArchivedLogFileName(dir_, logFile->LogNumber());
    return env->NewSequentialFile(fname, file, soptions_);
  } else {
    std::string fname = LogFileName(dir_, logFile->LogNumber());
    Status status = env->NewSequentialFile(fname, file, soptions_);
    if (!status.ok()) {
      fname = ArchivedLogFileName(dir_, logFile->LogNumber());
      status = env->NewSequentialFile(fname, file, soptions_);
    }
    return status;
  }
}
BatchResult TransactionLogIteratorImpl::GetBatch() {
  assert(isValid_);
  BatchResult result;
  result.sequence = currentBatchSeq_;
  result.writeBatchPtr = std::move(currentBatch_);
  return result;
}
Status TransactionLogIteratorImpl::status() {
  return currentStatus_;
}
bool TransactionLogIteratorImpl::Valid() {
  return started_ && isValid_;
}
bool TransactionLogIteratorImpl::RestrictedRead(
    Slice* record,
    std::string* scratch) {
  if (currentLastSeq_ >= dbimpl_->GetLatestSequenceNumber()) {
    return false;
  }
  return currentLogReader_->ReadRecord(record, scratch);
}
void TransactionLogIteratorImpl::SeekToStartSequence(
    uint64_t startFileIndex,
    bool strict) {
  std::string scratch;
  Slice record;
  started_ = false;
  isValid_ = false;
  if (files_->size() <= startFileIndex) {
    return;
  }
  Status s = OpenLogReader(files_->at(startFileIndex).get());
  if (!s.ok()) {
    currentStatus_ = s;
    return;
  }
  while (RestrictedRead(&record, &scratch)) {
    if (record.size() < 12) {
      reporter_.Corruption(
        record.size(), Status::Corruption("very small log record"));
      continue;
    }
    UpdateCurrentWriteBatch(record);
    if (currentLastSeq_ >= startingSequenceNumber_) {
      if (strict && currentBatchSeq_ != startingSequenceNumber_) {
        currentStatus_ = Status::Corruption("Gap in sequence number. Could not "
                                            "seek to required sequence number");
        reporter_.Info(currentStatus_.ToString().c_str());
        return;
      } else if (strict) {
        reporter_.Info("Could seek required sequence number. Iterator will "
                       "continue.");
      }
      isValid_ = true;
      started_ = true;
      return;
    } else {
      isValid_ = false;
    }
  }
  if (strict) {
    currentStatus_ = Status::Corruption("Gap in sequence number. Could not "
                                        "seek to required sequence number");
    reporter_.Info(currentStatus_.ToString().c_str());
  } else if (files_->size() != 1) {
    currentStatus_ = Status::Corruption("Start sequence was not found, "
                                        "skipping to the next available");
    reporter_.Info(currentStatus_.ToString().c_str());
    NextImpl(true);
  }
}
void TransactionLogIteratorImpl::Next() {
  return NextImpl(false);
}
void TransactionLogIteratorImpl::NextImpl(bool internal) {
  std::string scratch;
  Slice record;
  isValid_ = false;
  if (!internal && !started_) {
    return SeekToStartSequence();
  }
  while(true) {
    assert(currentLogReader_);
    if (currentLogReader_->IsEOF()) {
      currentLogReader_->UnmarkEOF();
    }
    while (RestrictedRead(&record, &scratch)) {
      if (record.size() < 12) {
        reporter_.Corruption(
          record.size(), Status::Corruption("very small log record"));
        continue;
      } else {
        assert(internal || started_);
        assert(!internal || !started_);
        UpdateCurrentWriteBatch(record);
        if (internal && !started_) {
          started_ = true;
        }
        return;
      }
    }
    if (currentFileIndex_ < files_->size() - 1) {
      ++currentFileIndex_;
      Status status =OpenLogReader(files_->at(currentFileIndex_).get());
      if (!status.ok()) {
        isValid_ = false;
        currentStatus_ = status;
        return;
      }
    } else {
      isValid_ = false;
      if (currentLastSeq_ == dbimpl_->GetLatestSequenceNumber()) {
        currentStatus_ = Status::OK();
      } else {
        currentStatus_ = Status::Corruption("NO MORE DATA LEFT");
      }
      return;
    }
  }
}
bool TransactionLogIteratorImpl::IsBatchExpected(
    const WriteBatch* batch,
    const SequenceNumber expectedSeq) {
  assert(batch);
  SequenceNumber batchSeq = WriteBatchInternal::Sequence(batch);
  if (batchSeq != expectedSeq) {
    char buf[200];
    snprintf(buf, sizeof(buf),
             "Discontinuity in log records. Got seq=%lu, Expected seq=%lu, "
             "Last flushed seq=%lu.Log iterator will reseek the correct "
             "batch.",
             (unsigned long)batchSeq,
             (unsigned long)expectedSeq,
             (unsigned long)dbimpl_->GetLatestSequenceNumber());
    reporter_.Info(buf);
    return false;
  }
  return true;
}
void TransactionLogIteratorImpl::UpdateCurrentWriteBatch(const Slice& record) {
  std::unique_ptr<WriteBatch> batch(new WriteBatch());
  WriteBatchInternal::SetContents(batch.get(), record);
  SequenceNumber expectedSeq = currentLastSeq_ + 1;
  if (started_ && !IsBatchExpected(batch.get(), expectedSeq)) {
    if (expectedSeq < files_->at(currentFileIndex_)->StartSequence()) {
      if (currentFileIndex_ != 0) {
        currentFileIndex_--;
      }
    }
    startingSequenceNumber_ = expectedSeq;
    currentStatus_ = Status::NotFound("Gap in sequence numbers");
    return SeekToStartSequence(currentFileIndex_, true);
  }
  currentBatchSeq_ = WriteBatchInternal::Sequence(batch.get());
  currentLastSeq_ = currentBatchSeq_ +
                    WriteBatchInternal::Count(batch.get()) - 1;
  assert(currentLastSeq_ <= dbimpl_->GetLatestSequenceNumber());
  currentBatch_ = move(batch);
  isValid_ = true;
  currentStatus_ = Status::OK();
}
Status TransactionLogIteratorImpl::OpenLogReader(const LogFile* logFile) {
  unique_ptr<SequentialFile> file;
  Status status = OpenLogFile(logFile, &file);
  if (!status.ok()) {
    return status;
  }
  assert(file);
  currentLogReader_.reset(new log::Reader(std::move(file), &reporter_,
                                          read_options_.verify_checksums_, 0));
  return Status::OK();
}
}
