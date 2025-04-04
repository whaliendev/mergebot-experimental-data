#ifndef GRPC_CORE_EXT_TRANSPORT_CHTTP2_TRANSPORT_INTERNAL_H
#define GRPC_CORE_EXT_TRANSPORT_CHTTP2_TRANSPORT_INTERNAL_H 
#include <assert.h>
#include <stdbool.h>
#include "src/core/ext/transport/chttp2/transport/frame.h"
#include "src/core/ext/transport/chttp2/transport/frame_data.h"
#include "src/core/ext/transport/chttp2/transport/frame_goaway.h"
#include "src/core/ext/transport/chttp2/transport/frame_ping.h"
#include "src/core/ext/transport/chttp2/transport/frame_rst_stream.h"
#include "src/core/ext/transport/chttp2/transport/frame_settings.h"
#include "src/core/ext/transport/chttp2/transport/frame_window_update.h"
#include "src/core/ext/transport/chttp2/transport/hpack_encoder.h"
#include "src/core/ext/transport/chttp2/transport/hpack_parser.h"
#include "src/core/ext/transport/chttp2/transport/incoming_metadata.h"
#include "src/core/ext/transport/chttp2/transport/stream_map.h"
#include "src/core/lib/iomgr/combiner.h"
#include "src/core/lib/iomgr/endpoint.h"
#include "src/core/lib/transport/bdp_estimator.h"
#include "src/core/lib/transport/connectivity_state.h"
#include "src/core/lib/transport/pid_controller.h"
#include "src/core/lib/transport/transport_impl.h"
typedef enum {
  GRPC_CHTTP2_LIST_WRITABLE,
  GRPC_CHTTP2_LIST_WRITING,
  GRPC_CHTTP2_LIST_STALLED_BY_TRANSPORT,
  GRPC_CHTTP2_LIST_WAITING_FOR_CONCURRENCY,
  STREAM_LIST_COUNT
} grpc_chttp2_stream_list_id;
typedef enum {
  GRPC_CHTTP2_LIST_WRITABLE,
  GRPC_CHTTP2_LIST_WRITING,
  GRPC_CHTTP2_LIST_STALLED_BY_TRANSPORT,
  GRPC_CHTTP2_LIST_STALLED_BY_STREAM,
  GRPC_CHTTP2_LIST_WAITING_FOR_CONCURRENCY,
  STREAM_LIST_COUNT
} grpc_chttp2_stream_list_id;
typedef enum {
  GRPC_CHTTP2_WRITE_STATE_IDLE,
  GRPC_CHTTP2_WRITE_STATE_WRITING,
  GRPC_CHTTP2_WRITE_STATE_WRITING_WITH_MORE,
  GRPC_CHTTP2_WRITE_STATE_WRITING_WITH_MORE_AND_COVERED_BY_POLLER,
} grpc_chttp2_write_state;
typedef enum {
  GRPC_CHTTP2_PING_ON_NEXT_WRITE = 0,
  GRPC_CHTTP2_PING_BEFORE_TRANSPORT_WINDOW_UPDATE,
  GRPC_CHTTP2_PING_TYPE_COUNT
} grpc_chttp2_ping_type;
typedef enum {
  GRPC_CHTTP2_PCL_INITIATE = 0,
  GRPC_CHTTP2_PCL_NEXT,
  GRPC_CHTTP2_PCL_INFLIGHT,
  GRPC_CHTTP2_PCL_COUNT
} grpc_chttp2_ping_closure_list;
typedef struct {
  grpc_closure_list lists[GRPC_CHTTP2_PCL_COUNT];
  uint64_t inflight_id;
} grpc_chttp2_ping_queue;
typedef struct {
  gpr_timespec min_time_between_pings;
  int max_pings_without_data;
} grpc_chttp2_repeated_ping_policy;
typedef struct {
  gpr_timespec last_ping_sent_time;
  int pings_before_data_required;
} grpc_chttp2_repeated_ping_state;
typedef enum {
  GRPC_DTS_CLIENT_PREFIX_0 = 0,
  GRPC_DTS_CLIENT_PREFIX_1,
  GRPC_DTS_CLIENT_PREFIX_2,
  GRPC_DTS_CLIENT_PREFIX_3,
  GRPC_DTS_CLIENT_PREFIX_4,
  GRPC_DTS_CLIENT_PREFIX_5,
  GRPC_DTS_CLIENT_PREFIX_6,
  GRPC_DTS_CLIENT_PREFIX_7,
  GRPC_DTS_CLIENT_PREFIX_8,
  GRPC_DTS_CLIENT_PREFIX_9,
  GRPC_DTS_CLIENT_PREFIX_10,
  GRPC_DTS_CLIENT_PREFIX_11,
  GRPC_DTS_CLIENT_PREFIX_12,
  GRPC_DTS_CLIENT_PREFIX_13,
  GRPC_DTS_CLIENT_PREFIX_14,
  GRPC_DTS_CLIENT_PREFIX_15,
  GRPC_DTS_CLIENT_PREFIX_16,
  GRPC_DTS_CLIENT_PREFIX_17,
  GRPC_DTS_CLIENT_PREFIX_18,
  GRPC_DTS_CLIENT_PREFIX_19,
  GRPC_DTS_CLIENT_PREFIX_20,
  GRPC_DTS_CLIENT_PREFIX_21,
  GRPC_DTS_CLIENT_PREFIX_22,
  GRPC_DTS_CLIENT_PREFIX_23,
  GRPC_DTS_FH_0,
  GRPC_DTS_FH_1,
  GRPC_DTS_FH_2,
  GRPC_DTS_FH_3,
  GRPC_DTS_FH_4,
  GRPC_DTS_FH_5,
  GRPC_DTS_FH_6,
  GRPC_DTS_FH_7,
  GRPC_DTS_FH_8,
  GRPC_DTS_FRAME
} grpc_chttp2_deframe_transport_state;
typedef struct {
  grpc_chttp2_stream *head;
  grpc_chttp2_stream *tail;
} grpc_chttp2_stream_list;
typedef struct {
  grpc_chttp2_stream *next;
  grpc_chttp2_stream *prev;
} grpc_chttp2_stream_link;
typedef struct grpc_chttp2_outstanding_ping {
  uint8_t id[8];
  grpc_closure *on_recv;
  struct grpc_chttp2_outstanding_ping *next;
  struct grpc_chttp2_outstanding_ping *prev;
} grpc_chttp2_outstanding_ping;
typedef enum {
  GRPC_PEER_SETTINGS = 0,
  GRPC_LOCAL_SETTINGS,
  GRPC_SENT_SETTINGS,
  GRPC_ACKED_SETTINGS,
  GRPC_NUM_SETTING_SETS
} grpc_chttp2_setting_set;
typedef enum {
  GRPC_CHTTP2_NO_GOAWAY_SEND,
  GRPC_CHTTP2_GOAWAY_SEND_SCHEDULED,
  GRPC_CHTTP2_GOAWAY_SENT,
} grpc_chttp2_sent_goaway_state;
typedef struct grpc_chttp2_write_cb {
  int64_t call_at_byte;
  grpc_closure *closure;
  struct grpc_chttp2_write_cb *next;
} grpc_chttp2_write_cb;
struct grpc_chttp2_incoming_byte_stream {
  grpc_byte_stream base;
  gpr_refcount refs;
  struct grpc_chttp2_incoming_byte_stream *next_message;
  grpc_error *error;
  grpc_chttp2_transport *transport;
  grpc_chttp2_stream *stream;
  bool is_tail;
gpr_mu slice_mu;
  grpc_slice_buffer slices;
  grpc_closure *on_next;
  grpc_slice *next;
  uint32_t remaining_bytes;
  struct {
    grpc_closure closure;
    grpc_slice *slice;
    size_t max_size_hint;
    grpc_closure *on_complete;
  } next_action;
  grpc_closure destroy_action;
  grpc_closure finished_action;
};
struct grpc_chttp2_transport {
grpc_transport base;
  gpr_refcount refs;
  grpc_endpoint *ep;
  char *peer_string;
  grpc_combiner *combiner;
  grpc_chttp2_write_state write_state;
  uint8_t destroying;
  uint8_t closed;
  uint8_t endpoint_reading;
  grpc_chttp2_stream_list lists[STREAM_LIST_COUNT];
  grpc_chttp2_stream_map stream_map;
  grpc_closure write_action_begin_locked;
  grpc_closure write_action;
  grpc_closure write_action_end_locked;
  grpc_closure read_action_locked;
  grpc_slice_buffer read_buffer;
  grpc_chttp2_stream **accepting_stream;
  struct {
    void (*accept_stream)(grpc_exec_ctx *exec_ctx, void *user_data,
                          grpc_transport *transport, const void *server_data);
    void *accept_stream_user_data;
    grpc_connectivity_state_tracker state_tracker;
  } channel_callback;
  grpc_slice_buffer outbuf;
  grpc_chttp2_hpack_compressor hpack_compressor;
  int64_t outgoing_window;
  uint8_t is_client;
  grpc_slice_buffer qbuf;
  uint32_t write_buffer_size;
  uint8_t seen_goaway;
  grpc_chttp2_sent_goaway_state sent_goaway_state;
  uint8_t dirtied_local_settings;
  uint8_t sent_local_settings;
  uint32_t force_send_settings;
  uint32_t settings[GRPC_NUM_SETTING_SETS][GRPC_CHTTP2_NUM_SETTINGS];
  uint32_t next_stream_id;
  uint32_t last_new_stream_id;
  grpc_chttp2_ping_queue ping_queues[GRPC_CHTTP2_PING_TYPE_COUNT];
  grpc_chttp2_repeated_ping_policy ping_policy;
  grpc_chttp2_repeated_ping_state ping_state;
uint64_t ping_ctr;
  grpc_chttp2_hpack_parser hpack_parser;
  union {
    grpc_chttp2_window_update_parser window_update;
    grpc_chttp2_settings_parser settings;
    grpc_chttp2_ping_parser ping;
    grpc_chttp2_rst_stream_parser rst_stream;
  } simple;
  grpc_chttp2_goaway_parser goaway_parser;
  int64_t initial_window_update;
  bool parse_saw_data_frames;
  int64_t incoming_window;
  grpc_chttp2_deframe_transport_state deframe_state;
  uint8_t incoming_frame_type;
  uint8_t incoming_frame_flags;
  uint8_t header_eof;
  bool is_first_frame;
  uint32_t expect_continuation_stream_id;
  uint32_t incoming_frame_size;
  uint32_t incoming_stream_id;
  void *parser_data;
  grpc_chttp2_stream *incoming_stream;
  grpc_error *(*parser)(grpc_exec_ctx *exec_ctx, void *parser_user_data,
                        grpc_chttp2_transport *t, grpc_chttp2_stream *s,
                        grpc_slice slice, int is_last);
  grpc_status_code goaway_error;
  uint32_t goaway_last_stream_index;
  grpc_slice goaway_text;
  grpc_chttp2_write_cb *write_cb_pool;
  grpc_bdp_estimator bdp_estimator;
  grpc_pid_controller pid_controller;
  grpc_closure start_bdp_ping_locked;
  grpc_closure finish_bdp_ping_locked;
  gpr_timespec last_bdp_ping_finished;
  gpr_timespec last_pid_update;
  grpc_error *close_transport_on_writes_finished;
  grpc_closure_list run_after_write;
  bool benign_reclaimer_registered;
  bool destructive_reclaimer_registered;
  grpc_closure benign_reclaimer_locked;
  grpc_closure destructive_reclaimer_locked;
};
typedef enum {
  GRPC_METADATA_NOT_PUBLISHED,
  GRPC_METADATA_SYNTHESIZED_FROM_FAKE,
  GRPC_METADATA_PUBLISHED_FROM_WIRE,
  GPRC_METADATA_PUBLISHED_AT_CLOSE
} grpc_published_metadata_method;
struct grpc_chttp2_stream {
  grpc_chttp2_transport *t;
  grpc_stream_refcount *refcount;
  grpc_closure destroy_stream;
  void *destroy_stream_arg;
  grpc_chttp2_stream_link links[STREAM_LIST_COUNT];
  uint8_t included[STREAM_LIST_COUNT];
  uint32_t id;
  int64_t outgoing_window_delta;
  grpc_metadata_batch *send_initial_metadata;
  grpc_closure *send_initial_metadata_finished;
  grpc_metadata_batch *send_trailing_metadata;
  grpc_closure *send_trailing_metadata_finished;
  grpc_byte_stream *fetching_send_message;
  uint32_t fetched_send_message_length;
  grpc_slice fetching_slice;
  int64_t next_message_end_offset;
  int64_t flow_controlled_bytes_written;
  bool complete_fetch_covered_by_poller;
  grpc_closure complete_fetch;
  grpc_closure complete_fetch_locked;
  grpc_closure *fetching_send_message_finished;
  grpc_metadata_batch *recv_initial_metadata;
  grpc_closure *recv_initial_metadata_ready;
  grpc_byte_stream **recv_message;
  grpc_closure *recv_message_ready;
  grpc_metadata_batch *recv_trailing_metadata;
  grpc_closure *recv_trailing_metadata_finished;
  grpc_transport_stream_stats *collecting_stats;
  grpc_transport_stream_stats stats;
  gpr_refcount active_streams;
  bool write_closed;
  bool read_closed;
  bool all_incoming_byte_streams_finished;
  bool seen_error;
  bool write_buffering;
  grpc_error *read_closed_error;
  grpc_error *write_closed_error;
  grpc_published_metadata_method published_metadata[2];
  bool final_metadata_requested;
  grpc_chttp2_incoming_metadata_buffer metadata_buffer[2];
  grpc_chttp2_incoming_frame_queue incoming_frames;
  gpr_timespec deadline;
  grpc_error *forced_close_error;
  uint8_t header_frames_received;
  int64_t incoming_window_delta;
  grpc_chttp2_data_parser data_parser;
  int64_t received_bytes;
  bool sent_initial_metadata;
  bool sent_trailing_metadata;
  uint32_t announce_window;
  grpc_slice_buffer flow_controlled_buffer;
  grpc_chttp2_write_cb *on_write_finished_cbs;
  grpc_chttp2_write_cb *finish_after_write;
  size_t sending_bytes;
};
void grpc_chttp2_initiate_write(grpc_exec_ctx *exec_ctx,
                                grpc_chttp2_transport *t,
                                bool covered_by_poller, const char *reason);
bool grpc_chttp2_begin_write(grpc_exec_ctx *exec_ctx, grpc_chttp2_transport *t);
void grpc_chttp2_end_write(grpc_exec_ctx *exec_ctx, grpc_chttp2_transport *t,
                           grpc_error *error);
grpc_error *grpc_chttp2_perform_read(grpc_exec_ctx *exec_ctx,
                                     grpc_chttp2_transport *t,
                                     grpc_slice slice);
bool grpc_chttp2_list_add_writable_stream(grpc_chttp2_transport *t,
                                          grpc_chttp2_stream *s);
bool grpc_chttp2_list_pop_writable_stream(grpc_chttp2_transport *t,
                                          grpc_chttp2_stream **s);
bool grpc_chttp2_list_remove_writable_stream(
    grpc_chttp2_transport *t, grpc_chttp2_stream *s)bool grpc_chttp2_list_add_writing_stream(grpc_chttp2_transport *t,
                                         grpc_chttp2_stream *s);
bool grpc_chttp2_list_have_writing_streams(grpc_chttp2_transport *t);
bool grpc_chttp2_list_pop_writing_stream(grpc_chttp2_transport *t,
                                         grpc_chttp2_stream **s);
void grpc_chttp2_list_add_written_stream(grpc_chttp2_transport *t,
                                         grpc_chttp2_stream *s);
bool grpc_chttp2_list_pop_written_stream(grpc_chttp2_transport *t,
                                         grpc_chttp2_stream **s);
void grpc_chttp2_list_add_waiting_for_concurrency(grpc_chttp2_transport *t,
                                                  grpc_chttp2_stream *s);
bool grpc_chttp2_list_pop_waiting_for_concurrency(grpc_chttp2_transport *t,
                                                  grpc_chttp2_stream **s);
void grpc_chttp2_list_remove_waiting_for_concurrency(grpc_chttp2_transport *t,
                                                     grpc_chttp2_stream *s);
void grpc_chttp2_list_add_stalled_by_transport(grpc_chttp2_transport *t,
                                               grpc_chttp2_stream *s);
bool grpc_chttp2_list_pop_stalled_by_transport(grpc_chttp2_transport *t,
                                               grpc_chttp2_stream **s);
void grpc_chttp2_list_remove_stalled_by_transport(grpc_chttp2_transport *t,
                                                  grpc_chttp2_stream *s);
void grpc_chttp2_list_add_stalled_by_stream(grpc_chttp2_transport *t,
                                            grpc_chttp2_stream *s);
bool grpc_chttp2_list_pop_stalled_by_stream(grpc_chttp2_transport *t,
                                            grpc_chttp2_stream **s);
bool grpc_chttp2_list_remove_stalled_by_stream(grpc_chttp2_transport *t,
                                               grpc_chttp2_stream *s);
grpc_chttp2_stream *grpc_chttp2_parsing_lookup_stream(grpc_chttp2_transport *t,
                                                      uint32_t id);
grpc_chttp2_stream *grpc_chttp2_parsing_accept_stream(grpc_exec_ctx *exec_ctx,
                                                      grpc_chttp2_transport *t,
                                                      uint32_t id);
void grpc_chttp2_add_incoming_goaway(grpc_exec_ctx *exec_ctx,
                                     grpc_chttp2_transport *t,
                                     uint32_t goaway_error,
                                     grpc_slice goaway_text);
void grpc_chttp2_parsing_become_skip_parser(grpc_exec_ctx *exec_ctx,
                                            grpc_chttp2_transport *t);
void grpc_chttp2_complete_closure_step(grpc_exec_ctx *exec_ctx,
                                       grpc_chttp2_transport *t,
                                       grpc_chttp2_stream *s,
                                       grpc_closure **pclosure,
                                       grpc_error *error, const char *desc);
#define GRPC_CHTTP2_CLIENT_CONNECT_STRING "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n"
#define GRPC_CHTTP2_CLIENT_CONNECT_STRLEN \
  (sizeof(GRPC_CHTTP2_CLIENT_CONNECT_STRING) - 1)
int grpc_http_trace = 0;
int grpc_flowctl_trace = 0;
typedef enum {
  GRPC_CHTTP2_FLOWCTL_MOVE,
  GRPC_CHTTP2_FLOWCTL_CREDIT,
  GRPC_CHTTP2_FLOWCTL_DEBIT
} grpc_chttp2_flowctl_op;
void grpc_chttp2_flowctl_trace(const char *file, int line, const char *phase,
                               grpc_chttp2_flowctl_op op, const char *context1,
                               const char *var1, const char *context2,
                               const char *var2, int is_client,
                               uint32_t stream_id, int64_t val1, int64_t val2);
void grpc_chttp2_fake_status(grpc_exec_ctx *exec_ctx, grpc_chttp2_transport *t,
                             grpc_chttp2_stream *stream,
                             grpc_status_code status, grpc_slice *details);
void grpc_chttp2_mark_stream_closed(grpc_exec_ctx *exec_ctx,
                                    grpc_chttp2_transport *t,
                                    grpc_chttp2_stream *s, int close_reads,
                                    int close_writes, grpc_error *error);
void grpc_chttp2_start_writing(grpc_exec_ctx *exec_ctx,
                               grpc_chttp2_transport *t);
#ifdef GRPC_STREAM_REFCOUNT_DEBUG
#define GRPC_CHTTP2_STREAM_REF(stream,reason) \
  grpc_chttp2_stream_ref(stream, reason)
#define GRPC_CHTTP2_STREAM_UNREF(exec_ctx,stream,reason) \
  grpc_chttp2_stream_unref(exec_ctx, stream, reason)
void grpc_chttp2_stream_ref(grpc_chttp2_stream *s, const char *reason);
void grpc_chttp2_stream_unref(grpc_exec_ctx *exec_ctx, grpc_chttp2_stream *s,
                              const char *reason);
#else
#define GRPC_CHTTP2_STREAM_REF(stream,reason) grpc_chttp2_stream_ref(stream)
#define GRPC_CHTTP2_STREAM_UNREF(exec_ctx,stream,reason) \
  grpc_chttp2_stream_unref(exec_ctx, stream)
void grpc_chttp2_stream_ref(grpc_chttp2_stream *s);
void grpc_chttp2_stream_unref(grpc_exec_ctx *exec_ctx, grpc_chttp2_stream *s);
#endif
#ifdef GRPC_CHTTP2_REFCOUNTING_DEBUG
#define GRPC_CHTTP2_REF_TRANSPORT(t,r) \
  grpc_chttp2_ref_transport(t, r, __FILE__, __LINE__)
#define GRPC_CHTTP2_UNREF_TRANSPORT(cl,t,r) \
  grpc_chttp2_unref_transport(cl, t, r, __FILE__, __LINE__)
void grpc_chttp2_unref_transport(grpc_exec_ctx *exec_ctx,
                                 grpc_chttp2_transport *t, const char *reason,
                                 const char *file, int line);
void grpc_chttp2_ref_transport(grpc_chttp2_transport *t, const char *reason,
                               const char *file, int line);
#else
#define GRPC_CHTTP2_REF_TRANSPORT(t,r) grpc_chttp2_ref_transport(t)
#define GRPC_CHTTP2_UNREF_TRANSPORT(cl,t,r) grpc_chttp2_unref_transport(cl, t)
void grpc_chttp2_unref_transport(grpc_exec_ctx *exec_ctx,
                                 grpc_chttp2_transport *t);
void grpc_chttp2_ref_transport(grpc_chttp2_transport *t);
#endif
grpc_chttp2_incoming_byte_stream *grpc_chttp2_incoming_byte_stream_create(
    grpc_exec_ctx *exec_ctx, grpc_chttp2_transport *t, grpc_chttp2_stream *s,
    uint32_t frame_size, uint32_t flags);
void grpc_chttp2_incoming_byte_stream_push(grpc_exec_ctx *exec_ctx,
                                           grpc_chttp2_incoming_byte_stream *bs,
                                           grpc_slice slice);
void grpc_chttp2_incoming_byte_stream_finished(
    grpc_exec_ctx *exec_ctx, grpc_chttp2_incoming_byte_stream *bs,
    grpc_error *error);
void grpc_chttp2_ack_ping(grpc_exec_ctx *exec_ctx, grpc_chttp2_transport *t,
                          uint64_t id);
void grpc_chttp2_become_writable(grpc_exec_ctx *exec_ctx,
                                 grpc_chttp2_transport *t,
                                 grpc_chttp2_stream *s, bool covered_by_poller,
                                 const char *reason);
void grpc_chttp2_cancel_stream(grpc_exec_ctx *exec_ctx,
                               grpc_chttp2_transport *t, grpc_chttp2_stream *s,
                               grpc_error *due_to_error);
void grpc_chttp2_maybe_complete_recv_initial_metadata(grpc_exec_ctx *exec_ctx,
                                                      grpc_chttp2_transport *t,
                                                      grpc_chttp2_stream *s);
void grpc_chttp2_maybe_complete_recv_message(grpc_exec_ctx *exec_ctx,
                                             grpc_chttp2_transport *t,
                                             grpc_chttp2_stream *s);
void grpc_chttp2_maybe_complete_recv_trailing_metadata(grpc_exec_ctx *exec_ctx,
                                                       grpc_chttp2_transport *t,
                                                       grpc_chttp2_stream *s);
void grpc_chttp2_fail_pending_writes(grpc_exec_ctx *exec_ctx,
                                     grpc_chttp2_transport *t,
                                     grpc_chttp2_stream *s, grpc_error *error);
#endif
