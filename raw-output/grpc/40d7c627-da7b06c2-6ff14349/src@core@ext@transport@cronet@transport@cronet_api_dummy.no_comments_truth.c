#include <stdbool.h>
#include <grpc/support/log.h>
#include "third_party/objective_c/Cronet/bidirectional_stream_c.h"
#ifdef GRPC_COMPILE_WITH_CRONET
#else
bidirectional_stream* bidirectional_stream_create(
    stream_engine* engine, void* annotation,
    bidirectional_stream_callback* callback) {
  GPR_ASSERT(0);
  return NULL;
}
int bidirectional_stream_destroy(bidirectional_stream* stream) {
  GPR_ASSERT(0);
  return 0;
}
int bidirectional_stream_start(bidirectional_stream* stream, const char* url,
                               int priority, const char* method,
                               const bidirectional_stream_header_array* headers,
                               bool end_of_stream) {
  GPR_ASSERT(0);
  return 0;
}
int bidirectional_stream_read(bidirectional_stream* stream, char* buffer,
                              int capacity) {
  GPR_ASSERT(0);
  return 0;
}
int bidirectional_stream_write(bidirectional_stream* stream, const char* buffer,
                               int count, bool end_of_stream) {
  GPR_ASSERT(0);
  return 0;
}
void bidirectional_stream_cancel(bidirectional_stream* stream) {
  GPR_ASSERT(0);
}
#endif
