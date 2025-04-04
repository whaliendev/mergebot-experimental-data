#ifndef THIRD_PARTY_SNAPPY_SNAPPY_H__
#define THIRD_PARTY_SNAPPY_SNAPPY_H__ 
#include <stddef.h>
#include <stdint.h>
#include <string>
#include "snappy-stubs-public.h"
namespace duckdb_snappy {
  class Source;
  class Sink;
  size_t Compress(Source* source, Sink* sink);
  bool GetUncompressedLength(Source* source, uint32_t* result);
  size_t Compress(const char* input, size_t input_length,
                  std::string* compressed);
  bool Uncompress(const char* compressed, size_t compressed_length,
                  std::string* uncompressed);
  bool Uncompress(Source* compressed, Sink* uncompressed);
  size_t UncompressAsMuchAsPossible(Source* compressed, Sink* uncompressed);
  void RawCompress(const char* input,
                   size_t input_length,
                   char* compressed,
                   size_t* compressed_length);
  bool RawUncompress(const char* compressed, size_t compressed_length,
                     char* uncompressed);
  bool RawUncompress(Source* compressed, char* uncompressed);
  bool RawUncompressToIOVec(const char* compressed, size_t compressed_length,
                            const struct iovec* iov, size_t iov_cnt);
  bool RawUncompressToIOVec(Source* compressed, const struct iovec* iov,
                            size_t iov_cnt);
  size_t MaxCompressedLength(size_t source_bytes);
  bool GetUncompressedLength(const char* compressed, size_t compressed_length,
                             size_t* result);
  bool IsValidCompressedBuffer(const char* compressed,
                               size_t compressed_length);
  bool IsValidCompressed(Source* compressed);
<<<<<<< HEAD
  static constexpr int kBlockLog = 16;
  static constexpr size_t kBlockSize = 1 << kBlockLog;
  static constexpr int kMinHashTableBits = 8;
  static constexpr size_t kMinHashTableSize = 1 << kMinHashTableBits;
  static constexpr int kMaxHashTableBits = 14;
  static constexpr size_t kMaxHashTableSize = 1 << kMaxHashTableBits;
}
||||||| 76dcfb8acf
}
=======
}
>>>>>>> ae856279
#endif
