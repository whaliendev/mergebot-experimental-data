#ifndef THIRD_PARTY_SNAPPY_SNAPPY_INTERNAL_H_
#define THIRD_PARTY_SNAPPY_SNAPPY_INTERNAL_H_ 
#include "snappy-stubs-internal.h"
namespace duckdb_snappy {
namespace internal {
class WorkingMemory {
 public:
  explicit WorkingMemory(size_t input_size);
  ~WorkingMemory();
  uint16* GetHashTable(size_t fragment_size, int* table_size) const;
  char* GetScratchInput() const { return input_; }
  char* GetScratchOutput() const { return output_; }
 private:
  char* mem_;
  size_t size_;
  uint16* table_;
  char* input_;
  char* output_;
  WorkingMemory(const WorkingMemory&);
  void operator=(const WorkingMemory&);
};
char* CompressFragment(const char* input,
                       size_t input_length,
                       char* op,
                       uint16* table,
                       const int table_size);
#if !defined(SNAPPY_IS_BIG_ENDIAN) && \
    (defined(ARCH_K8) || defined(ARCH_PPC) || defined(ARCH_ARM))
static inline std::pair<size_t, bool> FindMatchLength(const char* s1,
                                                      const char* s2,
                                                      const char* s2_limit) {
  assert(s2_limit >= s2);
  size_t matched = 0;
  if (SNAPPY_PREDICT_TRUE(s2 <= s2_limit - 8)) {
    uint64 a1 = UNALIGNED_LOAD64(s1);
    uint64 a2 = UNALIGNED_LOAD64(s2);
    if (a1 != a2) {
      return std::pair<size_t, bool>(Bits::FindLSBSetNonZero64(a1 ^ a2) >> 3,
                                     true);
    } else {
      matched = 8;
      s2 += 8;
    }
  }
  while (SNAPPY_PREDICT_TRUE(s2 <= s2_limit - 8)) {
    if (UNALIGNED_LOAD64(s2) == UNALIGNED_LOAD64(s1 + matched)) {
      s2 += 8;
      matched += 8;
    } else {
      uint64 x = UNALIGNED_LOAD64(s2) ^ UNALIGNED_LOAD64(s1 + matched);
      int matching_bits = Bits::FindLSBSetNonZero64(x);
      matched += matching_bits >> 3;
      assert(matched >= 8);
      return std::pair<size_t, bool>(matched, false);
    }
  }
  while (SNAPPY_PREDICT_TRUE(s2 < s2_limit)) {
    if (s1[matched] == *s2) {
      ++s2;
      ++matched;
    } else {
      return std::pair<size_t, bool>(matched, matched < 8);
    }
  }
  return std::pair<size_t, bool>(matched, matched < 8);
}
#else
static inline std::pair<size_t, bool> FindMatchLength(const char* s1,
                                                      const char* s2,
                                                      const char* s2_limit) {
  assert(s2_limit >= s2);
  int matched = 0;
  while (s2 <= s2_limit - 4 &&
         UNALIGNED_LOAD32(s2) == UNALIGNED_LOAD32(s1 + matched)) {
    s2 += 4;
    matched += 4;
  }
  if (LittleEndian::IsLittleEndian() && s2 <= s2_limit - 4) {
    uint32 x = UNALIGNED_LOAD32(s2) ^ UNALIGNED_LOAD32(s1 + matched);
    int matching_bits = Bits::FindLSBSetNonZero(x);
    matched += matching_bits >> 3;
  } else {
    while ((s2 < s2_limit) && (s1[matched] == *s2)) {
      ++s2;
      ++matched;
    }
  }
  return std::pair<size_t, bool>(matched, matched < 8);
}
#endif
enum {
  LITERAL = 0,
  COPY_1_BYTE_OFFSET = 1,
  COPY_2_BYTE_OFFSET = 2,
  COPY_4_BYTE_OFFSET = 3
};
static const int kMaximumTagLength = 5;
static const uint16 char_table[256] = {
  0x0001, 0x0804, 0x1001, 0x2001, 0x0002, 0x0805, 0x1002, 0x2002,
  0x0003, 0x0806, 0x1003, 0x2003, 0x0004, 0x0807, 0x1004, 0x2004,
  0x0005, 0x0808, 0x1005, 0x2005, 0x0006, 0x0809, 0x1006, 0x2006,
  0x0007, 0x080a, 0x1007, 0x2007, 0x0008, 0x080b, 0x1008, 0x2008,
  0x0009, 0x0904, 0x1009, 0x2009, 0x000a, 0x0905, 0x100a, 0x200a,
  0x000b, 0x0906, 0x100b, 0x200b, 0x000c, 0x0907, 0x100c, 0x200c,
  0x000d, 0x0908, 0x100d, 0x200d, 0x000e, 0x0909, 0x100e, 0x200e,
  0x000f, 0x090a, 0x100f, 0x200f, 0x0010, 0x090b, 0x1010, 0x2010,
  0x0011, 0x0a04, 0x1011, 0x2011, 0x0012, 0x0a05, 0x1012, 0x2012,
  0x0013, 0x0a06, 0x1013, 0x2013, 0x0014, 0x0a07, 0x1014, 0x2014,
  0x0015, 0x0a08, 0x1015, 0x2015, 0x0016, 0x0a09, 0x1016, 0x2016,
  0x0017, 0x0a0a, 0x1017, 0x2017, 0x0018, 0x0a0b, 0x1018, 0x2018,
  0x0019, 0x0b04, 0x1019, 0x2019, 0x001a, 0x0b05, 0x101a, 0x201a,
  0x001b, 0x0b06, 0x101b, 0x201b, 0x001c, 0x0b07, 0x101c, 0x201c,
  0x001d, 0x0b08, 0x101d, 0x201d, 0x001e, 0x0b09, 0x101e, 0x201e,
  0x001f, 0x0b0a, 0x101f, 0x201f, 0x0020, 0x0b0b, 0x1020, 0x2020,
  0x0021, 0x0c04, 0x1021, 0x2021, 0x0022, 0x0c05, 0x1022, 0x2022,
  0x0023, 0x0c06, 0x1023, 0x2023, 0x0024, 0x0c07, 0x1024, 0x2024,
  0x0025, 0x0c08, 0x1025, 0x2025, 0x0026, 0x0c09, 0x1026, 0x2026,
  0x0027, 0x0c0a, 0x1027, 0x2027, 0x0028, 0x0c0b, 0x1028, 0x2028,
  0x0029, 0x0d04, 0x1029, 0x2029, 0x002a, 0x0d05, 0x102a, 0x202a,
  0x002b, 0x0d06, 0x102b, 0x202b, 0x002c, 0x0d07, 0x102c, 0x202c,
  0x002d, 0x0d08, 0x102d, 0x202d, 0x002e, 0x0d09, 0x102e, 0x202e,
  0x002f, 0x0d0a, 0x102f, 0x202f, 0x0030, 0x0d0b, 0x1030, 0x2030,
  0x0031, 0x0e04, 0x1031, 0x2031, 0x0032, 0x0e05, 0x1032, 0x2032,
  0x0033, 0x0e06, 0x1033, 0x2033, 0x0034, 0x0e07, 0x1034, 0x2034,
  0x0035, 0x0e08, 0x1035, 0x2035, 0x0036, 0x0e09, 0x1036, 0x2036,
  0x0037, 0x0e0a, 0x1037, 0x2037, 0x0038, 0x0e0b, 0x1038, 0x2038,
  0x0039, 0x0f04, 0x1039, 0x2039, 0x003a, 0x0f05, 0x103a, 0x203a,
  0x003b, 0x0f06, 0x103b, 0x203b, 0x003c, 0x0f07, 0x103c, 0x203c,
  0x0801, 0x0f08, 0x103d, 0x203d, 0x1001, 0x0f09, 0x103e, 0x203e,
  0x1801, 0x0f0a, 0x103f, 0x203f, 0x2001, 0x0f0b, 0x1040, 0x2040
};
}
static const int kBlockLog = 16;
static const size_t kBlockSize = 1 << kBlockLog;
static const int kMaxHashTableBits = 14;
static const size_t kMaxHashTableSize = 1 << kMaxHashTableBits;
}
#endif
