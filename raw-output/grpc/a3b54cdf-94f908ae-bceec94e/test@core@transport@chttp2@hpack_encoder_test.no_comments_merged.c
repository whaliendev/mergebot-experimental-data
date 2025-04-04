#include "src/core/transport/chttp2/hpack_encoder.h"
#include <stdio.h>
#include <string.h>
#include <grpc/support/alloc.h>
#include <grpc/support/log.h>
#include <grpc/support/string_util.h>
<<<<<<< HEAD
#include "src/core/support/string.h"
#include "src/core/transport/chttp2/hpack_parser.h"
#include "src/core/transport/metadata.h"
||||||| bceec94ea4
#include "src/core/support/string.h"
#include "src/core/transport/chttp2/hpack_parser.h"
#include "src/core/transport/metadata.h"
#include <grpc/support/alloc.h>
#include <grpc/support/log.h>
#include <grpc/support/string_util.h>
=======
#include <grpc/support/alloc.h>
#include <grpc/support/log.h>
#include <grpc/support/string_util.h>
#include "src/core/support/string.h"
#include "src/core/transport/chttp2/hpack_parser.h"
#include "src/core/transport/metadata.h"
>>>>>>> 94f908ae
#include "test/core/util/parse_hexstring.h"
#include "test/core/util/slice_splitter.h"
#include "test/core/util/test_config.h"
#define TEST(x) run_test(x, #x)
grpc_chttp2_hpack_compressor g_compressor;
int g_failure = 0;
void **to_delete = NULL;
size_t num_to_delete = 0;
size_t cap_to_delete = 0;
static void verify(size_t window_available, int eof, size_t expect_window_used,
                   const char *expected, size_t nheaders, ...) {
  gpr_slice_buffer output;
  gpr_slice merged;
  gpr_slice expect = parse_hexstring(expected);
  size_t i;
  va_list l;
  grpc_linked_mdelem *e = gpr_malloc(sizeof(*e) * nheaders);
  grpc_metadata_batch b;
  grpc_metadata_batch_init(&b);
  va_start(l, nheaders);
  for (i = 0; i < nheaders; i++) {
    char *key = va_arg(l, char *);
    char *value = va_arg(l, char *);
    if (i) {
      e[i - 1].next = &e[i];
      e[i].prev = &e[i - 1];
    }
    e[i].md = grpc_mdelem_from_strings(key, value);
  }
  e[0].prev = NULL;
  e[nheaders - 1].next = NULL;
  va_end(l);
  b.list.head = &e[0];
  b.list.tail = &e[nheaders - 1];
  if (cap_to_delete == num_to_delete) {
    cap_to_delete = GPR_MAX(2 * cap_to_delete, 1000);
    to_delete = gpr_realloc(to_delete, sizeof(*to_delete) * cap_to_delete);
  }
  to_delete[num_to_delete++] = e;
  gpr_slice_buffer_init(&output);
  grpc_transport_one_way_stats stats;
  memset(&stats, 0, sizeof(stats));
  grpc_chttp2_encode_header(&g_compressor, 0xdeadbeef, &b, eof, &stats,
                            &output);
  merged = grpc_slice_merge(output.slices, output.count);
  gpr_slice_buffer_destroy(&output);
  grpc_metadata_batch_destroy(&b);
  if (0 != gpr_slice_cmp(merged, expect)) {
    char *expect_str = gpr_dump_slice(expect, GPR_DUMP_HEX | GPR_DUMP_ASCII);
    char *got_str = gpr_dump_slice(merged, GPR_DUMP_HEX | GPR_DUMP_ASCII);
    gpr_log(GPR_ERROR, "mismatched output for %s", expected);
    gpr_log(GPR_ERROR, "EXPECT: %s", expect_str);
    gpr_log(GPR_ERROR, "GOT:    %s", got_str);
    gpr_free(expect_str);
    gpr_free(got_str);
    g_failure = 1;
  }
  gpr_slice_unref(merged);
  gpr_slice_unref(expect);
}
static void test_basic_headers(void) {
  int i;
  verify(0, 0, 0, "000005 0104 deadbeef 40 0161 0161", 1, "a", "a");
  verify(0, 0, 0, "000001 0104 deadbeef be", 1, "a", "a");
  verify(0, 0, 0, "000001 0104 deadbeef be", 1, "a", "a");
  verify(0, 0, 0, "000006 0104 deadbeef be 40 0162 0163", 2, "a", "a", "b",
         "c");
  verify(0, 0, 0, "000002 0104 deadbeef bf be", 2, "a", "a", "b", "c");
  verify(0, 0, 0, "000004 0104 deadbeef 7f 00 0164", 1, "a", "d");
  for (i = 0; i < 350; i++) {
    verify(0, 0, 0, "000003 0104 deadbeef c0 bf be", 3, "a", "a", "b", "c", "a",
           "d");
  }
  verify(0, 0, 0, "000006 0104 deadbeef c0 00 016b 0176", 2, "a", "a", "k",
         "v");
  verify(0, 0, 0, "000004 0104 deadbeef 0f 2f 0176", 1, "a", "v");
}
static void encode_int_to_str(int i, char *p) {
  p[0] = (char)('a' + i % 26);
  i /= 26;
  GPR_ASSERT(i < 26);
  p[1] = (char)('a' + i);
  p[2] = 0;
}
static void test_decode_table_overflow(void) {
  int i;
  char key[3], value[3];
  char *expect;
  for (i = 0; i < 114; i++) {
    encode_int_to_str(i, key);
    encode_int_to_str(i + 1, value);
    if (i + 61 >= 127) {
      gpr_asprintf(&expect,
                   "000009 0104 deadbeef ff%02x 40 02%02x%02x 02%02x%02x",
                   i + 61 - 127, key[0], key[1], value[0], value[1]);
    } else if (i > 0) {
      gpr_asprintf(&expect,
                   "000008 0104 deadbeef %02x 40 02%02x%02x 02%02x%02x",
                   0x80 + 61 + i, key[0], key[1], value[0], value[1]);
    } else {
      gpr_asprintf(&expect, "000007 0104 deadbeef 40 02%02x%02x 02%02x%02x",
                   key[0], key[1], value[0], value[1]);
    }
    if (i > 0) {
      verify(0, 0, 0, expect, 2, "aa", "ba", key, value);
    } else {
      verify(0, 0, 0, expect, 1, key, value);
    }
    gpr_free(expect);
  }
  verify(0, 0, 0, "000007 0104 deadbeef 40 026161 026261", 1, "aa", "ba");
}
static void run_test(void (*test)(), const char *name) {
  gpr_log(GPR_INFO, "RUN TEST: %s", name);
  grpc_chttp2_hpack_compressor_init(&g_compressor);
  test();
  grpc_chttp2_hpack_compressor_destroy(&g_compressor);
}
int main(int argc, char **argv) {
  size_t i;
  grpc_test_only_set_metadata_hash_seed(0);
  grpc_test_init(argc, argv);
  grpc_init();
  TEST(test_basic_headers);
  TEST(test_decode_table_overflow);
  grpc_shutdown();
  for (i = 0; i < num_to_delete; i++) {
    gpr_free(to_delete[i]);
  }
  return g_failure;
}
