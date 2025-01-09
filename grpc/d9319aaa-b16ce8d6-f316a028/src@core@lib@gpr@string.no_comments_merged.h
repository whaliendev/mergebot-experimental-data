#ifndef GRPC_CORE_LIB_GPR_STRING_H
#define GRPC_CORE_LIB_GPR_STRING_H 
#include <grpc/support/port_platform.h>
#include <grpc/impl/codegen/gpr_types.h>
#include <stdbool.h>
#include <stddef.h>
#define GPR_DUMP_HEX 0x00000001
#define GPR_DUMP_ASCII 0x00000002
char* gpr_dump(const char* buf, size_t len, uint32_t flags);
char* gpr_dump_return_len(const char* buf, size_t len, uint32_t flags,
                          size_t* out_len);
int gpr_parse_bytes_to_uint32(const char* data, size_t length,
                              uint32_t* result);
#define GPR_LTOA_MIN_BUFSIZE (3 * sizeof(long))
int gpr_ltoa(long value, char* output);
#define GPR_INT64TOA_MIN_BUFSIZE (3 * sizeof(int64_t))
int int64_ttoa(int64_t value, char* output);
int gpr_parse_nonnegative_int(const char* value);
void gpr_reverse_bytes(char* str, int len);
char* gpr_leftpad(const char* str, char flag, size_t length);
char* gpr_strjoin(const char** strs, size_t nstrs, size_t* total_length);
char* gpr_strjoin_sep(const char** strs, size_t nstrs, const char* sep,
                      size_t* total_length);
void gpr_string_split(const char* input, const char* sep, char*** strs,
                      size_t* nstrs);
char* gpr_format_timespec(gpr_timespec);
int gpr_stricmp(const char* a, const char* b);
int gpr_strincmp(const char* a, const char* b, size_t n);
void* gpr_memrchr(const void* s, int c, size_t n);
bool gpr_parse_bool_value(const char* value, bool* dst);
#endif
