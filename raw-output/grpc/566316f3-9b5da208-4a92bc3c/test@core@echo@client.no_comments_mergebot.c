#include <grpc/grpc.h>
#include <string.h>
#include <grpc/support/log.h>
#include <grpc/support/time.h>
#include <grpc/byte_buffer.h>
#include "test/core/util/test_config.h"
enum {WRITE_SLICE_LENGTH = 1024, TOTAL_BYTES = 102400};
static void start_write_next_slice(grpc_call *call, int first, int length) {
  int i = 0;
  grpc_byte_buffer *byte_buffer = NULL;
  gpr_slice slice = gpr_slice_malloc(length);
  for (i = 0; i < length; i++)
    GPR_SLICE_START_PTR(slice)[i] = (first + i) % 256;
  byte_buffer = grpc_byte_buffer_create(&slice, 1);
  GPR_ASSERT(grpc_call_start_write_old(call, byte_buffer, (void *)1, 0) ==
             GRPC_CALL_OK);
  gpr_slice_unref(slice);
  grpc_byte_buffer_destroy(byte_buffer);
}
int main(int argc, char **argv) {
  grpc_channel *channel = NULL;
  grpc_call *call = NULL;
  grpc_event *ev = NULL;
  grpc_byte_buffer_reader *bb_reader = NULL;
  grpc_completion_queue *cq = NULL;
  int bytes_written = 0;
  int bytes_read = 0;
  unsigned i = 0;
  int waiting_finishes;
  gpr_slice read_slice;
  grpc_test_init(argc, argv);
  grpc_init();
  cq = grpc_completion_queue_create();
  GPR_ASSERT(argc == 2);
  channel = grpc_channel_create(argv[1], NULL);
<<<<<<< HEAD
  call = grpc_channel_create_call(
      channel, "/foo", "localhost",
      gpr_time_add(gpr_time_from_seconds(5), gpr_now()));
  GPR_ASSERT(grpc_call_invoke(call, cq, (void *)1, (void *)1, 0) ==
||||||| 4a92bc3c69
  call = grpc_channel_create_call(channel, "/foo", "localhost",
                                  gpr_time_add(gpr_time_from_seconds(5),
                                               gpr_now()));
  GPR_ASSERT(grpc_call_invoke(call, cq, (void *)1, (void *)1, 0) ==
=======
  call = grpc_channel_create_call_old(
      channel, "/foo", "localhost",
      gpr_time_add(gpr_time_from_seconds(5), gpr_now()));
  GPR_ASSERT(grpc_call_invoke_old(call, cq, (void *)1, (void *)1, 0) ==
>>>>>>> 9b5da208
             GRPC_CALL_OK);
  start_write_next_slice(call, bytes_written, WRITE_SLICE_LENGTH);
  bytes_written += WRITE_SLICE_LENGTH;
  GPR_ASSERT(grpc_call_start_read_old(call, (void *)1) == GRPC_CALL_OK);
  waiting_finishes = 2;
  while (waiting_finishes) {
    ev = grpc_completion_queue_next(cq, gpr_inf_future);
    switch (ev->type) {
      case GRPC_WRITE_ACCEPTED:
        if (bytes_written < TOTAL_BYTES) {
          start_write_next_slice(call, bytes_written, WRITE_SLICE_LENGTH);
          bytes_written += WRITE_SLICE_LENGTH;
        } else {
          GPR_ASSERT(grpc_call_writes_done_old(call, (void *)1) ==
                     GRPC_CALL_OK);
        }
        break;
      case GRPC_CLIENT_METADATA_READ:
        break;
      case GRPC_READ:
        bb_reader = grpc_byte_buffer_reader_create(ev->data.read);
        while (grpc_byte_buffer_reader_next(bb_reader, &read_slice)) {
          for (i = 0; i < GPR_SLICE_LENGTH(read_slice); i++) {
            GPR_ASSERT(GPR_SLICE_START_PTR(read_slice)[i] == bytes_read % 256);
            bytes_read++;
          }
          gpr_slice_unref(read_slice);
        }
        grpc_byte_buffer_reader_destroy(bb_reader);
        if (bytes_read < TOTAL_BYTES) {
          GPR_ASSERT(grpc_call_start_read_old(call, (void *)1) == GRPC_CALL_OK);
        }
        break;
      case GRPC_FINISHED:
      case GRPC_FINISH_ACCEPTED:
        waiting_finishes--;
        break;
      default:
        GPR_ASSERT(0 && "unexpected event");
        break;
    }
    grpc_event_finish(ev);
  }
  GPR_ASSERT(bytes_read == TOTAL_BYTES);
  gpr_log(GPR_INFO, "All data have been successfully echoed");
  grpc_call_destroy(call);
  grpc_channel_destroy(channel);
  grpc_completion_queue_destroy(cq);
  grpc_shutdown();
  return 0;
}
