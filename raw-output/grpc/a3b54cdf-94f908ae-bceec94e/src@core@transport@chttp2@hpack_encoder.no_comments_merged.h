#ifndef GRPC_CORE_TRANSPORT_CHTTP2_HPACK_ENCODER_H
#define GRPC_CORE_TRANSPORT_CHTTP2_HPACK_ENCODER_H 
#include <grpc/support/port_platform.h>
#include <grpc/support/slice.h>
#include <grpc/support/slice_buffer.h>
<<<<<<< HEAD
#include "src/core/transport/chttp2/frame.h"
#include "src/core/transport/metadata.h"
#include "src/core/transport/metadata_batch.h"
#include "src/core/transport/transport.h"
||||||| bceec94ea4
=======
#include "src/core/transport/chttp2/frame.h"
#include "src/core/transport/metadata.h"
#include "src/core/transport/metadata_batch.h"
>>>>>>> 94f908ae
#define GRPC_CHTTP2_HPACKC_NUM_FILTERS 256
#define GRPC_CHTTP2_HPACKC_NUM_VALUES 256
#define GRPC_CHTTP2_HPACKC_INITIAL_TABLE_SIZE 4096
#define GRPC_CHTTP2_HPACKC_MAX_TABLE_SIZE (1024 * 1024)
typedef struct {
  uint32_t filter_elems_sum;
  uint32_t max_table_size;
  uint32_t max_table_elems;
  uint32_t cap_table_elems;
  uint8_t advertise_table_size_change;
  uint32_t max_usable_size;
  uint32_t tail_remote_index;
  uint32_t table_size;
  uint32_t table_elems;
  uint8_t filter_elems[GRPC_CHTTP2_HPACKC_NUM_FILTERS];
  grpc_mdstr *entries_keys[GRPC_CHTTP2_HPACKC_NUM_VALUES];
  grpc_mdelem *entries_elems[GRPC_CHTTP2_HPACKC_NUM_VALUES];
  uint32_t indices_keys[GRPC_CHTTP2_HPACKC_NUM_VALUES];
  uint32_t indices_elems[GRPC_CHTTP2_HPACKC_NUM_VALUES];
  uint16_t *table_elem_size;
} grpc_chttp2_hpack_compressor;
void grpc_chttp2_hpack_compressor_init(grpc_chttp2_hpack_compressor *c);
void grpc_chttp2_hpack_compressor_destroy(grpc_chttp2_hpack_compressor *c);
void grpc_chttp2_hpack_compressor_set_max_table_size(
    grpc_chttp2_hpack_compressor *c, uint32_t max_table_size);
void grpc_chttp2_hpack_compressor_set_max_usable_size(
    grpc_chttp2_hpack_compressor *c, uint32_t max_table_size);
void grpc_chttp2_encode_header(grpc_chttp2_hpack_compressor *c, uint32_t id,
                               grpc_metadata_batch *metadata, int is_eof,
                               grpc_transport_one_way_stats *stats,
                               gpr_slice_buffer *outbuf);
#endif
