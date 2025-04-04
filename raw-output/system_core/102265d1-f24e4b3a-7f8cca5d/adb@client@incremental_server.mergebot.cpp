/*
 * Copyright (C) 2020 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define TRACE_TAG INCREMENTAL
#include "incremental_server.h"
#include <android-base/endian.h>
#include <android-base/strings.h>
#include <inttypes.h>
#include <lz4.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <array>
#include <deque>
#include <fstream>
#include <thread>
#include <type_traits>
#include <unordered_set>
#include "adb.h"
#include "adb_io.h"
#include "adb_trace.h"
#include "adb_unique_fd.h"
#include "adb_utils.h"
#include "incremental_utils.h"
#include "sysdeps.h"

namespace incremental {

static constexpr int kBlockSize = 4096;
static constexpr int kCompressedSizeMax = kBlockSize * 0.95;
static constexpr int8_t kTypeData = 0;
static constexpr int8_t kCompressionNone = 0;
static constexpr int8_t kCompressionLZ4 = 1;
static constexpr int kCompressBound = std::max(kBlockSize, LZ4_COMPRESSBOUND(kBlockSize));
static constexpr auto kReadBufferSize = 128 * 1024;
static constexpr int kPollTimeoutMillis = 300000;                                                   // 5 minutes
using BlockSize = int16_t;
using FileId = int16_t;
using BlockIdx = int32_t;
using NumBlocks = int32_t;
using BlockType = int8_t;
using CompressionType = int8_t;
using RequestType = int16_t;
using ChunkHeader = int32_t;
using MagicType = uint32_t;

static constexpr MagicType INCR = 0x494e4352;                                               // LE INCR
static constexpr RequestType SERVING_COMPLETE = 0;
static constexpr RequestType BLOCK_MISSING = 1;
static constexpr RequestType PREFETCH = 2;
static constexpr RequestType DESTROY = 3;

static constexpr inline int64_t roundDownToBlockOffset(int64_t val) {
    return val & ~(kBlockSize - 1);
}

static constexpr inline int64_t roundUpToBlockOffset(int64_t val) {
    return roundDownToBlockOffset(val + kBlockSize - 1);
}

static constexpr inline NumBlocks numBytesToNumBlocks(int64_t bytes) {
    return roundUpToBlockOffset(bytes) / kBlockSize;
}

static constexpr inline off64_t blockIndexToOffset(BlockIdx blockIdx) {
    return static_cast<off64_t>(blockIdx) * kBlockSize;
}

template <typename T>
static inline constexpr T toBigEndian(T t) {
    using unsigned_type = std::make_unsigned_t<T>;
    if constexpr (std::is_same_v<T, int16_t>) {
        return htobe16(static_cast<unsigned_type>(t));
    } else if constexpr (std::is_same_v<T, int32_t>) {
        return htobe32(static_cast<unsigned_type>(t));
    } else if constexpr (std::is_same_v<T, int64_t>) {
        return htobe64(static_cast<unsigned_type>(t));
    } else {
        return t;
    }
}

template <typename T>
static inline constexpr T readBigEndian(void* data) {
    using unsigned_type = std::make_unsigned_t<T>;
    if constexpr (std::is_same_v<T, int16_t>) {
        return static_cast<T>(be16toh(*reinterpret_cast<unsigned_type*>(data)));
    } else if constexpr (std::is_same_v<T, int32_t>) {
        return static_cast<T>(be32toh(*reinterpret_cast<unsigned_type*>(data)));
    } else if constexpr (std::is_same_v<T, int64_t>) {
        return static_cast<T>(be64toh(*reinterpret_cast<unsigned_type*>(data)));
    } else {
        return T();
    }
}

// Placed before actual data bytes of each block
struct ResponseHeader {
    FileId file_id;                    // 2 bytes
    CompressionType compression_type;  // 2 bytes
    BlockIdx block_idx;                // 4 bytes
    BlockSize block_size;              // 2 bytes
} __attribute__((packed));

// Placed before actual data bytes of each block
struct ResponseHeader {
    FileId file_id;                    // 2 bytes
    CompressionType compression_type;  // 2 bytes
    BlockIdx block_idx;                // 4 bytes
    BlockSize block_size;              // 2 bytes
} __attribute__((packed));

// Holds streaming state for a file
class File {
public:
    // Plain file
    File(const char* filepath, FileId id, int64_t size, unique_fd fd) : File(filepath, id, size) {
        this->fd_ = std::move(fd);
        priority_blocks_ = PriorityBlocksForFile(filepath, fd_.get(), size);
    }
    int64_t ReadBlock(BlockIdx block_idx, void* buf, bool* is_zip_compressed,
                      std::string* error) const {
        char* buf_ptr = static_cast<char*>(buf);
        int64_t bytes_read = -1;
        const off64_t offsetStart = blockIndexToOffset(block_idx);
        bytes_read = adb_pread(fd_, &buf_ptr[sizeof(ResponseHeader)], kBlockSize, offsetStart);
        return bytes_read;
    }
    
    const unique_fd& RawFd() const { return fd_; }
    
    const std::vector<BlockIdx>& PriorityBlocks() const { return priority_blocks_; }
    
    std::vector<bool> sentBlocks;
    NumBlocks sentBlocksCount = 0;
    
    const char* const filepath;
    const FileId id;
    const int64_t size;
    
private:
    File(const char* filepath, FileId id, int64_t size) : filepath(filepath), id(id), size(size) {
        sentBlocks.resize(numBytesToNumBlocks(size));
    }
    unique_fd fd_;
    std::vector<BlockIdx> priority_blocks_;
};

class IncrementalServer {
public:
    IncrementalServer(unique_fd fd, unique_fd adb_fd, unique_fd output_fd, std::vector<File> files): adb_fd_(std::move(fd)), adb_fd_(std::move(adb_fd)), output_fd_(std::move(output_fd)), files_(std::move(files)) {
        buffer_.reserve(kReadBufferSize);
    }
    
    bool Serve();
    
private:
    struct PrefetchState {
        const File* file;
        BlockIdx overallIndex = 0;
        BlockIdx overallEnd = 0;
        BlockIdx priorityIndex = 0;
    
        explicit PrefetchState(const File& f, BlockIdx start, int count)
            : file(&f),
              overallIndex(start),
              overallEnd(std::min<BlockIdx>(start + count, f.sentBlocks.size())) {}
    
        explicit PrefetchState(const File& f)
            : PrefetchState(f, 0, (BlockIdx)f.sentBlocks.size()) {}
    
        bool done() const {
            const bool overallSent = (overallIndex >= overallEnd);
            if (file->PriorityBlocks().empty()) {
                return overallSent;
            }
            return overallSent && (priorityIndex >= (BlockIdx)file->PriorityBlocks().size());
        }
    };
    
    bool SkipToRequest(void* buffer, size_t* size, bool blocking);
    std::optional<RequestCommand> ReadRequest(bool blocking);
    
    void erase_buffer_head(int count) { buffer_.erase(buffer_.begin(), buffer_.begin() + count); }
    
    enum class SendResult { Sent, Skipped, Error };
    SendResult SendBlock(FileId fileId, BlockIdx blockIdx, bool flush = false);
    bool SendDone();
    void RunPrefetching();
    
    void Send(const void* data, size_t size, bool flush);
    void Flush();
    using TimePoint = decltype(std::chrono::high_resolution_clock::now());
    bool ServingComplete(std::optional<TimePoint> startTime, int missesCount, int missesSent);
    
    unique_fd const adb_fd_;
    unique_fd const output_fd_;
    std::vector<File> files_;
    
    // Incoming data buffer.
    std::vector<char> buffer_;
    
    std::deque<PrefetchState> prefetches_;
    int compressed_ = 0, uncompressed_ = 0;
    long long sentSize_ = 0;
    
    std::vector<char> pendingBlocks_;
    
    // True when client notifies that all the data has been received
    bool servingComplete_;
};

bool IncrementalServer::SkipToRequest(void* buffer, size_t* size, bool blocking) {
    while (true) {
        // Looking for INCR magic.
        bool magic_found = false;
        int bcur = 0;
        int bsize = buffer_.size();
        for (bcur = 0; bcur + 4 < bsize; ++bcur) {
            uint32_t magic = be32toh(*(uint32_t*)(buffer_.data() + bcur));
            if (magic == INCR) {
                magic_found = true;
                break;
            }
        }

        if (bcur > 0) {
            // output the rest.
            WriteFdExactly(output_fd_, buffer_.data(), bcur);
            erase_buffer_head(bcur);
        }

        if (magic_found && buffer_.size() >= *size + sizeof(INCR)) {
            // fine, return
            memcpy(buffer, buffer_.data() + sizeof(INCR), *size);
            erase_buffer_head(*size + sizeof(INCR));
            return true;
        }

        adb_pollfd pfd = {adb_fd_.get(), POLLIN, 0};
        auto res = adb_poll(&pfd, 1, blocking ? kPollTimeoutMillis : 0);

        if (res != 1) {
            WriteFdExactly(output_fd_, buffer_.data(), buffer_.size());
            if (res < 0) {
                D("Failed to poll: %s\n", strerror(errno));
                return false;
            }
            if (blocking) {
                fprintf(stderr, "Timed out waiting for data from device.\n");
            }
            if (blocking && servingComplete_) {
                // timeout waiting from client. Serving is complete, so quit.
                return false;
            }
            *size = 0;
            return true;
        }

        bsize = buffer_.size();
        buffer_.resize(kReadBufferSize);
        int r = adb_read(adb_fd_, buffer_.data() + bsize, kReadBufferSize - bsize);
        if (r > 0) {
            buffer_.resize(bsize + r);
            continue;
        }

        D("Failed to read from fd %d: %d. Exit\n", adb_fd_.get(), errno);
        break;
    }
    // socket is closed. print remaining messages
    WriteFdExactly(output_fd_, buffer_.data(), buffer_.size());
    return false;
}

std::optional<RequestCommand> IncrementalServer::ReadRequest(bool blocking) {
    uint8_t commandBuf[sizeof(RequestCommand)];
    auto size = sizeof(commandBuf);
    if (!SkipToRequest(&commandBuf, &size, blocking)) {
        return {{DESTROY}};
    }
    if (size < sizeof(RequestCommand)) {
        return {};
    }
    RequestCommand request;
    request.request_type = readBigEndian<RequestType>(&commandBuf[0]);
    request.file_id = readBigEndian<FileId>(&commandBuf[2]);
    request.block_idx = readBigEndian<BlockIdx>(&commandBuf[4]);
    return request;
}

auto IncrementalServer::SendBlock(FileId fileId, BlockIdx blockIdx, bool flush) -> SendResult {
    auto& file = files_[fileId];
    if (blockIdx >= static_cast<long>(file.sentBlocks.size())) {
        fprintf(stderr, "Failed to read file %s at block %" PRId32 " (past end).\n", file.filepath,
                blockIdx);
        return SendResult::Error;
    }
    if (file.sentBlocks[blockIdx]) {
        return SendResult::Skipped;
    }
    std::string error;
    char raw[sizeof(ResponseHeader) + kBlockSize];
    bool isZipCompressed = false;
    const int64_t bytesRead = file.ReadBlock(blockIdx, &raw, &isZipCompressed, &error);
    if (bytesRead < 0) {
        fprintf(stderr, "Failed to get data for %s at blockIdx=%d (%s).\n", file.filepath, blockIdx,
                error.c_str());
        return SendResult::Error;
    }

    ResponseHeader* header = nullptr;
    char data[sizeof(ResponseHeader) + kCompressBound];
    char* compressed = data + sizeof(*header);
    int16_t compressedSize = 0;
    if (!isZipCompressed) {
        compressedSize =
                LZ4_compress_default(raw + sizeof(*header), compressed, bytesRead, kCompressBound);
    }
    int16_t blockSize;
    if (compressedSize > 0 && compressedSize < kCompressedSizeMax) {
        ++compressed_;
        blockSize = compressedSize;
        header = reinterpret_cast<ResponseHeader*>(data);
        header->compression_type = kCompressionLZ4;
    } else {
        ++uncompressed_;
        blockSize = bytesRead;
        header = reinterpret_cast<ResponseHeader*>(raw);
        header->compression_type = kCompressionNone;
    }

    header->block_type = kTypeData;

    header->file_id = toBigEndian(fileId);
    header->block_size = toBigEndian(blockSize);
    header->block_idx = toBigEndian(blockIdx);

    file.sentBlocks[blockIdx] = true;
    file.sentBlocksCount += 1;
    Send(header, sizeof(*header) + blockSize, flush);
    return SendResult::Sent;
}

bool IncrementalServer::SendDone() {
    ResponseHeader header;
    header.file_id = -1;
    header.block_type = 0;
    header.compression_type = 0;
    header.block_idx = 0;
    header.block_size = 0;
    Send(&header, sizeof(header), true);
    return true;
}

void IncrementalServer::RunPrefetching() {
    constexpr auto kPrefetchBlocksPerIteration = 128;

    int blocksToSend = kPrefetchBlocksPerIteration;
    while (!prefetches_.empty() && blocksToSend > 0) {
        auto& prefetch = prefetches_.front();
        const auto& file = *prefetch.file;
        const auto& priority_blocks = file.PriorityBlocks();
        if (!priority_blocks.empty()) {
            for (auto& i = prefetch.priorityIndex;
                 blocksToSend > 0 && i < (BlockIdx)priority_blocks.size(); ++i) {
                if (auto res = SendBlock(file.id, priority_blocks[i]); res == SendResult::Sent) {
                    --blocksToSend;
                } else if (res == SendResult::Error) {
                    fprintf(stderr, "Failed to send priority block %" PRId32 "\n", i);
                }
            }
        }
        for (auto& i = prefetch.overallIndex; blocksToSend > 0 && i < prefetch.overallEnd; ++i) {
            if (auto res = SendBlock(file.id, i); res == SendResult::Sent) {
                --blocksToSend;
            } else if (res == SendResult::Error) {
                fprintf(stderr, "Failed to send block %" PRId32 "\n", i);
            }
        }
        if (prefetch.done()) {
            prefetches_.pop_front();
        }
    }
}

void IncrementalServer::Send(const void* data, size_t size, bool flush) {
    constexpr auto kChunkFlushSize = 31 * kBlockSize;

    if (pendingBlocks_.empty()) {
        pendingBlocks_.resize(sizeof(ChunkHeader));
    }
    pendingBlocks_.insert(pendingBlocks_.end(), static_cast<const char*>(data),
                          static_cast<const char*>(data) + size);
    if (flush || pendingBlocks_.size() > kChunkFlushSize) {
        Flush();
    }
}

void IncrementalServer::Flush() {
    if (pendingBlocks_.empty()) {
        return;
    }

    *(ChunkHeader*)pendingBlocks_.data() =
            toBigEndian<int32_t>(pendingBlocks_.size() - sizeof(ChunkHeader));
    if (!WriteFdExactly(adb_fd_, pendingBlocks_.data(), pendingBlocks_.size())) {
        fprintf(stderr, "Failed to write %d bytes\n", int(pendingBlocks_.size()));
    }
    sentSize_ += pendingBlocks_.size();
    pendingBlocks_.clear();
}

bool IncrementalServer::Exit(std::optional<TimePoint> startTime, int missesCount, int missesSent) {
    servingComplete_ = true;
    using namespace std::chrono;
    auto endTime = high_resolution_clock::now();
    D("Streaming completed.\n"
      "Misses: %d, of those unique: %d; sent compressed: %d, uncompressed: "
      "%d, mb: %.3f\n"
      "Total time taken: %.3fms\n",
      missesCount, missesSent, compressed_, uncompressed_, sentSize_ / 1024.0 / 1024.0,
      duration_cast<microseconds>(endTime - (startTime ? *startTime : endTime)).count() / 1000.0);
    return true;
}

Result wait_for_installation(int read_fd) {
    static constexpr int maxMessageSize = 256;
    std::vector<char> child_stdout(CHUNK_SIZE);
    int bytes_read;
    int buf_size = 0;
    // TODO(b/150865433): optimize child's output parsing
    while ((bytes_read = adb_read(read_fd, child_stdout.data() + buf_size,
                                  child_stdout.size() - buf_size)) > 0) {
        // print to parent's stdout
        fprintf(stdout, "%.*s", bytes_read, child_stdout.data() + buf_size);

        buf_size += bytes_read;
        const std::string_view stdout_str(child_stdout.data(), buf_size);
        // wait till installation either succeeds or fails
        if (stdout_str.find("Success") != std::string::npos) {
            return Result::Success;
        }
        // on failure, wait for full message
        static constexpr auto failure_msg_head = "Failure ["sv;
        if (const auto begin_itr = stdout_str.find(failure_msg_head);
            begin_itr != std::string::npos) {
            if (buf_size >= maxMessageSize) {
                return Result::Failure;
            }
            const auto end_itr = stdout_str.rfind("]");
            if (end_itr != std::string::npos && end_itr >= begin_itr + failure_msg_head.size()) {
                return Result::Failure;
            }
        }
        child_stdout.resize(buf_size + CHUNK_SIZE);
    }
    return Result::None;
}

Result wait_for_installation(int read_fd) {
    static constexpr int maxMessageSize = 256;
    std::vector<char> child_stdout(CHUNK_SIZE);
    int bytes_read;
    int buf_size = 0;
    // TODO(b/150865433): optimize child's output parsing
    while ((bytes_read = adb_read(read_fd, child_stdout.data() + buf_size,
                                  child_stdout.size() - buf_size)) > 0) {
        // print to parent's stdout
        fprintf(stdout, "%.*s", bytes_read, child_stdout.data() + buf_size);

        buf_size += bytes_read;
        const std::string_view stdout_str(child_stdout.data(), buf_size);
        // wait till installation either succeeds or fails
        if (stdout_str.find("Success") != std::string::npos) {
            return Result::Success;
        }
        // on failure, wait for full message
        static constexpr auto failure_msg_head = "Failure ["sv;
        if (const auto begin_itr = stdout_str.find(failure_msg_head);
            begin_itr != std::string::npos) {
            if (buf_size >= maxMessageSize) {
                return Result::Failure;
            }
            const auto end_itr = stdout_str.rfind("]");
            if (end_itr != std::string::npos && end_itr >= begin_itr + failure_msg_head.size()) {
                return Result::Failure;
            }
        }
        child_stdout.resize(buf_size + CHUNK_SIZE);
    }
    return Result::None;
}

} // namespace incremental
