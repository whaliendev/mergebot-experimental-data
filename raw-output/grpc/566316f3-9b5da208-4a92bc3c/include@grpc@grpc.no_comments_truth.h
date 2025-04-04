#ifndef __GRPC_GRPC_H__
#define __GRPC_GRPC_H__ 
#include <grpc/status.h>
#include <stddef.h>
#include <grpc/support/slice.h>
#include <grpc/support/time.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct grpc_completion_queue grpc_completion_queue;
typedef struct grpc_channel grpc_channel;
typedef struct grpc_server grpc_server;
typedef struct grpc_call grpc_call;
typedef enum {
  GRPC_ARG_STRING,
  GRPC_ARG_INTEGER,
  GRPC_ARG_POINTER
} grpc_arg_type;
typedef struct {
  grpc_arg_type type;
  char *key;
  union {
    char *string;
    int integer;
    struct {
      void *p;
      void *(*copy)(void *p);
      void (*destroy)(void *p);
    } pointer;
  } value;
} grpc_arg;
typedef struct {
  size_t num_args;
  grpc_arg *args;
} grpc_channel_args;
#define GRPC_ARG_ENABLE_CENSUS "grpc.census"
#define GRPC_ARG_MAX_CONCURRENT_STREAMS "grpc.max_concurrent_streams"
#define GRPC_ARG_MAX_MESSAGE_LENGTH "grpc.max_message_length"
typedef enum grpc_call_error {
  GRPC_CALL_OK = 0,
  GRPC_CALL_ERROR,
  GRPC_CALL_ERROR_NOT_ON_SERVER,
  GRPC_CALL_ERROR_NOT_ON_CLIENT,
  GRPC_CALL_ERROR_ALREADY_ACCEPTED,
  GRPC_CALL_ERROR_ALREADY_INVOKED,
  GRPC_CALL_ERROR_NOT_INVOKED,
  GRPC_CALL_ERROR_ALREADY_FINISHED,
  GRPC_CALL_ERROR_TOO_MANY_OPERATIONS,
  GRPC_CALL_ERROR_INVALID_FLAGS
} grpc_call_error;
typedef enum grpc_op_error {
  GRPC_OP_OK = 0,
  GRPC_OP_ERROR
} grpc_op_error;
#define GRPC_WRITE_BUFFER_HINT (0x00000001u)
#define GRPC_WRITE_NO_COMPRESS (0x00000002u)
struct grpc_byte_buffer;
typedef struct grpc_byte_buffer grpc_byte_buffer;
grpc_byte_buffer *grpc_byte_buffer_create(gpr_slice *slices, size_t nslices);
grpc_byte_buffer *grpc_byte_buffer_copy(grpc_byte_buffer *bb);
size_t grpc_byte_buffer_length(grpc_byte_buffer *bb);
void grpc_byte_buffer_destroy(grpc_byte_buffer *byte_buffer);
struct grpc_byte_buffer_reader;
typedef struct grpc_byte_buffer_reader grpc_byte_buffer_reader;
grpc_byte_buffer_reader *grpc_byte_buffer_reader_create(
    grpc_byte_buffer *buffer);
int grpc_byte_buffer_reader_next(grpc_byte_buffer_reader *reader,
                                 gpr_slice *slice);
void grpc_byte_buffer_reader_destroy(grpc_byte_buffer_reader *reader);
typedef struct grpc_metadata {
  char *key;
  char *value;
  size_t value_length;
} grpc_metadata;
typedef enum grpc_completion_type {
  GRPC_QUEUE_SHUTDOWN,
  GRPC_IOREQ,
  GRPC_READ,
  GRPC_WRITE_ACCEPTED,
  GRPC_FINISH_ACCEPTED,
  GRPC_CLIENT_METADATA_READ,
  GRPC_FINISHED,
  GRPC_SERVER_RPC_NEW,
  GRPC_SERVER_SHUTDOWN,
  GRPC_COMPLETION_DO_NOT_USE
} grpc_completion_type;
typedef struct grpc_event {
  grpc_completion_type type;
  void *tag;
  grpc_call *call;
  union {
    grpc_byte_buffer *read;
    grpc_op_error write_accepted;
    grpc_op_error finish_accepted;
    grpc_op_error invoke_accepted;
    grpc_op_error ioreq;
    struct {
      size_t count;
      grpc_metadata *elements;
    } client_metadata_read;
    struct {
      grpc_status_code status;
      const char *details;
      size_t metadata_count;
      grpc_metadata *metadata_elements;
    } finished;
    struct {
      const char *method;
      const char *host;
      gpr_timespec deadline;
      size_t metadata_count;
      grpc_metadata *metadata_elements;
    } server_rpc_new;
  } data;
} grpc_event;
typedef struct {
  size_t count;
  size_t capacity;
  grpc_metadata *metadata;
} grpc_metadata_array;
typedef struct {
  const char *method;
  const char *host;
  gpr_timespec deadline;
} grpc_call_details;
typedef enum {
  GRPC_OP_SEND_INITIAL_METADATA = 0,
  GRPC_OP_SEND_MESSAGE,
  GRPC_OP_SEND_CLOSE_FROM_CLIENT,
  GRPC_OP_SEND_STATUS_FROM_SERVER,
  GRPC_OP_RECV_INITIAL_METADATA,
  GRPC_OP_RECV_MESSAGES,
  GRPC_OP_RECV_STATUS_ON_CLIENT,
  GRPC_OP_RECV_CLOSE_ON_SERVER
} grpc_op_type;
typedef struct grpc_op {
  grpc_op_type op;
  union {
    struct {
      size_t count;
      const grpc_metadata *metadata;
    } send_initial_metadata;
    grpc_byte_buffer *send_message;
    struct {
      size_t trailing_metadata_count;
      grpc_metadata *trailing_metadata;
      grpc_status_code status;
      const char *status_details;
    } send_status_from_server;
    grpc_metadata_array *recv_initial_metadata;
    grpc_byte_buffer **recv_message;
    struct {
      grpc_metadata_array *trailing_metadata;
      grpc_status_code *status;
      char **status_details;
      size_t *status_details_capacity;
    } recv_status_on_client;
    struct {
      int *cancelled;
    } recv_close_on_server;
  } data;
} grpc_op;
void grpc_init(void);
void grpc_shutdown(void);
grpc_completion_queue *grpc_completion_queue_create(void);
grpc_event *grpc_completion_queue_next(grpc_completion_queue *cq,
                                       gpr_timespec deadline);
grpc_event *grpc_completion_queue_pluck(grpc_completion_queue *cq, void *tag,
                                        gpr_timespec deadline);
void grpc_event_finish(grpc_event *event);
void grpc_completion_queue_shutdown(grpc_completion_queue *cq);
void grpc_completion_queue_destroy(grpc_completion_queue *cq);
grpc_call *grpc_channel_create_call_old(grpc_channel *channel,
                                        const char *method, const char *host,
                                        gpr_timespec deadline);
grpc_call_error grpc_call_start_batch(grpc_call *call, const grpc_op *ops,
                                      size_t nops, grpc_completion_queue *cq,
                                      void *tag);
grpc_channel *grpc_channel_create(const char *target,
                                  const grpc_channel_args *args);
void grpc_channel_destroy(grpc_channel *channel);
grpc_call_error grpc_call_add_metadata_old(grpc_call *call,
                                           grpc_metadata *metadata,
                                           gpr_uint32 flags);
grpc_call_error grpc_call_invoke_old(grpc_call *call, grpc_completion_queue *cq,
                                     void *metadata_read_tag,
                                     void *finished_tag, gpr_uint32 flags);
grpc_call_error grpc_call_server_accept_old(grpc_call *call,
                                            grpc_completion_queue *cq,
                                            void *finished_tag);
grpc_call_error grpc_call_server_end_initial_metadata_old(grpc_call *call,
                                                          gpr_uint32 flags);
grpc_call_error grpc_call_cancel(grpc_call *call);
grpc_call_error grpc_call_cancel_with_status(grpc_call *call,
                                             grpc_status_code status,
                                             const char *description);
grpc_call_error grpc_call_start_write_old(grpc_call *call,
                                          grpc_byte_buffer *byte_buffer,
                                          void *tag, gpr_uint32 flags);
grpc_call_error grpc_call_start_write_status_old(grpc_call *call,
                                                 grpc_status_code status_code,
                                                 const char *status_message,
                                                 void *tag);
grpc_call_error grpc_call_writes_done_old(grpc_call *call, void *tag);
grpc_call_error grpc_call_start_read_old(grpc_call *call, void *tag);
void grpc_call_destroy(grpc_call *call);
grpc_call_error grpc_server_request_call_old(grpc_server *server,
                                             void *tag_new);
grpc_server *grpc_server_create(grpc_completion_queue *cq,
                                const grpc_channel_args *args);
int grpc_server_add_http2_port(grpc_server *server, const char *addr);
int grpc_server_add_secure_http2_port(grpc_server *server, const char *addr);
void grpc_server_start(grpc_server *server);
void grpc_server_shutdown(grpc_server *server);
void grpc_server_shutdown_and_notify(grpc_server *server, void *tag);
void grpc_server_destroy(grpc_server *server);
#ifdef __cplusplus
}
#endif
#endif
