#ifndef GRPC_IMPL_CODEGEN_GRPC_TYPES_H
#define GRPC_IMPL_CODEGEN_GRPC_TYPES_H 
#include <grpc/impl/codegen/compression_types.h>
#include <grpc/impl/codegen/exec_ctx_fwd.h>
#include <grpc/impl/codegen/gpr_types.h>
#include <grpc/impl/codegen/slice.h>
#include <grpc/impl/codegen/status.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef enum {
  GRPC_BB_RAW
} grpc_byte_buffer_type;
typedef struct grpc_byte_buffer {
  void *reserved;
  grpc_byte_buffer_type type;
  union {
    struct {
      void *reserved[8];
    } reserved;
    struct {
      grpc_compression_algorithm compression;
      grpc_slice_buffer slice_buffer;
    } raw;
  } data;
} grpc_byte_buffer;
typedef struct grpc_completion_queue grpc_completion_queue;
typedef struct grpc_alarm grpc_alarm;
typedef struct grpc_channel grpc_channel;
typedef struct grpc_server grpc_server;
typedef struct grpc_call grpc_call;
typedef struct grpc_socket_mutator grpc_socket_mutator;
typedef enum {
  GRPC_ARG_STRING,
  GRPC_ARG_INTEGER,
  GRPC_ARG_POINTER
} grpc_arg_type;
typedef struct grpc_arg_pointer_vtable {
  void *(*copy)(void *p);
  void (*destroy)(grpc_exec_ctx *exec_ctx, void *p);
  int (*cmp)(void *p, void *q);
} grpc_arg_pointer_vtable;
typedef struct {
  grpc_arg_type type;
  char *key;
  union {
    char *string;
    int integer;
    struct {
      void *p;
      const grpc_arg_pointer_vtable *vtable;
    } pointer;
  } value;
} grpc_arg;
typedef struct {
  size_t num_args;
  grpc_arg *args;
} grpc_channel_args;
#define GRPC_ARG_ENABLE_CENSUS "grpc.census"
#define GRPC_ARG_ENABLE_LOAD_REPORTING "grpc.loadreporting"
#define GRPC_ARG_MAX_CONCURRENT_STREAMS "grpc.max_concurrent_streams"
#define GRPC_ARG_MAX_RECEIVE_MESSAGE_LENGTH "grpc.max_receive_message_length"
#define GRPC_ARG_MAX_MESSAGE_LENGTH GRPC_ARG_MAX_RECEIVE_MESSAGE_LENGTH
#define GRPC_ARG_MAX_SEND_MESSAGE_LENGTH "grpc.max_send_message_length"
#define GRPC_ARG_HTTP2_INITIAL_SEQUENCE_NUMBER \
  "grpc.http2.initial_sequence_number"
#define GRPC_ARG_HTTP2_STREAM_LOOKAHEAD_BYTES "grpc.http2.lookahead_bytes"
#define GRPC_ARG_HTTP2_HPACK_TABLE_SIZE_DECODER \
  "grpc.http2.hpack_table_size.decoder"
#define GRPC_ARG_HTTP2_HPACK_TABLE_SIZE_ENCODER \
  "grpc.http2.hpack_table_size.encoder"
#define GRPC_ARG_HTTP2_MAX_FRAME_SIZE "grpc.http2.max_frame_size"
<<<<<<< HEAD
#define GRPC_ARG_HTTP2_MIN_TIME_BETWEEN_PINGS_MS \
  "grpc.http2.min_time_between_pings_ms"
#define GRPC_ARG_HTTP2_MAX_PINGS_WITHOUT_DATA \
  "grpc.http2.max_pings_without_data"
||||||| 6b6954050c
=======
#define GRPC_ARG_HTTP2_WRITE_BUFFER_SIZE "grpc.http2.write_buffer_size"
>>>>>>> c2079f37
#define GRPC_ARG_DEFAULT_AUTHORITY "grpc.default_authority"
#define GRPC_ARG_PRIMARY_USER_AGENT_STRING "grpc.primary_user_agent"
#define GRPC_ARG_SECONDARY_USER_AGENT_STRING "grpc.secondary_user_agent"
#define GRPC_ARG_MAX_RECONNECT_BACKOFF_MS "grpc.max_reconnect_backoff_ms"
#define GRPC_ARG_INITIAL_RECONNECT_BACKOFF_MS \
  "grpc.initial_reconnect_backoff_ms"
#define GRPC_SSL_TARGET_NAME_OVERRIDE_ARG "grpc.ssl_target_name_override"
#define GRPC_ARG_MAX_METADATA_SIZE "grpc.max_metadata_size"
#define GRPC_ARG_ALLOW_REUSEPORT "grpc.so_reuseport"
#define GRPC_ARG_RESOURCE_QUOTA "grpc.resource_quota"
#define GRPC_ARG_SERVICE_CONFIG "grpc.service_config"
#define GRPC_ARG_LB_POLICY_NAME "grpc.lb_policy_name"
#define GRPC_ARG_SOCKET_MUTATOR "grpc.socket_mutator"
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
  GRPC_CALL_ERROR_INVALID_MESSAGE,
  GRPC_CALL_ERROR_NOT_SERVER_COMPLETION_QUEUE,
  GRPC_CALL_ERROR_BATCH_TOO_BIG,
  GRPC_CALL_ERROR_PAYLOAD_TYPE_MISMATCH
} grpc_call_error;
#define GRPC_DEFAULT_MAX_SEND_MESSAGE_LENGTH -1
#define GRPC_DEFAULT_MAX_RECV_MESSAGE_LENGTH (4 * 1024 * 1024)
#define GRPC_WRITE_BUFFER_HINT (0x00000001u)
#define GRPC_WRITE_NO_COMPRESS (0x00000002u)
#define GRPC_WRITE_USED_MASK (GRPC_WRITE_BUFFER_HINT | GRPC_WRITE_NO_COMPRESS)
#define GRPC_INITIAL_METADATA_IDEMPOTENT_REQUEST (0x00000010u)
#define GRPC_INITIAL_METADATA_WAIT_FOR_READY (0x00000020u)
#define GRPC_INITIAL_METADATA_CACHEABLE_REQUEST (0x00000040u)
#define GRPC_INITIAL_METADATA_WAIT_FOR_READY_EXPLICITLY_SET (0x00000080u)
#define GRPC_INITIAL_METADATA_USED_MASK \
  (GRPC_INITIAL_METADATA_IDEMPOTENT_REQUEST | \
   GRPC_INITIAL_METADATA_WAIT_FOR_READY | \
   GRPC_INITIAL_METADATA_CACHEABLE_REQUEST | \
   GRPC_INITIAL_METADATA_WAIT_FOR_READY_EXPLICITLY_SET)
typedef struct grpc_metadata {
  const char *key;
  const char *value;
  size_t value_length;
  uint32_t flags;
  struct {
    void *obfuscated[4];
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
typedef struct {
  char *method;
  size_t method_capacity;
  char *host;
  size_t host_capacity;
  gpr_timespec deadline;
  uint32_t flags;
  void *reserved;
} grpc_call_details;
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
struct grpc_byte_buffer;
typedef struct grpc_op {
  grpc_op_type op;
  uint32_t flags;
  void *reserved;
  union {
    struct {
      void *reserved[8];
    } reserved;
    struct {
      size_t count;
      grpc_metadata *metadata;
      struct {
        uint8_t is_set;
        grpc_compression_level level;
      } maybe_compression_level;
    } send_initial_metadata;
    struct grpc_byte_buffer *send_message;
    struct {
      size_t trailing_metadata_count;
      grpc_metadata *trailing_metadata;
      grpc_status_code status;
      const char *status_details;
    } send_status_from_server;
    grpc_metadata_array *recv_initial_metadata;
    struct grpc_byte_buffer **recv_message;
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
typedef struct {
  char **lb_policy_name;
  char **service_config_json;
} grpc_channel_info;
typedef struct grpc_resource_quota grpc_resource_quota;
#ifdef __cplusplus
}
#endif
#endif
