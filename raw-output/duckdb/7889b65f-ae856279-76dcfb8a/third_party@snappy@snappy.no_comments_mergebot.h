#ifndef THIRD_PARTY_SNAPPY_SNAPPY_H__
#define THIRD_PARTY_SNAPPY_SNAPPY_H__ 
#include <cstddef>
#include <stddef.h>
#include <stdint.h>
#include <string>
#include "snappy-stubs-public.h"
namespace duckdb_snappy {
  class Source;
  class Sink;
  size_t Compress(Source* source, Sink* sink);
  bool GetUncompressedLength(Source* source, uint32* result);
  size_t Compress(const char* input, size_t input_length, string* output);
  bool Uncompress(const char* compressed, size_t compressed_length,
                  string* uncompressed);
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
}
#endif
