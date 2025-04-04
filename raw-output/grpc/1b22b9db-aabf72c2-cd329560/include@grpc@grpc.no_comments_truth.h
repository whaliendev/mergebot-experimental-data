#ifndef GRPC_GRPC_H
#define GRPC_GRPC_H 
#include <grpc/status.h>
#include <stddef.h>
#include <grpc/byte_buffer.h>
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
#define GRPC_ARG_HTTP2_INITIAL_SEQUENCE_NUMBER \
  "grpc.http2.initial_sequence_number"
typedef enum {
  GRPC_CHANNEL_IDLE,
  GRPC_CHANNEL_CONNECTING,
  GRPC_CHANNEL_READY,
  GRPC_CHANNEL_TRANSIENT_FAILURE,
  GRPC_CHANNEL_FATAL_FAILURE
} grpc_connectivity_state;
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
  GRPC_CALL_ERROR_INVALID_FLAGS,
  GRPC_CALL_ERROR_INVALID_METADATA,
  GRPC_CALL_ERROR_NOT_SERVER_COMPLETION_QUEUE
} grpc_call_error;
#define GRPC_WRITE_BUFFER_HINT (0x00000001u)
#define GRPC_WRITE_NO_COMPRESS (0x00000002u)
#define GRPC_WRITE_USED_MASK (GRPC_WRITE_BUFFER_HINT | GRPC_WRITE_NO_COMPRESS)
typedef struct grpc_metadata {
  const char *key;
  const char *value;
  size_t value_length;
  struct {
    void *obfuscated[3];
  } internal_data;
} grpc_metadata;
typedef enum grpc_completion_type {
  GRPC_QUEUE_SHUTDOWN,
  GRPC_QUEUE_TIMEOUT,
  GRPC_OP_COMPLETE
} grpc_completion_type;
typedef struct grpc_event {
  grpc_completion_type type;
  int success;
  void *tag;
} grpc_event;
typedef struct {
  size_t count;
  size_t capacity;
  grpc_metadata *metadata;
} grpc_metadata_array;
void grpc_metadata_array_init(grpc_metadata_array *array);
void grpc_metadata_array_destroy(grpc_metadata_array *array);
typedef struct {
  char *method;
  size_t method_capacity;
  char *host;
  size_t host_capacity;
  gpr_timespec deadline;
} grpc_call_details;
void grpc_call_details_init(grpc_call_details *details);
void grpc_call_details_destroy(grpc_call_details *details);
typedef enum {
  GRPC_OP_SEND_INITIAL_METADATA = 0,
  GRPC_OP_SEND_MESSAGE,
  GRPC_OP_SEND_CLOSE_FROM_CLIENT,
  GRPC_OP_SEND_STATUS_FROM_SERVER,
  GRPC_OP_RECV_INITIAL_METADATA,
  GRPC_OP_RECV_MESSAGE,
  GRPC_OP_RECV_STATUS_ON_CLIENT,
  GRPC_OP_RECV_CLOSE_ON_SERVER
} grpc_op_type;
typedef struct grpc_op {
  grpc_op_type op;
  gpr_uint32 flags;
  union {
    struct {
      size_t count;
      grpc_metadata *metadata;
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
const char *grpc_version_string(void);
grpc_completion_queue *grpc_completion_queue_create(void);
grpc_event grpc_completion_queue_next(grpc_completion_queue *cq,
                                      gpr_timespec deadline);
grpc_event grpc_completion_queue_pluck(grpc_completion_queue *cq, void *tag,
                                       gpr_timespec deadline);
void grpc_completion_queue_shutdown(grpc_completion_queue *cq);
void grpc_completion_queue_destroy(grpc_completion_queue *cq);
grpc_call *grpc_channel_create_call(grpc_channel *channel,
                                    grpc_completion_queue *completion_queue,
                                    const char *method, const char *host,
                                    gpr_timespec deadline);
void *grpc_channel_register_call(grpc_channel *channel, const char *method,
                                 const char *host);
grpc_call *grpc_channel_create_registered_call(
    grpc_channel *channel, grpc_completion_queue *completion_queue,
    void *registered_call_handle, gpr_timespec deadline);
grpc_call_error grpc_call_start_batch(grpc_call *call, const grpc_op *ops,
                                      size_t nops, void *tag);
char *grpc_call_get_peer(grpc_call *call);
char *grpc_channel_get_target(grpc_channel *channel);
grpc_channel *grpc_channel_create(const char *target,
                                  const grpc_channel_args *args);
grpc_channel *grpc_lame_client_channel_create(const char *target);
void grpc_channel_destroy(grpc_channel *channel);
grpc_call_error grpc_call_cancel(grpc_call *call);
grpc_call_error grpc_call_cancel_with_status(grpc_call *call,
                                             grpc_status_code status,
                                             const char *description);
void grpc_call_destroy(grpc_call *call);
grpc_call_error grpc_server_request_call(
    grpc_server *server, grpc_call **call, grpc_call_details *details,
    grpc_metadata_array *request_metadata,
    grpc_completion_queue *cq_bound_to_call,
    grpc_completion_queue *cq_for_notification, void *tag_new);
void *grpc_server_register_method(grpc_server *server, const char *method,
                                  const char *host);
grpc_call_error grpc_server_request_registered_call(
    grpc_server *server, void *registered_method, grpc_call **call,
    gpr_timespec *deadline, grpc_metadata_array *request_metadata,
    grpc_byte_buffer **optional_payload,
    grpc_completion_queue *cq_bound_to_call,
    grpc_completion_queue *cq_for_notification, void *tag_new);
grpc_server *grpc_server_create(const grpc_channel_args *args);
void grpc_server_register_completion_queue(grpc_server *server,
                                           grpc_completion_queue *cq);
int grpc_server_add_http2_port(grpc_server *server, const char *addr);
void grpc_server_start(grpc_server *server);
void grpc_server_shutdown_and_notify(grpc_server *server,
                                     grpc_completion_queue *cq, void *tag);
void grpc_server_cancel_all_calls(grpc_server *server);
void grpc_server_destroy(grpc_server *server);
int grpc_tracer_set_enabled(const char *name, int enabled);
#ifdef __cplusplus
}
#endif
#endif
