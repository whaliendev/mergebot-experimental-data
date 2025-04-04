#ifndef GRPC_CORE_TRANSPORT_CHTTP2_FRAME_RST_STREAM_H
#define GRPC_CORE_TRANSPORT_CHTTP2_FRAME_RST_STREAM_H 
#include <grpc/support/slice.h>
#include "src/core/iomgr/exec_ctx.h"
<<<<<<< HEAD
#include "src/core/transport/chttp2/frame.h"
#include "src/core/transport/transport.h"
||||||| bceec94ea4
=======
#include "src/core/transport/chttp2/frame.h"
>>>>>>> 94f908ae
typedef struct {
  uint8_t byte;
  uint8_t reason_bytes[4];
} grpc_chttp2_rst_stream_parser;
gpr_slice grpc_chttp2_rst_stream_create(uint32_t stream_id, uint32_t code,
                                        grpc_transport_one_way_stats *stats);
grpc_chttp2_parse_error grpc_chttp2_rst_stream_parser_begin_frame(
    grpc_chttp2_rst_stream_parser *parser, uint32_t length, uint8_t flags);
grpc_chttp2_parse_error grpc_chttp2_rst_stream_parser_parse(
    grpc_exec_ctx *exec_ctx, void *parser,
    grpc_chttp2_transport_parsing *transport_parsing,
    grpc_chttp2_stream_parsing *stream_parsing, gpr_slice slice, int is_last);
#endif
