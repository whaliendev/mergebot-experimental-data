#include <grpc/support/port_platform.h>
#include <inttypes.h>
#include <limits.h>
#include <string.h>
#include "absl/strings/str_format.h"
#include "absl/strings/str_join.h"
#include <grpc/byte_buffer_reader.h>
#include <grpc/grpc.h>
#include <grpc/support/alloc.h>
#include <grpc/support/string_util.h>
#include <grpc/support/time.h>
#include "src/core/ext/filters/client_channel/client_channel.h"
#include "src/core/ext/filters/client_channel/parse_address.h"
#include "src/core/ext/filters/client_channel/server_address.h"
#include "src/core/ext/filters/client_channel/service_config.h"
#include "src/core/ext/filters/client_channel/xds/xds_api.h"
#include "src/core/ext/filters/client_channel/xds/xds_channel.h"
#include "src/core/ext/filters/client_channel/xds/xds_channel_args.h"
#include "src/core/ext/filters/client_channel/xds/xds_client.h"
#include "src/core/ext/filters/client_channel/xds/xds_client_stats.h"
#include "src/core/lib/backoff/backoff.h"
#include "src/core/lib/channel/channel_args.h"
#include "src/core/lib/channel/channel_stack.h"
#include "src/core/lib/gpr/string.h"
#include "src/core/lib/gprpp/map.h"
#include "src/core/lib/gprpp/memory.h"
#include "src/core/lib/gprpp/orphanable.h"
#include "src/core/lib/gprpp/ref_counted_ptr.h"
#include "src/core/lib/gprpp/sync.h"
#include "src/core/lib/iomgr/sockaddr.h"
#include "src/core/lib/iomgr/sockaddr_utils.h"
#include "src/core/lib/iomgr/timer.h"
#include "src/core/lib/iomgr/work_serializer.h"
#include "src/core/lib/slice/slice_hash_table.h"
#include "src/core/lib/slice/slice_internal.h"
#include "src/core/lib/slice/slice_string_helpers.h"
#include "src/core/lib/surface/call.h"
#include "src/core/lib/surface/channel.h"
#include "src/core/lib/surface/channel_init.h"
#include "src/core/lib/transport/static_metadata.h"
#define GRPC_XDS_INITIAL_CONNECT_BACKOFF_SECONDS 1
#define GRPC_XDS_RECONNECT_BACKOFF_MULTIPLIER 1.6
#define GRPC_XDS_RECONNECT_MAX_BACKOFF_SECONDS 120
#define GRPC_XDS_RECONNECT_JITTER 0.2
#define GRPC_XDS_MIN_CLIENT_LOAD_REPORTING_INTERVAL_MS 1000
namespace grpc_core {
TraceFlag grpc_xds_client_trace(false, "xds_client");
template <typename T>
class XdsClient::ChannelState::RetryableCall
    : public InternallyRefCounted<RetryableCall<T>> {
 public:
  explicit RetryableCall(RefCountedPtr<ChannelState> chand);
  void Orphan() override;
  void OnCallFinishedLocked();
  T* calld() const { return calld_.get(); }
  ChannelState* chand() const { return chand_.get(); }
  bool IsCurrentCallOnChannel() const;
 private:
  void StartNewCallLocked();
  void StartRetryTimerLocked();
  static void OnRetryTimer(void* arg, grpc_error* error);
  void OnRetryTimerLocked(grpc_error* error);
  OrphanablePtr<T> calld_;
  RefCountedPtr<ChannelState> chand_;
  BackOff backoff_;
  grpc_timer retry_timer_;
  grpc_closure on_retry_timer_;
  bool retry_timer_callback_pending_ = false;
  bool shutting_down_ = false;
};
class XdsClient::ChannelState::AdsCallState
    : public InternallyRefCounted<AdsCallState> {
 public:
  explicit AdsCallState(RefCountedPtr<RetryableCall<AdsCallState>> parent);
  ~AdsCallState() override;
  void Orphan() override;
  RetryableCall<AdsCallState>* parent() const { return parent_.get(); }
  ChannelState* chand() const { return parent_->chand(); }
  XdsClient* xds_client() const { return chand()->xds_client(); }
  bool seen_response() const { return seen_response_; }
  void Subscribe(const std::string& type_url, const std::string& name);
  void Unsubscribe(const std::string& type_url, const std::string& name,
                   bool delay_unsubscription);
  bool HasSubscribedResources() const;
 private:
  class ResourceState : public InternallyRefCounted<ResourceState> {
   public:
    ResourceState(const std::string& type_url, const std::string& name)
        : type_url_(type_url), name_(name) {
      GRPC_CLOSURE_INIT(&timer_callback_, OnTimer, this,
                        grpc_schedule_on_exec_ctx);
    }
    void Orphan() override {
      Finish();
      Unref();
    }
    void Start(RefCountedPtr<AdsCallState> ads_calld) {
      if (sent_) return;
      sent_ = true;
      ads_calld_ = std::move(ads_calld);
      Ref().release();
      timer_pending_ = true;
      grpc_timer_init(
          &timer_,
          ExecCtx::Get()->Now() + ads_calld_->xds_client()->request_timeout_,
          &timer_callback_);
    }
    void Finish() {
      if (timer_pending_) {
        grpc_timer_cancel(&timer_);
        timer_pending_ = false;
      }
    }
   private:
    static void OnTimer(void* arg, grpc_error* error) {
      ResourceState* self = static_cast<ResourceState*>(arg);
      GRPC_ERROR_REF(error);
      self->ads_calld_->xds_client()->work_serializer_->Run(
          [self, error]() { self->OnTimerLocked(error); }, DEBUG_LOCATION);
    }
    void OnTimerLocked(grpc_error* error) {
      if (error == GRPC_ERROR_NONE && timer_pending_) {
        timer_pending_ = false;
        char* msg;
        gpr_asprintf(
            &msg,
            "timeout obtaining resource {type=%s name=%s} from xds server",
            type_url_.c_str(), name_.c_str());
        grpc_error* watcher_error = GRPC_ERROR_CREATE_FROM_COPIED_STRING(msg);
        gpr_free(msg);
        if (GRPC_TRACE_FLAG_ENABLED(grpc_xds_client_trace)) {
          gpr_log(GPR_INFO, "[xds_client %p] %s", ads_calld_->xds_client(),
                  grpc_error_string(watcher_error));
        }
        if (type_url_ == XdsApi::kLdsTypeUrl ||
            type_url_ == XdsApi::kRdsTypeUrl) {
          ads_calld_->xds_client()->service_config_watcher_->OnError(
              watcher_error);
        } else if (type_url_ == XdsApi::kCdsTypeUrl) {
          ClusterState& state = ads_calld_->xds_client()->cluster_map_[name_];
          for (const auto& p : state.watchers) {
            p.first->OnError(GRPC_ERROR_REF(watcher_error));
          }
          GRPC_ERROR_UNREF(watcher_error);
        } else if (type_url_ == XdsApi::kEdsTypeUrl) {
          EndpointState& state = ads_calld_->xds_client()->endpoint_map_[name_];
          for (const auto& p : state.watchers) {
            p.first->OnError(GRPC_ERROR_REF(watcher_error));
          }
          GRPC_ERROR_UNREF(watcher_error);
        } else {
          GPR_UNREACHABLE_CODE(return );
        }
      }
      ads_calld_.reset();
      Unref();
      GRPC_ERROR_UNREF(error);
    }
    const std::string type_url_;
    const std::string name_;
    RefCountedPtr<AdsCallState> ads_calld_;
    bool sent_ = false;
    bool timer_pending_ = false;
    grpc_timer timer_;
    grpc_closure timer_callback_;
  };
  struct ResourceTypeState {
    ~ResourceTypeState() { GRPC_ERROR_UNREF(error); }
    std::string version;
    std::string nonce;
    grpc_error* error = GRPC_ERROR_NONE;
    std::map<std::string , OrphanablePtr<ResourceState>>
        subscribed_resources;
  };
  void SendMessageLocked(const std::string& type_url);
  void AcceptLdsUpdate(absl::optional<XdsApi::LdsUpdate> lds_update);
  void AcceptRdsUpdate(absl::optional<XdsApi::RdsUpdate> rds_update);
  void AcceptCdsUpdate(XdsApi::CdsUpdateMap cds_update_map);
  void AcceptEdsUpdate(XdsApi::EdsUpdateMap eds_update_map);
  static void OnRequestSent(void* arg, grpc_error* error);
  void OnRequestSentLocked(grpc_error* error);
  static void OnResponseReceived(void* arg, grpc_error* error);
  void OnResponseReceivedLocked();
  static void OnStatusReceived(void* arg, grpc_error* error);
  void OnStatusReceivedLocked(grpc_error* error);
  bool IsCurrentCallOnChannel() const;
  std::set<StringView> ClusterNamesForRequest();
  std::set<StringView> EdsServiceNamesForRequest();
  RefCountedPtr<RetryableCall<AdsCallState>> parent_;
  bool sent_initial_message_ = false;
  bool seen_response_ = false;
  grpc_call* call_;
  grpc_metadata_array initial_metadata_recv_;
  grpc_byte_buffer* send_message_payload_ = nullptr;
  grpc_closure on_request_sent_;
  grpc_byte_buffer* recv_message_payload_ = nullptr;
  grpc_closure on_response_received_;
  grpc_metadata_array trailing_metadata_recv_;
  grpc_status_code status_code_;
  grpc_slice status_details_;
  grpc_closure on_status_received_;
  std::set<std::string > buffered_requests_;
  std::map<std::string , ResourceTypeState> state_map_;
};
class XdsClient::ChannelState::LrsCallState
    : public InternallyRefCounted<LrsCallState> {
 public:
  explicit LrsCallState(RefCountedPtr<RetryableCall<LrsCallState>> parent);
  ~LrsCallState() override;
  void Orphan() override;
  void MaybeStartReportingLocked();
  RetryableCall<LrsCallState>* parent() { return parent_.get(); }
  ChannelState* chand() const { return parent_->chand(); }
  XdsClient* xds_client() const { return chand()->xds_client(); }
  bool seen_response() const { return seen_response_; }
 private:
  class Reporter : public InternallyRefCounted<Reporter> {
   public:
    Reporter(RefCountedPtr<LrsCallState> parent, grpc_millis report_interval)
        : parent_(std::move(parent)), report_interval_(report_interval) {
      GRPC_CLOSURE_INIT(&on_next_report_timer_, OnNextReportTimer, this,
                        grpc_schedule_on_exec_ctx);
      GRPC_CLOSURE_INIT(&on_report_done_, OnReportDone, this,
                        grpc_schedule_on_exec_ctx);
      ScheduleNextReportLocked();
    }
    void Orphan() override;
   private:
    void ScheduleNextReportLocked();
    static void OnNextReportTimer(void* arg, grpc_error* error);
    void OnNextReportTimerLocked(grpc_error* error);
    void SendReportLocked();
    static void OnReportDone(void* arg, grpc_error* error);
    void OnReportDoneLocked(grpc_error* error);
    bool IsCurrentReporterOnCall() const {
      return this == parent_->reporter_.get();
    }
    XdsClient* xds_client() const { return parent_->xds_client(); }
    RefCountedPtr<LrsCallState> parent_;
    const grpc_millis report_interval_;
    bool last_report_counters_were_zero_ = false;
    bool next_report_timer_callback_pending_ = false;
    grpc_timer next_report_timer_;
    grpc_closure on_next_report_timer_;
    grpc_closure on_report_done_;
  };
  static void OnInitialRequestSent(void* arg, grpc_error* error);
  void OnInitialRequestSentLocked();
  static void OnResponseReceived(void* arg, grpc_error* error);
  void OnResponseReceivedLocked();
  static void OnStatusReceived(void* arg, grpc_error* error);
  void OnStatusReceivedLocked(grpc_error* error);
  bool IsCurrentCallOnChannel() const;
  RefCountedPtr<RetryableCall<LrsCallState>> parent_;
  bool seen_response_ = false;
  grpc_call* call_;
  grpc_metadata_array initial_metadata_recv_;
  grpc_byte_buffer* send_message_payload_ = nullptr;
  grpc_closure on_initial_request_sent_;
  grpc_byte_buffer* recv_message_payload_ = nullptr;
  grpc_closure on_response_received_;
  grpc_metadata_array trailing_metadata_recv_;
  grpc_status_code status_code_;
  grpc_slice status_details_;
  grpc_closure on_status_received_;
  std::set<std::string> cluster_names_;
  grpc_millis load_reporting_interval_ = 0;
  OrphanablePtr<Reporter> reporter_;
};
class XdsClient::ChannelState::StateWatcher
    : public AsyncConnectivityStateWatcherInterface {
 public:
  explicit StateWatcher(RefCountedPtr<ChannelState> parent)
      : AsyncConnectivityStateWatcherInterface(
            parent->xds_client()->work_serializer_),
        parent_(std::move(parent)) {}
 private:
  void OnConnectivityStateChange(grpc_connectivity_state new_state) override {
    if (!parent_->shutting_down_ &&
        new_state == GRPC_CHANNEL_TRANSIENT_FAILURE) {
      gpr_log(GPR_INFO,
              "[xds_client %p] xds channel in state TRANSIENT_FAILURE",
              parent_->xds_client());
      parent_->xds_client()->NotifyOnError(GRPC_ERROR_CREATE_FROM_STATIC_STRING(
          "xds channel in TRANSIENT_FAILURE"));
    }
  }
  RefCountedPtr<ChannelState> parent_;
};
namespace {
grpc_channel_args* BuildXdsChannelArgs(const grpc_channel_args& args) {
  static const char* args_to_remove[] = {
      GRPC_ARG_LB_POLICY_NAME,
      GRPC_ARG_SERVICE_CONFIG,
      GRPC_ARG_SERVER_URI,
      GRPC_ARG_DEFAULT_AUTHORITY,
      GRPC_SSL_TARGET_NAME_OVERRIDE_ARG,
      GRPC_ARG_CHANNELZ_CHANNEL_NODE,
      GRPC_ARG_KEEPALIVE_TIME_MS,
  };
  InlinedVector<grpc_arg, 3> args_to_add;
  args_to_add.emplace_back(grpc_channel_arg_integer_create(
      const_cast<char*>(GRPC_ARG_KEEPALIVE_TIME_MS), 5000));
  args_to_add.emplace_back(grpc_channel_arg_integer_create(
      const_cast<char*>(GRPC_ARG_ADDRESS_IS_XDS_SERVER), 1));
  channelz::ChannelNode* channelz_node = nullptr;
  const grpc_arg* arg =
      grpc_channel_args_find(&args, GRPC_ARG_CHANNELZ_CHANNEL_NODE);
  if (arg != nullptr && arg->type == GRPC_ARG_POINTER &&
      arg->value.pointer.p != nullptr) {
    channelz_node = static_cast<channelz::ChannelNode*>(arg->value.pointer.p);
    args_to_add.emplace_back(
        channelz::MakeParentUuidArg(channelz_node->uuid()));
  }
  grpc_channel_args* new_args = grpc_channel_args_copy_and_add_and_remove(
      &args, args_to_remove, GPR_ARRAY_SIZE(args_to_remove), args_to_add.data(),
      args_to_add.size());
  return ModifyXdsChannelArgs(new_args);
}
}
XdsClient::ChannelState::ChannelState(RefCountedPtr<XdsClient> xds_client,
                                      grpc_channel* channel)
    : InternallyRefCounted<ChannelState>(&grpc_xds_client_trace),
      xds_client_(std::move(xds_client)),
      channel_(channel) {
  GPR_ASSERT(channel_ != nullptr);
  StartConnectivityWatchLocked();
}
XdsClient::ChannelState::~ChannelState() {
  if (GRPC_TRACE_FLAG_ENABLED(grpc_xds_client_trace)) {
    gpr_log(GPR_INFO, "[xds_client %p] Destroying xds channel %p", xds_client(),
            this);
  }
  grpc_channel_destroy(channel_);
}
void XdsClient::ChannelState::Orphan() {
  shutting_down_ = true;
  CancelConnectivityWatchLocked();
  ads_calld_.reset();
  lrs_calld_.reset();
  Unref(DEBUG_LOCATION, "ChannelState+orphaned");
}
XdsClient::ChannelState::AdsCallState* XdsClient::ChannelState::ads_calld()
    const {
  return ads_calld_->calld();
}
XdsClient::ChannelState::LrsCallState* XdsClient::ChannelState::lrs_calld()
    const {
  return lrs_calld_->calld();
}
bool XdsClient::ChannelState::HasActiveAdsCall() const {
  return ads_calld_->calld() != nullptr;
}
void XdsClient::ChannelState::MaybeStartLrsCall() {
  if (lrs_calld_ != nullptr) return;
  lrs_calld_.reset(
      new RetryableCall<LrsCallState>(Ref(DEBUG_LOCATION, "ChannelState+lrs")));
}
void XdsClient::ChannelState::StopLrsCall() { lrs_calld_.reset(); }
void XdsClient::ChannelState::StartConnectivityWatchLocked() {
  grpc_channel_element* client_channel_elem =
      grpc_channel_stack_last_element(grpc_channel_get_channel_stack(channel_));
  GPR_ASSERT(client_channel_elem->filter == &grpc_client_channel_filter);
  watcher_ = new StateWatcher(Ref());
  grpc_client_channel_start_connectivity_watch(
      client_channel_elem, GRPC_CHANNEL_IDLE,
      OrphanablePtr<AsyncConnectivityStateWatcherInterface>(watcher_));
}
void XdsClient::ChannelState::CancelConnectivityWatchLocked() {
  grpc_channel_element* client_channel_elem =
      grpc_channel_stack_last_element(grpc_channel_get_channel_stack(channel_));
  GPR_ASSERT(client_channel_elem->filter == &grpc_client_channel_filter);
  grpc_client_channel_stop_connectivity_watch(client_channel_elem, watcher_);
}
void XdsClient::ChannelState::Subscribe(const std::string& type_url,
                                        const std::string& name) {
  if (ads_calld_ == nullptr) {
    ads_calld_.reset(new RetryableCall<AdsCallState>(
        Ref(DEBUG_LOCATION, "ChannelState+ads")));
    return;
  }
  if (ads_calld() == nullptr) return;
  ads_calld()->Subscribe(type_url, name);
}
void XdsClient::ChannelState::Unsubscribe(const std::string& type_url,
                                          const std::string& name,
                                          bool delay_unsubscription) {
  if (ads_calld_ != nullptr) {
    ads_calld_->calld()->Unsubscribe(type_url, name, delay_unsubscription);
    if (!ads_calld_->calld()->HasSubscribedResources()) ads_calld_.reset();
  }
}
template <typename T>
XdsClient::ChannelState::RetryableCall<T>::RetryableCall(
    RefCountedPtr<ChannelState> chand)
    : chand_(std::move(chand)),
      backoff_(
          BackOff::Options()
              .set_initial_backoff(GRPC_XDS_INITIAL_CONNECT_BACKOFF_SECONDS *
                                   1000)
              .set_multiplier(GRPC_XDS_RECONNECT_BACKOFF_MULTIPLIER)
              .set_jitter(GRPC_XDS_RECONNECT_JITTER)
              .set_max_backoff(GRPC_XDS_RECONNECT_MAX_BACKOFF_SECONDS * 1000)) {
  GRPC_CLOSURE_INIT(&on_retry_timer_, OnRetryTimer, this,
                    grpc_schedule_on_exec_ctx);
  StartNewCallLocked();
}
template <typename T>
void XdsClient::ChannelState::RetryableCall<T>::Orphan() {
  shutting_down_ = true;
  calld_.reset();
  if (retry_timer_callback_pending_) grpc_timer_cancel(&retry_timer_);
  this->Unref(DEBUG_LOCATION, "RetryableCall+orphaned");
}
template <typename T>
void XdsClient::ChannelState::RetryableCall<T>::OnCallFinishedLocked() {
  const bool seen_response = calld_->seen_response();
  calld_.reset();
  if (seen_response) {
    backoff_.Reset();
    StartNewCallLocked();
  } else {
    StartRetryTimerLocked();
  }
}
template <typename T>
void XdsClient::ChannelState::RetryableCall<T>::StartNewCallLocked() {
  if (shutting_down_) return;
  GPR_ASSERT(chand_->channel_ != nullptr);
  GPR_ASSERT(calld_ == nullptr);
  if (GRPC_TRACE_FLAG_ENABLED(grpc_xds_client_trace)) {
    gpr_log(GPR_INFO,
            "[xds_client %p] Start new call from retryable call (chand: %p, "
            "retryable call: %p)",
            chand()->xds_client(), chand(), this);
  }
  calld_ = MakeOrphanable<T>(
      this->Ref(DEBUG_LOCATION, "RetryableCall+start_new_call"));
}
template <typename T>
void XdsClient::ChannelState::RetryableCall<T>::StartRetryTimerLocked() {
  if (shutting_down_) return;
  const grpc_millis next_attempt_time = backoff_.NextAttemptTime();
  if (GRPC_TRACE_FLAG_ENABLED(grpc_xds_client_trace)) {
    grpc_millis timeout = GPR_MAX(next_attempt_time - ExecCtx::Get()->Now(), 0);
    gpr_log(GPR_INFO,
            "[xds_client %p] Failed to connect to xds server (chand: %p) "
            "retry timer will fire in %" PRId64 "ms.",
            chand()->xds_client(), chand(), timeout);
  }
  this->Ref(DEBUG_LOCATION, "RetryableCall+retry_timer_start").release();
  grpc_timer_init(&retry_timer_, next_attempt_time, &on_retry_timer_);
  retry_timer_callback_pending_ = true;
}
template <typename T>
void XdsClient::ChannelState::RetryableCall<T>::OnRetryTimer(
    void* arg, grpc_error* error) {
  RetryableCall* calld = static_cast<RetryableCall*>(arg);
  GRPC_ERROR_REF(error);
  calld->chand_->xds_client()->work_serializer_->Run(
      [calld, error]() { calld->OnRetryTimerLocked(error); }, DEBUG_LOCATION);
}
template <typename T>
void XdsClient::ChannelState::RetryableCall<T>::OnRetryTimerLocked(
    grpc_error* error) {
  retry_timer_callback_pending_ = false;
  if (!shutting_down_ && error == GRPC_ERROR_NONE) {
    if (GRPC_TRACE_FLAG_ENABLED(grpc_xds_client_trace)) {
      gpr_log(
          GPR_INFO,
          "[xds_client %p] Retry timer fires (chand: %p, retryable call: %p)",
          chand()->xds_client(), chand(), this);
    }
    StartNewCallLocked();
  }
  this->Unref(DEBUG_LOCATION, "RetryableCall+retry_timer_done");
  GRPC_ERROR_UNREF(error);
}
XdsClient::ChannelState::AdsCallState::AdsCallState(
    RefCountedPtr<RetryableCall<AdsCallState>> parent)
    : InternallyRefCounted<AdsCallState>(&grpc_xds_client_trace),
      parent_(std::move(parent)) {
  GPR_ASSERT(xds_client() != nullptr);
  GPR_ASSERT(!xds_client()->server_name_.empty());
  call_ = grpc_channel_create_pollset_set_call(
      chand()->channel_, nullptr, GRPC_PROPAGATE_DEFAULTS,
      xds_client()->interested_parties_,
      GRPC_MDSTR_SLASH_ENVOY_DOT_SERVICE_DOT_DISCOVERY_DOT_V2_DOT_AGGREGATEDDISCOVERYSERVICE_SLASH_STREAMAGGREGATEDRESOURCES,
      nullptr, GRPC_MILLIS_INF_FUTURE, nullptr);
  GPR_ASSERT(call_ != nullptr);
  grpc_metadata_array_init(&initial_metadata_recv_);
  grpc_metadata_array_init(&trailing_metadata_recv_);
  if (GRPC_TRACE_FLAG_ENABLED(grpc_xds_client_trace)) {
    gpr_log(GPR_INFO,
            "[xds_client %p] Starting ADS call (chand: %p, calld: %p, "
            "call: %p)",
            xds_client(), chand(), this, call_);
  }
  grpc_call_error call_error;
  grpc_op ops[3];
  memset(ops, 0, sizeof(ops));
  grpc_op* op = ops;
  op->op = GRPC_OP_SEND_INITIAL_METADATA;
  op->data.send_initial_metadata.count = 0;
  op->flags = GRPC_INITIAL_METADATA_WAIT_FOR_READY |
              GRPC_INITIAL_METADATA_WAIT_FOR_READY_EXPLICITLY_SET;
  op->reserved = nullptr;
  op++;
  call_error = grpc_call_start_batch_and_execute(call_, ops, (size_t)(op - ops),
                                                 nullptr);
  GPR_ASSERT(GRPC_CALL_OK == call_error);
  GRPC_CLOSURE_INIT(&on_request_sent_, OnRequestSent, this,
                    grpc_schedule_on_exec_ctx);
  if (xds_client()->service_config_watcher_ != nullptr) {
    Subscribe(XdsApi::kLdsTypeUrl, xds_client()->server_name_);
    if (xds_client()->lds_result_.has_value() &&
        !xds_client()->lds_result_->route_config_name.empty()) {
      Subscribe(XdsApi::kRdsTypeUrl,
                xds_client()->lds_result_->route_config_name);
    }
  }
  for (const auto& p : xds_client()->cluster_map_) {
    Subscribe(XdsApi::kCdsTypeUrl, std::string(p.first));
  }
  for (const auto& p : xds_client()->endpoint_map_) {
    Subscribe(XdsApi::kEdsTypeUrl, std::string(p.first));
  }
  op = ops;
  op->op = GRPC_OP_RECV_INITIAL_METADATA;
  op->data.recv_initial_metadata.recv_initial_metadata =
      &initial_metadata_recv_;
  op->flags = 0;
  op->reserved = nullptr;
  op++;
  op->op = GRPC_OP_RECV_MESSAGE;
  op->data.recv_message.recv_message = &recv_message_payload_;
  op->flags = 0;
  op->reserved = nullptr;
  op++;
  Ref(DEBUG_LOCATION, "ADS+OnResponseReceivedLocked").release();
  GRPC_CLOSURE_INIT(&on_response_received_, OnResponseReceived, this,
                    grpc_schedule_on_exec_ctx);
  call_error = grpc_call_start_batch_and_execute(call_, ops, (size_t)(op - ops),
                                                 &on_response_received_);
  GPR_ASSERT(GRPC_CALL_OK == call_error);
  op = ops;
  op->op = GRPC_OP_RECV_STATUS_ON_CLIENT;
  op->data.recv_status_on_client.trailing_metadata = &trailing_metadata_recv_;
  op->data.recv_status_on_client.status = &status_code_;
  op->data.recv_status_on_client.status_details = &status_details_;
  op->flags = 0;
  op->reserved = nullptr;
  op++;
  GRPC_CLOSURE_INIT(&on_status_received_, OnStatusReceived, this,
                    grpc_schedule_on_exec_ctx);
  call_error = grpc_call_start_batch_and_execute(call_, ops, (size_t)(op - ops),
                                                 &on_status_received_);
  GPR_ASSERT(GRPC_CALL_OK == call_error);
}
XdsClient::ChannelState::AdsCallState::~AdsCallState() {
  grpc_metadata_array_destroy(&initial_metadata_recv_);
  grpc_metadata_array_destroy(&trailing_metadata_recv_);
  grpc_byte_buffer_destroy(send_message_payload_);
  grpc_byte_buffer_destroy(recv_message_payload_);
  grpc_slice_unref_internal(status_details_);
  GPR_ASSERT(call_ != nullptr);
  grpc_call_unref(call_);
}
void XdsClient::ChannelState::AdsCallState::Orphan() {
  GPR_ASSERT(call_ != nullptr);
  grpc_call_cancel(call_, nullptr);
  state_map_.clear();
}
void XdsClient::ChannelState::AdsCallState::SendMessageLocked(
    const std::string& type_url) {
  if (send_message_payload_ != nullptr) {
    buffered_requests_.insert(type_url);
    return;
  }
  auto& state = state_map_[type_url];
  grpc_slice request_payload_slice;
  std::set<StringView> resource_names;
  if (type_url == XdsApi::kLdsTypeUrl) {
    resource_names.insert(xds_client()->server_name_);
    request_payload_slice = xds_client()->api_.CreateLdsRequest(
        xds_client()->server_name_, state.version, state.nonce,
        GRPC_ERROR_REF(state.error), !sent_initial_message_);
    state.subscribed_resources[xds_client()->server_name_]->Start(Ref());
  } else if (type_url == XdsApi::kRdsTypeUrl) {
    resource_names.insert(xds_client()->lds_result_->route_config_name);
    request_payload_slice = xds_client()->api_.CreateRdsRequest(
        xds_client()->lds_result_->route_config_name, state.version,
        state.nonce, GRPC_ERROR_REF(state.error), !sent_initial_message_);
    state.subscribed_resources[xds_client()->lds_result_->route_config_name]
        ->Start(Ref());
  } else if (type_url == XdsApi::kCdsTypeUrl) {
    resource_names = ClusterNamesForRequest();
    request_payload_slice = xds_client()->api_.CreateCdsRequest(
        resource_names, state.version, state.nonce, GRPC_ERROR_REF(state.error),
        !sent_initial_message_);
  } else if (type_url == XdsApi::kEdsTypeUrl) {
    resource_names = EdsServiceNamesForRequest();
    request_payload_slice = xds_client()->api_.CreateEdsRequest(
        resource_names, state.version, state.nonce, GRPC_ERROR_REF(state.error),
        !sent_initial_message_);
  } else {
    request_payload_slice = xds_client()->api_.CreateUnsupportedTypeNackRequest(
        type_url, state.nonce, GRPC_ERROR_REF(state.error));
    state_map_.erase(type_url);
  }
  sent_initial_message_ = true;
  if (GRPC_TRACE_FLAG_ENABLED(grpc_xds_client_trace)) {
    gpr_log(GPR_INFO,
            "[xds_client %p] sending ADS request: type=%s version=%s nonce=%s "
            "error=%s resources=%s",
            xds_client(), type_url.c_str(), state.version.c_str(),
            state.nonce.c_str(), grpc_error_string(state.error),
            absl::StrJoin(resource_names, " ").c_str());
  }
  GRPC_ERROR_UNREF(state.error);
  state.error = GRPC_ERROR_NONE;
  send_message_payload_ =
      grpc_raw_byte_buffer_create(&request_payload_slice, 1);
  grpc_slice_unref_internal(request_payload_slice);
  grpc_op op;
  memset(&op, 0, sizeof(op));
  op.op = GRPC_OP_SEND_MESSAGE;
  op.data.send_message.send_message = send_message_payload_;
  Ref(DEBUG_LOCATION, "ADS+OnRequestSentLocked").release();
  GRPC_CLOSURE_INIT(&on_request_sent_, OnRequestSent, this,
                    grpc_schedule_on_exec_ctx);
  grpc_call_error call_error =
      grpc_call_start_batch_and_execute(call_, &op, 1, &on_request_sent_);
  if (GPR_UNLIKELY(call_error != GRPC_CALL_OK)) {
    gpr_log(GPR_ERROR,
            "[xds_client %p] calld=%p call_error=%d sending ADS message",
            xds_client(), this, call_error);
    GPR_ASSERT(GRPC_CALL_OK == call_error);
  }
}
void XdsClient::ChannelState::AdsCallState::Subscribe(
    const std::string& type_url, const std::string& name) {
  auto& state = state_map_[type_url].subscribed_resources[name];
  if (state == nullptr) {
    state = MakeOrphanable<ResourceState>(type_url, name);
    SendMessageLocked(type_url);
  }
}
void XdsClient::ChannelState::AdsCallState::Unsubscribe(
    const std::string& type_url, const std::string& name,
    bool delay_unsubscription) {
  state_map_[type_url].subscribed_resources.erase(name);
  if (!delay_unsubscription) SendMessageLocked(type_url);
}
bool XdsClient::ChannelState::AdsCallState::HasSubscribedResources() const {
  for (const auto& p : state_map_) {
    if (!p.second.subscribed_resources.empty()) return true;
  }
  return false;
}
void XdsClient::ChannelState::AdsCallState::AcceptLdsUpdate(
    absl::optional<XdsApi::LdsUpdate> lds_update) {
  if (!lds_update.has_value()) {
    gpr_log(GPR_INFO,
            "[xds_client %p] LDS update does not include requested resource",
            xds_client());
    xds_client()->service_config_watcher_->OnError(
        GRPC_ERROR_CREATE_FROM_STATIC_STRING(
            "LDS update does not include requested resource"));
    return;
  }
  if (GRPC_TRACE_FLAG_ENABLED(grpc_xds_client_trace)) {
    gpr_log(GPR_INFO,
            "[xds_client %p] LDS update received: route_config_name=%s",
            xds_client(),
            (!lds_update->route_config_name.empty()
                 ? lds_update->route_config_name.c_str()
                 : "<inlined>"));
    if (lds_update->rds_update.has_value()) {
      gpr_log(GPR_INFO, "  RouteConfiguration contains %" PRIuPTR " routes",
              lds_update->rds_update.value().routes.size());
      for (const auto& route : lds_update->rds_update.value().routes) {
        gpr_log(GPR_INFO,
                "  route: { service=\"%s\", "
                "method=\"%s\" }, cluster=\"%s\" }",
                route.service.c_str(), route.method.c_str(),
                route.cluster_name.c_str());
      }
    }
  }
  auto& lds_state = state_map_[XdsApi::kLdsTypeUrl];
  auto& state = lds_state.subscribed_resources[xds_client()->server_name_];
  if (state != nullptr) state->Finish();
  if (xds_client()->lds_result_ == lds_update) {
    if (GRPC_TRACE_FLAG_ENABLED(grpc_xds_client_trace)) {
      gpr_log(GPR_INFO,
              "[xds_client %p] LDS update identical to current, ignoring.",
              xds_client());
    }
    return;
  }
  if (xds_client()->lds_result_.has_value() &&
      !xds_client()->lds_result_->route_config_name.empty()) {
    Unsubscribe(
        XdsApi::kRdsTypeUrl, xds_client()->lds_result_->route_config_name,
                                 !lds_update->route_config_name.empty());
  }
  xds_client()->lds_result_ = std::move(lds_update);
  if (xds_client()->lds_result_->rds_update.has_value()) {
    RefCountedPtr<ServiceConfig> service_config;
    grpc_error* error = xds_client()->CreateServiceConfig(
        xds_client()->lds_result_->rds_update.value(), &service_config);
    if (error == GRPC_ERROR_NONE) {
      xds_client()->service_config_watcher_->OnServiceConfigChanged(
          std::move(service_config));
    } else {
      xds_client()->service_config_watcher_->OnError(error);
    }
  } else {
    Subscribe(XdsApi::kRdsTypeUrl,
              xds_client()->lds_result_->route_config_name);
  }
}
void XdsClient::ChannelState::AdsCallState::AcceptRdsUpdate(
    absl::optional<XdsApi::RdsUpdate> rds_update) {
  if (!rds_update.has_value()) {
    gpr_log(GPR_INFO,
            "[xds_client %p] RDS update does not include requested resource",
            xds_client());
    xds_client()->service_config_watcher_->OnError(
        GRPC_ERROR_CREATE_FROM_STATIC_STRING(
            "RDS update does not include requested resource"));
    return;
  }
  if (GRPC_TRACE_FLAG_ENABLED(grpc_xds_client_trace)) {
    gpr_log(GPR_INFO,
            "[xds_client %p] RDS update received;  RouteConfiguration contains "
            "%" PRIuPTR " routes",
            this, rds_update.value().routes.size());
    for (const auto& route : rds_update.value().routes) {
      gpr_log(GPR_INFO,
              "  route: { service=\"%s\", "
              "method=\"%s\" }, cluster=\"%s\" }",
              route.service.c_str(), route.method.c_str(),
              route.cluster_name.c_str());
    }
  }
  auto& rds_state = state_map_[XdsApi::kRdsTypeUrl];
  auto& state =
      rds_state
          .subscribed_resources[xds_client()->lds_result_->route_config_name];
  if (state != nullptr) state->Finish();
  if (xds_client()->rds_result_ == rds_update) {
    if (GRPC_TRACE_FLAG_ENABLED(grpc_xds_client_trace)) {
      gpr_log(GPR_INFO,
              "[xds_client %p] RDS update identical to current, ignoring.",
              xds_client());
    }
    return;
  }
  xds_client()->rds_result_ = std::move(rds_update);
  RefCountedPtr<ServiceConfig> service_config;
  grpc_error* error = xds_client()->CreateServiceConfig(
      xds_client()->rds_result_.value(), &service_config);
  if (error == GRPC_ERROR_NONE) {
    xds_client()->service_config_watcher_->OnServiceConfigChanged(
        std::move(service_config));
  } else {
    xds_client()->service_config_watcher_->OnError(error);
  }
}
void XdsClient::ChannelState::AdsCallState::AcceptCdsUpdate(
    XdsApi::CdsUpdateMap cds_update_map) {
  auto& cds_state = state_map_[XdsApi::kCdsTypeUrl];
  std::set<std::string> eds_resource_names_seen;
  for (auto& p : cds_update_map) {
    const char* cluster_name = p.first.c_str();
    XdsApi::CdsUpdate& cds_update = p.second;
    auto& state = cds_state.subscribed_resources[cluster_name];
    if (state != nullptr) state->Finish();
    if (GRPC_TRACE_FLAG_ENABLED(grpc_xds_client_trace)) {
      gpr_log(GPR_INFO,
              "[xds_client %p] CDS update (cluster=%s) received: "
              "eds_service_name=%s, lrs_load_reporting_server_name=%s",
              xds_client(), cluster_name, cds_update.eds_service_name.c_str(),
              cds_update.lrs_load_reporting_server_name.has_value()
                  ? cds_update.lrs_load_reporting_server_name.value().c_str()
                  : "(N/A)");
    }
    eds_resource_names_seen.insert(cds_update.eds_service_name.empty()
                                       ? cluster_name
                                       : cds_update.eds_service_name);
    ClusterState& cluster_state = xds_client()->cluster_map_[cluster_name];
    if (cluster_state.update.has_value() &&
        cds_update.eds_service_name == cluster_state.update->eds_service_name &&
        cds_update.lrs_load_reporting_server_name ==
            cluster_state.update->lrs_load_reporting_server_name) {
      if (GRPC_TRACE_FLAG_ENABLED(grpc_xds_client_trace)) {
        gpr_log(GPR_INFO,
                "[xds_client %p] CDS update identical to current, ignoring.",
                xds_client());
      }
      continue;
    }
    cluster_state.update = std::move(cds_update);
    for (const auto& p : cluster_state.watchers) {
      p.first->OnClusterChanged(cluster_state.update.value());
    }
  }
  for (const auto& p : cds_state.subscribed_resources) {
    const std::string& cluster_name = p.first;
    if (cds_update_map.find(cluster_name) == cds_update_map.end()) {
      ClusterState& cluster_state = xds_client()->cluster_map_[cluster_name];
      cluster_state.update.reset();
      for (const auto& p : cluster_state.watchers) {
        p.first->OnError(GRPC_ERROR_CREATE_FROM_STATIC_STRING(
            "Cluster not present in CDS update"));
      }
    }
  }
  auto& eds_state = state_map_[XdsApi::kEdsTypeUrl];
  for (const auto& p : eds_state.subscribed_resources) {
    const std::string& eds_resource_name = p.first;
    if (eds_resource_names_seen.find(eds_resource_name) ==
        eds_resource_names_seen.end()) {
      EndpointState& endpoint_state =
          xds_client()->endpoint_map_[eds_resource_name];
      endpoint_state.update.reset();
      for (const auto& p : endpoint_state.watchers) {
        p.first->OnError(GRPC_ERROR_CREATE_FROM_STATIC_STRING(
            "ClusterLoadAssignment resource removed due to CDS update"));
      }
    }
  }
}
void XdsClient::ChannelState::AdsCallState::AcceptEdsUpdate(
    XdsApi::EdsUpdateMap eds_update_map) {
  auto& eds_state = state_map_[XdsApi::kEdsTypeUrl];
  for (auto& p : eds_update_map) {
    const char* eds_service_name = p.first.c_str();
    XdsApi::EdsUpdate& eds_update = p.second;
    auto& state = eds_state.subscribed_resources[eds_service_name];
    if (state != nullptr) state->Finish();
    if (GRPC_TRACE_FLAG_ENABLED(grpc_xds_client_trace)) {
      gpr_log(GPR_INFO,
              "[xds_client %p] EDS response with %" PRIuPTR
              " priorities and %" PRIuPTR
              " drop categories received (drop_all=%d)",
              xds_client(), eds_update.priority_list_update.size(),
              eds_update.drop_config->drop_category_list().size(),
              eds_update.drop_config->drop_all());
      for (size_t priority = 0;
           priority < eds_update.priority_list_update.size(); ++priority) {
        const auto* locality_map_update = eds_update.priority_list_update.Find(
            static_cast<uint32_t>(priority));
        gpr_log(GPR_INFO,
                "[xds_client %p] Priority %" PRIuPTR " contains %" PRIuPTR
                " localities",
                xds_client(), priority, locality_map_update->size());
        size_t locality_count = 0;
        for (const auto& p : locality_map_update->localities) {
          const auto& locality = p.second;
          gpr_log(GPR_INFO,
                  "[xds_client %p] Priority %" PRIuPTR ", locality %" PRIuPTR
                  " %s has weight %d, contains %" PRIuPTR " server addresses",
                  xds_client(), priority, locality_count,
                  locality.name->AsHumanReadableString(), locality.lb_weight,
                  locality.serverlist.size());
          for (size_t i = 0; i < locality.serverlist.size(); ++i) {
            char* ipport;
            grpc_sockaddr_to_string(&ipport, &locality.serverlist[i].address(),
                                    false);
            gpr_log(GPR_INFO,
                    "[xds_client %p] Priority %" PRIuPTR ", locality %" PRIuPTR
                    " %s, server address %" PRIuPTR ": %s",
                    xds_client(), priority, locality_count,
                    locality.name->AsHumanReadableString(), i, ipport);
            gpr_free(ipport);
          }
          ++locality_count;
        }
      }
      for (size_t i = 0;
           i < eds_update.drop_config->drop_category_list().size(); ++i) {
        const XdsApi::DropConfig::DropCategory& drop_category =
            eds_update.drop_config->drop_category_list()[i];
        gpr_log(GPR_INFO,
                "[xds_client %p] Drop category %s has drop rate %d per million",
                xds_client(), drop_category.name.c_str(),
                drop_category.parts_per_million);
      }
    }
    EndpointState& endpoint_state =
        xds_client()->endpoint_map_[eds_service_name];
    if (endpoint_state.update.has_value()) {
      const XdsApi::EdsUpdate& prev_update = endpoint_state.update.value();
      const bool priority_list_changed =
          prev_update.priority_list_update != eds_update.priority_list_update;
      const bool drop_config_changed =
          prev_update.drop_config == nullptr ||
          *prev_update.drop_config != *eds_update.drop_config;
      if (!priority_list_changed && !drop_config_changed) {
        if (GRPC_TRACE_FLAG_ENABLED(grpc_xds_client_trace)) {
          gpr_log(GPR_INFO,
                  "[xds_client %p] EDS update identical to current, ignoring.",
                  xds_client());
        }
        continue;
      }
    }
    endpoint_state.update = std::move(eds_update);
    for (const auto& p : endpoint_state.watchers) {
      p.first->OnEndpointChanged(endpoint_state.update.value());
    }
  }
}
void XdsClient::ChannelState::AdsCallState::OnRequestSent(void* arg,
                                                          grpc_error* error) {
  AdsCallState* ads_calld = static_cast<AdsCallState*>(arg);
  GRPC_ERROR_REF(error);
  ads_calld->xds_client()->work_serializer_->Run(
      [ads_calld, error]() { ads_calld->OnRequestSentLocked(error); },
      DEBUG_LOCATION);
}
void XdsClient::ChannelState::AdsCallState::OnRequestSentLocked(
    grpc_error* error) {
  if (IsCurrentCallOnChannel() && error == GRPC_ERROR_NONE) {
    grpc_byte_buffer_destroy(send_message_payload_);
    send_message_payload_ = nullptr;
    auto it = buffered_requests_.begin();
    if (it != buffered_requests_.end()) {
      SendMessageLocked(*it);
      buffered_requests_.erase(it);
    }
  }
  Unref(DEBUG_LOCATION, "ADS+OnRequestSentLocked");
  GRPC_ERROR_UNREF(error);
}
void XdsClient::ChannelState::AdsCallState::OnResponseReceived(
    void* arg, grpc_error* ) {
  AdsCallState* ads_calld = static_cast<AdsCallState*>(arg);
  ads_calld->xds_client()->work_serializer_->Run(
      [ads_calld]() { ads_calld->OnResponseReceivedLocked(); }, DEBUG_LOCATION);
}
void XdsClient::ChannelState::AdsCallState::OnResponseReceivedLocked() {
  if (!IsCurrentCallOnChannel() || recv_message_payload_ == nullptr) {
    Unref(DEBUG_LOCATION, "ADS+OnResponseReceivedLocked");
    return;
  }
  grpc_byte_buffer_reader bbr;
  grpc_byte_buffer_reader_init(&bbr, recv_message_payload_);
  grpc_slice response_slice = grpc_byte_buffer_reader_readall(&bbr);
  grpc_byte_buffer_reader_destroy(&bbr);
  grpc_byte_buffer_destroy(recv_message_payload_);
  recv_message_payload_ = nullptr;
  absl::optional<XdsApi::LdsUpdate> lds_update;
  absl::optional<XdsApi::RdsUpdate> rds_update;
  XdsApi::CdsUpdateMap cds_update_map;
  XdsApi::EdsUpdateMap eds_update_map;
  std::string version;
  std::string nonce;
  std::string type_url;
  grpc_error* parse_error = xds_client()->api_.ParseAdsResponse(
      response_slice, xds_client()->server_name_,
      (xds_client()->lds_result_.has_value()
           ? xds_client()->lds_result_->route_config_name
           : ""),
<<<<<<< HEAD
      ClusterNamesForRequest(), EdsServiceNamesForRequest(), &lds_update,
      &rds_update, &cds_update_map, &eds_update_map, &version, &nonce,
      &type_url);
||||||| 7f36ff039f
      ads_calld->ClusterNamesForRequest(),
      ads_calld->EdsServiceNamesForRequest(), &lds_update, &rds_update,
      &cds_update_map, &eds_update_map, &version, &nonce, &type_url);
=======
      xds_client->xds_routing_enabled_, ads_calld->ClusterNamesForRequest(),
      ads_calld->EdsServiceNamesForRequest(), &lds_update, &rds_update,
      &cds_update_map, &eds_update_map, &version, &nonce, &type_url);
>>>>>>> 006c8158
  grpc_slice_unref_internal(response_slice);
  if (type_url.empty()) {
    gpr_log(GPR_ERROR,
            "[xds_client %p] Error parsing ADS response (%s) -- ignoring",
            xds_client(), grpc_error_string(parse_error));
    GRPC_ERROR_UNREF(parse_error);
  } else {
    auto& state = state_map_[type_url];
    state.nonce = std::move(nonce);
    if (parse_error != GRPC_ERROR_NONE) {
      GRPC_ERROR_UNREF(state.error);
      state.error = parse_error;
      gpr_log(GPR_ERROR,
              "[xds_client %p] ADS response invalid for resource type %s "
              "version %s, will NACK: nonce=%s error=%s",
              xds_client(), type_url.c_str(), version.c_str(),
              state.nonce.c_str(), grpc_error_string(parse_error));
      SendMessageLocked(type_url);
    } else {
      seen_response_ = true;
      if (type_url == XdsApi::kLdsTypeUrl) {
        AcceptLdsUpdate(std::move(lds_update));
      } else if (type_url == XdsApi::kRdsTypeUrl) {
        AcceptRdsUpdate(std::move(rds_update));
      } else if (type_url == XdsApi::kCdsTypeUrl) {
        AcceptCdsUpdate(std::move(cds_update_map));
      } else if (type_url == XdsApi::kEdsTypeUrl) {
        AcceptEdsUpdate(std::move(eds_update_map));
      }
      state.version = std::move(version);
      SendMessageLocked(type_url);
      auto& lrs_call = chand()->lrs_calld_;
      if (lrs_call != nullptr) {
        LrsCallState* lrs_calld = lrs_call->calld();
        if (lrs_calld != nullptr) lrs_calld->MaybeStartReportingLocked();
      }
    }
  }
  if (xds_client()->shutting_down_) {
    Unref(DEBUG_LOCATION, "ADS+OnResponseReceivedLocked+xds_shutdown");
    return;
  }
  grpc_op op;
  memset(&op, 0, sizeof(op));
  op.op = GRPC_OP_RECV_MESSAGE;
  op.data.recv_message.recv_message = &recv_message_payload_;
  op.flags = 0;
  op.reserved = nullptr;
  GPR_ASSERT(call_ != nullptr);
  const grpc_call_error call_error =
      grpc_call_start_batch_and_execute(call_, &op, 1, &on_response_received_);
  GPR_ASSERT(GRPC_CALL_OK == call_error);
}
void XdsClient::ChannelState::AdsCallState::OnStatusReceived(
    void* arg, grpc_error* error) {
  AdsCallState* ads_calld = static_cast<AdsCallState*>(arg);
  GRPC_ERROR_REF(error);
  ads_calld->xds_client()->work_serializer_->Run(
      [ads_calld, error]() { ads_calld->OnStatusReceivedLocked(error); },
      DEBUG_LOCATION);
}
void XdsClient::ChannelState::AdsCallState::OnStatusReceivedLocked(
    grpc_error* error) {
  if (GRPC_TRACE_FLAG_ENABLED(grpc_xds_client_trace)) {
    char* status_details = grpc_slice_to_c_string(status_details_);
    gpr_log(GPR_INFO,
            "[xds_client %p] ADS call status received. Status = %d, details "
            "= '%s', (chand: %p, ads_calld: %p, call: %p), error '%s'",
            xds_client(), status_code_, status_details, chand(), this, call_,
            grpc_error_string(error));
    gpr_free(status_details);
  }
  if (IsCurrentCallOnChannel()) {
    parent_->OnCallFinishedLocked();
    xds_client()->NotifyOnError(
        GRPC_ERROR_CREATE_FROM_STATIC_STRING("xds call failed"));
  }
  Unref(DEBUG_LOCATION, "ADS+OnStatusReceivedLocked");
  GRPC_ERROR_UNREF(error);
}
bool XdsClient::ChannelState::AdsCallState::IsCurrentCallOnChannel() const {
  if (chand()->ads_calld_ == nullptr) return false;
  return this == chand()->ads_calld_->calld();
}
std::set<StringView>
XdsClient::ChannelState::AdsCallState::ClusterNamesForRequest() {
  std::set<StringView> cluster_names;
  for (auto& p : state_map_[XdsApi::kCdsTypeUrl].subscribed_resources) {
    cluster_names.insert(p.first);
    OrphanablePtr<ResourceState>& state = p.second;
    state->Start(Ref());
  }
  return cluster_names;
}
std::set<StringView>
XdsClient::ChannelState::AdsCallState::EdsServiceNamesForRequest() {
  std::set<StringView> eds_names;
  for (auto& p : state_map_[XdsApi::kEdsTypeUrl].subscribed_resources) {
    eds_names.insert(p.first);
    OrphanablePtr<ResourceState>& state = p.second;
    state->Start(Ref());
  }
  return eds_names;
}
void XdsClient::ChannelState::LrsCallState::Reporter::Orphan() {
  if (next_report_timer_callback_pending_) {
    grpc_timer_cancel(&next_report_timer_);
  }
}
void XdsClient::ChannelState::LrsCallState::Reporter::
    ScheduleNextReportLocked() {
  const grpc_millis next_report_time = ExecCtx::Get()->Now() + report_interval_;
  grpc_timer_init(&next_report_timer_, next_report_time,
                  &on_next_report_timer_);
  next_report_timer_callback_pending_ = true;
}
void XdsClient::ChannelState::LrsCallState::Reporter::OnNextReportTimer(
    void* arg, grpc_error* error) {
  Reporter* self = static_cast<Reporter*>(arg);
  GRPC_ERROR_REF(error);
  self->xds_client()->work_serializer_->Run(
      [self, error]() { self->OnNextReportTimerLocked(error); },
      DEBUG_LOCATION);
}
void XdsClient::ChannelState::LrsCallState::Reporter::OnNextReportTimerLocked(
    grpc_error* error) {
  next_report_timer_callback_pending_ = false;
  if (error != GRPC_ERROR_NONE || !IsCurrentReporterOnCall()) {
    Unref(DEBUG_LOCATION, "Reporter+timer");
  } else {
    SendReportLocked();
  }
  GRPC_ERROR_UNREF(error);
}
namespace {
bool LoadReportCountersAreZero(const XdsApi::ClusterLoadReportMap& snapshot) {
  for (const auto& p : snapshot) {
    const XdsApi::ClusterLoadReport& cluster_snapshot = p.second;
    for (const auto& q : cluster_snapshot.dropped_requests) {
      if (q.second > 0) return false;
    }
    for (const auto& q : cluster_snapshot.locality_stats) {
      const XdsClusterLocalityStats::Snapshot& locality_snapshot = q.second;
      if (!locality_snapshot.IsZero()) return false;
    }
  }
  return true;
}
}
void XdsClient::ChannelState::LrsCallState::Reporter::SendReportLocked() {
  XdsApi::ClusterLoadReportMap snapshot =
      xds_client()->BuildLoadReportSnapshot(parent_->cluster_names_);
  const bool old_val = last_report_counters_were_zero_;
  last_report_counters_were_zero_ = LoadReportCountersAreZero(snapshot);
  if (old_val && last_report_counters_were_zero_) {
    ScheduleNextReportLocked();
    return;
  }
  grpc_slice request_payload_slice =
      xds_client()->api_.CreateLrsRequest(std::move(snapshot));
  parent_->send_message_payload_ =
      grpc_raw_byte_buffer_create(&request_payload_slice, 1);
  grpc_slice_unref_internal(request_payload_slice);
  grpc_op op;
  memset(&op, 0, sizeof(op));
  op.op = GRPC_OP_SEND_MESSAGE;
  op.data.send_message.send_message = parent_->send_message_payload_;
  grpc_call_error call_error = grpc_call_start_batch_and_execute(
      parent_->call_, &op, 1, &on_report_done_);
  if (GPR_UNLIKELY(call_error != GRPC_CALL_OK)) {
    gpr_log(GPR_ERROR,
            "[xds_client %p] calld=%p call_error=%d sending client load report",
            xds_client(), this, call_error);
    GPR_ASSERT(GRPC_CALL_OK == call_error);
  }
}
void XdsClient::ChannelState::LrsCallState::Reporter::OnReportDone(
    void* arg, grpc_error* error) {
  Reporter* self = static_cast<Reporter*>(arg);
  GRPC_ERROR_REF(error);
  self->xds_client()->work_serializer_->Run(
      [self, error]() { self->OnReportDoneLocked(error); }, DEBUG_LOCATION);
}
void XdsClient::ChannelState::LrsCallState::Reporter::OnReportDoneLocked(
    grpc_error* error) {
  grpc_byte_buffer_destroy(parent_->send_message_payload_);
  parent_->send_message_payload_ = nullptr;
  if (xds_client()->load_report_map_.empty()) {
    parent_->chand()->StopLrsCall();
    Unref(DEBUG_LOCATION, "Reporter+report_done+no_more_reporters");
    return;
  }
  if (error != GRPC_ERROR_NONE || !IsCurrentReporterOnCall()) {
    if (!IsCurrentReporterOnCall()) {
      parent_->MaybeStartReportingLocked();
    }
    Unref(DEBUG_LOCATION, "Reporter+report_done");
  } else {
    ScheduleNextReportLocked();
  }
  GRPC_ERROR_UNREF(error);
}
XdsClient::ChannelState::LrsCallState::LrsCallState(
    RefCountedPtr<RetryableCall<LrsCallState>> parent)
    : InternallyRefCounted<LrsCallState>(&grpc_xds_client_trace),
      parent_(std::move(parent)) {
  GPR_ASSERT(xds_client() != nullptr);
  GPR_ASSERT(!xds_client()->server_name_.empty());
  call_ = grpc_channel_create_pollset_set_call(
      chand()->channel_, nullptr, GRPC_PROPAGATE_DEFAULTS,
      xds_client()->interested_parties_,
      GRPC_MDSTR_SLASH_ENVOY_DOT_SERVICE_DOT_LOAD_STATS_DOT_V2_DOT_LOADREPORTINGSERVICE_SLASH_STREAMLOADSTATS,
      nullptr, GRPC_MILLIS_INF_FUTURE, nullptr);
  GPR_ASSERT(call_ != nullptr);
  grpc_slice request_payload_slice =
      xds_client()->api_.CreateLrsInitialRequest(xds_client()->server_name_);
  send_message_payload_ =
      grpc_raw_byte_buffer_create(&request_payload_slice, 1);
  grpc_slice_unref_internal(request_payload_slice);
  grpc_metadata_array_init(&initial_metadata_recv_);
  grpc_metadata_array_init(&trailing_metadata_recv_);
  if (GRPC_TRACE_FLAG_ENABLED(grpc_xds_client_trace)) {
    gpr_log(GPR_INFO,
            "[xds_client %p] Starting LRS call (chand: %p, calld: %p, "
            "call: %p)",
            xds_client(), chand(), this, call_);
  }
  grpc_call_error call_error;
  grpc_op ops[3];
  memset(ops, 0, sizeof(ops));
  grpc_op* op = ops;
  op->op = GRPC_OP_SEND_INITIAL_METADATA;
  op->data.send_initial_metadata.count = 0;
  op->flags = GRPC_INITIAL_METADATA_WAIT_FOR_READY |
              GRPC_INITIAL_METADATA_WAIT_FOR_READY_EXPLICITLY_SET;
  op->reserved = nullptr;
  op++;
  GPR_ASSERT(send_message_payload_ != nullptr);
  op->op = GRPC_OP_SEND_MESSAGE;
  op->data.send_message.send_message = send_message_payload_;
  op->flags = 0;
  op->reserved = nullptr;
  op++;
  Ref(DEBUG_LOCATION, "LRS+OnInitialRequestSentLocked").release();
  GRPC_CLOSURE_INIT(&on_initial_request_sent_, OnInitialRequestSent, this,
                    grpc_schedule_on_exec_ctx);
  call_error = grpc_call_start_batch_and_execute(call_, ops, (size_t)(op - ops),
                                                 &on_initial_request_sent_);
  GPR_ASSERT(GRPC_CALL_OK == call_error);
  op = ops;
  op->op = GRPC_OP_RECV_INITIAL_METADATA;
  op->data.recv_initial_metadata.recv_initial_metadata =
      &initial_metadata_recv_;
  op->flags = 0;
  op->reserved = nullptr;
  op++;
  op->op = GRPC_OP_RECV_MESSAGE;
  op->data.recv_message.recv_message = &recv_message_payload_;
  op->flags = 0;
  op->reserved = nullptr;
  op++;
  Ref(DEBUG_LOCATION, "LRS+OnResponseReceivedLocked").release();
  GRPC_CLOSURE_INIT(&on_response_received_, OnResponseReceived, this,
                    grpc_schedule_on_exec_ctx);
  call_error = grpc_call_start_batch_and_execute(call_, ops, (size_t)(op - ops),
                                                 &on_response_received_);
  GPR_ASSERT(GRPC_CALL_OK == call_error);
  op = ops;
  op->op = GRPC_OP_RECV_STATUS_ON_CLIENT;
  op->data.recv_status_on_client.trailing_metadata = &trailing_metadata_recv_;
  op->data.recv_status_on_client.status = &status_code_;
  op->data.recv_status_on_client.status_details = &status_details_;
  op->flags = 0;
  op->reserved = nullptr;
  op++;
  GRPC_CLOSURE_INIT(&on_status_received_, OnStatusReceived, this,
                    grpc_schedule_on_exec_ctx);
  call_error = grpc_call_start_batch_and_execute(call_, ops, (size_t)(op - ops),
                                                 &on_status_received_);
  GPR_ASSERT(GRPC_CALL_OK == call_error);
}
XdsClient::ChannelState::LrsCallState::~LrsCallState() {
  grpc_metadata_array_destroy(&initial_metadata_recv_);
  grpc_metadata_array_destroy(&trailing_metadata_recv_);
  grpc_byte_buffer_destroy(send_message_payload_);
  grpc_byte_buffer_destroy(recv_message_payload_);
  grpc_slice_unref_internal(status_details_);
  GPR_ASSERT(call_ != nullptr);
  grpc_call_unref(call_);
}
void XdsClient::ChannelState::LrsCallState::Orphan() {
  reporter_.reset();
  GPR_ASSERT(call_ != nullptr);
  grpc_call_cancel(call_, nullptr);
}
void XdsClient::ChannelState::LrsCallState::MaybeStartReportingLocked() {
  if (reporter_ != nullptr) return;
  if (send_message_payload_ != nullptr) return;
  if (!seen_response()) return;
  if (chand()->ads_calld_ == nullptr ||
      chand()->ads_calld_->calld() == nullptr ||
      !chand()->ads_calld_->calld()->seen_response()) {
    return;
  }
  reporter_ = MakeOrphanable<Reporter>(
      Ref(DEBUG_LOCATION, "LRS+load_report+start"), load_reporting_interval_);
}
void XdsClient::ChannelState::LrsCallState::OnInitialRequestSent(
    void* arg, grpc_error* ) {
  LrsCallState* lrs_calld = static_cast<LrsCallState*>(arg);
  lrs_calld->xds_client()->work_serializer_->Run(
      [lrs_calld]() { lrs_calld->OnInitialRequestSentLocked(); },
      DEBUG_LOCATION);
}
void XdsClient::ChannelState::LrsCallState::OnInitialRequestSentLocked() {
  grpc_byte_buffer_destroy(send_message_payload_);
  send_message_payload_ = nullptr;
  MaybeStartReportingLocked();
  Unref(DEBUG_LOCATION, "LRS+OnInitialRequestSentLocked");
}
void XdsClient::ChannelState::LrsCallState::OnResponseReceived(
    void* arg, grpc_error* ) {
  LrsCallState* lrs_calld = static_cast<LrsCallState*>(arg);
  lrs_calld->xds_client()->work_serializer_->Run(
      [lrs_calld]() { lrs_calld->OnResponseReceivedLocked(); }, DEBUG_LOCATION);
}
void XdsClient::ChannelState::LrsCallState::OnResponseReceivedLocked() {
  if (!IsCurrentCallOnChannel() || recv_message_payload_ == nullptr) {
    Unref(DEBUG_LOCATION, "LRS+OnResponseReceivedLocked");
    return;
  }
  grpc_byte_buffer_reader bbr;
  grpc_byte_buffer_reader_init(&bbr, recv_message_payload_);
  grpc_slice response_slice = grpc_byte_buffer_reader_readall(&bbr);
  grpc_byte_buffer_reader_destroy(&bbr);
  grpc_byte_buffer_destroy(recv_message_payload_);
  recv_message_payload_ = nullptr;
  [&]() {
    std::set<std::string> new_cluster_names;
    grpc_millis new_load_reporting_interval;
    grpc_error* parse_error = xds_client()->api_.ParseLrsResponse(
        response_slice, &new_cluster_names, &new_load_reporting_interval);
    if (parse_error != GRPC_ERROR_NONE) {
      gpr_log(GPR_ERROR,
              "[xds_client %p] LRS response parsing failed. error=%s",
              xds_client(), grpc_error_string(parse_error));
      GRPC_ERROR_UNREF(parse_error);
      return;
    }
    seen_response_ = true;
    if (GRPC_TRACE_FLAG_ENABLED(grpc_xds_client_trace)) {
      gpr_log(GPR_INFO,
              "[xds_client %p] LRS response received, %" PRIuPTR
              " cluster names, load_report_interval=%" PRId64 "ms",
              xds_client(), new_cluster_names.size(),
              new_load_reporting_interval);
      size_t i = 0;
      for (const auto& name : new_cluster_names) {
        gpr_log(GPR_INFO, "[xds_client %p] cluster_name %" PRIuPTR ": %s",
                xds_client(), i++, name.c_str());
      }
    }
    if (new_load_reporting_interval <
        GRPC_XDS_MIN_CLIENT_LOAD_REPORTING_INTERVAL_MS) {
      new_load_reporting_interval =
          GRPC_XDS_MIN_CLIENT_LOAD_REPORTING_INTERVAL_MS;
      if (GRPC_TRACE_FLAG_ENABLED(grpc_xds_client_trace)) {
        gpr_log(GPR_INFO,
                "[xds_client %p] Increased load_report_interval to minimum "
                "value %dms",
                xds_client(), GRPC_XDS_MIN_CLIENT_LOAD_REPORTING_INTERVAL_MS);
      }
    }
    if (cluster_names_ == new_cluster_names &&
        load_reporting_interval_ == new_load_reporting_interval) {
      if (GRPC_TRACE_FLAG_ENABLED(grpc_xds_client_trace)) {
        gpr_log(GPR_INFO,
                "[xds_client %p] Incoming LRS response identical to current, "
                "ignoring.",
                xds_client());
      }
      return;
    }
    reporter_.reset();
    cluster_names_ = std::move(new_cluster_names);
    load_reporting_interval_ = new_load_reporting_interval;
    MaybeStartReportingLocked();
  }();
  grpc_slice_unref_internal(response_slice);
  if (xds_client()->shutting_down_) {
    Unref(DEBUG_LOCATION, "LRS+OnResponseReceivedLocked+xds_shutdown");
    return;
  }
  grpc_op op;
  memset(&op, 0, sizeof(op));
  op.op = GRPC_OP_RECV_MESSAGE;
  op.data.recv_message.recv_message = &recv_message_payload_;
  op.flags = 0;
  op.reserved = nullptr;
  GPR_ASSERT(call_ != nullptr);
  const grpc_call_error call_error =
      grpc_call_start_batch_and_execute(call_, &op, 1, &on_response_received_);
  GPR_ASSERT(GRPC_CALL_OK == call_error);
}
void XdsClient::ChannelState::LrsCallState::OnStatusReceived(
    void* arg, grpc_error* error) {
  LrsCallState* lrs_calld = static_cast<LrsCallState*>(arg);
  GRPC_ERROR_REF(error);
  lrs_calld->xds_client()->work_serializer_->Run(
      [lrs_calld, error]() { lrs_calld->OnStatusReceivedLocked(error); },
      DEBUG_LOCATION);
}
void XdsClient::ChannelState::LrsCallState::OnStatusReceivedLocked(
    grpc_error* error) {
  GPR_ASSERT(call_ != nullptr);
  if (GRPC_TRACE_FLAG_ENABLED(grpc_xds_client_trace)) {
    char* status_details = grpc_slice_to_c_string(status_details_);
    gpr_log(GPR_INFO,
            "[xds_client %p] LRS call status received. Status = %d, details "
            "= '%s', (chand: %p, calld: %p, call: %p), error '%s'",
            xds_client(), status_code_, status_details, chand(), this, call_,
            grpc_error_string(error));
    gpr_free(status_details);
  }
  if (IsCurrentCallOnChannel()) {
    GPR_ASSERT(!xds_client()->shutting_down_);
    parent_->OnCallFinishedLocked();
  }
  Unref(DEBUG_LOCATION, "LRS+OnStatusReceivedLocked");
  GRPC_ERROR_UNREF(error);
}
bool XdsClient::ChannelState::LrsCallState::IsCurrentCallOnChannel() const {
  if (chand()->lrs_calld_ == nullptr) return false;
  return this == chand()->lrs_calld_->calld();
}
namespace {
grpc_millis GetRequestTimeout(const grpc_channel_args& args) {
  return grpc_channel_args_find_integer(
      &args, GRPC_ARG_XDS_RESOURCE_DOES_NOT_EXIST_TIMEOUT_MS,
      {15000, 0, INT_MAX});
}
bool GetXdsRoutingEnabled(const grpc_channel_args& args) {
  return grpc_channel_args_find_bool(&args, GRPC_ARG_XDS_ROUTING_ENABLED,
                                     false);
}
}
XdsClient::XdsClient(std::shared_ptr<WorkSerializer> work_serializer,
                     grpc_pollset_set* interested_parties,
                     StringView server_name,
                     std::unique_ptr<ServiceConfigWatcherInterface> watcher,
                     const grpc_channel_args& channel_args, grpc_error** error)
    : InternallyRefCounted<XdsClient>(&grpc_xds_client_trace),
      request_timeout_(GetRequestTimeout(channel_args)),
<<<<<<< HEAD
      work_serializer_(std::move(work_serializer)),
||||||| 7f36ff039f
      combiner_(GRPC_COMBINER_REF(combiner, "xds_client")),
=======
      xds_routing_enabled_(GetXdsRoutingEnabled(channel_args)),
      combiner_(GRPC_COMBINER_REF(combiner, "xds_client")),
>>>>>>> 006c8158
      interested_parties_(interested_parties),
      bootstrap_(
          XdsBootstrap::ReadFromFile(this, &grpc_xds_client_trace, error)),
      api_(this, &grpc_xds_client_trace,
           bootstrap_ == nullptr ? nullptr : bootstrap_->node()),
      server_name_(server_name),
      service_config_watcher_(std::move(watcher)) {
  if (GRPC_TRACE_FLAG_ENABLED(grpc_xds_client_trace)) {
    gpr_log(GPR_INFO, "[xds_client %p] creating xds client", this);
  }
  if (*error != GRPC_ERROR_NONE) {
    gpr_log(GPR_ERROR, "[xds_client %p] failed to read bootstrap file: %s",
            this, grpc_error_string(*error));
    return;
  }
  if (GRPC_TRACE_FLAG_ENABLED(grpc_xds_client_trace)) {
    gpr_log(GPR_INFO, "[xds_client %p] creating channel to %s", this,
            bootstrap_->server().server_uri.c_str());
  }
  grpc_channel_args* new_args = BuildXdsChannelArgs(channel_args);
  grpc_channel* channel = CreateXdsChannel(*bootstrap_, *new_args, error);
  grpc_channel_args_destroy(new_args);
  if (*error != GRPC_ERROR_NONE) {
    gpr_log(GPR_ERROR, "[xds_client %p] failed to create xds channel: %s", this,
            grpc_error_string(*error));
    return;
  }
  chand_ = MakeOrphanable<ChannelState>(
      Ref(DEBUG_LOCATION, "XdsClient+ChannelState"), channel);
  if (service_config_watcher_ != nullptr) {
    chand_->Subscribe(XdsApi::kLdsTypeUrl, std::string(server_name));
  }
}
XdsClient::~XdsClient() {
  if (GRPC_TRACE_FLAG_ENABLED(grpc_xds_client_trace)) {
    gpr_log(GPR_INFO, "[xds_client %p] destroying xds client", this);
  }
}
void XdsClient::Orphan() {
  if (GRPC_TRACE_FLAG_ENABLED(grpc_xds_client_trace)) {
    gpr_log(GPR_INFO, "[xds_client %p] shutting down xds client", this);
  }
  shutting_down_ = true;
  chand_.reset();
  if (service_config_watcher_ != nullptr) {
    cluster_map_.clear();
    endpoint_map_.clear();
  }
  Unref(DEBUG_LOCATION, "XdsClient::Orphan()");
}
void XdsClient::WatchClusterData(
    StringView cluster_name, std::unique_ptr<ClusterWatcherInterface> watcher) {
  std::string cluster_name_str = std::string(cluster_name);
  ClusterState& cluster_state = cluster_map_[cluster_name_str];
  ClusterWatcherInterface* w = watcher.get();
  cluster_state.watchers[w] = std::move(watcher);
  if (cluster_state.update.has_value()) {
    if (GRPC_TRACE_FLAG_ENABLED(grpc_xds_client_trace)) {
      gpr_log(GPR_INFO, "[xds_client %p] returning cached cluster data for %s",
              this, StringViewToCString(cluster_name).get());
    }
    w->OnClusterChanged(cluster_state.update.value());
  }
  chand_->Subscribe(XdsApi::kCdsTypeUrl, cluster_name_str);
}
void XdsClient::CancelClusterDataWatch(StringView cluster_name,
                                       ClusterWatcherInterface* watcher,
                                       bool delay_unsubscription) {
  if (shutting_down_) return;
  std::string cluster_name_str = std::string(cluster_name);
  ClusterState& cluster_state = cluster_map_[cluster_name_str];
  auto it = cluster_state.watchers.find(watcher);
  if (it != cluster_state.watchers.end()) {
    cluster_state.watchers.erase(it);
    if (cluster_state.watchers.empty()) {
      cluster_map_.erase(cluster_name_str);
      chand_->Unsubscribe(XdsApi::kCdsTypeUrl, cluster_name_str,
                          delay_unsubscription);
    }
  }
}
void XdsClient::WatchEndpointData(
    StringView eds_service_name,
    std::unique_ptr<EndpointWatcherInterface> watcher) {
  std::string eds_service_name_str = std::string(eds_service_name);
  EndpointState& endpoint_state = endpoint_map_[eds_service_name_str];
  EndpointWatcherInterface* w = watcher.get();
  endpoint_state.watchers[w] = std::move(watcher);
  if (endpoint_state.update.has_value()) {
    if (GRPC_TRACE_FLAG_ENABLED(grpc_xds_client_trace)) {
      gpr_log(GPR_INFO, "[xds_client %p] returning cached endpoint data for %s",
              this, StringViewToCString(eds_service_name).get());
    }
    w->OnEndpointChanged(endpoint_state.update.value());
  }
  chand_->Subscribe(XdsApi::kEdsTypeUrl, eds_service_name_str);
}
void XdsClient::CancelEndpointDataWatch(StringView eds_service_name,
                                        EndpointWatcherInterface* watcher,
                                        bool delay_unsubscription) {
  if (shutting_down_) return;
  std::string eds_service_name_str = std::string(eds_service_name);
  EndpointState& endpoint_state = endpoint_map_[eds_service_name_str];
  auto it = endpoint_state.watchers.find(watcher);
  if (it != endpoint_state.watchers.end()) {
    endpoint_state.watchers.erase(it);
    if (endpoint_state.watchers.empty()) {
      endpoint_map_.erase(eds_service_name_str);
      chand_->Unsubscribe(XdsApi::kEdsTypeUrl, eds_service_name_str,
                          delay_unsubscription);
    }
  }
}
RefCountedPtr<XdsClusterDropStats> XdsClient::AddClusterDropStats(
    StringView lrs_server, StringView cluster_name,
    StringView eds_service_name) {
  auto key =
      std::make_pair(std::string(cluster_name), std::string(eds_service_name));
  auto it = load_report_map_
                .emplace(std::make_pair(std::move(key), LoadReportState()))
                .first;
  auto cluster_drop_stats = MakeRefCounted<XdsClusterDropStats>(
      Ref(DEBUG_LOCATION, "DropStats"), lrs_server,
      it->first.first , it->first.second );
  it->second.drop_stats.insert(cluster_drop_stats.get());
  chand_->MaybeStartLrsCall();
  return cluster_drop_stats;
}
void XdsClient::RemoveClusterDropStats(
    StringView , StringView cluster_name,
    StringView eds_service_name, XdsClusterDropStats* cluster_drop_stats) {
  auto load_report_it = load_report_map_.find(
      std::make_pair(std::string(cluster_name), std::string(eds_service_name)));
  if (load_report_it == load_report_map_.end()) return;
  LoadReportState& load_report_state = load_report_it->second;
  auto it = load_report_state.drop_stats.find(cluster_drop_stats);
  if (it != load_report_state.drop_stats.end()) {
    for (const auto& p : cluster_drop_stats->GetSnapshotAndReset()) {
      load_report_state.deleted_drop_stats[p.first] += p.second;
    }
    load_report_state.drop_stats.erase(it);
  }
}
RefCountedPtr<XdsClusterLocalityStats> XdsClient::AddClusterLocalityStats(
    StringView lrs_server, StringView cluster_name, StringView eds_service_name,
    RefCountedPtr<XdsLocalityName> locality) {
  auto key =
      std::make_pair(std::string(cluster_name), std::string(eds_service_name));
  auto it = load_report_map_
                .emplace(std::make_pair(std::move(key), LoadReportState()))
                .first;
  auto cluster_locality_stats = MakeRefCounted<XdsClusterLocalityStats>(
      Ref(DEBUG_LOCATION, "LocalityStats"), lrs_server,
      it->first.first , it->first.second ,
      locality);
  it->second.locality_stats[std::move(locality)].locality_stats.insert(
      cluster_locality_stats.get());
  chand_->MaybeStartLrsCall();
  return cluster_locality_stats;
}
void XdsClient::RemoveClusterLocalityStats(
    StringView , StringView cluster_name,
    StringView eds_service_name, const RefCountedPtr<XdsLocalityName>& locality,
    XdsClusterLocalityStats* cluster_locality_stats) {
  auto load_report_it = load_report_map_.find(
      std::make_pair(std::string(cluster_name), std::string(eds_service_name)));
  if (load_report_it == load_report_map_.end()) return;
  LoadReportState& load_report_state = load_report_it->second;
  auto locality_it = load_report_state.locality_stats.find(locality);
  if (locality_it == load_report_state.locality_stats.end()) return;
  auto& locality_set = locality_it->second.locality_stats;
  auto it = locality_set.find(cluster_locality_stats);
  if (it != locality_set.end()) {
    locality_it->second.deleted_locality_stats.emplace_back(
        cluster_locality_stats->GetSnapshotAndReset());
    locality_set.erase(it);
  }
}
void XdsClient::ResetBackoff() {
  if (chand_ != nullptr) {
    grpc_channel_reset_connect_backoff(chand_->channel());
  }
}
namespace {
std::string CreateServiceConfigActionCluster(const std::string& cluster_name) {
  return absl::StrFormat(
      "      \"cds:%s\":{\n"
      "        \"child_policy\":[ {\n"
      "          \"cds_experimental\":{\n"
      "            \"cluster\": \"%s\"\n"
      "          }\n"
      "        } ]\n"
      "       }",
      cluster_name.c_str(), cluster_name.c_str());
}
std::string CreateServiceConfigRoute(const std::string& cluster_name,
                                     const std::string& service,
                                     const std::string& method) {
  return absl::StrFormat(
      "      { \n"
      "         \"methodName\": {\n"
      "           \"service\": \"%s\",\n"
      "           \"method\": \"%s\"\n"
      "        },\n"
      "        \"action\": \"cds:%s\"\n"
      "      }",
      service.c_str(), method.c_str(), cluster_name.c_str());
}
}
grpc_error* XdsClient::CreateServiceConfig(
    const XdsApi::RdsUpdate& rds_update,
    RefCountedPtr<ServiceConfig>* service_config) const {
  std::vector<std::string> config_parts;
  config_parts.push_back(
      "{\n"
      "  \"loadBalancingConfig\":[\n"
      "    { \"xds_routing_experimental\":{\n"
      "      \"actions\":{\n");
  std::vector<std::string> actions_vector;
  for (size_t i = 0; i < rds_update.routes.size(); ++i) {
    auto route = rds_update.routes[i];
    actions_vector.push_back(
        CreateServiceConfigActionCluster(route.cluster_name.c_str()));
  }
  config_parts.push_back(absl::StrJoin(actions_vector, ",\n"));
  config_parts.push_back(
      "    },\n"
      "      \"routes\":[\n");
  std::vector<std::string> routes_vector;
  for (size_t i = 0; i < rds_update.routes.size(); ++i) {
    auto route_info = rds_update.routes[i];
    routes_vector.push_back(CreateServiceConfigRoute(
        route_info.cluster_name.c_str(), route_info.service.c_str(),
        route_info.method.c_str()));
  }
  config_parts.push_back(absl::StrJoin(routes_vector, ",\n"));
  config_parts.push_back(
      "    ]\n"
      "    } }\n"
      "  ]\n"
      "}");
  std::string json = absl::StrJoin(config_parts, "");
  grpc_error* error = GRPC_ERROR_NONE;
  *service_config = ServiceConfig::Create(json.c_str(), &error);
  return error;
}
XdsApi::ClusterLoadReportMap XdsClient::BuildLoadReportSnapshot(
    const std::set<std::string>& clusters) {
  XdsApi::ClusterLoadReportMap snapshot_map;
  for (auto load_report_it = load_report_map_.begin();
       load_report_it != load_report_map_.end();) {
    const auto& cluster_key = load_report_it->first;
    LoadReportState& load_report = load_report_it->second;
    const bool record_stats =
        clusters.find(cluster_key.first) != clusters.end();
    XdsApi::ClusterLoadReport snapshot;
    snapshot.dropped_requests = std::move(load_report.deleted_drop_stats);
    for (auto& drop_stats : load_report.drop_stats) {
      for (const auto& p : drop_stats->GetSnapshotAndReset()) {
        snapshot.dropped_requests[p.first] += p.second;
      }
    }
    for (auto it = load_report.locality_stats.begin();
         it != load_report.locality_stats.end();) {
      const RefCountedPtr<XdsLocalityName>& locality_name = it->first;
      auto& locality_state = it->second;
      XdsClusterLocalityStats::Snapshot& locality_snapshot =
          snapshot.locality_stats[locality_name];
      for (auto& locality_stats : locality_state.locality_stats) {
        locality_snapshot += locality_stats->GetSnapshotAndReset();
      }
      for (auto& deleted_locality_stats :
           locality_state.deleted_locality_stats) {
        locality_snapshot += deleted_locality_stats;
      }
      locality_state.deleted_locality_stats.clear();
      if (locality_state.locality_stats.empty()) {
        it = load_report.locality_stats.erase(it);
      } else {
        ++it;
      }
    }
    if (record_stats) {
      const grpc_millis now = ExecCtx::Get()->Now();
      snapshot.load_report_interval = now - load_report.last_report_time;
      load_report.last_report_time = now;
      snapshot_map[cluster_key] = std::move(snapshot);
    }
    if (load_report.locality_stats.empty() && load_report.drop_stats.empty()) {
      load_report_it = load_report_map_.erase(load_report_it);
    } else {
      ++load_report_it;
    }
  }
  return snapshot_map;
}
void XdsClient::NotifyOnError(grpc_error* error) {
  if (service_config_watcher_ != nullptr) {
    service_config_watcher_->OnError(GRPC_ERROR_REF(error));
  }
  for (const auto& p : cluster_map_) {
    const ClusterState& cluster_state = p.second;
    for (const auto& p : cluster_state.watchers) {
      p.first->OnError(GRPC_ERROR_REF(error));
    }
  }
  for (const auto& p : endpoint_map_) {
    const EndpointState& endpoint_state = p.second;
    for (const auto& p : endpoint_state.watchers) {
      p.first->OnError(GRPC_ERROR_REF(error));
    }
  }
  GRPC_ERROR_UNREF(error);
}
void* XdsClient::ChannelArgCopy(void* p) {
  XdsClient* xds_client = static_cast<XdsClient*>(p);
  xds_client->Ref(DEBUG_LOCATION, "channel arg").release();
  return p;
}
void XdsClient::ChannelArgDestroy(void* p) {
  XdsClient* xds_client = static_cast<XdsClient*>(p);
  xds_client->Unref(DEBUG_LOCATION, "channel arg");
}
int XdsClient::ChannelArgCmp(void* p, void* q) { return GPR_ICMP(p, q); }
const grpc_arg_pointer_vtable XdsClient::kXdsClientVtable = {
    XdsClient::ChannelArgCopy, XdsClient::ChannelArgDestroy,
    XdsClient::ChannelArgCmp};
grpc_arg XdsClient::MakeChannelArg() const {
  return grpc_channel_arg_pointer_create(const_cast<char*>(GRPC_ARG_XDS_CLIENT),
                                         const_cast<XdsClient*>(this),
                                         &XdsClient::kXdsClientVtable);
}
RefCountedPtr<XdsClient> XdsClient::GetFromChannelArgs(
    const grpc_channel_args& args) {
  XdsClient* xds_client =
      grpc_channel_args_find_pointer<XdsClient>(&args, GRPC_ARG_XDS_CLIENT);
  if (xds_client != nullptr) return xds_client->Ref();
  return nullptr;
}
}
