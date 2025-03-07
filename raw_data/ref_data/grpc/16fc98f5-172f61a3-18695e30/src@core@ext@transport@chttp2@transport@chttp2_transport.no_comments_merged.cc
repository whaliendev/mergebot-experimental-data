#include <grpc/support/port_platform.h>
#include "src/core/ext/transport/chttp2/transport/chttp2_transport.h"
#include <grpc/slice_buffer.h>
#include <grpc/support/alloc.h>
#include <grpc/support/log.h>
#include <grpc/support/port_platform.h>
#include <grpc/support/string_util.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "absl/strings/str_format.h"
#include "src/core/ext/transport/chttp2/transport/context_list.h"
#include "src/core/ext/transport/chttp2/transport/frame_data.h"
#include "src/core/ext/transport/chttp2/transport/internal.h"
#include "src/core/ext/transport/chttp2/transport/varint.h"
#include "src/core/lib/channel/channel_args.h"
#include "src/core/lib/compression/stream_compression.h"
#include "src/core/lib/debug/stats.h"
#include "src/core/lib/gpr/env.h"
#include "src/core/lib/gpr/string.h"
#include "src/core/lib/gprpp/memory.h"
#include "src/core/lib/http/parser.h"
#include "src/core/lib/iomgr/executor.h"
#include "src/core/lib/iomgr/iomgr.h"
#include "src/core/lib/iomgr/timer.h"
#include "src/core/lib/profiling/timers.h"
#include "src/core/lib/slice/slice_internal.h"
#include "src/core/lib/slice/slice_string_helpers.h"
#include "src/core/lib/transport/error_utils.h"
#include "src/core/lib/transport/http2_errors.h"
#include "src/core/lib/transport/static_metadata.h"
#include "src/core/lib/transport/status_conversion.h"
#include "src/core/lib/transport/timeout_encoding.h"
#include "src/core/lib/transport/transport.h"
#include "src/core/lib/transport/transport_impl.h"
#include "src/core/lib/uri/uri_parser.h"
#define DEFAULT_CONNECTION_WINDOW_TARGET (1024 * 1024)
#define MAX_WINDOW 0x7fffffffu
#define MAX_WRITE_BUFFER_SIZE (64 * 1024 * 1024)
#define DEFAULT_MAX_HEADER_LIST_SIZE (8 * 1024)
#define DEFAULT_CLIENT_KEEPALIVE_TIME_MS INT_MAX
#define DEFAULT_CLIENT_KEEPALIVE_TIMEOUT_MS 20000
#define DEFAULT_SERVER_KEEPALIVE_TIME_MS 7200000
#define DEFAULT_SERVER_KEEPALIVE_TIMEOUT_MS 20000
#define DEFAULT_KEEPALIVE_PERMIT_WITHOUT_CALLS false
#define KEEPALIVE_TIME_BACKOFF_MULTIPLIER 2
#define DEFAULT_MIN_SENT_PING_INTERVAL_WITHOUT_DATA_MS 300000
#define DEFAULT_MIN_RECV_PING_INTERVAL_WITHOUT_DATA_MS 300000
#define DEFAULT_MAX_PINGS_BETWEEN_DATA 2
#define DEFAULT_MAX_PING_STRIKES 2
#define DEFAULT_MAX_PENDING_INDUCED_FRAMES 10000
static int g_default_client_keepalive_time_ms =
    DEFAULT_CLIENT_KEEPALIVE_TIME_MS;
static int g_default_client_keepalive_timeout_ms =
    DEFAULT_CLIENT_KEEPALIVE_TIMEOUT_MS;
static int g_default_server_keepalive_time_ms =
    DEFAULT_SERVER_KEEPALIVE_TIME_MS;
static int g_default_server_keepalive_timeout_ms =
    DEFAULT_SERVER_KEEPALIVE_TIMEOUT_MS;
static bool g_default_client_keepalive_permit_without_calls =
    DEFAULT_KEEPALIVE_PERMIT_WITHOUT_CALLS;
static bool g_default_server_keepalive_permit_without_calls =
    DEFAULT_KEEPALIVE_PERMIT_WITHOUT_CALLS;
static int g_default_min_sent_ping_interval_without_data_ms =
    DEFAULT_MIN_SENT_PING_INTERVAL_WITHOUT_DATA_MS;
static int g_default_min_recv_ping_interval_without_data_ms =
    DEFAULT_MIN_RECV_PING_INTERVAL_WITHOUT_DATA_MS;
static int g_default_max_pings_without_data = DEFAULT_MAX_PINGS_BETWEEN_DATA;
static int g_default_max_ping_strikes = DEFAULT_MAX_PING_STRIKES;
#define MAX_CLIENT_STREAM_ID 0x7fffffffu
grpc_core::TraceFlag grpc_http_trace(false, "http");
grpc_core::TraceFlag grpc_keepalive_trace(false, "http_keepalive");
grpc_core::DebugOnlyTraceFlag grpc_trace_chttp2_refcount(false,
                                                         "chttp2_refcount");
static void write_action_begin_locked(void* t, grpc_error* error);
static void write_action(void* t, grpc_error* error);
static void write_action_end(void* t, grpc_error* error);
static void write_action_end_locked(void* t, grpc_error* error);
static void read_action(void* t, grpc_error* error);
static void read_action_locked(void* t, grpc_error* error);
static void continue_read_action_locked(grpc_chttp2_transport* t);
static void complete_fetch(void* gs, grpc_error* error);
static void complete_fetch_locked(void* gs, grpc_error* error);
static void queue_setting_update(grpc_chttp2_transport* t,
                                 grpc_chttp2_setting_id id, uint32_t value);
static void close_from_api(grpc_chttp2_transport* t, grpc_chttp2_stream* s,
                           grpc_error* error);
static void maybe_start_some_streams(grpc_chttp2_transport* t);
static void connectivity_state_set(grpc_chttp2_transport* t,
                                   grpc_connectivity_state state,
                                   const char* reason);
static void benign_reclaimer(void* t, grpc_error* error);
static void destructive_reclaimer(void* t, grpc_error* error);
static void benign_reclaimer_locked(void* t, grpc_error* error);
static void destructive_reclaimer_locked(void* t, grpc_error* error);
static void post_benign_reclaimer(grpc_chttp2_transport* t);
static void post_destructive_reclaimer(grpc_chttp2_transport* t);
static void close_transport_locked(grpc_chttp2_transport* t, grpc_error* error);
static void end_all_the_calls(grpc_chttp2_transport* t, grpc_error* error);
static void schedule_bdp_ping_locked(grpc_chttp2_transport* t);
static void start_bdp_ping(void* tp, grpc_error* error);
static void finish_bdp_ping(void* tp, grpc_error* error);
static void start_bdp_ping_locked(void* tp, grpc_error* error);
static void finish_bdp_ping_locked(void* tp, grpc_error* error);
static void next_bdp_ping_timer_expired(void* tp, grpc_error* error);
static void next_bdp_ping_timer_expired_locked(void* tp, grpc_error* error);
static void cancel_pings(grpc_chttp2_transport* t, grpc_error* error);
static void send_ping_locked(grpc_chttp2_transport* t,
                             grpc_closure* on_initiate,
                             grpc_closure* on_complete);
static void retry_initiate_ping_locked(void* tp, grpc_error* error);
static void init_keepalive_ping(void* arg, grpc_error* error);
static void init_keepalive_ping_locked(void* arg, grpc_error* error);
static void start_keepalive_ping(void* arg, grpc_error* error);
static void finish_keepalive_ping(void* arg, grpc_error* error);
static void start_keepalive_ping_locked(void* arg, grpc_error* error);
static void finish_keepalive_ping_locked(void* arg, grpc_error* error);
static void keepalive_watchdog_fired(void* arg, grpc_error* error);
static void keepalive_watchdog_fired_locked(void* arg, grpc_error* error);
static void reset_byte_stream(void* arg, grpc_error* error);
bool g_flow_control_enabled = true;
grpc_chttp2_transport::~grpc_chttp2_transport() {
  size_t i;
  if (channelz_socket != nullptr) {
    channelz_socket.reset();
  }
  grpc_endpoint_destroy(ep);
  grpc_slice_buffer_destroy_internal(&qbuf);
  grpc_slice_buffer_destroy_internal(&outbuf);
  grpc_chttp2_hpack_compressor_destroy(&hpack_compressor);
  grpc_error* error =
      GRPC_ERROR_CREATE_FROM_STATIC_STRING("Transport destroyed");
  grpc_core::ContextList::Execute(cl, nullptr, error);
  GRPC_ERROR_UNREF(error);
  cl = nullptr;
  grpc_slice_buffer_destroy_internal(&read_buffer);
  grpc_chttp2_hpack_parser_destroy(&hpack_parser);
  grpc_chttp2_goaway_parser_destroy(&goaway_parser);
  for (i = 0; i < STREAM_LIST_COUNT; i++) {
    GPR_ASSERT(lists[i].head == nullptr);
    GPR_ASSERT(lists[i].tail == nullptr);
  }
  GRPC_ERROR_UNREF(goaway_error);
  GPR_ASSERT(grpc_chttp2_stream_map_size(&stream_map) == 0);
  grpc_chttp2_stream_map_destroy(&stream_map);
  GRPC_COMBINER_UNREF(combiner, "chttp2_transport");
  cancel_pings(this,
               GRPC_ERROR_CREATE_FROM_STATIC_STRING("Transport destroyed"));
  while (write_cb_pool) {
    grpc_chttp2_write_cb* next = write_cb_pool->next;
    gpr_free(write_cb_pool);
    write_cb_pool = next;
  }
  flow_control.Destroy();
  GRPC_ERROR_UNREF(closed_with_error);
  gpr_free(ping_acks);
  gpr_free(peer_string);
}
static const grpc_transport_vtable* get_vtable(void);
static bool read_channel_args(grpc_chttp2_transport* t,
                              const grpc_channel_args* channel_args,
                              bool is_client) {
  bool enable_bdp = true;
  bool channelz_enabled = GRPC_ENABLE_CHANNELZ_DEFAULT;
  size_t i;
  int j;
  for (i = 0; i < channel_args->num_args; i++) {
    if (0 == strcmp(channel_args->args[i].key,
                    GRPC_ARG_HTTP2_INITIAL_SEQUENCE_NUMBER)) {
      const grpc_integer_options options = {-1, 0, INT_MAX};
      const int value =
          grpc_channel_arg_get_integer(&channel_args->args[i], options);
      if (value >= 0) {
        if ((t->next_stream_id & 1) != (value & 1)) {
          gpr_log(GPR_ERROR, "%s: low bit must be %d on %s",
                  GRPC_ARG_HTTP2_INITIAL_SEQUENCE_NUMBER, t->next_stream_id & 1,
                  is_client ? "client" : "server");
        } else {
          t->next_stream_id = static_cast<uint32_t>(value);
        }
      }
    } else if (0 == strcmp(channel_args->args[i].key,
                           GRPC_ARG_HTTP2_HPACK_TABLE_SIZE_ENCODER)) {
      const grpc_integer_options options = {-1, 0, INT_MAX};
      const int value =
          grpc_channel_arg_get_integer(&channel_args->args[i], options);
      if (value >= 0) {
        grpc_chttp2_hpack_compressor_set_max_usable_size(
            &t->hpack_compressor, static_cast<uint32_t>(value));
      }
    } else if (0 == strcmp(channel_args->args[i].key,
                           GRPC_ARG_HTTP2_MAX_PINGS_WITHOUT_DATA)) {
      t->ping_policy.max_pings_without_data = grpc_channel_arg_get_integer(
          &channel_args->args[i],
          {g_default_max_pings_without_data, 0, INT_MAX});
    } else if (0 == strcmp(channel_args->args[i].key,
                           GRPC_ARG_HTTP2_MAX_PING_STRIKES)) {
      t->ping_policy.max_ping_strikes = grpc_channel_arg_get_integer(
          &channel_args->args[i], {g_default_max_ping_strikes, 0, INT_MAX});
    } else if (0 ==
               strcmp(channel_args->args[i].key,
                      GRPC_ARG_HTTP2_MIN_SENT_PING_INTERVAL_WITHOUT_DATA_MS)) {
      t->ping_policy.min_sent_ping_interval_without_data =
          grpc_channel_arg_get_integer(
              &channel_args->args[i],
              grpc_integer_options{
                  g_default_min_sent_ping_interval_without_data_ms, 0,
                  INT_MAX});
    } else if (0 ==
               strcmp(channel_args->args[i].key,
                      GRPC_ARG_HTTP2_MIN_RECV_PING_INTERVAL_WITHOUT_DATA_MS)) {
      t->ping_policy.min_recv_ping_interval_without_data =
          grpc_channel_arg_get_integer(
              &channel_args->args[i],
              grpc_integer_options{
                  g_default_min_recv_ping_interval_without_data_ms, 0,
                  INT_MAX});
    } else if (0 == strcmp(channel_args->args[i].key,
                           GRPC_ARG_HTTP2_WRITE_BUFFER_SIZE)) {
      t->write_buffer_size = static_cast<uint32_t>(grpc_channel_arg_get_integer(
          &channel_args->args[i], {0, 0, MAX_WRITE_BUFFER_SIZE}));
    } else if (0 ==
               strcmp(channel_args->args[i].key, GRPC_ARG_HTTP2_BDP_PROBE)) {
      enable_bdp = grpc_channel_arg_get_bool(&channel_args->args[i], true);
    } else if (0 ==
               strcmp(channel_args->args[i].key, GRPC_ARG_KEEPALIVE_TIME_MS)) {
      const int value = grpc_channel_arg_get_integer(
          &channel_args->args[i],
          grpc_integer_options{t->is_client
                                   ? g_default_client_keepalive_time_ms
                                   : g_default_server_keepalive_time_ms,
                               1, INT_MAX});
      t->keepalive_time = value == INT_MAX ? GRPC_MILLIS_INF_FUTURE : value;
    } else if (0 == strcmp(channel_args->args[i].key,
                           GRPC_ARG_KEEPALIVE_TIMEOUT_MS)) {
      const int value = grpc_channel_arg_get_integer(
          &channel_args->args[i],
          grpc_integer_options{t->is_client
                                   ? g_default_client_keepalive_timeout_ms
                                   : g_default_server_keepalive_timeout_ms,
                               0, INT_MAX});
      t->keepalive_timeout = value == INT_MAX ? GRPC_MILLIS_INF_FUTURE : value;
    } else if (0 == strcmp(channel_args->args[i].key,
                           GRPC_ARG_KEEPALIVE_PERMIT_WITHOUT_CALLS)) {
      t->keepalive_permit_without_calls = static_cast<uint32_t>(
          grpc_channel_arg_get_integer(&channel_args->args[i], {0, 0, 1}));
    } else if (0 == strcmp(channel_args->args[i].key,
                           GRPC_ARG_OPTIMIZATION_TARGET)) {
      gpr_log(GPR_INFO, "GRPC_ARG_OPTIMIZATION_TARGET is deprecated");
    } else if (0 ==
               strcmp(channel_args->args[i].key, GRPC_ARG_ENABLE_CHANNELZ)) {
      channelz_enabled = grpc_channel_arg_get_bool(
          &channel_args->args[i], GRPC_ENABLE_CHANNELZ_DEFAULT);
    } else {
      static const struct {
        const char* channel_arg_name;
        grpc_chttp2_setting_id setting_id;
        grpc_integer_options integer_options;
        bool availability[2] ;
      } settings_map[] = {{GRPC_ARG_MAX_CONCURRENT_STREAMS,
                           GRPC_CHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS,
                           {-1, 0, INT32_MAX},
                           {true, false}},
                          {GRPC_ARG_HTTP2_HPACK_TABLE_SIZE_DECODER,
                           GRPC_CHTTP2_SETTINGS_HEADER_TABLE_SIZE,
                           {-1, 0, INT32_MAX},
                           {true, true}},
                          {GRPC_ARG_MAX_METADATA_SIZE,
                           GRPC_CHTTP2_SETTINGS_MAX_HEADER_LIST_SIZE,
                           {-1, 0, INT32_MAX},
                           {true, true}},
                          {GRPC_ARG_HTTP2_MAX_FRAME_SIZE,
                           GRPC_CHTTP2_SETTINGS_MAX_FRAME_SIZE,
                           {-1, 16384, 16777215},
                           {true, true}},
                          {GRPC_ARG_HTTP2_ENABLE_TRUE_BINARY,
                           GRPC_CHTTP2_SETTINGS_GRPC_ALLOW_TRUE_BINARY_METADATA,
                           {1, 0, 1},
                           {true, true}},
                          {GRPC_ARG_HTTP2_STREAM_LOOKAHEAD_BYTES,
                           GRPC_CHTTP2_SETTINGS_INITIAL_WINDOW_SIZE,
                           {-1, 5, INT32_MAX},
                           {true, true}}};
      for (j = 0; j < static_cast<int> GPR_ARRAY_SIZE(settings_map); j++) {
        if (0 == strcmp(channel_args->args[i].key,
                        settings_map[j].channel_arg_name)) {
          if (!settings_map[j].availability[is_client]) {
            gpr_log(GPR_DEBUG, "%s is not available on %s",
                    settings_map[j].channel_arg_name,
                    is_client ? "clients" : "servers");
          } else {
            int value = grpc_channel_arg_get_integer(
                &channel_args->args[i], settings_map[j].integer_options);
            if (value >= 0) {
              queue_setting_update(t, settings_map[j].setting_id,
                                   static_cast<uint32_t>(value));
            }
          }
          break;
        }
      }
    }
  }
  if (channelz_enabled) {
    t->channelz_socket =
        grpc_core::MakeRefCounted<grpc_core::channelz::SocketNode>(
            "", t->peer_string,
            absl::StrFormat("%s %s", get_vtable()->name, t->peer_string));
  }
  return enable_bdp;
}
static void init_transport_keepalive_settings(grpc_chttp2_transport* t) {
  if (t->is_client) {
    t->keepalive_time = g_default_client_keepalive_time_ms == INT_MAX
                            ? GRPC_MILLIS_INF_FUTURE
                            : g_default_client_keepalive_time_ms;
    t->keepalive_timeout = g_default_client_keepalive_timeout_ms == INT_MAX
                               ? GRPC_MILLIS_INF_FUTURE
                               : g_default_client_keepalive_timeout_ms;
    t->keepalive_permit_without_calls =
        g_default_client_keepalive_permit_without_calls;
  } else {
    t->keepalive_time = g_default_server_keepalive_time_ms == INT_MAX
                            ? GRPC_MILLIS_INF_FUTURE
                            : g_default_server_keepalive_time_ms;
    t->keepalive_timeout = g_default_server_keepalive_timeout_ms == INT_MAX
                               ? GRPC_MILLIS_INF_FUTURE
                               : g_default_server_keepalive_timeout_ms;
    t->keepalive_permit_without_calls =
        g_default_server_keepalive_permit_without_calls;
  }
}
static void configure_transport_ping_policy(grpc_chttp2_transport* t) {
  t->ping_policy.max_pings_without_data = g_default_max_pings_without_data;
  t->ping_policy.min_sent_ping_interval_without_data =
      g_default_min_sent_ping_interval_without_data_ms;
  t->ping_policy.max_ping_strikes = g_default_max_ping_strikes;
  t->ping_policy.min_recv_ping_interval_without_data =
      g_default_min_recv_ping_interval_without_data_ms;
}
static void init_keepalive_pings_if_enabled(grpc_chttp2_transport* t) {
  if (t->keepalive_time != GRPC_MILLIS_INF_FUTURE) {
    t->keepalive_state = GRPC_CHTTP2_KEEPALIVE_STATE_WAITING;
    GRPC_CHTTP2_REF_TRANSPORT(t, "init keepalive ping");
    GRPC_CLOSURE_INIT(&t->init_keepalive_ping_locked, init_keepalive_ping, t,
                      grpc_schedule_on_exec_ctx);
    grpc_timer_init(&t->keepalive_ping_timer,
                    grpc_core::ExecCtx::Get()->Now() + t->keepalive_time,
                    &t->init_keepalive_ping_locked);
  } else {
    t->keepalive_state = GRPC_CHTTP2_KEEPALIVE_STATE_DISABLED;
  }
}
grpc_chttp2_transport::grpc_chttp2_transport(
    const grpc_channel_args* channel_args, grpc_endpoint* ep, bool is_client,
    grpc_resource_user* resource_user)
    : refs(1, &grpc_trace_chttp2_refcount),
      ep(ep),
      peer_string(grpc_endpoint_get_peer(ep)),
      resource_user(resource_user),
      combiner(grpc_combiner_create()),
      state_tracker(is_client ? "client_transport" : "server_transport",
                    GRPC_CHANNEL_READY),
      is_client(is_client),
      next_stream_id(is_client ? 1 : 2),
      deframe_state(is_client ? GRPC_DTS_FH_0 : GRPC_DTS_CLIENT_PREFIX_0) {
  GPR_ASSERT(strlen(GRPC_CHTTP2_CLIENT_CONNECT_STRING) ==
             GRPC_CHTTP2_CLIENT_CONNECT_STRLEN);
  base.vtable = get_vtable();
  grpc_chttp2_stream_map_init(&stream_map, 8);
  grpc_slice_buffer_init(&read_buffer);
  grpc_slice_buffer_init(&outbuf);
  if (is_client) {
    grpc_slice_buffer_add(&outbuf, grpc_slice_from_copied_string(
                                       GRPC_CHTTP2_CLIENT_CONNECT_STRING));
  }
  grpc_chttp2_hpack_compressor_init(&hpack_compressor);
  grpc_slice_buffer_init(&qbuf);
  size_t i;
  int j;
  for (i = 0; i < GRPC_CHTTP2_NUM_SETTINGS; i++) {
    for (j = 0; j < GRPC_NUM_SETTING_SETS; j++) {
      settings[j][i] = grpc_chttp2_settings_parameters[i].default_value;
    }
  }
  grpc_chttp2_hpack_parser_init(&hpack_parser);
  grpc_chttp2_goaway_parser_init(&goaway_parser);
  if (is_client) {
    queue_setting_update(this, GRPC_CHTTP2_SETTINGS_ENABLE_PUSH, 0);
    queue_setting_update(this, GRPC_CHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 0);
  }
  queue_setting_update(this, GRPC_CHTTP2_SETTINGS_MAX_HEADER_LIST_SIZE,
                       DEFAULT_MAX_HEADER_LIST_SIZE);
  queue_setting_update(this,
                       GRPC_CHTTP2_SETTINGS_GRPC_ALLOW_TRUE_BINARY_METADATA, 1);
  configure_transport_ping_policy(this);
  init_transport_keepalive_settings(this);
  bool enable_bdp = true;
  if (channel_args) {
    enable_bdp = read_channel_args(this, channel_args, is_client);
  }
  if (g_flow_control_enabled) {
    flow_control.Init<grpc_core::chttp2::TransportFlowControl>(this,
                                                               enable_bdp);
  } else {
    flow_control.Init<grpc_core::chttp2::TransportFlowControlDisabled>(this);
    enable_bdp = false;
  }
  ping_state.pings_before_data_required = 0;
  ping_state.is_delayed_ping_timer_set = false;
  ping_state.last_ping_sent_time = GRPC_MILLIS_INF_PAST;
  ping_recv_state.last_ping_recv_time = GRPC_MILLIS_INF_PAST;
  ping_recv_state.ping_strikes = 0;
  init_keepalive_pings_if_enabled(this);
  if (enable_bdp) {
    GRPC_CHTTP2_REF_TRANSPORT(this, "bdp_ping");
    schedule_bdp_ping_locked(this);
    grpc_chttp2_act_on_flowctl_action(flow_control->PeriodicUpdate(), this,
                                      nullptr);
  }
  grpc_chttp2_initiate_write(this, GRPC_CHTTP2_INITIATE_WRITE_INITIAL_WRITE);
  post_benign_reclaimer(this);
}
static void destroy_transport_locked(void* tp, grpc_error* ) {
  grpc_chttp2_transport* t = static_cast<grpc_chttp2_transport*>(tp);
  t->destroying = 1;
  close_transport_locked(
      t, grpc_error_set_int(
             GRPC_ERROR_CREATE_FROM_STATIC_STRING("Transport destroyed"),
             GRPC_ERROR_INT_OCCURRED_DURING_WRITE, t->write_state));
  GRPC_CHTTP2_UNREF_TRANSPORT(t, "destroy");
}
static void destroy_transport(grpc_transport* gt) {
  grpc_chttp2_transport* t = reinterpret_cast<grpc_chttp2_transport*>(gt);
  t->combiner->Run(GRPC_CLOSURE_CREATE(destroy_transport_locked, t, nullptr),
                   GRPC_ERROR_NONE);
}
static void close_transport_locked(grpc_chttp2_transport* t,
                                   grpc_error* error) {
  end_all_the_calls(t, GRPC_ERROR_REF(error));
  cancel_pings(t, GRPC_ERROR_REF(error));
  if (t->closed_with_error == GRPC_ERROR_NONE) {
    if (!grpc_error_has_clear_grpc_status(error)) {
      error = grpc_error_set_int(error, GRPC_ERROR_INT_GRPC_STATUS,
                                 GRPC_STATUS_UNAVAILABLE);
    }
    if (t->write_state != GRPC_CHTTP2_WRITE_STATE_IDLE) {
      if (t->close_transport_on_writes_finished == nullptr) {
        t->close_transport_on_writes_finished =
            GRPC_ERROR_CREATE_FROM_STATIC_STRING(
                "Delayed close due to in-progress write");
      }
      t->close_transport_on_writes_finished =
          grpc_error_add_child(t->close_transport_on_writes_finished, error);
      return;
    }
    GPR_ASSERT(error != GRPC_ERROR_NONE);
    t->closed_with_error = GRPC_ERROR_REF(error);
    connectivity_state_set(t, GRPC_CHANNEL_SHUTDOWN, "close_transport");
    if (t->ping_state.is_delayed_ping_timer_set) {
      grpc_timer_cancel(&t->ping_state.delayed_ping_timer);
    }
    if (t->have_next_bdp_ping_timer) {
      grpc_timer_cancel(&t->next_bdp_ping_timer);
    }
    switch (t->keepalive_state) {
      case GRPC_CHTTP2_KEEPALIVE_STATE_WAITING:
        grpc_timer_cancel(&t->keepalive_ping_timer);
        break;
      case GRPC_CHTTP2_KEEPALIVE_STATE_PINGING:
        grpc_timer_cancel(&t->keepalive_ping_timer);
        grpc_timer_cancel(&t->keepalive_watchdog_timer);
        break;
      case GRPC_CHTTP2_KEEPALIVE_STATE_DYING:
      case GRPC_CHTTP2_KEEPALIVE_STATE_DISABLED:
        break;
    }
    grpc_chttp2_stream* s;
    while (grpc_chttp2_list_pop_writable_stream(t, &s)) {
      GRPC_CHTTP2_STREAM_UNREF(s, "chttp2_writing:close");
    }
    GPR_ASSERT(t->write_state == GRPC_CHTTP2_WRITE_STATE_IDLE);
    grpc_endpoint_shutdown(t->ep, GRPC_ERROR_REF(error));
  }
  if (t->notify_on_receive_settings != nullptr) {
    grpc_core::ExecCtx::Run(DEBUG_LOCATION, t->notify_on_receive_settings,
                            GRPC_ERROR_CANCELLED);
    t->notify_on_receive_settings = nullptr;
  }
  GRPC_ERROR_UNREF(error);
}
#ifndef NDEBUG
void grpc_chttp2_stream_ref(grpc_chttp2_stream* s, const char* reason) {
  grpc_stream_ref(s->refcount, reason);
}
void grpc_chttp2_stream_unref(grpc_chttp2_stream* s, const char* reason) {
  grpc_stream_unref(s->refcount, reason);
}
#else
void grpc_chttp2_stream_ref(grpc_chttp2_stream* s) {
  grpc_stream_ref(s->refcount);
}
void grpc_chttp2_stream_unref(grpc_chttp2_stream* s) {
  grpc_stream_unref(s->refcount);
}
#endif
grpc_chttp2_stream::Reffer::Reffer(grpc_chttp2_stream* s) {
  GRPC_CHTTP2_STREAM_REF(s, "chttp2");
  GRPC_CHTTP2_REF_TRANSPORT(s->t, "stream");
}
grpc_chttp2_stream::grpc_chttp2_stream(grpc_chttp2_transport* t,
                                       grpc_stream_refcount* refcount,
                                       const void* server_data,
                                       grpc_core::Arena* arena)
    : t(t),
      refcount(refcount),
      reffer(this),
      metadata_buffer{grpc_chttp2_incoming_metadata_buffer(arena),
                      grpc_chttp2_incoming_metadata_buffer(arena)} {
  if (server_data) {
    id = static_cast<uint32_t>((uintptr_t)server_data);
    *t->accepting_stream = this;
    grpc_chttp2_stream_map_add(&t->stream_map, id, this);
    post_destructive_reclaimer(t);
  }
  if (t->flow_control->flow_control_enabled()) {
    flow_control.Init<grpc_core::chttp2::StreamFlowControl>(
        static_cast<grpc_core::chttp2::TransportFlowControl*>(
            t->flow_control.get()),
        this);
  } else {
    flow_control.Init<grpc_core::chttp2::StreamFlowControlDisabled>();
  }
  grpc_slice_buffer_init(&frame_storage);
  grpc_slice_buffer_init(&unprocessed_incoming_frames_buffer);
  grpc_slice_buffer_init(&flow_controlled_buffer);
  GRPC_CLOSURE_INIT(&reset_byte_stream, ::reset_byte_stream, this, nullptr);
}
grpc_chttp2_stream::~grpc_chttp2_stream() {
  if (t->channelz_socket != nullptr) {
    if ((t->is_client && eos_received) || (!t->is_client && eos_sent)) {
      t->channelz_socket->RecordStreamSucceeded();
    } else {
      t->channelz_socket->RecordStreamFailed();
    }
  }
  GPR_ASSERT((write_closed && read_closed) || id == 0);
  if (id != 0) {
    GPR_ASSERT(grpc_chttp2_stream_map_find(&t->stream_map, id) == nullptr);
  }
  grpc_slice_buffer_destroy_internal(&unprocessed_incoming_frames_buffer);
  grpc_slice_buffer_destroy_internal(&frame_storage);
  if (stream_compression_method != GRPC_STREAM_COMPRESSION_IDENTITY_COMPRESS) {
    grpc_slice_buffer_destroy_internal(&compressed_data_buffer);
  }
  if (stream_decompression_method !=
      GRPC_STREAM_COMPRESSION_IDENTITY_DECOMPRESS) {
    grpc_slice_buffer_destroy_internal(&decompressed_data_buffer);
  }
  grpc_chttp2_list_remove_stalled_by_transport(t, this);
  grpc_chttp2_list_remove_stalled_by_stream(t, this);
  for (int i = 0; i < STREAM_LIST_COUNT; i++) {
    if (GPR_UNLIKELY(included[i])) {
      gpr_log(GPR_ERROR, "%s stream %d still included in list %d",
              t->is_client ? "client" : "server", id, i);
      abort();
    }
  }
  GPR_ASSERT(send_initial_metadata_finished == nullptr);
  GPR_ASSERT(fetching_send_message == nullptr);
  GPR_ASSERT(send_trailing_metadata_finished == nullptr);
  GPR_ASSERT(recv_initial_metadata_ready == nullptr);
  GPR_ASSERT(recv_message_ready == nullptr);
  GPR_ASSERT(recv_trailing_metadata_finished == nullptr);
  grpc_slice_buffer_destroy_internal(&flow_controlled_buffer);
  GRPC_ERROR_UNREF(read_closed_error);
  GRPC_ERROR_UNREF(write_closed_error);
  GRPC_ERROR_UNREF(byte_stream_error);
  flow_control.Destroy();
  if (t->resource_user != nullptr) {
    grpc_resource_user_free(t->resource_user, GRPC_RESOURCE_QUOTA_CALL_SIZE);
  }
  GRPC_CHTTP2_UNREF_TRANSPORT(t, "stream");
  grpc_core::ExecCtx::Run(DEBUG_LOCATION, destroy_stream_arg, GRPC_ERROR_NONE);
}
static int init_stream(grpc_transport* gt, grpc_stream* gs,
                       grpc_stream_refcount* refcount, const void* server_data,
                       grpc_core::Arena* arena) {
  GPR_TIMER_SCOPE("init_stream", 0);
  grpc_chttp2_transport* t = reinterpret_cast<grpc_chttp2_transport*>(gt);
  new (gs) grpc_chttp2_stream(t, refcount, server_data, arena);
  return 0;
}
static void destroy_stream_locked(void* sp, grpc_error* ) {
  GPR_TIMER_SCOPE("destroy_stream", 0);
  grpc_chttp2_stream* s = static_cast<grpc_chttp2_stream*>(sp);
  s->~grpc_chttp2_stream();
}
static void destroy_stream(grpc_transport* gt, grpc_stream* gs,
                           grpc_closure* then_schedule_closure) {
  GPR_TIMER_SCOPE("destroy_stream", 0);
  grpc_chttp2_transport* t = reinterpret_cast<grpc_chttp2_transport*>(gt);
  grpc_chttp2_stream* s = reinterpret_cast<grpc_chttp2_stream*>(gs);
  if (s->stream_compression_method !=
          GRPC_STREAM_COMPRESSION_IDENTITY_COMPRESS &&
      s->stream_compression_ctx != nullptr) {
    grpc_stream_compression_context_destroy(s->stream_compression_ctx);
    s->stream_compression_ctx = nullptr;
  }
  if (s->stream_decompression_method !=
          GRPC_STREAM_COMPRESSION_IDENTITY_DECOMPRESS &&
      s->stream_decompression_ctx != nullptr) {
    grpc_stream_compression_context_destroy(s->stream_decompression_ctx);
    s->stream_decompression_ctx = nullptr;
  }
  s->destroy_stream_arg = then_schedule_closure;
  t->combiner->Run(
      GRPC_CLOSURE_INIT(&s->destroy_stream, destroy_stream_locked, s, nullptr),
      GRPC_ERROR_NONE);
}
grpc_chttp2_stream* grpc_chttp2_parsing_accept_stream(grpc_chttp2_transport* t,
                                                      uint32_t id) {
  if (t->accept_stream_cb == nullptr) {
    return nullptr;
  }
  if (t->resource_user != nullptr &&
      !grpc_resource_user_safe_alloc(t->resource_user,
                                     GRPC_RESOURCE_QUOTA_CALL_SIZE)) {
    gpr_log(GPR_ERROR, "Memory exhausted, rejecting the stream.");
    grpc_chttp2_add_rst_stream_to_next_write(t, id, GRPC_HTTP2_REFUSED_STREAM,
                                             nullptr);
    grpc_chttp2_initiate_write(t, GRPC_CHTTP2_INITIATE_WRITE_RST_STREAM);
    return nullptr;
  }
  grpc_chttp2_stream* accepting = nullptr;
  GPR_ASSERT(t->accepting_stream == nullptr);
  t->accepting_stream = &accepting;
  t->accept_stream_cb(t->accept_stream_cb_user_data, &t->base,
                      (void*)static_cast<uintptr_t>(id));
  t->accepting_stream = nullptr;
  return accepting;
}
static const char* write_state_name(grpc_chttp2_write_state st) {
  switch (st) {
    case GRPC_CHTTP2_WRITE_STATE_IDLE:
      return "IDLE";
    case GRPC_CHTTP2_WRITE_STATE_WRITING:
      return "WRITING";
    case GRPC_CHTTP2_WRITE_STATE_WRITING_WITH_MORE:
      return "WRITING+MORE";
  }
  GPR_UNREACHABLE_CODE(return "UNKNOWN");
}
static void set_write_state(grpc_chttp2_transport* t,
                            grpc_chttp2_write_state st, const char* reason) {
  GRPC_CHTTP2_IF_TRACING(
      gpr_log(GPR_INFO, "W:%p %s [%s] state %s -> %s [%s]", t,
              t->is_client ? "CLIENT" : "SERVER", t->peer_string,
              write_state_name(t->write_state), write_state_name(st), reason));
  t->write_state = st;
  if (st == GRPC_CHTTP2_WRITE_STATE_IDLE) {
    grpc_core::ExecCtx::RunList(DEBUG_LOCATION, &t->run_after_write);
    if (t->close_transport_on_writes_finished != nullptr) {
      grpc_error* err = t->close_transport_on_writes_finished;
      t->close_transport_on_writes_finished = nullptr;
      close_transport_locked(t, err);
    }
  }
}
static void inc_initiate_write_reason(
    grpc_chttp2_initiate_write_reason reason) {
  switch (reason) {
    case GRPC_CHTTP2_INITIATE_WRITE_INITIAL_WRITE:
      GRPC_STATS_INC_HTTP2_INITIATE_WRITE_DUE_TO_INITIAL_WRITE();
      break;
    case GRPC_CHTTP2_INITIATE_WRITE_START_NEW_STREAM:
      GRPC_STATS_INC_HTTP2_INITIATE_WRITE_DUE_TO_START_NEW_STREAM();
      break;
    case GRPC_CHTTP2_INITIATE_WRITE_SEND_MESSAGE:
      GRPC_STATS_INC_HTTP2_INITIATE_WRITE_DUE_TO_SEND_MESSAGE();
      break;
    case GRPC_CHTTP2_INITIATE_WRITE_SEND_INITIAL_METADATA:
      GRPC_STATS_INC_HTTP2_INITIATE_WRITE_DUE_TO_SEND_INITIAL_METADATA();
      break;
    case GRPC_CHTTP2_INITIATE_WRITE_SEND_TRAILING_METADATA:
      GRPC_STATS_INC_HTTP2_INITIATE_WRITE_DUE_TO_SEND_TRAILING_METADATA();
      break;
    case GRPC_CHTTP2_INITIATE_WRITE_RETRY_SEND_PING:
      GRPC_STATS_INC_HTTP2_INITIATE_WRITE_DUE_TO_RETRY_SEND_PING();
      break;
    case GRPC_CHTTP2_INITIATE_WRITE_CONTINUE_PINGS:
      GRPC_STATS_INC_HTTP2_INITIATE_WRITE_DUE_TO_CONTINUE_PINGS();
      break;
    case GRPC_CHTTP2_INITIATE_WRITE_GOAWAY_SENT:
      GRPC_STATS_INC_HTTP2_INITIATE_WRITE_DUE_TO_GOAWAY_SENT();
      break;
    case GRPC_CHTTP2_INITIATE_WRITE_RST_STREAM:
      GRPC_STATS_INC_HTTP2_INITIATE_WRITE_DUE_TO_RST_STREAM();
      break;
    case GRPC_CHTTP2_INITIATE_WRITE_CLOSE_FROM_API:
      GRPC_STATS_INC_HTTP2_INITIATE_WRITE_DUE_TO_CLOSE_FROM_API();
      break;
    case GRPC_CHTTP2_INITIATE_WRITE_STREAM_FLOW_CONTROL:
      GRPC_STATS_INC_HTTP2_INITIATE_WRITE_DUE_TO_STREAM_FLOW_CONTROL();
      break;
    case GRPC_CHTTP2_INITIATE_WRITE_TRANSPORT_FLOW_CONTROL:
      GRPC_STATS_INC_HTTP2_INITIATE_WRITE_DUE_TO_TRANSPORT_FLOW_CONTROL();
      break;
    case GRPC_CHTTP2_INITIATE_WRITE_SEND_SETTINGS:
      GRPC_STATS_INC_HTTP2_INITIATE_WRITE_DUE_TO_SEND_SETTINGS();
      break;
    case GRPC_CHTTP2_INITIATE_WRITE_FLOW_CONTROL_UNSTALLED_BY_SETTING:
      GRPC_STATS_INC_HTTP2_INITIATE_WRITE_DUE_TO_FLOW_CONTROL_UNSTALLED_BY_SETTING();
      break;
    case GRPC_CHTTP2_INITIATE_WRITE_FLOW_CONTROL_UNSTALLED_BY_UPDATE:
      GRPC_STATS_INC_HTTP2_INITIATE_WRITE_DUE_TO_FLOW_CONTROL_UNSTALLED_BY_UPDATE();
      break;
    case GRPC_CHTTP2_INITIATE_WRITE_APPLICATION_PING:
      GRPC_STATS_INC_HTTP2_INITIATE_WRITE_DUE_TO_APPLICATION_PING();
      break;
    case GRPC_CHTTP2_INITIATE_WRITE_KEEPALIVE_PING:
      GRPC_STATS_INC_HTTP2_INITIATE_WRITE_DUE_TO_KEEPALIVE_PING();
      break;
    case GRPC_CHTTP2_INITIATE_WRITE_TRANSPORT_FLOW_CONTROL_UNSTALLED:
      GRPC_STATS_INC_HTTP2_INITIATE_WRITE_DUE_TO_TRANSPORT_FLOW_CONTROL_UNSTALLED();
      break;
    case GRPC_CHTTP2_INITIATE_WRITE_PING_RESPONSE:
      GRPC_STATS_INC_HTTP2_INITIATE_WRITE_DUE_TO_PING_RESPONSE();
      break;
    case GRPC_CHTTP2_INITIATE_WRITE_FORCE_RST_STREAM:
      GRPC_STATS_INC_HTTP2_INITIATE_WRITE_DUE_TO_FORCE_RST_STREAM();
      break;
  }
}
void grpc_chttp2_initiate_write(grpc_chttp2_transport* t,
                                grpc_chttp2_initiate_write_reason reason) {
  GPR_TIMER_SCOPE("grpc_chttp2_initiate_write", 0);
  switch (t->write_state) {
    case GRPC_CHTTP2_WRITE_STATE_IDLE:
      inc_initiate_write_reason(reason);
      set_write_state(t, GRPC_CHTTP2_WRITE_STATE_WRITING,
                      grpc_chttp2_initiate_write_reason_string(reason));
      GRPC_CHTTP2_REF_TRANSPORT(t, "writing");
      t->combiner->FinallyRun(
          GRPC_CLOSURE_INIT(&t->write_action_begin_locked,
                            write_action_begin_locked, t, nullptr),
          GRPC_ERROR_NONE);
      break;
    case GRPC_CHTTP2_WRITE_STATE_WRITING:
      set_write_state(t, GRPC_CHTTP2_WRITE_STATE_WRITING_WITH_MORE,
                      grpc_chttp2_initiate_write_reason_string(reason));
      break;
    case GRPC_CHTTP2_WRITE_STATE_WRITING_WITH_MORE:
      break;
  }
}
void grpc_chttp2_mark_stream_writable(grpc_chttp2_transport* t,
                                      grpc_chttp2_stream* s) {
  if (t->closed_with_error == GRPC_ERROR_NONE &&
      grpc_chttp2_list_add_writable_stream(t, s)) {
    GRPC_CHTTP2_STREAM_REF(s, "chttp2_writing:become");
  }
}
static const char* begin_writing_desc(bool partial) {
  if (partial) {
    return "begin partial write in background";
  } else {
    return "begin write in current thread";
  }
}
static void write_action_begin_locked(void* gt, grpc_error* ) {
  GPR_TIMER_SCOPE("write_action_begin_locked", 0);
  grpc_chttp2_transport* t = static_cast<grpc_chttp2_transport*>(gt);
  GPR_ASSERT(t->write_state != GRPC_CHTTP2_WRITE_STATE_IDLE);
  grpc_chttp2_begin_write_result r;
  if (t->closed_with_error != GRPC_ERROR_NONE) {
    r.writing = false;
  } else {
    r = grpc_chttp2_begin_write(t);
  }
  if (r.writing) {
    if (r.partial) {
      GRPC_STATS_INC_HTTP2_PARTIAL_WRITES();
    }
    set_write_state(t,
                    r.partial ? GRPC_CHTTP2_WRITE_STATE_WRITING_WITH_MORE
                              : GRPC_CHTTP2_WRITE_STATE_WRITING,
                    begin_writing_desc(r.partial));
    write_action(t, GRPC_ERROR_NONE);
    if (t->reading_paused_on_pending_induced_frames) {
      GPR_ASSERT(t->num_pending_induced_frames == 0);
      GRPC_CHTTP2_IF_TRACING(gpr_log(
          GPR_INFO,
          "transport %p : Resuming reading after being paused due to too "
          "many unwritten SETTINGS ACK, PINGS ACK and RST_STREAM frames",
          t));
      t->reading_paused_on_pending_induced_frames = false;
      continue_read_action_locked(t);
    }
  } else {
    GRPC_STATS_INC_HTTP2_SPURIOUS_WRITES_BEGUN();
    set_write_state(t, GRPC_CHTTP2_WRITE_STATE_IDLE, "begin writing nothing");
    GRPC_CHTTP2_UNREF_TRANSPORT(t, "writing");
  }
}
static void write_action(void* gt, grpc_error* ) {
  GPR_TIMER_SCOPE("write_action", 0);
  grpc_chttp2_transport* t = static_cast<grpc_chttp2_transport*>(gt);
  void* cl = t->cl;
  t->cl = nullptr;
  grpc_endpoint_write(
      t->ep, &t->outbuf,
      GRPC_CLOSURE_INIT(&t->write_action_end_locked, write_action_end, t,
                        grpc_schedule_on_exec_ctx),
      cl);
}
static void write_action_end(void* tp, grpc_error* error) {
  grpc_chttp2_transport* t = static_cast<grpc_chttp2_transport*>(tp);
  t->combiner->Run(GRPC_CLOSURE_INIT(&t->write_action_end_locked,
                                     write_action_end_locked, t, nullptr),
                   GRPC_ERROR_REF(error));
}
static void write_action_end_locked(void* tp, grpc_error* error) {
  GPR_TIMER_SCOPE("terminate_writing_with_lock", 0);
  grpc_chttp2_transport* t = static_cast<grpc_chttp2_transport*>(tp);
  bool closed = false;
  if (error != GRPC_ERROR_NONE) {
    close_transport_locked(t, GRPC_ERROR_REF(error));
    closed = true;
  }
  if (t->sent_goaway_state == GRPC_CHTTP2_GOAWAY_SEND_SCHEDULED) {
    t->sent_goaway_state = GRPC_CHTTP2_GOAWAY_SENT;
    closed = true;
    if (grpc_chttp2_stream_map_size(&t->stream_map) == 0) {
      close_transport_locked(
          t, GRPC_ERROR_CREATE_FROM_STATIC_STRING("goaway sent"));
    }
  }
  switch (t->write_state) {
    case GRPC_CHTTP2_WRITE_STATE_IDLE:
      GPR_UNREACHABLE_CODE(break);
    case GRPC_CHTTP2_WRITE_STATE_WRITING:
      GPR_TIMER_MARK("state=writing", 0);
      set_write_state(t, GRPC_CHTTP2_WRITE_STATE_IDLE, "finish writing");
      break;
    case GRPC_CHTTP2_WRITE_STATE_WRITING_WITH_MORE:
      GPR_TIMER_MARK("state=writing_stale_no_poller", 0);
      set_write_state(t, GRPC_CHTTP2_WRITE_STATE_WRITING, "continue writing");
      GRPC_CHTTP2_REF_TRANSPORT(t, "writing");
      if (!closed) {
        grpc_core::ExecCtx::RunList(DEBUG_LOCATION, &t->run_after_write);
      }
      t->combiner->FinallyRun(
          GRPC_CLOSURE_INIT(&t->write_action_begin_locked,
                            write_action_begin_locked, t, nullptr),
          GRPC_ERROR_NONE);
      break;
  }
  grpc_chttp2_end_write(t, GRPC_ERROR_REF(error));
  GRPC_CHTTP2_UNREF_TRANSPORT(t, "writing");
}
static void queue_setting_update(grpc_chttp2_transport* t,
                                 grpc_chttp2_setting_id id, uint32_t value) {
  const grpc_chttp2_setting_parameters* sp =
      &grpc_chttp2_settings_parameters[id];
  uint32_t use_value = GPR_CLAMP(value, sp->min_value, sp->max_value);
  if (use_value != value) {
    gpr_log(GPR_INFO, "Requested parameter %s clamped from %d to %d", sp->name,
            value, use_value);
  }
  if (use_value != t->settings[GRPC_LOCAL_SETTINGS][id]) {
    t->settings[GRPC_LOCAL_SETTINGS][id] = use_value;
    t->dirtied_local_settings = 1;
  }
}
void grpc_chttp2_add_incoming_goaway(grpc_chttp2_transport* t,
                                     uint32_t goaway_error,
                                     uint32_t last_stream_id,
                                     const grpc_slice& goaway_text) {
  if (t->goaway_error != GRPC_ERROR_NONE) {
    GRPC_ERROR_UNREF(t->goaway_error);
  }
  t->goaway_error = grpc_error_set_str(
      grpc_error_set_int(
          grpc_error_set_int(
              GRPC_ERROR_CREATE_FROM_STATIC_STRING("GOAWAY received"),
              GRPC_ERROR_INT_HTTP2_ERROR, static_cast<intptr_t>(goaway_error)),
          GRPC_ERROR_INT_GRPC_STATUS, GRPC_STATUS_UNAVAILABLE),
      GRPC_ERROR_STR_RAW_BYTES, goaway_text);
  GRPC_CHTTP2_IF_TRACING(
      gpr_log(GPR_INFO, "transport %p got goaway with last stream id %d", t,
              last_stream_id));
  if (goaway_error != GRPC_HTTP2_NO_ERROR) {
    gpr_log(GPR_INFO, "%s: Got goaway [%d] err=%s", t->peer_string,
            goaway_error, grpc_error_string(t->goaway_error));
  }
  if (GPR_UNLIKELY(t->is_client &&
                   goaway_error == GRPC_HTTP2_ENHANCE_YOUR_CALM &&
                   grpc_slice_str_cmp(goaway_text, "too_many_pings") == 0)) {
    gpr_log(GPR_ERROR,
            "Received a GOAWAY with error code ENHANCE_YOUR_CALM and debug "
            "data equal to \"too_many_pings\"");
    double current_keepalive_time_ms = static_cast<double>(t->keepalive_time);
    constexpr int max_keepalive_time_ms =
        INT_MAX / KEEPALIVE_TIME_BACKOFF_MULTIPLIER;
    t->keepalive_time =
        current_keepalive_time_ms > static_cast<double>(max_keepalive_time_ms)
            ? GRPC_MILLIS_INF_FUTURE
            : static_cast<grpc_millis>(current_keepalive_time_ms *
                                       KEEPALIVE_TIME_BACKOFF_MULTIPLIER);
  }
  connectivity_state_set(t, GRPC_CHANNEL_TRANSIENT_FAILURE, "got_goaway");
}
static void maybe_start_some_streams(grpc_chttp2_transport* t) {
  grpc_chttp2_stream* s;
  if (t->goaway_error != GRPC_ERROR_NONE) {
    while (grpc_chttp2_list_pop_waiting_for_concurrency(t, &s)) {
      grpc_chttp2_cancel_stream(
          t, s,
          grpc_error_set_int(
              GRPC_ERROR_CREATE_FROM_STATIC_STRING("GOAWAY received"),
              GRPC_ERROR_INT_GRPC_STATUS, GRPC_STATUS_UNAVAILABLE));
    }
    return;
  }
  while (t->next_stream_id <= MAX_CLIENT_STREAM_ID &&
         grpc_chttp2_stream_map_size(&t->stream_map) <
             t->settings[GRPC_PEER_SETTINGS]
                        [GRPC_CHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS] &&
         grpc_chttp2_list_pop_waiting_for_concurrency(t, &s)) {
    GRPC_CHTTP2_IF_TRACING(gpr_log(
        GPR_INFO,
        "HTTP:%s: Transport %p allocating new grpc_chttp2_stream %p to id %d",
        t->is_client ? "CLI" : "SVR", t, s, t->next_stream_id));
    GPR_ASSERT(s->id == 0);
    s->id = t->next_stream_id;
    t->next_stream_id += 2;
    if (t->next_stream_id >= MAX_CLIENT_STREAM_ID) {
      connectivity_state_set(t, GRPC_CHANNEL_TRANSIENT_FAILURE,
                             "no_more_stream_ids");
    }
    grpc_chttp2_stream_map_add(&t->stream_map, s->id, s);
    post_destructive_reclaimer(t);
    grpc_chttp2_mark_stream_writable(t, s);
    grpc_chttp2_initiate_write(t, GRPC_CHTTP2_INITIATE_WRITE_START_NEW_STREAM);
  }
  if (t->next_stream_id >= MAX_CLIENT_STREAM_ID) {
    while (grpc_chttp2_list_pop_waiting_for_concurrency(t, &s)) {
      grpc_chttp2_cancel_stream(
          t, s,
          grpc_error_set_int(
              GRPC_ERROR_CREATE_FROM_STATIC_STRING("Stream IDs exhausted"),
              GRPC_ERROR_INT_GRPC_STATUS, GRPC_STATUS_UNAVAILABLE));
    }
  }
}
#define CLOSURE_BARRIER_MAY_COVER_WRITE (1 << 0)
#define CLOSURE_BARRIER_FIRST_REF_BIT (1 << 16)
static grpc_closure* add_closure_barrier(grpc_closure* closure) {
  closure->next_data.scratch += CLOSURE_BARRIER_FIRST_REF_BIT;
  return closure;
}
static void null_then_sched_closure(grpc_closure** closure) {
  grpc_closure* c = *closure;
  *closure = nullptr;
  grpc_core::ExecCtx::Run(DEBUG_LOCATION, c, GRPC_ERROR_NONE);
}
void grpc_chttp2_complete_closure_step(grpc_chttp2_transport* t,
                                       grpc_chttp2_stream* ,
                                       grpc_closure** pclosure,
                                       grpc_error* error, const char* desc) {
  grpc_closure* closure = *pclosure;
  *pclosure = nullptr;
  if (closure == nullptr) {
    GRPC_ERROR_UNREF(error);
    return;
  }
  closure->next_data.scratch -= CLOSURE_BARRIER_FIRST_REF_BIT;
  if (GRPC_TRACE_FLAG_ENABLED(grpc_http_trace)) {
    const char* errstr = grpc_error_string(error);
    gpr_log(
        GPR_INFO,
        "complete_closure_step: t=%p %p refs=%d flags=0x%04x desc=%s err=%s "
        "write_state=%s",
        t, closure,
        static_cast<int>(closure->next_data.scratch /
                         CLOSURE_BARRIER_FIRST_REF_BIT),
        static_cast<int>(closure->next_data.scratch %
                         CLOSURE_BARRIER_FIRST_REF_BIT),
        desc, errstr, write_state_name(t->write_state));
  }
  if (error != GRPC_ERROR_NONE) {
    if (closure->error_data.error == GRPC_ERROR_NONE) {
      closure->error_data.error = GRPC_ERROR_CREATE_FROM_STATIC_STRING(
          "Error in HTTP transport completing operation");
      closure->error_data.error = grpc_error_set_str(
          closure->error_data.error, GRPC_ERROR_STR_TARGET_ADDRESS,
          grpc_slice_from_copied_string(t->peer_string));
    }
    closure->error_data.error =
        grpc_error_add_child(closure->error_data.error, error);
  }
  if (closure->next_data.scratch < CLOSURE_BARRIER_FIRST_REF_BIT) {
    if ((t->write_state == GRPC_CHTTP2_WRITE_STATE_IDLE) ||
        !(closure->next_data.scratch & CLOSURE_BARRIER_MAY_COVER_WRITE)) {
      grpc_core::ExecCtx::Run(DEBUG_LOCATION, closure,
                              closure->error_data.error);
    } else {
      grpc_closure_list_append(&t->run_after_write, closure,
                               closure->error_data.error);
    }
  }
}
static bool contains_non_ok_status(grpc_metadata_batch* batch) {
  if (batch->idx.named.grpc_status != nullptr) {
    return !grpc_mdelem_static_value_eq(batch->idx.named.grpc_status->md,
                                        GRPC_MDELEM_GRPC_STATUS_0);
  }
  return false;
}
static void maybe_become_writable_due_to_send_msg(grpc_chttp2_transport* t,
                                                  grpc_chttp2_stream* s) {
  if (s->id != 0 && (!s->write_buffering ||
                     s->flow_controlled_buffer.length > t->write_buffer_size)) {
    grpc_chttp2_mark_stream_writable(t, s);
    grpc_chttp2_initiate_write(t, GRPC_CHTTP2_INITIATE_WRITE_SEND_MESSAGE);
  }
}
static void add_fetched_slice_locked(grpc_chttp2_transport* t,
                                     grpc_chttp2_stream* s) {
  s->fetched_send_message_length +=
      static_cast<uint32_t> GRPC_SLICE_LENGTH(s->fetching_slice);
  grpc_slice_buffer_add(&s->flow_controlled_buffer, s->fetching_slice);
  maybe_become_writable_due_to_send_msg(t, s);
}
static void continue_fetching_send_locked(grpc_chttp2_transport* t,
                                          grpc_chttp2_stream* s) {
  for (;;) {
    if (s->fetching_send_message == nullptr) {
      abort();
      return;
    }
    if (s->fetched_send_message_length == s->fetching_send_message->length()) {
      int64_t notify_offset = s->next_message_end_offset;
      if (notify_offset <= s->flow_controlled_bytes_written) {
        grpc_chttp2_complete_closure_step(
            t, s, &s->fetching_send_message_finished, GRPC_ERROR_NONE,
            "fetching_send_message_finished");
      } else {
        grpc_chttp2_write_cb* cb = t->write_cb_pool;
        if (cb == nullptr) {
          cb = static_cast<grpc_chttp2_write_cb*>(gpr_malloc(sizeof(*cb)));
        } else {
          t->write_cb_pool = cb->next;
        }
        cb->call_at_byte = notify_offset;
        cb->closure = s->fetching_send_message_finished;
        s->fetching_send_message_finished = nullptr;
        grpc_chttp2_write_cb** list =
            s->fetching_send_message->flags() & GRPC_WRITE_THROUGH
                ? &s->on_write_finished_cbs
                : &s->on_flow_controlled_cbs;
        cb->next = *list;
        *list = cb;
      }
      s->fetching_send_message.reset();
      return;
    } else if (s->fetching_send_message->Next(
                   UINT32_MAX, GRPC_CLOSURE_INIT(&s->complete_fetch_locked,
                                                 ::complete_fetch, s,
                                                 grpc_schedule_on_exec_ctx))) {
      grpc_error* error = s->fetching_send_message->Pull(&s->fetching_slice);
      if (error != GRPC_ERROR_NONE) {
        s->fetching_send_message.reset();
        grpc_chttp2_cancel_stream(t, s, error);
      } else {
        add_fetched_slice_locked(t, s);
      }
    }
  }
}
static void complete_fetch(void* gs, grpc_error* error) {
  grpc_chttp2_stream* s = static_cast<grpc_chttp2_stream*>(gs);
  s->t->combiner->Run(GRPC_CLOSURE_INIT(&s->complete_fetch_locked,
                                        ::complete_fetch_locked, s, nullptr),
                      GRPC_ERROR_REF(error));
}
static void complete_fetch_locked(void* gs, grpc_error* error) {
  grpc_chttp2_stream* s = static_cast<grpc_chttp2_stream*>(gs);
  grpc_chttp2_transport* t = s->t;
  if (error == GRPC_ERROR_NONE) {
    error = s->fetching_send_message->Pull(&s->fetching_slice);
    if (error == GRPC_ERROR_NONE) {
      add_fetched_slice_locked(t, s);
      continue_fetching_send_locked(t, s);
    }
  }
  if (error != GRPC_ERROR_NONE) {
    s->fetching_send_message.reset();
    grpc_chttp2_cancel_stream(t, s, error);
  }
}
static void log_metadata(const grpc_metadata_batch* md_batch, uint32_t id,
                         bool is_client, bool is_initial) {
  for (grpc_linked_mdelem* md = md_batch->list.head; md != nullptr;
       md = md->next) {
    char* key = grpc_slice_to_c_string(GRPC_MDKEY(md->md));
    char* value = grpc_slice_to_c_string(GRPC_MDVALUE(md->md));
    gpr_log(GPR_INFO, "HTTP:%d:%s:%s: %s: %s", id, is_initial ? "HDR" : "TRL",
            is_client ? "CLI" : "SVR", key, value);
    gpr_free(key);
    gpr_free(value);
  }
}
static void perform_stream_op_locked(void* stream_op,
                                     grpc_error* ) {
  GPR_TIMER_SCOPE("perform_stream_op_locked", 0);
  grpc_transport_stream_op_batch* op =
      static_cast<grpc_transport_stream_op_batch*>(stream_op);
  grpc_chttp2_stream* s =
      static_cast<grpc_chttp2_stream*>(op->handler_private.extra_arg);
  grpc_transport_stream_op_batch_payload* op_payload = op->payload;
  grpc_chttp2_transport* t = s->t;
  GRPC_STATS_INC_HTTP2_OP_BATCHES();
  s->context = op->payload->context;
  s->traced = op->is_traced;
  if (GRPC_TRACE_FLAG_ENABLED(grpc_http_trace)) {
    gpr_log(GPR_INFO, "perform_stream_op_locked: %s; on_complete = %p",
            grpc_transport_stream_op_batch_string(op).c_str(), op->on_complete);
    if (op->send_initial_metadata) {
      log_metadata(op_payload->send_initial_metadata.send_initial_metadata,
                   s->id, t->is_client, true);
    }
    if (op->send_trailing_metadata) {
      log_metadata(op_payload->send_trailing_metadata.send_trailing_metadata,
                   s->id, t->is_client, false);
    }
  }
  grpc_closure* on_complete = op->on_complete;
  if (on_complete != nullptr) {
    on_complete->next_data.scratch = CLOSURE_BARRIER_FIRST_REF_BIT;
    on_complete->error_data.error = GRPC_ERROR_NONE;
  }
  if (op->cancel_stream) {
    GRPC_STATS_INC_HTTP2_OP_CANCEL();
    grpc_chttp2_cancel_stream(t, s, op_payload->cancel_stream.cancel_error);
  }
  if (op->send_initial_metadata) {
    if (t->is_client && t->channelz_socket != nullptr) {
      t->channelz_socket->RecordStreamStartedFromLocal();
    }
    GRPC_STATS_INC_HTTP2_OP_SEND_INITIAL_METADATA();
    GPR_ASSERT(s->send_initial_metadata_finished == nullptr);
    on_complete->next_data.scratch |= CLOSURE_BARRIER_MAY_COVER_WRITE;
    if (op_payload->send_initial_metadata.send_initial_metadata->idx.named
                .content_encoding == nullptr ||
        grpc_stream_compression_method_parse(
            GRPC_MDVALUE(
                op_payload->send_initial_metadata.send_initial_metadata->idx
                    .named.content_encoding->md),
            true, &s->stream_compression_method) == 0) {
      s->stream_compression_method = GRPC_STREAM_COMPRESSION_IDENTITY_COMPRESS;
    }
    if (s->stream_compression_method !=
        GRPC_STREAM_COMPRESSION_IDENTITY_COMPRESS) {
      s->uncompressed_data_size = 0;
      s->stream_compression_ctx = nullptr;
      grpc_slice_buffer_init(&s->compressed_data_buffer);
    }
    s->send_initial_metadata_finished = add_closure_barrier(on_complete);
    s->send_initial_metadata =
        op_payload->send_initial_metadata.send_initial_metadata;
    const size_t metadata_size =
        grpc_metadata_batch_size(s->send_initial_metadata);
    const size_t metadata_peer_limit =
        t->settings[GRPC_PEER_SETTINGS]
                   [GRPC_CHTTP2_SETTINGS_MAX_HEADER_LIST_SIZE];
    if (t->is_client) {
      s->deadline = GPR_MIN(s->deadline, s->send_initial_metadata->deadline);
    }
    if (metadata_size > metadata_peer_limit) {
      grpc_chttp2_cancel_stream(
          t, s,
          grpc_error_set_int(
              grpc_error_set_int(
                  grpc_error_set_int(GRPC_ERROR_CREATE_FROM_STATIC_STRING(
                                         "to-be-sent initial metadata size "
                                         "exceeds peer limit"),
                                     GRPC_ERROR_INT_SIZE,
                                     static_cast<intptr_t>(metadata_size)),
                  GRPC_ERROR_INT_LIMIT,
                  static_cast<intptr_t>(metadata_peer_limit)),
              GRPC_ERROR_INT_GRPC_STATUS, GRPC_STATUS_RESOURCE_EXHAUSTED));
    } else {
      if (contains_non_ok_status(s->send_initial_metadata)) {
        s->seen_error = true;
      }
      if (!s->write_closed) {
        if (t->is_client) {
          if (t->closed_with_error == GRPC_ERROR_NONE) {
            GPR_ASSERT(s->id == 0);
            grpc_chttp2_list_add_waiting_for_concurrency(t, s);
            maybe_start_some_streams(t);
          } else {
            grpc_chttp2_cancel_stream(
                t, s,
                grpc_error_set_int(
                    GRPC_ERROR_CREATE_REFERENCING_FROM_STATIC_STRING(
                        "Transport closed", &t->closed_with_error, 1),
                    GRPC_ERROR_INT_GRPC_STATUS, GRPC_STATUS_UNAVAILABLE));
          }
        } else {
          GPR_ASSERT(s->id != 0);
          grpc_chttp2_mark_stream_writable(t, s);
          if (!(op->send_message &&
                (op->payload->send_message.send_message->flags() &
                 GRPC_WRITE_BUFFER_HINT))) {
            grpc_chttp2_initiate_write(
                t, GRPC_CHTTP2_INITIATE_WRITE_SEND_INITIAL_METADATA);
          }
        }
      } else {
        s->send_initial_metadata = nullptr;
        grpc_chttp2_complete_closure_step(
            t, s, &s->send_initial_metadata_finished,
            GRPC_ERROR_CREATE_REFERENCING_FROM_STATIC_STRING(
                "Attempt to send initial metadata after stream was closed",
                &s->write_closed_error, 1),
            "send_initial_metadata_finished");
      }
    }
    if (op_payload->send_initial_metadata.peer_string != nullptr) {
      gpr_atm_rel_store(op_payload->send_initial_metadata.peer_string,
                        (gpr_atm)t->peer_string);
    }
  }
  if (op->send_message) {
    GRPC_STATS_INC_HTTP2_OP_SEND_MESSAGE();
    t->num_messages_in_next_write++;
    GRPC_STATS_INC_HTTP2_SEND_MESSAGE_SIZE(
        op->payload->send_message.send_message->length());
    on_complete->next_data.scratch |= CLOSURE_BARRIER_MAY_COVER_WRITE;
    s->fetching_send_message_finished = add_closure_barrier(op->on_complete);
    if (s->write_closed) {
      op->payload->send_message.stream_write_closed = true;
      op->payload->send_message.send_message.reset();
      grpc_chttp2_complete_closure_step(
          t, s, &s->fetching_send_message_finished, GRPC_ERROR_NONE,
          "fetching_send_message_finished");
    } else {
      GPR_ASSERT(s->fetching_send_message == nullptr);
      uint8_t* frame_hdr = grpc_slice_buffer_tiny_add(
          &s->flow_controlled_buffer, GRPC_HEADER_SIZE_IN_BYTES);
      uint32_t flags = op_payload->send_message.send_message->flags();
      frame_hdr[0] = (flags & GRPC_WRITE_INTERNAL_COMPRESS) != 0;
      size_t len = op_payload->send_message.send_message->length();
      frame_hdr[1] = static_cast<uint8_t>(len >> 24);
      frame_hdr[2] = static_cast<uint8_t>(len >> 16);
      frame_hdr[3] = static_cast<uint8_t>(len >> 8);
      frame_hdr[4] = static_cast<uint8_t>(len);
      s->fetching_send_message =
          std::move(op_payload->send_message.send_message);
      s->fetched_send_message_length = 0;
      s->next_message_end_offset =
          s->flow_controlled_bytes_written +
          static_cast<int64_t>(s->flow_controlled_buffer.length) +
          static_cast<int64_t>(len);
      if (flags & GRPC_WRITE_BUFFER_HINT) {
        s->next_message_end_offset -= t->write_buffer_size;
        s->write_buffering = true;
      } else {
        s->write_buffering = false;
      }
      continue_fetching_send_locked(t, s);
      maybe_become_writable_due_to_send_msg(t, s);
    }
  }
  if (op->send_trailing_metadata) {
    GRPC_STATS_INC_HTTP2_OP_SEND_TRAILING_METADATA();
    GPR_ASSERT(s->send_trailing_metadata_finished == nullptr);
    on_complete->next_data.scratch |= CLOSURE_BARRIER_MAY_COVER_WRITE;
    s->send_trailing_metadata_finished = add_closure_barrier(on_complete);
    s->send_trailing_metadata =
        op_payload->send_trailing_metadata.send_trailing_metadata;
    s->sent_trailing_metadata_op = op_payload->send_trailing_metadata.sent;
    s->write_buffering = false;
    const size_t metadata_size =
        grpc_metadata_batch_size(s->send_trailing_metadata);
    const size_t metadata_peer_limit =
        t->settings[GRPC_PEER_SETTINGS]
                   [GRPC_CHTTP2_SETTINGS_MAX_HEADER_LIST_SIZE];
    if (metadata_size > metadata_peer_limit) {
      grpc_chttp2_cancel_stream(
          t, s,
          grpc_error_set_int(
              grpc_error_set_int(
                  grpc_error_set_int(GRPC_ERROR_CREATE_FROM_STATIC_STRING(
                                         "to-be-sent trailing metadata size "
                                         "exceeds peer limit"),
                                     GRPC_ERROR_INT_SIZE,
                                     static_cast<intptr_t>(metadata_size)),
                  GRPC_ERROR_INT_LIMIT,
                  static_cast<intptr_t>(metadata_peer_limit)),
              GRPC_ERROR_INT_GRPC_STATUS, GRPC_STATUS_RESOURCE_EXHAUSTED));
    } else {
      if (contains_non_ok_status(s->send_trailing_metadata)) {
        s->seen_error = true;
      }
      if (s->write_closed) {
        s->send_trailing_metadata = nullptr;
        s->sent_trailing_metadata_op = nullptr;
        grpc_chttp2_complete_closure_step(
            t, s, &s->send_trailing_metadata_finished,
            grpc_metadata_batch_is_empty(
                op->payload->send_trailing_metadata.send_trailing_metadata)
                ? GRPC_ERROR_NONE
                : GRPC_ERROR_CREATE_FROM_STATIC_STRING(
                      "Attempt to send trailing metadata after "
                      "stream was closed"),
            "send_trailing_metadata_finished");
      } else if (s->id != 0) {
        grpc_chttp2_mark_stream_writable(t, s);
        grpc_chttp2_initiate_write(
            t, GRPC_CHTTP2_INITIATE_WRITE_SEND_TRAILING_METADATA);
      }
    }
  }
  if (op->recv_initial_metadata) {
    GRPC_STATS_INC_HTTP2_OP_RECV_INITIAL_METADATA();
    GPR_ASSERT(s->recv_initial_metadata_ready == nullptr);
    s->recv_initial_metadata_ready =
        op_payload->recv_initial_metadata.recv_initial_metadata_ready;
    s->recv_initial_metadata =
        op_payload->recv_initial_metadata.recv_initial_metadata;
    s->trailing_metadata_available =
        op_payload->recv_initial_metadata.trailing_metadata_available;
    if (op_payload->recv_initial_metadata.peer_string != nullptr) {
      gpr_atm_rel_store(op_payload->recv_initial_metadata.peer_string,
                        (gpr_atm)t->peer_string);
    }
    grpc_chttp2_maybe_complete_recv_initial_metadata(t, s);
  }
  if (op->recv_message) {
    GRPC_STATS_INC_HTTP2_OP_RECV_MESSAGE();
    size_t before = 0;
    GPR_ASSERT(s->recv_message_ready == nullptr);
    GPR_ASSERT(!s->pending_byte_stream);
    s->recv_message_ready = op_payload->recv_message.recv_message_ready;
    s->recv_message = op_payload->recv_message.recv_message;
    if (s->id != 0) {
      if (!s->read_closed) {
        before = s->frame_storage.length +
                 s->unprocessed_incoming_frames_buffer.length;
      }
    }
    grpc_chttp2_maybe_complete_recv_message(t, s);
    if (s->id != 0) {
      if (!s->read_closed && s->frame_storage.length == 0) {
        size_t after = s->frame_storage.length +
                       s->unprocessed_incoming_frames_buffer_cached_length;
        s->flow_control->IncomingByteStreamUpdate(GRPC_HEADER_SIZE_IN_BYTES,
                                                  before - after);
        grpc_chttp2_act_on_flowctl_action(s->flow_control->MakeAction(), t, s);
      }
    }
  }
  if (op->recv_trailing_metadata) {
    GRPC_STATS_INC_HTTP2_OP_RECV_TRAILING_METADATA();
    GPR_ASSERT(s->collecting_stats == nullptr);
    s->collecting_stats = op_payload->recv_trailing_metadata.collect_stats;
    GPR_ASSERT(s->recv_trailing_metadata_finished == nullptr);
    s->recv_trailing_metadata_finished =
        op_payload->recv_trailing_metadata.recv_trailing_metadata_ready;
    s->recv_trailing_metadata =
        op_payload->recv_trailing_metadata.recv_trailing_metadata;
    s->final_metadata_requested = true;
    grpc_chttp2_maybe_complete_recv_trailing_metadata(t, s);
  }
  if (on_complete != nullptr) {
    grpc_chttp2_complete_closure_step(t, s, &on_complete, GRPC_ERROR_NONE,
                                      "op->on_complete");
  }
  GRPC_CHTTP2_STREAM_UNREF(s, "perform_stream_op");
}
static void perform_stream_op(grpc_transport* gt, grpc_stream* gs,
                              grpc_transport_stream_op_batch* op) {
  GPR_TIMER_SCOPE("perform_stream_op", 0);
  grpc_chttp2_transport* t = reinterpret_cast<grpc_chttp2_transport*>(gt);
  grpc_chttp2_stream* s = reinterpret_cast<grpc_chttp2_stream*>(gs);
  if (!t->is_client) {
    if (op->send_initial_metadata) {
      grpc_millis deadline =
          op->payload->send_initial_metadata.send_initial_metadata->deadline;
      GPR_ASSERT(deadline == GRPC_MILLIS_INF_FUTURE);
    }
    if (op->send_trailing_metadata) {
      grpc_millis deadline =
          op->payload->send_trailing_metadata.send_trailing_metadata->deadline;
      GPR_ASSERT(deadline == GRPC_MILLIS_INF_FUTURE);
    }
  }
  if (GRPC_TRACE_FLAG_ENABLED(grpc_http_trace)) {
    gpr_log(GPR_INFO, "perform_stream_op[s=%p]: %s", s,
            grpc_transport_stream_op_batch_string(op).c_str());
  }
  GRPC_CHTTP2_STREAM_REF(s, "perform_stream_op");
  op->handler_private.extra_arg = gs;
  t->combiner->Run(GRPC_CLOSURE_INIT(&op->handler_private.closure,
                                     perform_stream_op_locked, op, nullptr),
                   GRPC_ERROR_NONE);
}
static void cancel_pings(grpc_chttp2_transport* t, grpc_error* error) {
  grpc_chttp2_ping_queue* pq = &t->ping_queue;
  GPR_ASSERT(error != GRPC_ERROR_NONE);
  for (size_t j = 0; j < GRPC_CHTTP2_PCL_COUNT; j++) {
    grpc_closure_list_fail_all(&pq->lists[j], GRPC_ERROR_REF(error));
    grpc_core::ExecCtx::RunList(DEBUG_LOCATION, &pq->lists[j]);
  }
  GRPC_ERROR_UNREF(error);
}
static void send_ping_locked(grpc_chttp2_transport* t,
                             grpc_closure* on_initiate, grpc_closure* on_ack) {
  if (t->closed_with_error != GRPC_ERROR_NONE) {
    grpc_core::ExecCtx::Run(DEBUG_LOCATION, on_initiate,
                            GRPC_ERROR_REF(t->closed_with_error));
    grpc_core::ExecCtx::Run(DEBUG_LOCATION, on_ack,
                            GRPC_ERROR_REF(t->closed_with_error));
    return;
  }
  grpc_chttp2_ping_queue* pq = &t->ping_queue;
  grpc_closure_list_append(&pq->lists[GRPC_CHTTP2_PCL_INITIATE], on_initiate,
                           GRPC_ERROR_NONE);
  grpc_closure_list_append(&pq->lists[GRPC_CHTTP2_PCL_NEXT], on_ack,
                           GRPC_ERROR_NONE);
}
static void send_keepalive_ping_locked(grpc_chttp2_transport* t) {
  if (t->closed_with_error != GRPC_ERROR_NONE) {
    t->combiner->Run(GRPC_CLOSURE_INIT(&t->start_keepalive_ping_locked,
                                       start_keepalive_ping_locked, t, nullptr),
                     GRPC_ERROR_REF(t->closed_with_error));
    t->combiner->Run(
        GRPC_CLOSURE_INIT(&t->finish_keepalive_ping_locked,
                          finish_keepalive_ping_locked, t, nullptr),
        GRPC_ERROR_REF(t->closed_with_error));
    return;
  }
  grpc_chttp2_ping_queue* pq = &t->ping_queue;
  if (!grpc_closure_list_empty(pq->lists[GRPC_CHTTP2_PCL_INFLIGHT])) {
    t->combiner->Run(GRPC_CLOSURE_INIT(&t->start_keepalive_ping_locked,
                                       start_keepalive_ping_locked, t, nullptr),
                     GRPC_ERROR_REF(t->closed_with_error));
    grpc_closure_list_append(
        &pq->lists[GRPC_CHTTP2_PCL_INFLIGHT],
        GRPC_CLOSURE_INIT(&t->finish_keepalive_ping_locked,
                          finish_keepalive_ping, t, grpc_schedule_on_exec_ctx),
        GRPC_ERROR_NONE);
    return;
  }
  grpc_closure_list_append(
      &pq->lists[GRPC_CHTTP2_PCL_INITIATE],
      GRPC_CLOSURE_INIT(&t->start_keepalive_ping_locked, start_keepalive_ping,
                        t, grpc_schedule_on_exec_ctx),
      GRPC_ERROR_NONE);
  grpc_closure_list_append(
      &pq->lists[GRPC_CHTTP2_PCL_NEXT],
      GRPC_CLOSURE_INIT(&t->finish_keepalive_ping_locked, finish_keepalive_ping,
                        t, grpc_schedule_on_exec_ctx),
      GRPC_ERROR_NONE);
}
void grpc_chttp2_retry_initiate_ping(void* tp, grpc_error* error) {
  grpc_chttp2_transport* t = static_cast<grpc_chttp2_transport*>(tp);
  t->combiner->Run(GRPC_CLOSURE_INIT(&t->retry_initiate_ping_locked,
                                     retry_initiate_ping_locked, t, nullptr),
                   GRPC_ERROR_REF(error));
}
static void retry_initiate_ping_locked(void* tp, grpc_error* error) {
  grpc_chttp2_transport* t = static_cast<grpc_chttp2_transport*>(tp);
  t->ping_state.is_delayed_ping_timer_set = false;
  if (error == GRPC_ERROR_NONE) {
    grpc_chttp2_initiate_write(t, GRPC_CHTTP2_INITIATE_WRITE_RETRY_SEND_PING);
  }
  GRPC_CHTTP2_UNREF_TRANSPORT(t, "retry_initiate_ping_locked");
}
void grpc_chttp2_ack_ping(grpc_chttp2_transport* t, uint64_t id) {
  grpc_chttp2_ping_queue* pq = &t->ping_queue;
  if (pq->inflight_id != id) {
    char* from = grpc_endpoint_get_peer(t->ep);
    gpr_log(GPR_DEBUG, "Unknown ping response from %s: %" PRIx64, from, id);
    gpr_free(from);
    return;
  }
  grpc_core::ExecCtx::RunList(DEBUG_LOCATION,
                              &pq->lists[GRPC_CHTTP2_PCL_INFLIGHT]);
  if (!grpc_closure_list_empty(pq->lists[GRPC_CHTTP2_PCL_NEXT])) {
    grpc_chttp2_initiate_write(t, GRPC_CHTTP2_INITIATE_WRITE_CONTINUE_PINGS);
  }
}
static void send_goaway(grpc_chttp2_transport* t, grpc_error* error) {
  gpr_log(GPR_INFO, "%s: Sending goaway err=%s", t->peer_string,
          grpc_error_string(error));
  t->sent_goaway_state = GRPC_CHTTP2_GOAWAY_SEND_SCHEDULED;
  grpc_http2_error_code http_error;
  grpc_slice slice;
  grpc_error_get_status(error, GRPC_MILLIS_INF_FUTURE, nullptr, &slice,
                        &http_error, nullptr);
  grpc_chttp2_goaway_append(t->last_new_stream_id,
                            static_cast<uint32_t>(http_error),
                            grpc_slice_ref_internal(slice), &t->qbuf);
  grpc_chttp2_initiate_write(t, GRPC_CHTTP2_INITIATE_WRITE_GOAWAY_SENT);
  GRPC_ERROR_UNREF(error);
}
void grpc_chttp2_add_ping_strike(grpc_chttp2_transport* t) {
  if (++t->ping_recv_state.ping_strikes > t->ping_policy.max_ping_strikes &&
      t->ping_policy.max_ping_strikes != 0) {
    send_goaway(t,
                grpc_error_set_int(
                    GRPC_ERROR_CREATE_FROM_STATIC_STRING("too_many_pings"),
                    GRPC_ERROR_INT_HTTP2_ERROR, GRPC_HTTP2_ENHANCE_YOUR_CALM));
    close_transport_locked(
        t, grpc_error_set_int(
               GRPC_ERROR_CREATE_FROM_STATIC_STRING("Too many pings"),
               GRPC_ERROR_INT_GRPC_STATUS, GRPC_STATUS_UNAVAILABLE));
  }
}
void grpc_chttp2_reset_ping_clock(grpc_chttp2_transport* t) {
  if (!t->is_client) {
    t->ping_recv_state.last_ping_recv_time = GRPC_MILLIS_INF_PAST;
    t->ping_recv_state.ping_strikes = 0;
  }
  t->ping_state.pings_before_data_required =
      t->ping_policy.max_pings_without_data;
}
static void perform_transport_op_locked(void* stream_op,
                                        grpc_error* ) {
  grpc_transport_op* op = static_cast<grpc_transport_op*>(stream_op);
  grpc_chttp2_transport* t =
      static_cast<grpc_chttp2_transport*>(op->handler_private.extra_arg);
  if (op->goaway_error) {
    send_goaway(t, op->goaway_error);
  }
  if (op->set_accept_stream) {
    t->accept_stream_cb = op->set_accept_stream_fn;
    t->accept_stream_cb_user_data = op->set_accept_stream_user_data;
  }
  if (op->bind_pollset) {
    grpc_endpoint_add_to_pollset(t->ep, op->bind_pollset);
  }
  if (op->bind_pollset_set) {
    grpc_endpoint_add_to_pollset_set(t->ep, op->bind_pollset_set);
  }
  if (op->send_ping.on_initiate != nullptr || op->send_ping.on_ack != nullptr) {
    send_ping_locked(t, op->send_ping.on_initiate, op->send_ping.on_ack);
    grpc_chttp2_initiate_write(t, GRPC_CHTTP2_INITIATE_WRITE_APPLICATION_PING);
  }
  if (op->start_connectivity_watch != nullptr) {
    t->state_tracker.AddWatcher(op->start_connectivity_watch_state,
                                std::move(op->start_connectivity_watch));
  }
  if (op->stop_connectivity_watch != nullptr) {
    t->state_tracker.RemoveWatcher(op->stop_connectivity_watch);
  }
  if (op->disconnect_with_error != GRPC_ERROR_NONE) {
    close_transport_locked(t, op->disconnect_with_error);
  }
  grpc_core::ExecCtx::Run(DEBUG_LOCATION, op->on_consumed, GRPC_ERROR_NONE);
  GRPC_CHTTP2_UNREF_TRANSPORT(t, "transport_op");
}
static void perform_transport_op(grpc_transport* gt, grpc_transport_op* op) {
  grpc_chttp2_transport* t = reinterpret_cast<grpc_chttp2_transport*>(gt);
  if (GRPC_TRACE_FLAG_ENABLED(grpc_http_trace)) {
    gpr_log(GPR_INFO, "perform_transport_op[t=%p]: %s", t,
            grpc_transport_op_string(op).c_str());
  }
  op->handler_private.extra_arg = gt;
  GRPC_CHTTP2_REF_TRANSPORT(t, "transport_op");
  t->combiner->Run(GRPC_CLOSURE_INIT(&op->handler_private.closure,
                                     perform_transport_op_locked, op, nullptr),
                   GRPC_ERROR_NONE);
}
void grpc_chttp2_maybe_complete_recv_initial_metadata(
    grpc_chttp2_transport* , grpc_chttp2_stream* s) {
  if (s->recv_initial_metadata_ready != nullptr &&
      s->published_metadata[0] != GRPC_METADATA_NOT_PUBLISHED) {
    if (s->seen_error) {
      grpc_slice_buffer_reset_and_unref_internal(&s->frame_storage);
      if (!s->pending_byte_stream) {
        grpc_slice_buffer_reset_and_unref_internal(
            &s->unprocessed_incoming_frames_buffer);
      }
    }
    grpc_chttp2_incoming_metadata_buffer_publish(&s->metadata_buffer[0],
                                                 s->recv_initial_metadata);
    null_then_sched_closure(&s->recv_initial_metadata_ready);
  }
}
void grpc_chttp2_maybe_complete_recv_message(grpc_chttp2_transport* ,
                                             grpc_chttp2_stream* s) {
  grpc_error* error = GRPC_ERROR_NONE;
  if (s->recv_message_ready != nullptr) {
    *s->recv_message = nullptr;
    if (s->final_metadata_requested && s->seen_error) {
      grpc_slice_buffer_reset_and_unref_internal(&s->frame_storage);
      if (!s->pending_byte_stream) {
        grpc_slice_buffer_reset_and_unref_internal(
            &s->unprocessed_incoming_frames_buffer);
      }
    }
    if (!s->pending_byte_stream) {
      while (s->unprocessed_incoming_frames_buffer.length > 0 ||
             s->frame_storage.length > 0) {
        if (s->unprocessed_incoming_frames_buffer.length == 0) {
          grpc_slice_buffer_swap(&s->unprocessed_incoming_frames_buffer,
                                 &s->frame_storage);
          s->unprocessed_incoming_frames_decompressed = false;
        }
        if (!s->unprocessed_incoming_frames_decompressed &&
            s->stream_decompression_method !=
                GRPC_STREAM_COMPRESSION_IDENTITY_DECOMPRESS) {
          GPR_ASSERT(s->decompressed_data_buffer.length == 0);
          bool end_of_context;
          if (!s->stream_decompression_ctx) {
            s->stream_decompression_ctx =
                grpc_stream_compression_context_create(
                    s->stream_decompression_method);
          }
          if (!grpc_stream_decompress(
                  s->stream_decompression_ctx,
                  &s->unprocessed_incoming_frames_buffer,
                  &s->decompressed_data_buffer, nullptr,
                  GRPC_HEADER_SIZE_IN_BYTES - s->decompressed_header_bytes,
                  &end_of_context)) {
            grpc_slice_buffer_reset_and_unref_internal(&s->frame_storage);
            grpc_slice_buffer_reset_and_unref_internal(
                &s->unprocessed_incoming_frames_buffer);
            error = GRPC_ERROR_CREATE_FROM_STATIC_STRING(
                "Stream decompression error.");
          } else {
            s->decompressed_header_bytes += s->decompressed_data_buffer.length;
            if (s->decompressed_header_bytes == GRPC_HEADER_SIZE_IN_BYTES) {
              s->decompressed_header_bytes = 0;
            }
            error = grpc_deframe_unprocessed_incoming_frames(
                &s->data_parser, s, &s->decompressed_data_buffer, nullptr,
                s->recv_message);
            if (end_of_context) {
              grpc_stream_compression_context_destroy(
                  s->stream_decompression_ctx);
              s->stream_decompression_ctx = nullptr;
            }
          }
        } else {
          error = grpc_deframe_unprocessed_incoming_frames(
              &s->data_parser, s, &s->unprocessed_incoming_frames_buffer,
              nullptr, s->recv_message);
        }
        if (error != GRPC_ERROR_NONE) {
          s->seen_error = true;
          grpc_slice_buffer_reset_and_unref_internal(&s->frame_storage);
          grpc_slice_buffer_reset_and_unref_internal(
              &s->unprocessed_incoming_frames_buffer);
          break;
        } else if (*s->recv_message != nullptr) {
          break;
        }
      }
    }
    s->unprocessed_incoming_frames_buffer_cached_length =
        s->unprocessed_incoming_frames_buffer.length;
    if (error == GRPC_ERROR_NONE && *s->recv_message != nullptr) {
      null_then_sched_closure(&s->recv_message_ready);
    } else if (s->published_metadata[1] != GRPC_METADATA_NOT_PUBLISHED) {
      *s->recv_message = nullptr;
      null_then_sched_closure(&s->recv_message_ready);
    }
    GRPC_ERROR_UNREF(error);
  }
}
void grpc_chttp2_maybe_complete_recv_trailing_metadata(grpc_chttp2_transport* t,
                                                       grpc_chttp2_stream* s) {
  grpc_chttp2_maybe_complete_recv_message(t, s);
  if (s->recv_trailing_metadata_finished != nullptr && s->read_closed &&
      s->write_closed) {
    if (s->seen_error || !t->is_client) {
      grpc_slice_buffer_reset_and_unref_internal(&s->frame_storage);
      if (!s->pending_byte_stream) {
        grpc_slice_buffer_reset_and_unref_internal(
            &s->unprocessed_incoming_frames_buffer);
      }
    }
    bool pending_data = s->pending_byte_stream ||
                        s->unprocessed_incoming_frames_buffer.length > 0;
    if (s->read_closed && s->frame_storage.length > 0 && !pending_data &&
        !s->seen_error && s->recv_trailing_metadata_finished != nullptr) {
      if (s->stream_decompression_method ==
          GRPC_STREAM_COMPRESSION_IDENTITY_DECOMPRESS) {
        grpc_slice_buffer_move_first(
            &s->frame_storage,
            GPR_MIN(s->frame_storage.length, GRPC_HEADER_SIZE_IN_BYTES),
            &s->unprocessed_incoming_frames_buffer);
        if (s->unprocessed_incoming_frames_buffer.length > 0) {
          s->unprocessed_incoming_frames_decompressed = true;
          pending_data = true;
        }
      } else {
        bool end_of_context;
        if (!s->stream_decompression_ctx) {
          s->stream_decompression_ctx = grpc_stream_compression_context_create(
              s->stream_decompression_method);
        }
        if (!grpc_stream_decompress(
                s->stream_decompression_ctx, &s->frame_storage,
                &s->unprocessed_incoming_frames_buffer, nullptr,
                GRPC_HEADER_SIZE_IN_BYTES, &end_of_context)) {
          grpc_slice_buffer_reset_and_unref_internal(&s->frame_storage);
          grpc_slice_buffer_reset_and_unref_internal(
              &s->unprocessed_incoming_frames_buffer);
          s->seen_error = true;
        } else {
          if (s->unprocessed_incoming_frames_buffer.length > 0) {
            s->unprocessed_incoming_frames_decompressed = true;
            pending_data = true;
          }
          if (end_of_context) {
            grpc_stream_compression_context_destroy(
                s->stream_decompression_ctx);
            s->stream_decompression_ctx = nullptr;
          }
        }
      }
    }
    if (s->read_closed && s->frame_storage.length == 0 && !pending_data &&
        s->recv_trailing_metadata_finished != nullptr) {
      grpc_transport_move_stats(&s->stats, s->collecting_stats);
      s->collecting_stats = nullptr;
      grpc_chttp2_incoming_metadata_buffer_publish(&s->metadata_buffer[1],
                                                   s->recv_trailing_metadata);
      null_then_sched_closure(&s->recv_trailing_metadata_finished);
    }
  }
}
static void remove_stream(grpc_chttp2_transport* t, uint32_t id,
                          grpc_error* error) {
  grpc_chttp2_stream* s = static_cast<grpc_chttp2_stream*>(
      grpc_chttp2_stream_map_delete(&t->stream_map, id));
  GPR_DEBUG_ASSERT(s);
  if (t->incoming_stream == s) {
    t->incoming_stream = nullptr;
    grpc_chttp2_parsing_become_skip_parser(t);
  }
  if (s->pending_byte_stream) {
    if (s->on_next != nullptr) {
      grpc_core::Chttp2IncomingByteStream* bs = s->data_parser.parsing_frame;
      if (error == GRPC_ERROR_NONE) {
        error = GRPC_ERROR_CREATE_FROM_STATIC_STRING("Truncated message");
      }
      bs->PublishError(error);
      bs->Unref();
      s->data_parser.parsing_frame = nullptr;
    } else {
      GRPC_ERROR_UNREF(s->byte_stream_error);
      s->byte_stream_error = GRPC_ERROR_REF(error);
    }
  }
  if (grpc_chttp2_stream_map_size(&t->stream_map) == 0) {
    post_benign_reclaimer(t);
    if (t->sent_goaway_state == GRPC_CHTTP2_GOAWAY_SENT) {
      close_transport_locked(
          t, GRPC_ERROR_CREATE_REFERENCING_FROM_STATIC_STRING(
                 "Last stream closed after sending GOAWAY", &error, 1));
    }
  }
  if (grpc_chttp2_list_remove_writable_stream(t, s)) {
    GRPC_CHTTP2_STREAM_UNREF(s, "chttp2_writing:remove_stream");
  }
  GRPC_ERROR_UNREF(error);
  maybe_start_some_streams(t);
}
void grpc_chttp2_cancel_stream(grpc_chttp2_transport* t, grpc_chttp2_stream* s,
                               grpc_error* due_to_error) {
  if (!t->is_client && !s->sent_trailing_metadata &&
      grpc_error_has_clear_grpc_status(due_to_error)) {
    close_from_api(t, s, due_to_error);
    return;
  }
  if (!s->read_closed || !s->write_closed) {
    if (s->id != 0) {
      grpc_http2_error_code http_error;
      grpc_error_get_status(due_to_error, s->deadline, nullptr, nullptr,
                            &http_error, nullptr);
      grpc_chttp2_add_rst_stream_to_next_write(
          t, s->id, static_cast<uint32_t>(http_error), &s->stats.outgoing);
      grpc_chttp2_initiate_write(t, GRPC_CHTTP2_INITIATE_WRITE_RST_STREAM);
    }
  }
  if (due_to_error != GRPC_ERROR_NONE && !s->seen_error) {
    s->seen_error = true;
  }
  grpc_chttp2_mark_stream_closed(t, s, 1, 1, due_to_error);
}
void grpc_chttp2_fake_status(grpc_chttp2_transport* t, grpc_chttp2_stream* s,
                             grpc_error* error) {
  grpc_status_code status;
  grpc_slice slice;
  grpc_error_get_status(error, s->deadline, &status, &slice, nullptr, nullptr);
  if (status != GRPC_STATUS_OK) {
    s->seen_error = true;
  }
  if (s->published_metadata[1] == GRPC_METADATA_NOT_PUBLISHED ||
      s->recv_trailing_metadata_finished != nullptr) {
    char status_string[GPR_LTOA_MIN_BUFSIZE];
    gpr_ltoa(status, status_string);
    GRPC_LOG_IF_ERROR("add_status",
                      grpc_chttp2_incoming_metadata_buffer_replace_or_add(
                          &s->metadata_buffer[1],
                          grpc_mdelem_from_slices(
                              GRPC_MDSTR_GRPC_STATUS,
                              grpc_core::UnmanagedMemorySlice(status_string))));
    if (!GRPC_SLICE_IS_EMPTY(slice)) {
      GRPC_LOG_IF_ERROR(
          "add_status_message",
          grpc_chttp2_incoming_metadata_buffer_replace_or_add(
              &s->metadata_buffer[1],
              grpc_mdelem_create(GRPC_MDSTR_GRPC_MESSAGE, slice, nullptr)));
    }
    s->published_metadata[1] = GRPC_METADATA_SYNTHESIZED_FROM_FAKE;
    grpc_chttp2_maybe_complete_recv_trailing_metadata(t, s);
  }
  GRPC_ERROR_UNREF(error);
}
static void add_error(grpc_error* error, grpc_error** refs, size_t* nrefs) {
  if (error == GRPC_ERROR_NONE) return;
  for (size_t i = 0; i < *nrefs; i++) {
    if (error == refs[i]) {
      return;
    }
  }
  refs[*nrefs] = error;
  ++*nrefs;
}
static grpc_error* removal_error(grpc_error* extra_error, grpc_chttp2_stream* s,
                                 const char* master_error_msg) {
  grpc_error* refs[3];
  size_t nrefs = 0;
  add_error(s->read_closed_error, refs, &nrefs);
  add_error(s->write_closed_error, refs, &nrefs);
  add_error(extra_error, refs, &nrefs);
  grpc_error* error = GRPC_ERROR_NONE;
  if (nrefs > 0) {
    error = GRPC_ERROR_CREATE_REFERENCING_FROM_STATIC_STRING(master_error_msg,
                                                             refs, nrefs);
  }
  GRPC_ERROR_UNREF(extra_error);
  return error;
}
static void flush_write_list(grpc_chttp2_transport* t, grpc_chttp2_stream* s,
                             grpc_chttp2_write_cb** list, grpc_error* error) {
  while (*list) {
    grpc_chttp2_write_cb* cb = *list;
    *list = cb->next;
    grpc_chttp2_complete_closure_step(t, s, &cb->closure, GRPC_ERROR_REF(error),
                                      "on_write_finished_cb");
    cb->next = t->write_cb_pool;
    t->write_cb_pool = cb;
  }
  GRPC_ERROR_UNREF(error);
}
void grpc_chttp2_fail_pending_writes(grpc_chttp2_transport* t,
                                     grpc_chttp2_stream* s, grpc_error* error) {
  error =
      removal_error(error, s, "Pending writes failed due to stream closure");
  s->send_initial_metadata = nullptr;
  grpc_chttp2_complete_closure_step(t, s, &s->send_initial_metadata_finished,
                                    GRPC_ERROR_REF(error),
                                    "send_initial_metadata_finished");
  s->send_trailing_metadata = nullptr;
  s->sent_trailing_metadata_op = nullptr;
  grpc_chttp2_complete_closure_step(t, s, &s->send_trailing_metadata_finished,
                                    GRPC_ERROR_REF(error),
                                    "send_trailing_metadata_finished");
  s->fetching_send_message.reset();
  grpc_chttp2_complete_closure_step(t, s, &s->fetching_send_message_finished,
                                    GRPC_ERROR_REF(error),
                                    "fetching_send_message_finished");
  flush_write_list(t, s, &s->on_write_finished_cbs, GRPC_ERROR_REF(error));
  flush_write_list(t, s, &s->on_flow_controlled_cbs, error);
}
void grpc_chttp2_mark_stream_closed(grpc_chttp2_transport* t,
                                    grpc_chttp2_stream* s, int close_reads,
                                    int close_writes, grpc_error* error) {
  if (s->read_closed && s->write_closed) {
    grpc_error* overall_error = removal_error(error, s, "Stream removed");
    if (overall_error != GRPC_ERROR_NONE) {
      grpc_chttp2_fake_status(t, s, overall_error);
    }
    grpc_chttp2_maybe_complete_recv_trailing_metadata(t, s);
    return;
  }
  bool closed_read = false;
  bool became_closed = false;
  if (close_reads && !s->read_closed) {
    s->read_closed_error = GRPC_ERROR_REF(error);
    s->read_closed = true;
    closed_read = true;
  }
  if (close_writes && !s->write_closed) {
    s->write_closed_error = GRPC_ERROR_REF(error);
    s->write_closed = true;
    grpc_chttp2_fail_pending_writes(t, s, GRPC_ERROR_REF(error));
  }
  if (s->read_closed && s->write_closed) {
    became_closed = true;
    grpc_error* overall_error =
        removal_error(GRPC_ERROR_REF(error), s, "Stream removed");
    if (s->id != 0) {
      remove_stream(t, s->id, GRPC_ERROR_REF(overall_error));
    } else {
      grpc_chttp2_list_remove_waiting_for_concurrency(t, s);
    }
    if (overall_error != GRPC_ERROR_NONE) {
      grpc_chttp2_fake_status(t, s, overall_error);
    }
  }
  if (closed_read) {
    for (int i = 0; i < 2; i++) {
      if (s->published_metadata[i] == GRPC_METADATA_NOT_PUBLISHED) {
        s->published_metadata[i] = GRPC_METADATA_PUBLISHED_AT_CLOSE;
      }
    }
    grpc_chttp2_maybe_complete_recv_initial_metadata(t, s);
    grpc_chttp2_maybe_complete_recv_message(t, s);
  }
  if (became_closed) {
    grpc_chttp2_maybe_complete_recv_trailing_metadata(t, s);
    GRPC_CHTTP2_STREAM_UNREF(s, "chttp2");
  }
  GRPC_ERROR_UNREF(error);
}
static void close_from_api(grpc_chttp2_transport* t, grpc_chttp2_stream* s,
                           grpc_error* error) {
  grpc_slice hdr;
  grpc_slice status_hdr;
  grpc_slice http_status_hdr;
  grpc_slice content_type_hdr;
  grpc_slice message_pfx;
  uint8_t* p;
  uint32_t len = 0;
  grpc_status_code grpc_status;
  grpc_slice slice;
  grpc_error_get_status(error, s->deadline, &grpc_status, &slice, nullptr,
                        nullptr);
  GPR_ASSERT(grpc_status >= 0 && (int)grpc_status < 100);
  if (!s->sent_initial_metadata) {
    http_status_hdr = GRPC_SLICE_MALLOC(13);
    p = GRPC_SLICE_START_PTR(http_status_hdr);
    *p++ = 0x00;
    *p++ = 7;
    *p++ = ':';
    *p++ = 's';
    *p++ = 't';
    *p++ = 'a';
    *p++ = 't';
    *p++ = 'u';
    *p++ = 's';
    *p++ = 3;
    *p++ = '2';
    *p++ = '0';
    *p++ = '0';
    GPR_ASSERT(p == GRPC_SLICE_END_PTR(http_status_hdr));
    len += static_cast<uint32_t> GRPC_SLICE_LENGTH(http_status_hdr);
    content_type_hdr = GRPC_SLICE_MALLOC(31);
    p = GRPC_SLICE_START_PTR(content_type_hdr);
    *p++ = 0x00;
    *p++ = 12;
    *p++ = 'c';
    *p++ = 'o';
    *p++ = 'n';
    *p++ = 't';
    *p++ = 'e';
    *p++ = 'n';
    *p++ = 't';
    *p++ = '-';
    *p++ = 't';
    *p++ = 'y';
    *p++ = 'p';
    *p++ = 'e';
    *p++ = 16;
    *p++ = 'a';
    *p++ = 'p';
    *p++ = 'p';
    *p++ = 'l';
    *p++ = 'i';
    *p++ = 'c';
    *p++ = 'a';
    *p++ = 't';
    *p++ = 'i';
    *p++ = 'o';
    *p++ = 'n';
    *p++ = '/';
    *p++ = 'g';
    *p++ = 'r';
    *p++ = 'p';
    *p++ = 'c';
    GPR_ASSERT(p == GRPC_SLICE_END_PTR(content_type_hdr));
    len += static_cast<uint32_t> GRPC_SLICE_LENGTH(content_type_hdr);
  }
  status_hdr = GRPC_SLICE_MALLOC(15 + (grpc_status >= 10));
  p = GRPC_SLICE_START_PTR(status_hdr);
  *p++ = 0x00;
  *p++ = 11;
  *p++ = 'g';
  *p++ = 'r';
  *p++ = 'p';
  *p++ = 'c';
  *p++ = '-';
  *p++ = 's';
  *p++ = 't';
  *p++ = 'a';
  *p++ = 't';
  *p++ = 'u';
  *p++ = 's';
  if (grpc_status < 10) {
    *p++ = 1;
    *p++ = static_cast<uint8_t>('0' + grpc_status);
  } else {
    *p++ = 2;
    *p++ = static_cast<uint8_t>('0' + (grpc_status / 10));
    *p++ = static_cast<uint8_t>('0' + (grpc_status % 10));
  }
  GPR_ASSERT(p == GRPC_SLICE_END_PTR(status_hdr));
  len += static_cast<uint32_t> GRPC_SLICE_LENGTH(status_hdr);
  size_t msg_len = GRPC_SLICE_LENGTH(slice);
  GPR_ASSERT(msg_len <= UINT32_MAX);
  uint32_t msg_len_len = GRPC_CHTTP2_VARINT_LENGTH((uint32_t)msg_len, 1);
  message_pfx = GRPC_SLICE_MALLOC(14 + msg_len_len);
  p = GRPC_SLICE_START_PTR(message_pfx);
  *p++ = 0x00;
  *p++ = 12;
  *p++ = 'g';
  *p++ = 'r';
  *p++ = 'p';
  *p++ = 'c';
  *p++ = '-';
  *p++ = 'm';
  *p++ = 'e';
  *p++ = 's';
  *p++ = 's';
  *p++ = 'a';
  *p++ = 'g';
  *p++ = 'e';
  GRPC_CHTTP2_WRITE_VARINT((uint32_t)msg_len, 1, 0, p, (uint32_t)msg_len_len);
  p += msg_len_len;
  GPR_ASSERT(p == GRPC_SLICE_END_PTR(message_pfx));
  len += static_cast<uint32_t> GRPC_SLICE_LENGTH(message_pfx);
  len += static_cast<uint32_t>(msg_len);
  hdr = GRPC_SLICE_MALLOC(9);
  p = GRPC_SLICE_START_PTR(hdr);
  *p++ = static_cast<uint8_t>(len >> 16);
  *p++ = static_cast<uint8_t>(len >> 8);
  *p++ = static_cast<uint8_t>(len);
  *p++ = GRPC_CHTTP2_FRAME_HEADER;
  *p++ = GRPC_CHTTP2_DATA_FLAG_END_STREAM | GRPC_CHTTP2_DATA_FLAG_END_HEADERS;
  *p++ = static_cast<uint8_t>(s->id >> 24);
  *p++ = static_cast<uint8_t>(s->id >> 16);
  *p++ = static_cast<uint8_t>(s->id >> 8);
  *p++ = static_cast<uint8_t>(s->id);
  GPR_ASSERT(p == GRPC_SLICE_END_PTR(hdr));
  grpc_slice_buffer_add(&t->qbuf, hdr);
  if (!s->sent_initial_metadata) {
    grpc_slice_buffer_add(&t->qbuf, http_status_hdr);
    grpc_slice_buffer_add(&t->qbuf, content_type_hdr);
  }
  grpc_slice_buffer_add(&t->qbuf, status_hdr);
  grpc_slice_buffer_add(&t->qbuf, message_pfx);
  grpc_slice_buffer_add(&t->qbuf, grpc_slice_ref_internal(slice));
  grpc_chttp2_reset_ping_clock(t);
  grpc_chttp2_add_rst_stream_to_next_write(t, s->id, GRPC_HTTP2_NO_ERROR,
                                           &s->stats.outgoing);
  grpc_chttp2_mark_stream_closed(t, s, 1, 1, error);
  grpc_chttp2_initiate_write(t, GRPC_CHTTP2_INITIATE_WRITE_CLOSE_FROM_API);
}
struct cancel_stream_cb_args {
  grpc_error* error;
  grpc_chttp2_transport* t;
};
static void cancel_stream_cb(void* user_data, uint32_t , void* stream) {
  cancel_stream_cb_args* args = static_cast<cancel_stream_cb_args*>(user_data);
  grpc_chttp2_stream* s = static_cast<grpc_chttp2_stream*>(stream);
  grpc_chttp2_cancel_stream(args->t, s, GRPC_ERROR_REF(args->error));
}
static void end_all_the_calls(grpc_chttp2_transport* t, grpc_error* error) {
  intptr_t http2_error;
  if (!t->is_client && !grpc_error_has_clear_grpc_status(error) &&
      !grpc_error_get_int(error, GRPC_ERROR_INT_HTTP2_ERROR, &http2_error)) {
    error = grpc_error_set_int(error, GRPC_ERROR_INT_GRPC_STATUS,
                               GRPC_STATUS_UNAVAILABLE);
  }
  cancel_stream_cb_args args = {error, t};
  grpc_chttp2_stream_map_for_each(&t->stream_map, cancel_stream_cb, &args);
  GRPC_ERROR_UNREF(error);
}
template <class F>
static void WithUrgency(grpc_chttp2_transport* t,
                        grpc_core::chttp2::FlowControlAction::Urgency urgency,
                        grpc_chttp2_initiate_write_reason reason, F action) {
  switch (urgency) {
    case grpc_core::chttp2::FlowControlAction::Urgency::NO_ACTION_NEEDED:
      break;
    case grpc_core::chttp2::FlowControlAction::Urgency::UPDATE_IMMEDIATELY:
      grpc_chttp2_initiate_write(t, reason);
    case grpc_core::chttp2::FlowControlAction::Urgency::QUEUE_UPDATE:
      action();
      break;
  }
}
void grpc_chttp2_act_on_flowctl_action(
    const grpc_core::chttp2::FlowControlAction& action,
    grpc_chttp2_transport* t, grpc_chttp2_stream* s) {
  WithUrgency(t, action.send_stream_update(),
              GRPC_CHTTP2_INITIATE_WRITE_STREAM_FLOW_CONTROL,
              [t, s]() { grpc_chttp2_mark_stream_writable(t, s); });
  WithUrgency(t, action.send_transport_update(),
              GRPC_CHTTP2_INITIATE_WRITE_TRANSPORT_FLOW_CONTROL, []() {});
  WithUrgency(t, action.send_initial_window_update(),
              GRPC_CHTTP2_INITIATE_WRITE_SEND_SETTINGS, [t, &action]() {
                queue_setting_update(t,
                                     GRPC_CHTTP2_SETTINGS_INITIAL_WINDOW_SIZE,
                                     action.initial_window_size());
              });
  WithUrgency(t, action.send_max_frame_size_update(),
              GRPC_CHTTP2_INITIATE_WRITE_SEND_SETTINGS, [t, &action]() {
                queue_setting_update(t, GRPC_CHTTP2_SETTINGS_MAX_FRAME_SIZE,
                                     action.max_frame_size());
              });
}
static grpc_error* try_http_parsing(grpc_chttp2_transport* t) {
  grpc_http_parser parser;
  size_t i = 0;
  grpc_error* error = GRPC_ERROR_NONE;
  grpc_http_response response;
  grpc_http_parser_init(&parser, GRPC_HTTP_RESPONSE, &response);
  grpc_error* parse_error = GRPC_ERROR_NONE;
  for (; i < t->read_buffer.count && parse_error == GRPC_ERROR_NONE; i++) {
    parse_error =
        grpc_http_parser_parse(&parser, t->read_buffer.slices[i], nullptr);
  }
  if (parse_error == GRPC_ERROR_NONE &&
      (parse_error = grpc_http_parser_eof(&parser)) == GRPC_ERROR_NONE) {
    error = grpc_error_set_int(
        grpc_error_set_int(GRPC_ERROR_CREATE_FROM_STATIC_STRING(
                               "Trying to connect an http1.x server"),
                           GRPC_ERROR_INT_HTTP_STATUS, response.status),
        GRPC_ERROR_INT_GRPC_STATUS,
        grpc_http2_status_to_grpc_status(response.status));
  }
  GRPC_ERROR_UNREF(parse_error);
  grpc_http_parser_destroy(&parser);
  grpc_http_response_destroy(&response);
  return error;
}
static void read_action(void* tp, grpc_error* error) {
  grpc_chttp2_transport* t = static_cast<grpc_chttp2_transport*>(tp);
  t->combiner->Run(
      GRPC_CLOSURE_INIT(&t->read_action_locked, read_action_locked, t, nullptr),
      GRPC_ERROR_REF(error));
}
static void read_action_locked(void* tp, grpc_error* error) {
  GPR_TIMER_SCOPE("reading_action_locked", 0);
  grpc_chttp2_transport* t = static_cast<grpc_chttp2_transport*>(tp);
  GRPC_ERROR_REF(error);
  grpc_error* err = error;
  if (err != GRPC_ERROR_NONE) {
    err = grpc_error_set_int(GRPC_ERROR_CREATE_REFERENCING_FROM_STATIC_STRING(
                                 "Endpoint read failed", &err, 1),
                             GRPC_ERROR_INT_OCCURRED_DURING_WRITE,
                             t->write_state);
  }
  GPR_SWAP(grpc_error*, err, error);
  GRPC_ERROR_UNREF(err);
  if (t->closed_with_error == GRPC_ERROR_NONE) {
    GPR_TIMER_SCOPE("reading_action.parse", 0);
    size_t i = 0;
    grpc_error* errors[3] = {GRPC_ERROR_REF(error), GRPC_ERROR_NONE,
                             GRPC_ERROR_NONE};
    for (; i < t->read_buffer.count && errors[1] == GRPC_ERROR_NONE; i++) {
      grpc_core::BdpEstimator* bdp_est = t->flow_control->bdp_estimator();
      if (bdp_est) {
        bdp_est->AddIncomingBytes(
            static_cast<int64_t> GRPC_SLICE_LENGTH(t->read_buffer.slices[i]));
      }
      errors[1] = grpc_chttp2_perform_read(t, t->read_buffer.slices[i]);
    }
    if (errors[1] != GRPC_ERROR_NONE) {
      errors[2] = try_http_parsing(t);
      GRPC_ERROR_UNREF(error);
      error = GRPC_ERROR_CREATE_REFERENCING_FROM_STATIC_STRING(
          "Failed parsing HTTP/2", errors, GPR_ARRAY_SIZE(errors));
    }
    for (i = 0; i < GPR_ARRAY_SIZE(errors); i++) {
      GRPC_ERROR_UNREF(errors[i]);
    }
    GPR_TIMER_SCOPE("post_parse_locked", 0);
    if (t->initial_window_update != 0) {
      if (t->initial_window_update > 0) {
        grpc_chttp2_stream* s;
        while (grpc_chttp2_list_pop_stalled_by_stream(t, &s)) {
          grpc_chttp2_mark_stream_writable(t, s);
          grpc_chttp2_initiate_write(
              t, GRPC_CHTTP2_INITIATE_WRITE_FLOW_CONTROL_UNSTALLED_BY_SETTING);
        }
      }
      t->initial_window_update = 0;
    }
  }
  GPR_TIMER_SCOPE("post_reading_action_locked", 0);
  bool keep_reading = false;
  if (error == GRPC_ERROR_NONE && t->closed_with_error != GRPC_ERROR_NONE) {
    error = GRPC_ERROR_CREATE_REFERENCING_FROM_STATIC_STRING(
        "Transport closed", &t->closed_with_error, 1);
  }
  if (error != GRPC_ERROR_NONE) {
    if (t->goaway_error != GRPC_ERROR_NONE) {
      error = grpc_error_add_child(error, GRPC_ERROR_REF(t->goaway_error));
    }
    close_transport_locked(t, GRPC_ERROR_REF(error));
    t->endpoint_reading = 0;
  } else if (t->closed_with_error == GRPC_ERROR_NONE) {
    keep_reading = true;
    if (t->keepalive_state == GRPC_CHTTP2_KEEPALIVE_STATE_WAITING) {
      grpc_timer_cancel(&t->keepalive_ping_timer);
    }
  }
  grpc_slice_buffer_reset_and_unref_internal(&t->read_buffer);
  if (keep_reading) {
    if (t->num_pending_induced_frames >= DEFAULT_MAX_PENDING_INDUCED_FRAMES) {
      t->reading_paused_on_pending_induced_frames = true;
      GRPC_CHTTP2_IF_TRACING(
          gpr_log(GPR_INFO,
                  "transport %p : Pausing reading due to too "
                  "many unwritten SETTINGS ACK and RST_STREAM frames",
                  t));
    } else {
      continue_read_action_locked(t);
    }
  } else {
    GRPC_CHTTP2_UNREF_TRANSPORT(t, "reading_action");
  }
  GRPC_ERROR_UNREF(error);
}
static void continue_read_action_locked(grpc_chttp2_transport* t) {
  const bool urgent = t->goaway_error != GRPC_ERROR_NONE;
  GRPC_CLOSURE_INIT(&t->read_action_locked, read_action, t,
                    grpc_schedule_on_exec_ctx);
  grpc_endpoint_read(t->ep, &t->read_buffer, &t->read_action_locked, urgent);
  grpc_chttp2_act_on_flowctl_action(t->flow_control->MakeAction(), t, nullptr);
}
static void schedule_bdp_ping_locked(grpc_chttp2_transport* t) {
  t->flow_control->bdp_estimator()->SchedulePing();
  send_ping_locked(
      t,
      GRPC_CLOSURE_INIT(&t->start_bdp_ping_locked, start_bdp_ping, t,
                        grpc_schedule_on_exec_ctx),
      GRPC_CLOSURE_INIT(&t->finish_bdp_ping_locked, finish_bdp_ping, t,
                        grpc_schedule_on_exec_ctx));
}
static void start_bdp_ping(void* tp, grpc_error* error) {
  grpc_chttp2_transport* t = static_cast<grpc_chttp2_transport*>(tp);
  t->combiner->Run(GRPC_CLOSURE_INIT(&t->start_bdp_ping_locked,
                                     start_bdp_ping_locked, t, nullptr),
                   GRPC_ERROR_REF(error));
}
static void start_bdp_ping_locked(void* tp, grpc_error* error) {
  grpc_chttp2_transport* t = static_cast<grpc_chttp2_transport*>(tp);
  if (GRPC_TRACE_FLAG_ENABLED(grpc_http_trace)) {
    gpr_log(GPR_INFO, "%s: Start BDP ping err=%s", t->peer_string,
            grpc_error_string(error));
  }
  if (error != GRPC_ERROR_NONE || t->closed_with_error != GRPC_ERROR_NONE) {
    return;
  }
  if (t->keepalive_state == GRPC_CHTTP2_KEEPALIVE_STATE_WAITING) {
    grpc_timer_cancel(&t->keepalive_ping_timer);
  }
  t->flow_control->bdp_estimator()->StartPing();
  t->bdp_ping_started = true;
}
static void finish_bdp_ping(void* tp, grpc_error* error) {
  grpc_chttp2_transport* t = static_cast<grpc_chttp2_transport*>(tp);
  t->combiner->Run(GRPC_CLOSURE_INIT(&t->finish_bdp_ping_locked,
                                     finish_bdp_ping_locked, t, nullptr),
                   GRPC_ERROR_REF(error));
}
static void finish_bdp_ping_locked(void* tp, grpc_error* error) {
  grpc_chttp2_transport* t = static_cast<grpc_chttp2_transport*>(tp);
  if (GRPC_TRACE_FLAG_ENABLED(grpc_http_trace)) {
    gpr_log(GPR_INFO, "%s: Complete BDP ping err=%s", t->peer_string,
            grpc_error_string(error));
  }
  if (error != GRPC_ERROR_NONE || t->closed_with_error != GRPC_ERROR_NONE) {
    GRPC_CHTTP2_UNREF_TRANSPORT(t, "bdp_ping");
    return;
  }
  if (!t->bdp_ping_started) {
    t->combiner->Run(GRPC_CLOSURE_INIT(&t->finish_bdp_ping_locked,
                                       finish_bdp_ping_locked, t, nullptr),
                     GRPC_ERROR_REF(error));
    return;
  }
  t->bdp_ping_started = false;
  grpc_millis next_ping = t->flow_control->bdp_estimator()->CompletePing();
  grpc_chttp2_act_on_flowctl_action(t->flow_control->PeriodicUpdate(), t,
                                    nullptr);
  GPR_ASSERT(!t->have_next_bdp_ping_timer);
  t->have_next_bdp_ping_timer = true;
  GRPC_CLOSURE_INIT(&t->next_bdp_ping_timer_expired_locked,
                    next_bdp_ping_timer_expired, t, grpc_schedule_on_exec_ctx);
  grpc_timer_init(&t->next_bdp_ping_timer, next_ping,
                  &t->next_bdp_ping_timer_expired_locked);
}
static void next_bdp_ping_timer_expired(void* tp, grpc_error* error) {
  grpc_chttp2_transport* t = static_cast<grpc_chttp2_transport*>(tp);
  t->combiner->Run(
      GRPC_CLOSURE_INIT(&t->next_bdp_ping_timer_expired_locked,
                        next_bdp_ping_timer_expired_locked, t, nullptr),
      GRPC_ERROR_REF(error));
}
static void next_bdp_ping_timer_expired_locked(void* tp, grpc_error* error) {
  grpc_chttp2_transport* t = static_cast<grpc_chttp2_transport*>(tp);
  GPR_ASSERT(t->have_next_bdp_ping_timer);
  t->have_next_bdp_ping_timer = false;
  if (error != GRPC_ERROR_NONE) {
    GRPC_CHTTP2_UNREF_TRANSPORT(t, "bdp_ping");
    return;
  }
  schedule_bdp_ping_locked(t);
}
void grpc_chttp2_config_default_keepalive_args(grpc_channel_args* args,
                                               bool is_client) {
  size_t i;
  if (args) {
    for (i = 0; i < args->num_args; i++) {
      if (0 == strcmp(args->args[i].key, GRPC_ARG_KEEPALIVE_TIME_MS)) {
        const int value = grpc_channel_arg_get_integer(
            &args->args[i], {is_client ? g_default_client_keepalive_time_ms
                                       : g_default_server_keepalive_time_ms,
                             1, INT_MAX});
        if (is_client) {
          g_default_client_keepalive_time_ms = value;
        } else {
          g_default_server_keepalive_time_ms = value;
        }
      } else if (0 ==
                 strcmp(args->args[i].key, GRPC_ARG_KEEPALIVE_TIMEOUT_MS)) {
        const int value = grpc_channel_arg_get_integer(
            &args->args[i], {is_client ? g_default_client_keepalive_timeout_ms
                                       : g_default_server_keepalive_timeout_ms,
                             0, INT_MAX});
        if (is_client) {
          g_default_client_keepalive_timeout_ms = value;
        } else {
          g_default_server_keepalive_timeout_ms = value;
        }
      } else if (0 == strcmp(args->args[i].key,
                             GRPC_ARG_KEEPALIVE_PERMIT_WITHOUT_CALLS)) {
        const bool value = static_cast<uint32_t>(grpc_channel_arg_get_integer(
            &args->args[i],
            {is_client ? g_default_client_keepalive_permit_without_calls
                       : g_default_server_keepalive_timeout_ms,
             0, 1}));
        if (is_client) {
          g_default_client_keepalive_permit_without_calls = value;
        } else {
          g_default_server_keepalive_permit_without_calls = value;
        }
      } else if (0 ==
                 strcmp(args->args[i].key, GRPC_ARG_HTTP2_MAX_PING_STRIKES)) {
        g_default_max_ping_strikes = grpc_channel_arg_get_integer(
            &args->args[i], {g_default_max_ping_strikes, 0, INT_MAX});
      } else if (0 == strcmp(args->args[i].key,
                             GRPC_ARG_HTTP2_MAX_PINGS_WITHOUT_DATA)) {
        g_default_max_pings_without_data = grpc_channel_arg_get_integer(
            &args->args[i], {g_default_max_pings_without_data, 0, INT_MAX});
      } else if (0 ==
                 strcmp(
                     args->args[i].key,
                     GRPC_ARG_HTTP2_MIN_SENT_PING_INTERVAL_WITHOUT_DATA_MS)) {
        g_default_min_sent_ping_interval_without_data_ms =
            grpc_channel_arg_get_integer(
                &args->args[i],
                {g_default_min_sent_ping_interval_without_data_ms, 0, INT_MAX});
      } else if (0 ==
                 strcmp(
                     args->args[i].key,
                     GRPC_ARG_HTTP2_MIN_RECV_PING_INTERVAL_WITHOUT_DATA_MS)) {
        g_default_min_recv_ping_interval_without_data_ms =
            grpc_channel_arg_get_integer(
                &args->args[i],
                {g_default_min_recv_ping_interval_without_data_ms, 0, INT_MAX});
      }
    }
  }
}
static void init_keepalive_ping(void* arg, grpc_error* error) {
  grpc_chttp2_transport* t = static_cast<grpc_chttp2_transport*>(arg);
  t->combiner->Run(GRPC_CLOSURE_INIT(&t->init_keepalive_ping_locked,
                                     init_keepalive_ping_locked, t, nullptr),
                   GRPC_ERROR_REF(error));
}
static void init_keepalive_ping_locked(void* arg, grpc_error* error) {
  grpc_chttp2_transport* t = static_cast<grpc_chttp2_transport*>(arg);
  GPR_ASSERT(t->keepalive_state == GRPC_CHTTP2_KEEPALIVE_STATE_WAITING);
  if (t->destroying || t->closed_with_error != GRPC_ERROR_NONE) {
    t->keepalive_state = GRPC_CHTTP2_KEEPALIVE_STATE_DYING;
  } else if (error == GRPC_ERROR_NONE) {
    if (t->keepalive_permit_without_calls ||
        grpc_chttp2_stream_map_size(&t->stream_map) > 0) {
      t->keepalive_state = GRPC_CHTTP2_KEEPALIVE_STATE_PINGING;
      GRPC_CHTTP2_REF_TRANSPORT(t, "keepalive ping end");
      grpc_timer_init_unset(&t->keepalive_watchdog_timer);
      send_keepalive_ping_locked(t);
      grpc_chttp2_initiate_write(t, GRPC_CHTTP2_INITIATE_WRITE_KEEPALIVE_PING);
    } else {
      GRPC_CHTTP2_REF_TRANSPORT(t, "init keepalive ping");
      GRPC_CLOSURE_INIT(&t->init_keepalive_ping_locked, init_keepalive_ping, t,
                        grpc_schedule_on_exec_ctx);
      grpc_timer_init(&t->keepalive_ping_timer,
                      grpc_core::ExecCtx::Get()->Now() + t->keepalive_time,
                      &t->init_keepalive_ping_locked);
    }
  } else if (error == GRPC_ERROR_CANCELLED) {
    GRPC_CHTTP2_REF_TRANSPORT(t, "init keepalive ping");
    GRPC_CLOSURE_INIT(&t->init_keepalive_ping_locked, init_keepalive_ping, t,
                      grpc_schedule_on_exec_ctx);
    grpc_timer_init(&t->keepalive_ping_timer,
                    grpc_core::ExecCtx::Get()->Now() + t->keepalive_time,
                    &t->init_keepalive_ping_locked);
  }
  GRPC_CHTTP2_UNREF_TRANSPORT(t, "init keepalive ping");
}
static void start_keepalive_ping(void* arg, grpc_error* error) {
  grpc_chttp2_transport* t = static_cast<grpc_chttp2_transport*>(arg);
  t->combiner->Run(GRPC_CLOSURE_INIT(&t->start_keepalive_ping_locked,
                                     start_keepalive_ping_locked, t, nullptr),
                   GRPC_ERROR_REF(error));
}
static void start_keepalive_ping_locked(void* arg, grpc_error* error) {
  grpc_chttp2_transport* t = static_cast<grpc_chttp2_transport*>(arg);
  if (error != GRPC_ERROR_NONE) {
    return;
  }
  if (t->channelz_socket != nullptr) {
    t->channelz_socket->RecordKeepaliveSent();
  }
  if (GRPC_TRACE_FLAG_ENABLED(grpc_http_trace) ||
      GRPC_TRACE_FLAG_ENABLED(grpc_keepalive_trace)) {
    gpr_log(GPR_INFO, "%s: Start keepalive ping", t->peer_string);
  }
  GRPC_CHTTP2_REF_TRANSPORT(t, "keepalive watchdog");
  GRPC_CLOSURE_INIT(&t->keepalive_watchdog_fired_locked,
                    keepalive_watchdog_fired, t, grpc_schedule_on_exec_ctx);
  grpc_timer_init(&t->keepalive_watchdog_timer,
                  grpc_core::ExecCtx::Get()->Now() + t->keepalive_timeout,
                  &t->keepalive_watchdog_fired_locked);
  t->keepalive_ping_started = true;
}
static void finish_keepalive_ping(void* arg, grpc_error* error) {
  grpc_chttp2_transport* t = static_cast<grpc_chttp2_transport*>(arg);
  t->combiner->Run(GRPC_CLOSURE_INIT(&t->finish_keepalive_ping_locked,
                                     finish_keepalive_ping_locked, t, nullptr),
                   GRPC_ERROR_REF(error));
}
static void finish_keepalive_ping_locked(void* arg, grpc_error* error) {
  grpc_chttp2_transport* t = static_cast<grpc_chttp2_transport*>(arg);
  if (t->keepalive_state == GRPC_CHTTP2_KEEPALIVE_STATE_PINGING) {
    if (error == GRPC_ERROR_NONE) {
      if (GRPC_TRACE_FLAG_ENABLED(grpc_http_trace) ||
          GRPC_TRACE_FLAG_ENABLED(grpc_keepalive_trace)) {
        gpr_log(GPR_INFO, "%s: Finish keepalive ping", t->peer_string);
      }
      if (!t->keepalive_ping_started) {
        t->combiner->Run(
            GRPC_CLOSURE_INIT(&t->finish_keepalive_ping_locked,
                              finish_keepalive_ping_locked, t, nullptr),
            GRPC_ERROR_REF(error));
        return;
      }
      t->keepalive_ping_started = false;
      t->keepalive_state = GRPC_CHTTP2_KEEPALIVE_STATE_WAITING;
      grpc_timer_cancel(&t->keepalive_watchdog_timer);
      GRPC_CHTTP2_REF_TRANSPORT(t, "init keepalive ping");
      GRPC_CLOSURE_INIT(&t->init_keepalive_ping_locked, init_keepalive_ping, t,
                        grpc_schedule_on_exec_ctx);
      grpc_timer_init(&t->keepalive_ping_timer,
                      grpc_core::ExecCtx::Get()->Now() + t->keepalive_time,
                      &t->init_keepalive_ping_locked);
    }
  }
  GRPC_CHTTP2_UNREF_TRANSPORT(t, "keepalive ping end");
}
static void keepalive_watchdog_fired(void* arg, grpc_error* error) {
  grpc_chttp2_transport* t = static_cast<grpc_chttp2_transport*>(arg);
  t->combiner->Run(
      GRPC_CLOSURE_INIT(&t->keepalive_watchdog_fired_locked,
                        keepalive_watchdog_fired_locked, t, nullptr),
      GRPC_ERROR_REF(error));
}
static void keepalive_watchdog_fired_locked(void* arg, grpc_error* error) {
  grpc_chttp2_transport* t = static_cast<grpc_chttp2_transport*>(arg);
  if (t->keepalive_state == GRPC_CHTTP2_KEEPALIVE_STATE_PINGING) {
    if (error == GRPC_ERROR_NONE) {
      gpr_log(GPR_INFO, "%s: Keepalive watchdog fired. Closing transport.",
              t->peer_string);
      t->keepalive_state = GRPC_CHTTP2_KEEPALIVE_STATE_DYING;
      close_transport_locked(
          t, grpc_error_set_int(GRPC_ERROR_CREATE_FROM_STATIC_STRING(
                                    "keepalive watchdog timeout"),
                                GRPC_ERROR_INT_GRPC_STATUS,
                                GRPC_STATUS_UNAVAILABLE));
    }
  } else {
    if (GPR_UNLIKELY(error != GRPC_ERROR_CANCELLED)) {
      gpr_log(GPR_ERROR, "keepalive_ping_end state error: %d (expect: %d)",
              t->keepalive_state, GRPC_CHTTP2_KEEPALIVE_STATE_PINGING);
    }
  }
  GRPC_CHTTP2_UNREF_TRANSPORT(t, "keepalive watchdog");
}
static void connectivity_state_set(grpc_chttp2_transport* t,
                                   grpc_connectivity_state state,
                                   const char* reason) {
  GRPC_CHTTP2_IF_TRACING(
      gpr_log(GPR_INFO, "transport %p set connectivity_state=%d", t, state));
  t->state_tracker.SetState(state, reason);
}
static void set_pollset(grpc_transport* gt, grpc_stream* ,
                        grpc_pollset* pollset) {
  grpc_chttp2_transport* t = reinterpret_cast<grpc_chttp2_transport*>(gt);
  grpc_endpoint_add_to_pollset(t->ep, pollset);
}
static void set_pollset_set(grpc_transport* gt, grpc_stream* ,
                            grpc_pollset_set* pollset_set) {
  grpc_chttp2_transport* t = reinterpret_cast<grpc_chttp2_transport*>(gt);
  grpc_endpoint_add_to_pollset_set(t->ep, pollset_set);
}
static void reset_byte_stream(void* arg, grpc_error* error) {
  grpc_chttp2_stream* s = static_cast<grpc_chttp2_stream*>(arg);
  s->pending_byte_stream = false;
  if (error == GRPC_ERROR_NONE) {
    grpc_chttp2_maybe_complete_recv_message(s->t, s);
    grpc_chttp2_maybe_complete_recv_trailing_metadata(s->t, s);
  } else {
    GPR_ASSERT(error != GRPC_ERROR_NONE);
    grpc_core::ExecCtx::Run(DEBUG_LOCATION, s->on_next, GRPC_ERROR_REF(error));
    s->on_next = nullptr;
    GRPC_ERROR_UNREF(s->byte_stream_error);
    s->byte_stream_error = GRPC_ERROR_NONE;
    grpc_chttp2_cancel_stream(s->t, s, GRPC_ERROR_REF(error));
    s->byte_stream_error = GRPC_ERROR_REF(error);
  }
}
namespace grpc_core {
Chttp2IncomingByteStream::Chttp2IncomingByteStream(
    grpc_chttp2_transport* transport, grpc_chttp2_stream* stream,
    uint32_t frame_size, uint32_t flags)
    : ByteStream(frame_size, flags),
      transport_(transport),
      stream_(stream),
      refs_(2),
      remaining_bytes_(frame_size) {
  GRPC_ERROR_UNREF(stream->byte_stream_error);
  stream->byte_stream_error = GRPC_ERROR_NONE;
}
void Chttp2IncomingByteStream::OrphanLocked(void* arg,
                                            grpc_error* ) {
  Chttp2IncomingByteStream* bs = static_cast<Chttp2IncomingByteStream*>(arg);
  grpc_chttp2_stream* s = bs->stream_;
  grpc_chttp2_transport* t = s->t;
  bs->Unref();
  s->pending_byte_stream = false;
  grpc_chttp2_maybe_complete_recv_message(t, s);
  grpc_chttp2_maybe_complete_recv_trailing_metadata(t, s);
}
void Chttp2IncomingByteStream::Orphan() {
  GPR_TIMER_SCOPE("incoming_byte_stream_destroy", 0);
  transport_->combiner->Run(
      GRPC_CLOSURE_INIT(&destroy_action_,
                        &Chttp2IncomingByteStream::OrphanLocked, this, nullptr),
      GRPC_ERROR_NONE);
}
void Chttp2IncomingByteStream::NextLocked(void* arg,
                                          grpc_error* ) {
  Chttp2IncomingByteStream* bs = static_cast<Chttp2IncomingByteStream*>(arg);
  grpc_chttp2_transport* t = bs->transport_;
  grpc_chttp2_stream* s = bs->stream_;
  size_t cur_length = s->frame_storage.length;
  if (!s->read_closed) {
    s->flow_control->IncomingByteStreamUpdate(bs->next_action_.max_size_hint,
                                              cur_length);
    grpc_chttp2_act_on_flowctl_action(s->flow_control->MakeAction(), t, s);
  }
  GPR_ASSERT(s->unprocessed_incoming_frames_buffer.length == 0);
  if (s->frame_storage.length > 0) {
    grpc_slice_buffer_swap(&s->frame_storage,
                           &s->unprocessed_incoming_frames_buffer);
    s->unprocessed_incoming_frames_decompressed = false;
    grpc_core::ExecCtx::Run(DEBUG_LOCATION, bs->next_action_.on_complete,
                            GRPC_ERROR_NONE);
  } else if (s->byte_stream_error != GRPC_ERROR_NONE) {
    grpc_core::ExecCtx::Run(DEBUG_LOCATION, bs->next_action_.on_complete,
                            GRPC_ERROR_REF(s->byte_stream_error));
    if (s->data_parser.parsing_frame != nullptr) {
      s->data_parser.parsing_frame->Unref();
      s->data_parser.parsing_frame = nullptr;
    }
  } else if (s->read_closed) {
    if (bs->remaining_bytes_ != 0) {
      s->byte_stream_error = GRPC_ERROR_CREATE_REFERENCING_FROM_STATIC_STRING(
          "Truncated message", &s->read_closed_error, 1);
      grpc_core::ExecCtx::Run(DEBUG_LOCATION, bs->next_action_.on_complete,
                              GRPC_ERROR_REF(s->byte_stream_error));
      if (s->data_parser.parsing_frame != nullptr) {
        s->data_parser.parsing_frame->Unref();
        s->data_parser.parsing_frame = nullptr;
      }
    } else {
      GPR_ASSERT(false);
    }
  } else {
    s->on_next = bs->next_action_.on_complete;
  }
  bs->Unref();
}
bool Chttp2IncomingByteStream::Next(size_t max_size_hint,
                                    grpc_closure* on_complete) {
  GPR_TIMER_SCOPE("incoming_byte_stream_next", 0);
  if (stream_->unprocessed_incoming_frames_buffer.length > 0) {
    return true;
  } else {
    Ref();
    next_action_.max_size_hint = max_size_hint;
    next_action_.on_complete = on_complete;
    transport_->combiner->Run(
        GRPC_CLOSURE_INIT(&next_action_.closure,
                          &Chttp2IncomingByteStream::NextLocked, this, nullptr),
        GRPC_ERROR_NONE);
    return false;
  }
}
void Chttp2IncomingByteStream::MaybeCreateStreamDecompressionCtx() {
  GPR_DEBUG_ASSERT(stream_->stream_decompression_method !=
                   GRPC_STREAM_COMPRESSION_IDENTITY_DECOMPRESS);
  if (!stream_->stream_decompression_ctx) {
    stream_->stream_decompression_ctx = grpc_stream_compression_context_create(
        stream_->stream_decompression_method);
  }
}
grpc_error* Chttp2IncomingByteStream::Pull(grpc_slice* slice) {
  GPR_TIMER_SCOPE("incoming_byte_stream_pull", 0);
  grpc_error* error;
  if (stream_->unprocessed_incoming_frames_buffer.length > 0) {
    if (!stream_->unprocessed_incoming_frames_decompressed &&
        stream_->stream_decompression_method !=
            GRPC_STREAM_COMPRESSION_IDENTITY_DECOMPRESS) {
      bool end_of_context;
      MaybeCreateStreamDecompressionCtx();
      if (!grpc_stream_decompress(stream_->stream_decompression_ctx,
                                  &stream_->unprocessed_incoming_frames_buffer,
                                  &stream_->decompressed_data_buffer, nullptr,
                                  MAX_SIZE_T, &end_of_context)) {
        error =
            GRPC_ERROR_CREATE_FROM_STATIC_STRING("Stream decompression error.");
        return error;
      }
      GPR_ASSERT(stream_->unprocessed_incoming_frames_buffer.length == 0);
      grpc_slice_buffer_swap(&stream_->unprocessed_incoming_frames_buffer,
                             &stream_->decompressed_data_buffer);
      stream_->unprocessed_incoming_frames_decompressed = true;
      if (end_of_context) {
        grpc_stream_compression_context_destroy(
            stream_->stream_decompression_ctx);
        stream_->stream_decompression_ctx = nullptr;
      }
      if (stream_->unprocessed_incoming_frames_buffer.length == 0) {
        *slice = grpc_empty_slice();
      }
    }
    error = grpc_deframe_unprocessed_incoming_frames(
        &stream_->data_parser, stream_,
        &stream_->unprocessed_incoming_frames_buffer, slice, nullptr);
    if (error != GRPC_ERROR_NONE) {
      return error;
    }
  } else {
    error = GRPC_ERROR_CREATE_FROM_STATIC_STRING("Truncated message");
    stream_->t->combiner->Run(&stream_->reset_byte_stream,
                              GRPC_ERROR_REF(error));
    return error;
  }
  return GRPC_ERROR_NONE;
}
void Chttp2IncomingByteStream::PublishError(grpc_error* error) {
  GPR_ASSERT(error != GRPC_ERROR_NONE);
  grpc_core::ExecCtx::Run(DEBUG_LOCATION, stream_->on_next,
                          GRPC_ERROR_REF(error));
  stream_->on_next = nullptr;
  GRPC_ERROR_UNREF(stream_->byte_stream_error);
  stream_->byte_stream_error = GRPC_ERROR_REF(error);
  grpc_chttp2_cancel_stream(transport_, stream_, GRPC_ERROR_REF(error));
}
grpc_error* Chttp2IncomingByteStream::Push(const grpc_slice& slice,
                                           grpc_slice* slice_out) {
  if (remaining_bytes_ < GRPC_SLICE_LENGTH(slice)) {
    grpc_error* error =
        GRPC_ERROR_CREATE_FROM_STATIC_STRING("Too many bytes in stream");
    transport_->combiner->Run(&stream_->reset_byte_stream,
                              GRPC_ERROR_REF(error));
    grpc_slice_unref_internal(slice);
    return error;
  } else {
    remaining_bytes_ -= static_cast<uint32_t> GRPC_SLICE_LENGTH(slice);
    if (slice_out != nullptr) {
      *slice_out = slice;
    }
    return GRPC_ERROR_NONE;
  }
}
grpc_error* Chttp2IncomingByteStream::Finished(grpc_error* error,
                                               bool reset_on_error) {
  if (error == GRPC_ERROR_NONE) {
    if (remaining_bytes_ != 0) {
      error = GRPC_ERROR_CREATE_FROM_STATIC_STRING("Truncated message");
    }
  }
  if (error != GRPC_ERROR_NONE && reset_on_error) {
    transport_->combiner->Run(&stream_->reset_byte_stream,
                              GRPC_ERROR_REF(error));
  }
  Unref();
  return error;
}
void Chttp2IncomingByteStream::Shutdown(grpc_error* error) {
  GRPC_ERROR_UNREF(Finished(error, true ));
}
}
static void post_benign_reclaimer(grpc_chttp2_transport* t) {
  if (!t->benign_reclaimer_registered) {
    t->benign_reclaimer_registered = true;
    GRPC_CHTTP2_REF_TRANSPORT(t, "benign_reclaimer");
    GRPC_CLOSURE_INIT(&t->benign_reclaimer_locked, benign_reclaimer, t,
                      grpc_schedule_on_exec_ctx);
    grpc_resource_user_post_reclaimer(grpc_endpoint_get_resource_user(t->ep),
                                      false, &t->benign_reclaimer_locked);
  }
}
static void post_destructive_reclaimer(grpc_chttp2_transport* t) {
  if (!t->destructive_reclaimer_registered) {
    t->destructive_reclaimer_registered = true;
    GRPC_CHTTP2_REF_TRANSPORT(t, "destructive_reclaimer");
    GRPC_CLOSURE_INIT(&t->destructive_reclaimer_locked, destructive_reclaimer,
                      t, grpc_schedule_on_exec_ctx);
    grpc_resource_user_post_reclaimer(grpc_endpoint_get_resource_user(t->ep),
                                      true, &t->destructive_reclaimer_locked);
  }
}
static void benign_reclaimer(void* arg, grpc_error* error) {
  grpc_chttp2_transport* t = static_cast<grpc_chttp2_transport*>(arg);
  t->combiner->Run(GRPC_CLOSURE_INIT(&t->benign_reclaimer_locked,
                                     benign_reclaimer_locked, t, nullptr),
                   GRPC_ERROR_REF(error));
}
static void benign_reclaimer_locked(void* arg, grpc_error* error) {
  grpc_chttp2_transport* t = static_cast<grpc_chttp2_transport*>(arg);
  if (error == GRPC_ERROR_NONE &&
      grpc_chttp2_stream_map_size(&t->stream_map) == 0) {
    if (GRPC_TRACE_FLAG_ENABLED(grpc_resource_quota_trace)) {
      gpr_log(GPR_INFO, "HTTP2: %s - send goaway to free memory",
              t->peer_string);
    }
    send_goaway(t,
                grpc_error_set_int(
                    GRPC_ERROR_CREATE_FROM_STATIC_STRING("Buffers full"),
                    GRPC_ERROR_INT_HTTP2_ERROR, GRPC_HTTP2_ENHANCE_YOUR_CALM));
  } else if (error == GRPC_ERROR_NONE &&
             GRPC_TRACE_FLAG_ENABLED(grpc_resource_quota_trace)) {
    gpr_log(GPR_INFO,
            "HTTP2: %s - skip benign reclamation, there are still %" PRIdPTR
            " streams",
            t->peer_string, grpc_chttp2_stream_map_size(&t->stream_map));
  }
  t->benign_reclaimer_registered = false;
  if (error != GRPC_ERROR_CANCELLED) {
    grpc_resource_user_finish_reclamation(
        grpc_endpoint_get_resource_user(t->ep));
  }
  GRPC_CHTTP2_UNREF_TRANSPORT(t, "benign_reclaimer");
}
static void destructive_reclaimer(void* arg, grpc_error* error) {
  grpc_chttp2_transport* t = static_cast<grpc_chttp2_transport*>(arg);
  t->combiner->Run(GRPC_CLOSURE_INIT(&t->destructive_reclaimer_locked,
                                     destructive_reclaimer_locked, t, nullptr),
                   GRPC_ERROR_REF(error));
}
static void destructive_reclaimer_locked(void* arg, grpc_error* error) {
  grpc_chttp2_transport* t = static_cast<grpc_chttp2_transport*>(arg);
  size_t n = grpc_chttp2_stream_map_size(&t->stream_map);
  t->destructive_reclaimer_registered = false;
  if (error == GRPC_ERROR_NONE && n > 0) {
    grpc_chttp2_stream* s = static_cast<grpc_chttp2_stream*>(
        grpc_chttp2_stream_map_rand(&t->stream_map));
    if (GRPC_TRACE_FLAG_ENABLED(grpc_resource_quota_trace)) {
      gpr_log(GPR_INFO, "HTTP2: %s - abandon stream id %d", t->peer_string,
              s->id);
    }
    grpc_chttp2_cancel_stream(
        t, s,
        grpc_error_set_int(GRPC_ERROR_CREATE_FROM_STATIC_STRING("Buffers full"),
                           GRPC_ERROR_INT_HTTP2_ERROR,
                           GRPC_HTTP2_ENHANCE_YOUR_CALM));
    if (n > 1) {
      post_destructive_reclaimer(t);
    }
  }
  if (error != GRPC_ERROR_CANCELLED) {
    grpc_resource_user_finish_reclamation(
        grpc_endpoint_get_resource_user(t->ep));
  }
  GRPC_CHTTP2_UNREF_TRANSPORT(t, "destructive_reclaimer");
}
const char* grpc_chttp2_initiate_write_reason_string(
    grpc_chttp2_initiate_write_reason reason) {
  switch (reason) {
    case GRPC_CHTTP2_INITIATE_WRITE_INITIAL_WRITE:
      return "INITIAL_WRITE";
    case GRPC_CHTTP2_INITIATE_WRITE_START_NEW_STREAM:
      return "START_NEW_STREAM";
    case GRPC_CHTTP2_INITIATE_WRITE_SEND_MESSAGE:
      return "SEND_MESSAGE";
    case GRPC_CHTTP2_INITIATE_WRITE_SEND_INITIAL_METADATA:
      return "SEND_INITIAL_METADATA";
    case GRPC_CHTTP2_INITIATE_WRITE_SEND_TRAILING_METADATA:
      return "SEND_TRAILING_METADATA";
    case GRPC_CHTTP2_INITIATE_WRITE_RETRY_SEND_PING:
      return "RETRY_SEND_PING";
    case GRPC_CHTTP2_INITIATE_WRITE_CONTINUE_PINGS:
      return "CONTINUE_PINGS";
    case GRPC_CHTTP2_INITIATE_WRITE_GOAWAY_SENT:
      return "GOAWAY_SENT";
    case GRPC_CHTTP2_INITIATE_WRITE_RST_STREAM:
      return "RST_STREAM";
    case GRPC_CHTTP2_INITIATE_WRITE_CLOSE_FROM_API:
      return "CLOSE_FROM_API";
    case GRPC_CHTTP2_INITIATE_WRITE_STREAM_FLOW_CONTROL:
      return "STREAM_FLOW_CONTROL";
    case GRPC_CHTTP2_INITIATE_WRITE_TRANSPORT_FLOW_CONTROL:
      return "TRANSPORT_FLOW_CONTROL";
    case GRPC_CHTTP2_INITIATE_WRITE_SEND_SETTINGS:
      return "SEND_SETTINGS";
    case GRPC_CHTTP2_INITIATE_WRITE_FLOW_CONTROL_UNSTALLED_BY_SETTING:
      return "FLOW_CONTROL_UNSTALLED_BY_SETTING";
    case GRPC_CHTTP2_INITIATE_WRITE_FLOW_CONTROL_UNSTALLED_BY_UPDATE:
      return "FLOW_CONTROL_UNSTALLED_BY_UPDATE";
    case GRPC_CHTTP2_INITIATE_WRITE_APPLICATION_PING:
      return "APPLICATION_PING";
    case GRPC_CHTTP2_INITIATE_WRITE_KEEPALIVE_PING:
      return "KEEPALIVE_PING";
    case GRPC_CHTTP2_INITIATE_WRITE_TRANSPORT_FLOW_CONTROL_UNSTALLED:
      return "TRANSPORT_FLOW_CONTROL_UNSTALLED";
    case GRPC_CHTTP2_INITIATE_WRITE_PING_RESPONSE:
      return "PING_RESPONSE";
    case GRPC_CHTTP2_INITIATE_WRITE_FORCE_RST_STREAM:
      return "FORCE_RST_STREAM";
  }
  GPR_UNREACHABLE_CODE(return "unknown");
}
static grpc_endpoint* chttp2_get_endpoint(grpc_transport* t) {
  return (reinterpret_cast<grpc_chttp2_transport*>(t))->ep;
}
static const grpc_transport_vtable vtable = {sizeof(grpc_chttp2_stream),
                                             "chttp2",
                                             init_stream,
                                             set_pollset,
                                             set_pollset_set,
                                             perform_stream_op,
                                             perform_transport_op,
                                             destroy_stream,
                                             destroy_transport,
                                             chttp2_get_endpoint};
static const grpc_transport_vtable* get_vtable(void) { return &vtable; }
grpc_core::RefCountedPtr<grpc_core::channelz::SocketNode>
grpc_chttp2_transport_get_socket_node(grpc_transport* transport) {
  grpc_chttp2_transport* t =
      reinterpret_cast<grpc_chttp2_transport*>(transport);
  return t->channelz_socket;
}
grpc_transport* grpc_create_chttp2_transport(
    const grpc_channel_args* channel_args, grpc_endpoint* ep, bool is_client,
    grpc_resource_user* resource_user) {
  auto t =
      new grpc_chttp2_transport(channel_args, ep, is_client, resource_user);
  return &t->base;
}
void grpc_chttp2_transport_start_reading(
    grpc_transport* transport, grpc_slice_buffer* read_buffer,
    grpc_closure* notify_on_receive_settings) {
  grpc_chttp2_transport* t =
      reinterpret_cast<grpc_chttp2_transport*>(transport);
  GRPC_CHTTP2_REF_TRANSPORT(
      t, "reading_action");
  if (read_buffer != nullptr) {
    grpc_slice_buffer_move_into(read_buffer, &t->read_buffer);
    gpr_free(read_buffer);
  }
  t->notify_on_receive_settings = notify_on_receive_settings;
  t->combiner->Run(
      GRPC_CLOSURE_INIT(&t->read_action_locked, read_action_locked, t, nullptr),
      GRPC_ERROR_NONE);
}
