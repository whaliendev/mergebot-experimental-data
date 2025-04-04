#include "src/core/ext/transport/chttp2/transport/internal.h"
#include <limits.h>
#include <grpc/support/log.h>
#include "src/core/lib/debug/stats.h"
#include "src/core/lib/profiling/timers.h"
#include "src/core/lib/slice/slice_internal.h"
#include "src/core/lib/transport/http2_errors.h"
static void add_to_write_list(grpc_chttp2_write_cb **list,
                              grpc_chttp2_write_cb *cb) {
  cb->next = *list;
  *list = cb;
}
static void finish_write_cb(grpc_exec_ctx *exec_ctx, grpc_chttp2_transport *t,
                            grpc_chttp2_stream *s, grpc_chttp2_write_cb *cb,
                            grpc_error *error) {
  grpc_chttp2_complete_closure_step(exec_ctx, t, s, &cb->closure, error,
                                    "finish_write_cb");
  cb->next = t->write_cb_pool;
  t->write_cb_pool = cb;
}
static void maybe_initiate_ping(grpc_exec_ctx *exec_ctx,
                                grpc_chttp2_transport *t) {
  grpc_chttp2_ping_queue *pq = &t->ping_queue;
  if (grpc_closure_list_empty(pq->lists[GRPC_CHTTP2_PCL_NEXT])) {
    return;
  }
  if (!grpc_closure_list_empty(pq->lists[GRPC_CHTTP2_PCL_INFLIGHT])) {
    if (GRPC_TRACER_ON(grpc_http_trace) ||
        GRPC_TRACER_ON(grpc_bdp_estimator_trace)) {
      gpr_log(GPR_DEBUG, "%s: Ping delayed [%p]: already pinging",
              t->is_client ? "CLIENT" : "SERVER", t->peer_string);
    }
    return;
  }
  if (t->ping_state.pings_before_data_required == 0 &&
      t->ping_policy.max_pings_without_data != 0) {
    if (GRPC_TRACER_ON(grpc_http_trace) ||
        GRPC_TRACER_ON(grpc_bdp_estimator_trace)) {
      gpr_log(GPR_DEBUG, "%s: Ping delayed [%p]: too many recent pings: %d/%d",
              t->is_client ? "CLIENT" : "SERVER", t->peer_string,
              t->ping_state.pings_before_data_required,
              t->ping_policy.max_pings_without_data);
    }
    return;
  }
  grpc_millis now = grpc_exec_ctx_now(exec_ctx);
  grpc_millis next_allowed_ping =
      t->ping_state.last_ping_sent_time +
      t->ping_policy.min_sent_ping_interval_without_data;
  if (t->keepalive_permit_without_calls == 0 &&
      grpc_chttp2_stream_map_size(&t->stream_map) == 0) {
    next_allowed_ping =
        t->ping_recv_state.last_ping_recv_time + 7200 * GPR_MS_PER_SEC;
  }
  if (next_allowed_ping > now) {
    if (GRPC_TRACER_ON(grpc_http_trace) ||
        GRPC_TRACER_ON(grpc_bdp_estimator_trace)) {
      gpr_log(GPR_DEBUG,
              "%s: Ping delayed [%p]: not enough time elapsed since last ping",
              t->is_client ? "CLIENT" : "SERVER", t->peer_string);
    }
    if (!t->ping_state.is_delayed_ping_timer_set) {
      t->ping_state.is_delayed_ping_timer_set = true;
      grpc_timer_init(exec_ctx, &t->ping_state.delayed_ping_timer,
                      next_allowed_ping, &t->retry_initiate_ping_locked);
    }
    return;
  }
  pq->inflight_id = t->ping_ctr;
  t->ping_ctr++;
  GRPC_CLOSURE_LIST_SCHED(exec_ctx, &pq->lists[GRPC_CHTTP2_PCL_INITIATE]);
  grpc_closure_list_move(&pq->lists[GRPC_CHTTP2_PCL_NEXT],
                         &pq->lists[GRPC_CHTTP2_PCL_INFLIGHT]);
  grpc_slice_buffer_add(&t->outbuf,
                        grpc_chttp2_ping_create(false, pq->inflight_id));
  GRPC_STATS_INC_HTTP2_PINGS_SENT(exec_ctx);
  t->ping_state.last_ping_sent_time = now;
  if (GRPC_TRACER_ON(grpc_http_trace) ||
      GRPC_TRACER_ON(grpc_bdp_estimator_trace)) {
    gpr_log(GPR_DEBUG, "%s: Ping sent [%p]: %d/%d",
            t->is_client ? "CLIENT" : "SERVER", t->peer_string,
            t->ping_state.pings_before_data_required,
            t->ping_policy.max_pings_without_data);
  }
  t->ping_state.pings_before_data_required -=
      (t->ping_state.pings_before_data_required != 0);
}
static bool update_list(grpc_exec_ctx *exec_ctx, grpc_chttp2_transport *t,
                        grpc_chttp2_stream *s, int64_t send_bytes,
                        grpc_chttp2_write_cb **list, int64_t *ctr,
                        grpc_error *error) {
  bool sched_any = false;
  grpc_chttp2_write_cb *cb = *list;
  *list = NULL;
  *ctr += send_bytes;
  while (cb) {
    grpc_chttp2_write_cb *next = cb->next;
    if (cb->call_at_byte <= *ctr) {
      sched_any = true;
      finish_write_cb(exec_ctx, t, s, cb, GRPC_ERROR_REF(error));
    } else {
      add_to_write_list(list, cb);
    }
    cb = next;
  }
  GRPC_ERROR_UNREF(error);
  return sched_any;
}
static void report_stall(grpc_chttp2_transport *t, grpc_chttp2_stream *s,
                         const char *staller) {
  gpr_log(
      GPR_DEBUG,
      "%s:%p stream %d stalled by %s [fc:pending=%" PRIdPTR ":flowed=%" PRId64
      ":peer_initwin=%d:t_win=%" PRId64 ":s_win=%d:s_delta=%" PRId64 "]",
      t->peer_string, t, s->id, staller, s->flow_controlled_buffer.length,
      s->flow_controlled_bytes_flowed,
      t->settings[GRPC_ACKED_SETTINGS]
                 [GRPC_CHTTP2_SETTINGS_INITIAL_WINDOW_SIZE],
      t->flow_control.remote_window,
      (uint32_t)GPR_MAX(
          0,
          s->flow_control.remote_window_delta +
              (int64_t)t->settings[GRPC_PEER_SETTINGS]
                                  [GRPC_CHTTP2_SETTINGS_INITIAL_WINDOW_SIZE]),
      s->flow_control.remote_window_delta);
}
static bool stream_ref_if_not_destroyed(gpr_refcount *r) {
  gpr_atm count;
  do {
    count = gpr_atm_acq_load(&r->count);
    if (count == 0) return false;
  } while (!gpr_atm_rel_cas(&r->count, count, count + 1));
  return true;
}
static uint32_t target_write_size(grpc_chttp2_transport *t) {
  return 1024 * 1024;
}
static bool is_default_initial_metadata(grpc_metadata_batch *initial_metadata) {
  return initial_metadata->list.default_count == initial_metadata->list.count;
}
namespace {
class StreamWriteContext;
class WriteContext {
 public:
  WriteContext(grpc_exec_ctx *exec_ctx, grpc_chttp2_transport *t) : t_(t) {
    GRPC_STATS_INC_HTTP2_WRITES_BEGUN(exec_ctx);
    GPR_TIMER_BEGIN("grpc_chttp2_begin_write", 0);
  }
  void FlushStats(grpc_exec_ctx *exec_ctx) {
    GRPC_STATS_INC_HTTP2_SEND_INITIAL_METADATA_PER_WRITE(
        exec_ctx, initial_metadata_writes_);
    GRPC_STATS_INC_HTTP2_SEND_MESSAGE_PER_WRITE(exec_ctx, message_writes_);
    GRPC_STATS_INC_HTTP2_SEND_TRAILING_METADATA_PER_WRITE(
        exec_ctx, trailing_metadata_writes_);
    GRPC_STATS_INC_HTTP2_SEND_FLOWCTL_PER_WRITE(exec_ctx, flow_control_writes_);
  }
  void FlushSettings(grpc_exec_ctx *exec_ctx) {
    if (t_->dirtied_local_settings && !t_->sent_local_settings) {
      grpc_slice_buffer_add(
          &t_->outbuf, grpc_chttp2_settings_create(
                           t_->settings[GRPC_SENT_SETTINGS],
                           t_->settings[GRPC_LOCAL_SETTINGS],
                           t_->force_send_settings, GRPC_CHTTP2_NUM_SETTINGS));
      t_->force_send_settings = false;
      t_->dirtied_local_settings = false;
      t_->sent_local_settings = true;
      GRPC_STATS_INC_HTTP2_SETTINGS_WRITES(exec_ctx);
    }
  }
  void FlushQueuedBuffers(grpc_exec_ctx *exec_ctx) {
    grpc_slice_buffer_move_into(&t_->qbuf, &t_->outbuf);
    GPR_ASSERT(t_->qbuf.count == 0);
  }
  void FlushWindowUpdates(grpc_exec_ctx *exec_ctx) {
    uint32_t transport_announce =
        grpc_chttp2_flowctl_maybe_send_transport_update(&t_->flow_control);
    if (transport_announce) {
      maybe_initiate_ping(exec_ctx, t_,
                          GRPC_CHTTP2_PING_BEFORE_TRANSPORT_WINDOW_UPDATE);
      grpc_transport_one_way_stats throwaway_stats;
      grpc_slice_buffer_add(
          &t_->outbuf, grpc_chttp2_window_update_create(0, transport_announce,
                                                        &throwaway_stats));
      ResetPingRecvClock();
    }
  }
  void FlushPingAcks() {
    for (size_t i = 0; i < t_->ping_ack_count; i++) {
      grpc_slice_buffer_add(&t_->outbuf,
                            grpc_chttp2_ping_create(true, t_->ping_acks[i]));
    }
    t_->ping_ack_count = 0;
  }
  void EnactHpackSettings(grpc_exec_ctx *exec_ctx) {
    grpc_chttp2_hpack_compressor_set_max_table_size(
        &t_->hpack_compressor,
        t_->settings[GRPC_PEER_SETTINGS]
                    [GRPC_CHTTP2_SETTINGS_HEADER_TABLE_SIZE]);
  }
  void UpdateStreamsNoLongerStalled() {
    grpc_chttp2_stream *s;
    while (grpc_chttp2_list_pop_stalled_by_transport(t_, &s)) {
      if (!t_->closed && grpc_chttp2_list_add_writable_stream(t_, s)) {
        if (!stream_ref_if_not_destroyed(&s->refcount->refs)) {
          grpc_chttp2_list_remove_writable_stream(t_, s);
        }
      }
    }
  }
  grpc_chttp2_stream *NextStream() {
    if (t_->outbuf.length > target_write_size(t_)) {
      result_.partial = true;
      return nullptr;
    }
    grpc_chttp2_stream *s;
    if (!grpc_chttp2_list_pop_writable_stream(t_, &s)) {
      return nullptr;
    }
    return s;
  }
  void ResetPingRecvClock() {
    if (!t_->is_client) {
      t_->ping_recv_state.last_ping_recv_time =
          gpr_inf_past(GPR_CLOCK_MONOTONIC);
      t_->ping_recv_state.ping_strikes = 0;
    }
  }
  void IncInitialMetadataWrites() { ++initial_metadata_writes_; }
  void IncWindowUpdateWrites() { ++flow_control_writes_; }
  void IncMessageWrites() { ++message_writes_; }
  void IncTrailingMetadataWrites() { ++trailing_metadata_writes_; }
  void NoteScheduledResults() { result_.early_results_scheduled = true; }
  grpc_chttp2_transport *transport() const { return t_; }
  grpc_chttp2_begin_write_result Result() {
    result_.writing = t_->outbuf.count > 0;
    return result_;
  }
 private:
  grpc_chttp2_transport *const t_;
  int flow_control_writes_ = 0;
  int initial_metadata_writes_ = 0;
  int trailing_metadata_writes_ = 0;
  int message_writes_ = 0;
  grpc_chttp2_begin_write_result result_ = {false, false, false};
};
class DataSendContext {
 public:
  DataSendContext(WriteContext *write_context, grpc_chttp2_transport *t,
                  grpc_chttp2_stream *s)
      : write_context_(write_context),
        t_(t),
        s_(s),
        sending_bytes_before_(s_->sending_bytes) {}
  uint32_t stream_remote_window() const {
    return (uint32_t)GPR_MAX(
        0, s_->flow_control.remote_window_delta +
               (int64_t)t_->settings[GRPC_PEER_SETTINGS]
                                    [GRPC_CHTTP2_SETTINGS_INITIAL_WINDOW_SIZE]);
  }
<<<<<<< HEAD
  uint32_t max_outgoing() const {
    return (uint32_t)GPR_MIN(
        t_->settings[GRPC_PEER_SETTINGS][GRPC_CHTTP2_SETTINGS_MAX_FRAME_SIZE],
        GPR_MIN(stream_remote_window(), t_->flow_control.remote_window));
  }
||||||| 52ac18b54e
  grpc_slice_buffer_move_into(&t->qbuf, &t->outbuf);
  GPR_ASSERT(t->qbuf.count == 0);
=======
  for (size_t i = 0; i < t->ping_ack_count; i++) {
    grpc_slice_buffer_add(&t->outbuf,
                          grpc_chttp2_ping_create(1, t->ping_acks[i]));
  }
  t->ping_ack_count = 0;
  grpc_slice_buffer_move_into(&t->qbuf, &t->outbuf);
  GPR_ASSERT(t->qbuf.count == 0);
>>>>>>> abecd394
  bool AnyOutgoing() const { return max_outgoing() != 0; }
  void FlushCompressedBytes() {
    uint32_t send_bytes =
        (uint32_t)GPR_MIN(max_outgoing(), s_->compressed_data_buffer.length);
    bool is_last_data_frame =
        (send_bytes == s_->compressed_data_buffer.length &&
         s_->flow_controlled_buffer.length == 0 &&
         s_->fetching_send_message == NULL);
    if (is_last_data_frame && s_->send_trailing_metadata != NULL &&
        s_->stream_compression_ctx != NULL) {
      if (!grpc_stream_compress(s_->stream_compression_ctx,
                                &s_->flow_controlled_buffer,
                                &s_->compressed_data_buffer, NULL, MAX_SIZE_T,
                                GRPC_STREAM_COMPRESSION_FLUSH_FINISH)) {
        gpr_log(GPR_ERROR, "Stream compression failed.");
      }
      grpc_stream_compression_context_destroy(s_->stream_compression_ctx);
      s_->stream_compression_ctx = NULL;
      return;
    }
    is_last_frame_ = is_last_data_frame && s_->send_trailing_metadata != NULL &&
                     grpc_metadata_batch_is_empty(s_->send_trailing_metadata);
    grpc_chttp2_encode_data(s_->id, &s_->compressed_data_buffer, send_bytes,
                            is_last_frame_, &s_->stats.outgoing, &t_->outbuf);
    grpc_chttp2_flowctl_sent_data(&t_->flow_control, &s_->flow_control,
                                  send_bytes);
    if (s_->compressed_data_buffer.length == 0) {
      s_->sending_bytes += s_->uncompressed_data_size;
    }
  }
  void CompressMoreBytes() {
    if (s_->stream_compression_ctx == NULL) {
      s_->stream_compression_ctx =
          grpc_stream_compression_context_create(s_->stream_compression_method);
    }
    s_->uncompressed_data_size = s_->flow_controlled_buffer.length;
    if (!grpc_stream_compress(s_->stream_compression_ctx,
                              &s_->flow_controlled_buffer,
                              &s_->compressed_data_buffer, NULL, MAX_SIZE_T,
                              GRPC_STREAM_COMPRESSION_FLUSH_SYNC)) {
      gpr_log(GPR_ERROR, "Stream compression failed.");
    }
  }
  bool WasLastFrame() const { return is_last_frame_; }
  void CallCallbacks(grpc_exec_ctx *exec_ctx) {
    if (update_list(exec_ctx, t_, s_,
                    (int64_t)(s_->sending_bytes - sending_bytes_before_),
                    &s_->on_flow_controlled_cbs,
                    &s_->flow_controlled_bytes_flowed, GRPC_ERROR_NONE)) {
      write_context_->NoteScheduledResults();
    }
  }
 private:
  WriteContext *write_context_;
  grpc_chttp2_transport *t_;
  grpc_chttp2_stream *s_;
  const size_t sending_bytes_before_;
  bool is_last_frame_ = false;
};
class StreamWriteContext {
 public:
  StreamWriteContext(WriteContext *write_context, grpc_chttp2_stream *s)
      : write_context_(write_context),
        t_(write_context->transport()),
        s_(s),
        sent_initial_metadata_(s->sent_initial_metadata) {
    GRPC_CHTTP2_IF_TRACING(
        gpr_log(GPR_DEBUG, "W:%p %s[%d] im-(sent,send)=(%d,%d) announce=%d", t_,
                t_->is_client ? "CLIENT" : "SERVER", s->id,
                sent_initial_metadata_, s->send_initial_metadata != NULL,
                (int)(s->flow_control.local_window_delta -
                      s->flow_control.announced_window_delta)));
  }
  void FlushInitialMetadata(grpc_exec_ctx *exec_ctx) {
    if (sent_initial_metadata_) return;
    if (s_->send_initial_metadata == nullptr) return;
    if (!t_->is_client && s_->fetching_send_message == nullptr &&
        s_->flow_controlled_buffer.length == 0 &&
        s_->send_trailing_metadata == nullptr &&
        is_default_initial_metadata(s_->send_initial_metadata)) {
      ConvertInitialMetadataToTrailingMetadata();
      return;
    }
    grpc_encode_header_options hopt = {
        s_->id,
        false,
        t_->settings[GRPC_PEER_SETTINGS]
                    [GRPC_CHTTP2_SETTINGS_GRPC_ALLOW_TRUE_BINARY_METADATA] !=
            0,
        t_->settings[GRPC_PEER_SETTINGS]
                    [GRPC_CHTTP2_SETTINGS_MAX_FRAME_SIZE],
        &s_->stats.outgoing
    };
    grpc_chttp2_encode_header(exec_ctx, &t_->hpack_compressor, NULL, 0,
                              s_->send_initial_metadata, &hopt, &t_->outbuf);
    stream_became_writable_ = true;
    write_context_->ResetPingRecvClock();
    write_context_->IncInitialMetadataWrites();
    s_->send_initial_metadata = NULL;
    s_->sent_initial_metadata = true;
    sent_initial_metadata_ = true;
    write_context_->NoteScheduledResults();
    grpc_chttp2_complete_closure_step(
        exec_ctx, t_, s_, &s_->send_initial_metadata_finished, GRPC_ERROR_NONE,
        "send_initial_metadata_finished");
  }
  void FlushWindowUpdates(grpc_exec_ctx *exec_ctx) {
    uint32_t stream_announce = grpc_chttp2_flowctl_maybe_send_stream_update(
        &t_->flow_control, &s_->flow_control);
    if (stream_announce == 0) return;
    grpc_slice_buffer_add(
        &t_->outbuf, grpc_chttp2_window_update_create(s_->id, stream_announce,
                                                      &s_->stats.outgoing));
    write_context_->ResetPingRecvClock();
    write_context_->IncWindowUpdateWrites();
  }
  void FlushData(grpc_exec_ctx *exec_ctx) {
    if (!sent_initial_metadata_) return;
    if (s_->flow_controlled_buffer.length == 0 &&
        s_->compressed_data_buffer.length == 0) {
      return;
    }
    DataSendContext data_send_context(write_context_, t_, s_);
    if (!data_send_context.AnyOutgoing()) {
      if (t_->flow_control.remote_window == 0) {
        grpc_chttp2_list_add_stalled_by_transport(t_, s_);
      } else if (data_send_context.stream_remote_window() == 0) {
        grpc_chttp2_list_add_stalled_by_stream(t_, s_);
      }
      return;
    }
    while ((s_->flow_controlled_buffer.length > 0 ||
            s_->compressed_data_buffer.length > 0) &&
           data_send_context.max_outgoing() > 0) {
      if (s_->compressed_data_buffer.length > 0) {
        data_send_context.FlushCompressedBytes();
      } else {
        data_send_context.CompressMoreBytes();
      }
    }
    write_context_->ResetPingRecvClock();
    if (data_send_context.WasLastFrame()) {
      SentLastFrame(exec_ctx);
    }
    data_send_context.CallCallbacks(exec_ctx);
    stream_became_writable_ = true;
    if (s_->flow_controlled_buffer.length > 0 ||
        s_->compressed_data_buffer.length > 0) {
      GRPC_CHTTP2_STREAM_REF(s_, "chttp2_writing:fork");
      grpc_chttp2_list_add_writable_stream(t_, s_);
    }
    write_context_->IncMessageWrites();
  }
  void FlushTrailingMetadata(grpc_exec_ctx *exec_ctx) {
    if (!sent_initial_metadata_) return;
    if (s_->send_trailing_metadata == NULL) return;
    if (s_->fetching_send_message != NULL) return;
    if (s_->flow_controlled_buffer.length != 0) return;
    if (s_->compressed_data_buffer.length != 0) return;
    GRPC_CHTTP2_IF_TRACING(gpr_log(GPR_INFO, "sending trailing_metadata"));
    if (grpc_metadata_batch_is_empty(s_->send_trailing_metadata)) {
      grpc_chttp2_encode_data(s_->id, &s_->flow_controlled_buffer, 0, true,
                              &s_->stats.outgoing, &t_->outbuf);
    } else {
      grpc_encode_header_options hopt = {
          s_->id, true,
          t_->settings[GRPC_PEER_SETTINGS]
                      [GRPC_CHTTP2_SETTINGS_GRPC_ALLOW_TRUE_BINARY_METADATA] !=
              0,
          t_->settings[GRPC_PEER_SETTINGS][GRPC_CHTTP2_SETTINGS_MAX_FRAME_SIZE],
          &s_->stats.outgoing};
      grpc_chttp2_encode_header(exec_ctx, &t_->hpack_compressor,
                                extra_headers_for_trailing_metadata_,
                                num_extra_headers_for_trailing_metadata_,
                                s_->send_trailing_metadata, &hopt, &t_->outbuf);
    }
    write_context_->IncTrailingMetadataWrites();
    SentLastFrame(exec_ctx);
    write_context_->NoteScheduledResults();
    grpc_chttp2_complete_closure_step(
        exec_ctx, t_, s_, &s_->send_trailing_metadata_finished, GRPC_ERROR_NONE,
        "send_trailing_metadata_finished");
  }
  bool stream_became_writable() { return stream_became_writable_; }
 private:
  void ConvertInitialMetadataToTrailingMetadata() {
    GRPC_CHTTP2_IF_TRACING(
        gpr_log(GPR_INFO, "not sending initial_metadata (Trailers-Only)"));
    if (s_->send_initial_metadata->idx.named.status != NULL) {
      extra_headers_for_trailing_metadata_
          [num_extra_headers_for_trailing_metadata_++] =
              &s_->send_initial_metadata->idx.named.status->md;
    }
    if (s_->send_initial_metadata->idx.named.content_type != NULL) {
      extra_headers_for_trailing_metadata_
          [num_extra_headers_for_trailing_metadata_++] =
              &s_->send_initial_metadata->idx.named.content_type->md;
    }
  }
  void SentLastFrame(grpc_exec_ctx *exec_ctx) {
    s_->send_trailing_metadata = NULL;
    s_->sent_trailing_metadata = true;
    if (!t_->is_client && !s_->read_closed) {
      grpc_slice_buffer_add(
          &t_->outbuf, grpc_chttp2_rst_stream_create(
                           s_->id, GRPC_HTTP2_NO_ERROR, &s_->stats.outgoing));
    }
    grpc_chttp2_mark_stream_closed(exec_ctx, t_, s_, !t_->is_client, true,
                                   GRPC_ERROR_NONE);
  }
  WriteContext *const write_context_;
  grpc_chttp2_transport *const t_;
  grpc_chttp2_stream *const s_;
  bool sent_initial_metadata_;
  bool stream_became_writable_ = false;
  grpc_mdelem *extra_headers_for_trailing_metadata_[2];
  size_t num_extra_headers_for_trailing_metadata_ = 0;
};
}
grpc_chttp2_begin_write_result grpc_chttp2_begin_write(
    grpc_exec_ctx *exec_ctx, grpc_chttp2_transport *t) {
  WriteContext ctx(exec_ctx, t);
  ctx.FlushSettings(exec_ctx);
  ctx.FlushQueuedBuffers(exec_ctx);
  ctx.EnactHpackSettings(exec_ctx);
  if (t->flow_control.remote_window > 0) {
    ctx.UpdateStreamsNoLongerStalled();
  }
<<<<<<< HEAD
  while (grpc_chttp2_stream *s = ctx.NextStream()) {
    StreamWriteContext stream_ctx(&ctx, s);
    stream_ctx.FlushInitialMetadata(exec_ctx);
    stream_ctx.FlushWindowUpdates(exec_ctx);
    stream_ctx.FlushData(exec_ctx);
    stream_ctx.FlushTrailingMetadata(exec_ctx);
||||||| 52ac18b54e
  while (true) {
    if (t->outbuf.length > target_write_size(t)) {
      result.partial = true;
      break;
    }
    if (!grpc_chttp2_list_pop_writable_stream(t, &s)) {
      break;
    }
    bool sent_initial_metadata = s->sent_initial_metadata;
    bool now_writing = false;
    GRPC_CHTTP2_IF_TRACING(
        gpr_log(GPR_DEBUG, "W:%p %s[%d] im-(sent,send)=(%d,%d) announce=%d", t,
                t->is_client ? "CLIENT" : "SERVER", s->id,
                sent_initial_metadata, s->send_initial_metadata != NULL,
                (int)(s->flow_control.local_window_delta -
                      s->flow_control.announced_window_delta)));
    grpc_mdelem *extra_headers_for_trailing_metadata[2];
    size_t num_extra_headers_for_trailing_metadata = 0;
    if (!sent_initial_metadata && s->send_initial_metadata != NULL) {
      if (t->is_client || s->fetching_send_message != NULL ||
          s->flow_controlled_buffer.length != 0 ||
          s->send_trailing_metadata == NULL ||
          !is_default_initial_metadata(s->send_initial_metadata)) {
        grpc_encode_header_options hopt = {
            s->id,
            false,
            t->settings[GRPC_PEER_SETTINGS]
                       [GRPC_CHTTP2_SETTINGS_GRPC_ALLOW_TRUE_BINARY_METADATA] !=
                0,
            t->settings[GRPC_PEER_SETTINGS]
                       [GRPC_CHTTP2_SETTINGS_MAX_FRAME_SIZE],
            &s->stats.outgoing
        };
        grpc_chttp2_encode_header(exec_ctx, &t->hpack_compressor, NULL, 0,
                                  s->send_initial_metadata, &hopt, &t->outbuf);
        now_writing = true;
        if (!t->is_client) {
          t->ping_recv_state.last_ping_recv_time =
              gpr_inf_past(GPR_CLOCK_MONOTONIC);
          t->ping_recv_state.ping_strikes = 0;
        }
        initial_metadata_writes++;
      } else {
        GRPC_CHTTP2_IF_TRACING(
            gpr_log(GPR_INFO, "not sending initial_metadata (Trailers-Only)"));
        if (s->send_initial_metadata->idx.named.status != NULL) {
          extra_headers_for_trailing_metadata
              [num_extra_headers_for_trailing_metadata++] =
                  &s->send_initial_metadata->idx.named.status->md;
        }
        if (s->send_initial_metadata->idx.named.content_type != NULL) {
          extra_headers_for_trailing_metadata
              [num_extra_headers_for_trailing_metadata++] =
                  &s->send_initial_metadata->idx.named.content_type->md;
        }
        trailing_metadata_writes++;
      }
      s->send_initial_metadata = NULL;
      s->sent_initial_metadata = true;
      sent_initial_metadata = true;
      result.early_results_scheduled = true;
      grpc_chttp2_complete_closure_step(
          exec_ctx, t, s, &s->send_initial_metadata_finished, GRPC_ERROR_NONE,
          "send_initial_metadata_finished");
    }
    uint32_t stream_announce = grpc_chttp2_flowctl_maybe_send_stream_update(
        &t->flow_control, &s->flow_control);
    if (stream_announce > 0) {
      grpc_slice_buffer_add(
          &t->outbuf, grpc_chttp2_window_update_create(s->id, stream_announce,
                                                       &s->stats.outgoing));
      if (!t->is_client) {
        t->ping_recv_state.last_ping_recv_time =
            gpr_inf_past(GPR_CLOCK_MONOTONIC);
        t->ping_recv_state.ping_strikes = 0;
      }
      flow_control_writes++;
    }
    if (sent_initial_metadata) {
      if (s->flow_controlled_buffer.length > 0 ||
          s->compressed_data_buffer.length > 0) {
        uint32_t stream_remote_window = (uint32_t)GPR_MAX(
            0,
            s->flow_control.remote_window_delta +
                (int64_t)t->settings[GRPC_PEER_SETTINGS]
                                    [GRPC_CHTTP2_SETTINGS_INITIAL_WINDOW_SIZE]);
        uint32_t max_outgoing = (uint32_t)GPR_MIN(
            t->settings[GRPC_PEER_SETTINGS]
                       [GRPC_CHTTP2_SETTINGS_MAX_FRAME_SIZE],
            GPR_MIN(stream_remote_window, t->flow_control.remote_window));
        if (max_outgoing > 0) {
          bool is_last_data_frame = false;
          bool is_last_frame = false;
          size_t sending_bytes_before = s->sending_bytes;
          while ((s->flow_controlled_buffer.length > 0 ||
                  s->compressed_data_buffer.length > 0) &&
                 max_outgoing > 0) {
            if (s->compressed_data_buffer.length > 0) {
              uint32_t send_bytes = (uint32_t)GPR_MIN(
                  max_outgoing, s->compressed_data_buffer.length);
              is_last_data_frame =
                  (send_bytes == s->compressed_data_buffer.length &&
                   s->flow_controlled_buffer.length == 0 &&
                   s->fetching_send_message == NULL);
              if (is_last_data_frame && s->send_trailing_metadata != NULL &&
                  s->stream_compression_ctx != NULL) {
                if (!grpc_stream_compress(
                        s->stream_compression_ctx, &s->flow_controlled_buffer,
                        &s->compressed_data_buffer, NULL, MAX_SIZE_T,
                        GRPC_STREAM_COMPRESSION_FLUSH_FINISH)) {
                  gpr_log(GPR_ERROR, "Stream compression failed.");
                }
                grpc_stream_compression_context_destroy(
                    s->stream_compression_ctx);
                s->stream_compression_ctx = NULL;
                continue;
              }
              is_last_frame =
                  is_last_data_frame && s->send_trailing_metadata != NULL &&
                  grpc_metadata_batch_is_empty(s->send_trailing_metadata);
              grpc_chttp2_encode_data(s->id, &s->compressed_data_buffer,
                                      send_bytes, is_last_frame,
                                      &s->stats.outgoing, &t->outbuf);
              grpc_chttp2_flowctl_sent_data(&t->flow_control, &s->flow_control,
                                            send_bytes);
              max_outgoing -= send_bytes;
              if (s->compressed_data_buffer.length == 0) {
                s->sending_bytes += s->uncompressed_data_size;
              }
            } else {
              if (s->stream_compression_ctx == NULL) {
                s->stream_compression_ctx =
                    grpc_stream_compression_context_create(
                        s->stream_compression_method);
              }
              s->uncompressed_data_size = s->flow_controlled_buffer.length;
              if (!grpc_stream_compress(
                      s->stream_compression_ctx, &s->flow_controlled_buffer,
                      &s->compressed_data_buffer, NULL, MAX_SIZE_T,
                      GRPC_STREAM_COMPRESSION_FLUSH_SYNC)) {
                gpr_log(GPR_ERROR, "Stream compression failed.");
              }
            }
          }
          if (!t->is_client) {
            t->ping_recv_state.last_ping_recv_time =
                gpr_inf_past(GPR_CLOCK_MONOTONIC);
            t->ping_recv_state.ping_strikes = 0;
          }
          if (is_last_frame) {
            s->send_trailing_metadata = NULL;
            s->sent_trailing_metadata = true;
            if (!t->is_client && !s->read_closed) {
              grpc_slice_buffer_add(&t->outbuf, grpc_chttp2_rst_stream_create(
                                                    s->id, GRPC_HTTP2_NO_ERROR,
                                                    &s->stats.outgoing));
            }
            grpc_chttp2_mark_stream_closed(exec_ctx, t, s, !t->is_client, 1,
                                           GRPC_ERROR_NONE);
          }
          result.early_results_scheduled |=
              update_list(exec_ctx, t, s,
                          (int64_t)(s->sending_bytes - sending_bytes_before),
                          &s->on_flow_controlled_cbs,
                          &s->flow_controlled_bytes_flowed, GRPC_ERROR_NONE);
          now_writing = true;
          if (s->flow_controlled_buffer.length > 0 ||
              s->compressed_data_buffer.length > 0) {
            GRPC_CHTTP2_STREAM_REF(s, "chttp2_writing:fork");
            grpc_chttp2_list_add_writable_stream(t, s);
          }
          message_writes++;
        } else if (t->flow_control.remote_window == 0) {
          grpc_chttp2_list_add_stalled_by_transport(t, s);
          now_writing = true;
        } else if (stream_remote_window == 0) {
          grpc_chttp2_list_add_stalled_by_stream(t, s);
          now_writing = true;
        }
      }
      if (s->send_trailing_metadata != NULL &&
          s->fetching_send_message == NULL &&
          s->flow_controlled_buffer.length == 0 &&
          s->compressed_data_buffer.length == 0) {
        GRPC_CHTTP2_IF_TRACING(gpr_log(GPR_INFO, "sending trailing_metadata"));
        if (grpc_metadata_batch_is_empty(s->send_trailing_metadata)) {
          grpc_chttp2_encode_data(s->id, &s->flow_controlled_buffer, 0, true,
                                  &s->stats.outgoing, &t->outbuf);
        } else {
          grpc_encode_header_options hopt = {
              s->id, true,
              t->settings
                      [GRPC_PEER_SETTINGS]
                      [GRPC_CHTTP2_SETTINGS_GRPC_ALLOW_TRUE_BINARY_METADATA] !=
                  0,
              t->settings[GRPC_PEER_SETTINGS]
                         [GRPC_CHTTP2_SETTINGS_MAX_FRAME_SIZE],
              &s->stats.outgoing};
          grpc_chttp2_encode_header(exec_ctx, &t->hpack_compressor,
                                    extra_headers_for_trailing_metadata,
                                    num_extra_headers_for_trailing_metadata,
                                    s->send_trailing_metadata, &hopt,
                                    &t->outbuf);
          trailing_metadata_writes++;
        }
        s->send_trailing_metadata = NULL;
        s->sent_trailing_metadata = true;
        if (!t->is_client && !s->read_closed) {
          grpc_slice_buffer_add(
              &t->outbuf, grpc_chttp2_rst_stream_create(
                              s->id, GRPC_HTTP2_NO_ERROR, &s->stats.outgoing));
        }
        grpc_chttp2_mark_stream_closed(exec_ctx, t, s, !t->is_client, 1,
                                       GRPC_ERROR_NONE);
        now_writing = true;
        result.early_results_scheduled = true;
        grpc_chttp2_complete_closure_step(
            exec_ctx, t, s, &s->send_trailing_metadata_finished,
            GRPC_ERROR_NONE, "send_trailing_metadata_finished");
      }
    }
    if (now_writing) {
      GRPC_STATS_INC_HTTP2_SEND_INITIAL_METADATA_PER_WRITE(
          exec_ctx, initial_metadata_writes);
      GRPC_STATS_INC_HTTP2_SEND_MESSAGE_PER_WRITE(exec_ctx, message_writes);
      GRPC_STATS_INC_HTTP2_SEND_TRAILING_METADATA_PER_WRITE(
          exec_ctx, trailing_metadata_writes);
      GRPC_STATS_INC_HTTP2_SEND_FLOWCTL_PER_WRITE(exec_ctx,
                                                  flow_control_writes);
=======
  while (true) {
    if (t->outbuf.length > target_write_size(t)) {
      result.partial = true;
      break;
    }
    if (!grpc_chttp2_list_pop_writable_stream(t, &s)) {
      break;
    }
    bool sent_initial_metadata = s->sent_initial_metadata;
    bool now_writing = false;
    GRPC_CHTTP2_IF_TRACING(
        gpr_log(GPR_DEBUG, "W:%p %s[%d] im-(sent,send)=(%d,%d) announce=%d", t,
                t->is_client ? "CLIENT" : "SERVER", s->id,
                sent_initial_metadata, s->send_initial_metadata != NULL,
                (int)(s->flow_control.local_window_delta -
                      s->flow_control.announced_window_delta)));
    grpc_mdelem *extra_headers_for_trailing_metadata[2];
    size_t num_extra_headers_for_trailing_metadata = 0;
    if (!sent_initial_metadata && s->send_initial_metadata != NULL) {
      if (t->is_client || s->fetching_send_message != NULL ||
          s->flow_controlled_buffer.length != 0 ||
          s->send_trailing_metadata == NULL ||
          !is_default_initial_metadata(s->send_initial_metadata)) {
        grpc_encode_header_options hopt = {
            s->id,
            false,
            t->settings[GRPC_PEER_SETTINGS]
                       [GRPC_CHTTP2_SETTINGS_GRPC_ALLOW_TRUE_BINARY_METADATA] !=
                0,
            t->settings[GRPC_PEER_SETTINGS]
                       [GRPC_CHTTP2_SETTINGS_MAX_FRAME_SIZE],
            &s->stats.outgoing
        };
        grpc_chttp2_encode_header(exec_ctx, &t->hpack_compressor, NULL, 0,
                                  s->send_initial_metadata, &hopt, &t->outbuf);
        now_writing = true;
        if (!t->is_client) {
          t->ping_recv_state.last_ping_recv_time = GRPC_MILLIS_INF_PAST;
          t->ping_recv_state.ping_strikes = 0;
        }
        initial_metadata_writes++;
      } else {
        GRPC_CHTTP2_IF_TRACING(
            gpr_log(GPR_INFO, "not sending initial_metadata (Trailers-Only)"));
        if (s->send_initial_metadata->idx.named.status != NULL) {
          extra_headers_for_trailing_metadata
              [num_extra_headers_for_trailing_metadata++] =
                  &s->send_initial_metadata->idx.named.status->md;
        }
        if (s->send_initial_metadata->idx.named.content_type != NULL) {
          extra_headers_for_trailing_metadata
              [num_extra_headers_for_trailing_metadata++] =
                  &s->send_initial_metadata->idx.named.content_type->md;
        }
        trailing_metadata_writes++;
      }
      s->send_initial_metadata = NULL;
      s->sent_initial_metadata = true;
      sent_initial_metadata = true;
      result.early_results_scheduled = true;
      grpc_chttp2_complete_closure_step(
          exec_ctx, t, s, &s->send_initial_metadata_finished, GRPC_ERROR_NONE,
          "send_initial_metadata_finished");
    }
    uint32_t stream_announce = grpc_chttp2_flowctl_maybe_send_stream_update(
        &t->flow_control, &s->flow_control);
    if (stream_announce > 0) {
      grpc_slice_buffer_add(
          &t->outbuf, grpc_chttp2_window_update_create(s->id, stream_announce,
                                                       &s->stats.outgoing));
      if (!t->is_client) {
        t->ping_recv_state.last_ping_recv_time = GRPC_MILLIS_INF_PAST;
        t->ping_recv_state.ping_strikes = 0;
      }
      flow_control_writes++;
    }
    if (sent_initial_metadata) {
      if (s->flow_controlled_buffer.length > 0 ||
          s->compressed_data_buffer.length > 0) {
        uint32_t stream_remote_window = (uint32_t)GPR_MAX(
            0,
            s->flow_control.remote_window_delta +
                (int64_t)t->settings[GRPC_PEER_SETTINGS]
                                    [GRPC_CHTTP2_SETTINGS_INITIAL_WINDOW_SIZE]);
        uint32_t max_outgoing = (uint32_t)GPR_MIN(
            t->settings[GRPC_PEER_SETTINGS]
                       [GRPC_CHTTP2_SETTINGS_MAX_FRAME_SIZE],
            GPR_MIN(stream_remote_window, t->flow_control.remote_window));
        if (max_outgoing > 0) {
          bool is_last_data_frame = false;
          bool is_last_frame = false;
          size_t sending_bytes_before = s->sending_bytes;
          while ((s->flow_controlled_buffer.length > 0 ||
                  s->compressed_data_buffer.length > 0) &&
                 max_outgoing > 0) {
            if (s->compressed_data_buffer.length > 0) {
              uint32_t send_bytes = (uint32_t)GPR_MIN(
                  max_outgoing, s->compressed_data_buffer.length);
              is_last_data_frame =
                  (send_bytes == s->compressed_data_buffer.length &&
                   s->flow_controlled_buffer.length == 0 &&
                   s->fetching_send_message == NULL);
              if (is_last_data_frame && s->send_trailing_metadata != NULL &&
                  s->stream_compression_ctx != NULL) {
                if (!grpc_stream_compress(
                        s->stream_compression_ctx, &s->flow_controlled_buffer,
                        &s->compressed_data_buffer, NULL, MAX_SIZE_T,
                        GRPC_STREAM_COMPRESSION_FLUSH_FINISH)) {
                  gpr_log(GPR_ERROR, "Stream compression failed.");
                }
                grpc_stream_compression_context_destroy(
                    s->stream_compression_ctx);
                s->stream_compression_ctx = NULL;
                continue;
              }
              is_last_frame =
                  is_last_data_frame && s->send_trailing_metadata != NULL &&
                  grpc_metadata_batch_is_empty(s->send_trailing_metadata);
              grpc_chttp2_encode_data(s->id, &s->compressed_data_buffer,
                                      send_bytes, is_last_frame,
                                      &s->stats.outgoing, &t->outbuf);
              grpc_chttp2_flowctl_sent_data(&t->flow_control, &s->flow_control,
                                            send_bytes);
              max_outgoing -= send_bytes;
              if (s->compressed_data_buffer.length == 0) {
                s->sending_bytes += s->uncompressed_data_size;
              }
            } else {
              if (s->stream_compression_ctx == NULL) {
                s->stream_compression_ctx =
                    grpc_stream_compression_context_create(
                        s->stream_compression_method);
              }
              s->uncompressed_data_size = s->flow_controlled_buffer.length;
              if (!grpc_stream_compress(
                      s->stream_compression_ctx, &s->flow_controlled_buffer,
                      &s->compressed_data_buffer, NULL, MAX_SIZE_T,
                      GRPC_STREAM_COMPRESSION_FLUSH_SYNC)) {
                gpr_log(GPR_ERROR, "Stream compression failed.");
              }
            }
          }
          if (!t->is_client) {
            t->ping_recv_state.last_ping_recv_time = 0;
            t->ping_recv_state.ping_strikes = 0;
          }
          if (is_last_frame) {
            s->send_trailing_metadata = NULL;
            s->sent_trailing_metadata = true;
            if (!t->is_client && !s->read_closed) {
              grpc_slice_buffer_add(&t->outbuf, grpc_chttp2_rst_stream_create(
                                                    s->id, GRPC_HTTP2_NO_ERROR,
                                                    &s->stats.outgoing));
            }
            grpc_chttp2_mark_stream_closed(exec_ctx, t, s, !t->is_client, 1,
                                           GRPC_ERROR_NONE);
          }
          result.early_results_scheduled |=
              update_list(exec_ctx, t, s,
                          (int64_t)(s->sending_bytes - sending_bytes_before),
                          &s->on_flow_controlled_cbs,
                          &s->flow_controlled_bytes_flowed, GRPC_ERROR_NONE);
          now_writing = true;
          if (s->flow_controlled_buffer.length > 0 ||
              s->compressed_data_buffer.length > 0) {
            GRPC_CHTTP2_STREAM_REF(s, "chttp2_writing:fork");
            grpc_chttp2_list_add_writable_stream(t, s);
          }
          message_writes++;
        } else if (t->flow_control.remote_window == 0) {
          report_stall(t, s, "transport");
          grpc_chttp2_list_add_stalled_by_transport(t, s);
          now_writing = true;
        } else if (stream_remote_window == 0) {
          report_stall(t, s, "stream");
          grpc_chttp2_list_add_stalled_by_stream(t, s);
          now_writing = true;
        }
      }
      if (s->send_trailing_metadata != NULL &&
          s->fetching_send_message == NULL &&
          s->flow_controlled_buffer.length == 0 &&
          s->compressed_data_buffer.length == 0) {
        GRPC_CHTTP2_IF_TRACING(gpr_log(GPR_INFO, "sending trailing_metadata"));
        if (grpc_metadata_batch_is_empty(s->send_trailing_metadata)) {
          grpc_chttp2_encode_data(s->id, &s->flow_controlled_buffer, 0, true,
                                  &s->stats.outgoing, &t->outbuf);
        } else {
          grpc_encode_header_options hopt = {
              s->id, true,
              t->settings
                      [GRPC_PEER_SETTINGS]
                      [GRPC_CHTTP2_SETTINGS_GRPC_ALLOW_TRUE_BINARY_METADATA] !=
                  0,
              t->settings[GRPC_PEER_SETTINGS]
                         [GRPC_CHTTP2_SETTINGS_MAX_FRAME_SIZE],
              &s->stats.outgoing};
          grpc_chttp2_encode_header(exec_ctx, &t->hpack_compressor,
                                    extra_headers_for_trailing_metadata,
                                    num_extra_headers_for_trailing_metadata,
                                    s->send_trailing_metadata, &hopt,
                                    &t->outbuf);
          trailing_metadata_writes++;
        }
        s->send_trailing_metadata = NULL;
        s->sent_trailing_metadata = true;
        if (!t->is_client) {
          t->ping_recv_state.last_ping_recv_time = GRPC_MILLIS_INF_PAST;
          t->ping_recv_state.ping_strikes = 0;
        }
        if (!t->is_client && !s->read_closed) {
          grpc_slice_buffer_add(
              &t->outbuf, grpc_chttp2_rst_stream_create(
                              s->id, GRPC_HTTP2_NO_ERROR, &s->stats.outgoing));
        }
        grpc_chttp2_mark_stream_closed(exec_ctx, t, s, !t->is_client, 1,
                                       GRPC_ERROR_NONE);
        now_writing = true;
        result.early_results_scheduled = true;
        grpc_chttp2_complete_closure_step(
            exec_ctx, t, s, &s->send_trailing_metadata_finished,
            GRPC_ERROR_NONE, "send_trailing_metadata_finished");
      }
    }
    if (now_writing) {
      GRPC_STATS_INC_HTTP2_SEND_INITIAL_METADATA_PER_WRITE(
          exec_ctx, initial_metadata_writes);
      GRPC_STATS_INC_HTTP2_SEND_MESSAGE_PER_WRITE(exec_ctx, message_writes);
      GRPC_STATS_INC_HTTP2_SEND_TRAILING_METADATA_PER_WRITE(
          exec_ctx, trailing_metadata_writes);
      GRPC_STATS_INC_HTTP2_SEND_FLOWCTL_PER_WRITE(exec_ctx,
                                                  flow_control_writes);
>>>>>>> abecd394
    if (stream_ctx.stream_became_writable()) {
      if (!grpc_chttp2_list_add_writing_stream(t, s)) {
        GRPC_CHTTP2_STREAM_UNREF(exec_ctx, s, "chttp2_writing:already_writing");
      } else {
      }
    } else {
      GRPC_CHTTP2_STREAM_UNREF(exec_ctx, s, "chttp2_writing:no_write");
    }
  }
<<<<<<< HEAD
  ctx.FlushWindowUpdates(exec_ctx);
  ctx.FlushPingAcks();
  maybe_initiate_ping(exec_ctx, t, GRPC_CHTTP2_PING_ON_NEXT_WRITE);
||||||| 52ac18b54e
  uint32_t transport_announce =
      grpc_chttp2_flowctl_maybe_send_transport_update(&t->flow_control);
  if (transport_announce) {
    maybe_initiate_ping(exec_ctx, t,
                        GRPC_CHTTP2_PING_BEFORE_TRANSPORT_WINDOW_UPDATE);
    grpc_transport_one_way_stats throwaway_stats;
    grpc_slice_buffer_add(
        &t->outbuf, grpc_chttp2_window_update_create(0, transport_announce,
                                                     &throwaway_stats));
    if (!t->is_client) {
      t->ping_recv_state.last_ping_recv_time =
          gpr_inf_past(GPR_CLOCK_MONOTONIC);
      t->ping_recv_state.ping_strikes = 0;
    }
  }
  for (size_t i = 0; i < t->ping_ack_count; i++) {
    grpc_slice_buffer_add(&t->outbuf,
                          grpc_chttp2_ping_create(1, t->ping_acks[i]));
  }
  t->ping_ack_count = 0;
  maybe_initiate_ping(exec_ctx, t, GRPC_CHTTP2_PING_ON_NEXT_WRITE);
=======
  maybe_initiate_ping(exec_ctx, t);
  uint32_t transport_announce = grpc_chttp2_flowctl_maybe_send_transport_update(
      &t->flow_control, t->outbuf.count > 0);
  if (transport_announce) {
    grpc_transport_one_way_stats throwaway_stats;
    grpc_slice_buffer_add(
        &t->outbuf, grpc_chttp2_window_update_create(0, transport_announce,
                                                     &throwaway_stats));
    if (!t->is_client) {
      t->ping_recv_state.last_ping_recv_time = GRPC_MILLIS_INF_PAST;
      t->ping_recv_state.ping_strikes = 0;
    }
  }
>>>>>>> abecd394
  GPR_TIMER_END("grpc_chttp2_begin_write", 0);
  return ctx.Result();
}
void grpc_chttp2_end_write(grpc_exec_ctx *exec_ctx, grpc_chttp2_transport *t,
                           grpc_error *error) {
  GPR_TIMER_BEGIN("grpc_chttp2_end_write", 0);
  grpc_chttp2_stream *s;
  while (grpc_chttp2_list_pop_writing_stream(t, &s)) {
    if (s->sending_bytes != 0) {
      update_list(exec_ctx, t, s, (int64_t)s->sending_bytes,
                  &s->on_write_finished_cbs, &s->flow_controlled_bytes_written,
                  GRPC_ERROR_REF(error));
      s->sending_bytes = 0;
    }
    GRPC_CHTTP2_STREAM_UNREF(exec_ctx, s, "chttp2_writing:end");
  }
  grpc_slice_buffer_reset_and_unref_internal(exec_ctx, &t->outbuf);
  GRPC_ERROR_UNREF(error);
  GPR_TIMER_END("grpc_chttp2_end_write", 0);
}
