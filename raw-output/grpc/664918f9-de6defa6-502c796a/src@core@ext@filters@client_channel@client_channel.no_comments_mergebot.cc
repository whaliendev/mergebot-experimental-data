#include <grpc/support/port_platform.h>
#include "src/core/ext/filters/client_channel/client_channel.h"
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <set>
#include "absl/strings/string_view.h"
#include <grpc/support/alloc.h>
#include <grpc/support/log.h>
#include <grpc/support/string_util.h>
#include <grpc/support/sync.h>
#include "absl/container/inlined_vector.h"
#include "absl/types/optional.h"
#include "src/core/ext/filters/client_channel/backend_metric.h"
#include "src/core/ext/filters/client_channel/backup_poller.h"
#include "src/core/ext/filters/client_channel/config_selector.h"
#include "src/core/ext/filters/client_channel/global_subchannel_pool.h"
#include "src/core/ext/filters/client_channel/http_connect_handshaker.h"
#include "src/core/ext/filters/client_channel/lb_policy_registry.h"
#include "src/core/ext/filters/client_channel/local_subchannel_pool.h"
#include "src/core/ext/filters/client_channel/proxy_mapper_registry.h"
#include "src/core/ext/filters/client_channel/resolver_registry.h"
#include "src/core/ext/filters/client_channel/resolver_result_parsing.h"
#include "src/core/ext/filters/client_channel/resolving_lb_policy.h"
#include "src/core/ext/filters/client_channel/retry_throttle.h"
#include "src/core/ext/filters/client_channel/service_config.h"
#include "src/core/ext/filters/client_channel/service_config_call_data.h"
#include "src/core/ext/filters/client_channel/subchannel.h"
#include "src/core/ext/filters/deadline/deadline_filter.h"
#include "src/core/lib/backoff/backoff.h"
#include "src/core/lib/channel/channel_args.h"
#include "src/core/lib/channel/connected_channel.h"
#include "src/core/lib/channel/status_util.h"
#include "src/core/lib/gpr/string.h"
#include "src/core/lib/gprpp/manual_constructor.h"
#include "src/core/lib/gprpp/map.h"
#include "src/core/lib/gprpp/sync.h"
#include "src/core/lib/iomgr/iomgr.h"
#include "src/core/lib/iomgr/polling_entity.h"
#include "src/core/lib/iomgr/work_serializer.h"
#include "src/core/lib/profiling/timers.h"
#include "src/core/lib/slice/slice_internal.h"
#include "src/core/lib/slice/slice_string_helpers.h"
#include "src/core/lib/surface/channel.h"
#include "src/core/lib/transport/connectivity_state.h"
#include "src/core/lib/transport/error_utils.h"
#include "src/core/lib/transport/metadata.h"
#include "src/core/lib/transport/metadata_batch.h"
#include "src/core/lib/transport/static_metadata.h"
#include "src/core/lib/transport/status_metadata.h"
using grpc_core::internal::ClientChannelMethodParsedConfig;
using grpc_core::internal::ServerRetryThrottleData;
#define DEFAULT_PER_RPC_RETRY_BUFFER_SIZE (256 << 10)
#define RETRY_BACKOFF_JITTER 0.2
#define MAX_PENDING_BATCHES 6
namespace grpc_core {
TraceFlag grpc_client_channel_call_trace(false, "client_channel_call");
TraceFlag grpc_client_channel_routing_trace(false, "client_channel_routing");
namespace {
class ChannelData {
public:
  struct QueuedPick {
    grpc_call_element* elem;
    QueuedPick* next = nullptr;
  };
  static grpc_error* Init(grpc_channel_element* elem,
                          grpc_channel_element_args* args);
  static void Destroy(grpc_channel_element* elem);
  static void StartTransportOp(grpc_channel_element* elem,
                               grpc_transport_op* op);
  static void GetChannelInfo(grpc_channel_element* elem,
                             const grpc_channel_info* info);
  bool deadline_checking_enabled() const { return deadline_checking_enabled_; }
  bool enable_retries() const { return enable_retries_; }
  size_t per_rpc_retry_buffer_size() const {
    return per_rpc_retry_buffer_size_;
  }
  grpc_channel_stack* owning_stack() const { return owning_stack_; }
  grpc_error* disconnect_error() const {
    return disconnect_error_.Load(MemoryOrder::ACQUIRE);
  }
  Mutex* data_plane_mu() const { return &data_plane_mu_; }
  LoadBalancingPolicy::SubchannelPicker* picker() const {
    return picker_.get();
  }
  void AddQueuedPick(QueuedPick* pick, grpc_polling_entity* pollent);
  void RemoveQueuedPick(QueuedPick* to_remove, grpc_polling_entity* pollent);
  bool received_service_config_data() const {
    return received_service_config_data_;
  }
  grpc_error* resolver_transient_failure_error() const {
    return resolver_transient_failure_error_;
  }
  RefCountedPtr<ServerRetryThrottleData> retry_throttle_data() const {
    return retry_throttle_data_;
  }
  RefCountedPtr<ServiceConfig> service_config() const {
    return service_config_;
  }
  ConfigSelector* config_selector() const { return config_selector_.get(); }
  WorkSerializer* work_serializer() const { return work_serializer_.get(); }
  RefCountedPtr<ConnectedSubchannel> GetConnectedSubchannelInDataPlane(
      SubchannelInterface* subchannel) const;
  grpc_connectivity_state CheckConnectivityState(bool try_to_connect);
  void AddExternalConnectivityWatcher(grpc_polling_entity pollent,
                                      grpc_connectivity_state* state,
                                      grpc_closure* on_complete,
                                      grpc_closure* watcher_timer_init) {
    new ExternalConnectivityWatcher(this, pollent, state, on_complete,
                                    watcher_timer_init);
  }
  void RemoveExternalConnectivityWatcher(grpc_closure* on_complete,
                                         bool cancel) {
<<<<<<< HEAD
    ExternalConnectivityWatcher::RemoveWatcherFromExternalWatchersMap(
        this, on_complete, cancel);
||||||| 502c796a94
    MutexLock lock(&external_watchers_mu_);
    auto it = external_watchers_.find(on_complete);
    if (it != external_watchers_.end()) {
      if (cancel) it->second->Cancel();
      external_watchers_.erase(it);
    }
=======
    ExternalConnectivityWatcher* watcher = nullptr;
    {
      MutexLock lock(&external_watchers_mu_);
      auto it = external_watchers_.find(on_complete);
      if (it != external_watchers_.end()) {
        watcher = it->second;
        external_watchers_.erase(it);
      }
>>>>>>> de6defa6
  } if (watcher != nullptr && cancel) watcher->Cancel();
  }
  int NumExternalConnectivityWatchers() const {
    MutexLock lock(&external_watchers_mu_);
    return static_cast<int>(external_watchers_.size());
  }
  void AddConnectivityWatcher(
      grpc_connectivity_state initial_state,
      OrphanablePtr<AsyncConnectivityStateWatcherInterface> watcher);
  void RemoveConnectivityWatcher(
      AsyncConnectivityStateWatcherInterface* watcher);
private:
  class SubchannelWrapper;
  class ClientChannelControlHelper;
  class ConnectivityWatcherAdder;
  class ConnectivityWatcherRemover;
  class ExternalConnectivityWatcher : public ConnectivityStateWatcherInterface {
   public:
    ExternalConnectivityWatcher(ChannelData* chand, grpc_polling_entity pollent,
                                grpc_connectivity_state* state,
                                grpc_closure* on_complete,
                                grpc_closure* watcher_timer_init);
    ~ExternalConnectivityWatcher();
    static void RemoveWatcherFromExternalWatchersMap(ChannelData* chand,
                                                     grpc_closure* on_complete,
                                                     bool cancel);
    void Notify(grpc_connectivity_state state) override;
    void Cancel();
   private:
    void AddWatcherLocked();
    void RemoveWatcherLocked();
    ChannelData* chand_;
    grpc_polling_entity pollent_;
    grpc_connectivity_state initial_state_;
    grpc_connectivity_state* state_;
    grpc_closure* on_complete_;
    grpc_closure* watcher_timer_init_;
    Atomic<bool> done_{false};
  };
  class ChannelConfigHelper
      : public ResolvingLoadBalancingPolicy::ChannelConfigHelper {
   public:
    explicit ChannelConfigHelper(ChannelData* chand) : chand_(chand) {}
    ApplyServiceConfigResult ApplyServiceConfig(
        const Resolver::Result& result) override;
    void ApplyConfigSelector(
        bool service_config_changed,
        RefCountedPtr<ConfigSelector> config_selector) override;
    void ResolverTransientFailure(grpc_error* error) override;
   private:
    static void ProcessLbPolicy(
        const Resolver::Result& resolver_result,
        const internal::ClientChannelGlobalParsedConfig* parsed_service_config,
        RefCountedPtr<LoadBalancingPolicy::Config>* lb_policy_config);
    ChannelData* chand_;
  };
  ChannelData(grpc_channel_element_args* args, grpc_error** error);
  ~ChannelData();
  void UpdateStateAndPickerLocked(
      grpc_connectivity_state state, const char* reason,
      std::unique_ptr<LoadBalancingPolicy::SubchannelPicker> picker);
  void UpdateServiceConfigInDataPlaneLocked(
      bool service_config_changed,
      RefCountedPtr<ConfigSelector> config_selector);
  void CreateResolvingLoadBalancingPolicyLocked();
  void DestroyResolvingLoadBalancingPolicyLocked();
  grpc_error* DoPingLocked(grpc_transport_op* op);
  void StartTransportOpLocked(grpc_transport_op* op);
  void TryToConnectLocked();
  const bool deadline_checking_enabled_;
  const bool enable_retries_;
  const size_t per_rpc_retry_buffer_size_;
  grpc_channel_stack* owning_stack_;
  ClientChannelFactory* client_channel_factory_;
  const grpc_channel_args* channel_args_;
  RefCountedPtr<ServiceConfig> default_service_config_;
  grpc_core::UniquePtr<char> server_name_;
  grpc_core::UniquePtr<char> target_uri_;
  channelz::ChannelNode* channelz_node_;
  ChannelConfigHelper channel_config_helper_;
  mutable Mutex data_plane_mu_;
  std::unique_ptr<LoadBalancingPolicy::SubchannelPicker> picker_;
QueuedPick* queued_picks_ = nullptr; grpc_error* resolver_transient_failure_error_ = GRPC_ERROR_NONE;
  bool received_service_config_data_ = false;
  RefCountedPtr<ServerRetryThrottleData> retry_throttle_data_;
  RefCountedPtr<ServiceConfig> service_config_;
  RefCountedPtr<ConfigSelector> config_selector_;
  std::shared_ptr<WorkSerializer> work_serializer_;
  grpc_pollset_set* interested_parties_;
  RefCountedPtr<SubchannelPoolInterface> subchannel_pool_;
  OrphanablePtr<ResolvingLoadBalancingPolicy> resolving_lb_policy_;
  ConnectivityStateTracker state_tracker_;
  grpc_core::UniquePtr<char> health_check_service_name_;
  RefCountedPtr<ServiceConfig> saved_service_config_;
  RefCountedPtr<ConfigSelector> saved_config_selector_;
  bool received_first_resolver_result_ = false;
  std::map<Subchannel*, int> subchannel_refcount_map_;
  std::set<SubchannelWrapper*> subchannel_wrappers_;
  std::map<RefCountedPtr<SubchannelWrapper>, RefCountedPtr<ConnectedSubchannel>>
      pending_subchannel_updates_;
  Atomic<grpc_error*> disconnect_error_;
  gpr_mu info_mu_;
  grpc_core::UniquePtr<char> info_lb_policy_name_;
  grpc_core::UniquePtr<char> info_service_config_json_;
  mutable Mutex external_watchers_mu_;
  std::map<grpc_closure*, RefCountedPtr<ExternalConnectivityWatcher>>
      external_watchers_;
};
class CallData {
public:
  static grpc_error* Init(grpc_call_element* elem,
                          const grpc_call_element_args* args);
  static void Destroy(grpc_call_element* elem,
                      const grpc_call_final_info* final_info,
                      grpc_closure* then_schedule_closure);
  static void StartTransportStreamOpBatch(
      grpc_call_element* elem, grpc_transport_stream_op_batch* batch);
  static void SetPollent(grpc_call_element* elem, grpc_polling_entity* pollent);
  RefCountedPtr<SubchannelCall> subchannel_call() { return subchannel_call_; }
  static void PickSubchannel(void* arg, grpc_error* error);
  bool PickSubchannelLocked(grpc_call_element* elem, grpc_error** error);
  void AsyncPickDone(grpc_call_element* elem, grpc_error* error);
private:
  class QueuedPickCanceller;
  class Metadata : public LoadBalancingPolicy::MetadataInterface {
   public:
    Metadata(CallData* calld, grpc_metadata_batch* batch)
        : calld_(calld), batch_(batch) {}
    void Add(absl::string_view key, absl::string_view value) override {
      grpc_linked_mdelem* linked_mdelem = static_cast<grpc_linked_mdelem*>(
          calld_->arena_->Alloc(sizeof(grpc_linked_mdelem)));
      linked_mdelem->md = grpc_mdelem_from_slices(
          grpc_core::ExternallyManagedSlice(key.data(), key.size()),
          grpc_core::ExternallyManagedSlice(value.data(), value.size()));
      GPR_ASSERT(grpc_metadata_batch_link_tail(batch_, linked_mdelem) ==
                 GRPC_ERROR_NONE);
    }
    iterator begin() const override {
      static_assert(sizeof(grpc_linked_mdelem*) <= sizeof(intptr_t),
                    "iterator size too large");
      return iterator(
          this, reinterpret_cast<intptr_t>(MaybeSkipEntry(batch_->list.head)));
    }
    iterator end() const override {
      static_assert(sizeof(grpc_linked_mdelem*) <= sizeof(intptr_t),
                    "iterator size too large");
      return iterator(this, 0);
    }
    iterator erase(iterator it) override {
      grpc_linked_mdelem* linked_mdelem =
          reinterpret_cast<grpc_linked_mdelem*>(GetIteratorHandle(it));
      intptr_t handle = reinterpret_cast<intptr_t>(linked_mdelem->next);
      grpc_metadata_batch_remove(batch_, linked_mdelem);
      return iterator(this, handle);
    }
   private:
    grpc_linked_mdelem* MaybeSkipEntry(grpc_linked_mdelem* entry) const {
      if (entry != nullptr && batch_->idx.named.path == entry) {
        return entry->next;
      }
      return entry;
    }
    intptr_t IteratorHandleNext(intptr_t handle) const override {
      grpc_linked_mdelem* linked_mdelem =
          reinterpret_cast<grpc_linked_mdelem*>(handle);
      return reinterpret_cast<intptr_t>(MaybeSkipEntry(linked_mdelem->next));
    }
    std::pair<absl::string_view, absl::string_view> IteratorHandleGet(
        intptr_t handle) const override {
      grpc_linked_mdelem* linked_mdelem =
          reinterpret_cast<grpc_linked_mdelem*>(handle);
      return std::make_pair(
          StringViewFromSlice(GRPC_MDKEY(linked_mdelem->md)),
          StringViewFromSlice(GRPC_MDVALUE(linked_mdelem->md)));
    }
    CallData* calld_;
    grpc_metadata_batch* batch_;
  };
  class LbCallState : public LoadBalancingPolicy::CallState {
   public:
    explicit LbCallState(CallData* calld) : calld_(calld) {}
    void* Alloc(size_t size) override { return calld_->arena_->Alloc(size); }
    const LoadBalancingPolicy::BackendMetricData* GetBackendMetricData()
        override {
      if (calld_->backend_metric_data_ == nullptr) {
        grpc_linked_mdelem* md = calld_->recv_trailing_metadata_->idx.named
                                     .x_endpoint_load_metrics_bin;
        if (md != nullptr) {
          calld_->backend_metric_data_ =
              ParseBackendMetricData(GRPC_MDVALUE(md->md), calld_->arena_);
        }
      }
      return calld_->backend_metric_data_;
    }
    absl::string_view ExperimentalGetCallAttribute(const char* key) override {
      auto it = calld_->call_attributes_.find(key);
      if (it == calld_->call_attributes_.end()) return absl::string_view();
      return it->second;
    }
   private:
    CallData* calld_;
  };
  struct SubchannelCallBatchData {
    static SubchannelCallBatchData* Create(grpc_call_element* elem,
                                           int refcount, bool set_on_complete);
    void Unref() {
      if (gpr_unref(&refs)) Destroy();
    }
    SubchannelCallBatchData(grpc_call_element* elem, CallData* calld,
                            int refcount, bool set_on_complete);
    ~SubchannelCallBatchData() { Destroy(); }
    void Destroy();
    gpr_refcount refs;
    grpc_call_element* elem;
    RefCountedPtr<SubchannelCall> subchannel_call;
    grpc_transport_stream_op_batch batch;
    grpc_closure on_complete;
  };
  struct SubchannelCallRetryState {
    explicit SubchannelCallRetryState(grpc_call_context_element* context)
        : batch_payload(context),
          started_send_initial_metadata(false),
          completed_send_initial_metadata(false),
          started_send_trailing_metadata(false),
          completed_send_trailing_metadata(false),
          started_recv_initial_metadata(false),
          completed_recv_initial_metadata(false),
          started_recv_trailing_metadata(false),
          completed_recv_trailing_metadata(false),
          retry_dispatched(false) {}
    grpc_transport_stream_op_batch_payload batch_payload;
    grpc_linked_mdelem* send_initial_metadata_storage;
    grpc_metadata_batch send_initial_metadata;
    ManualConstructor<ByteStreamCache::CachingByteStream> send_message;
    grpc_linked_mdelem* send_trailing_metadata_storage;
    grpc_metadata_batch send_trailing_metadata;
    grpc_metadata_batch recv_initial_metadata;
    grpc_closure recv_initial_metadata_ready;
    bool trailing_metadata_available = false;
    grpc_closure recv_message_ready;
    OrphanablePtr<ByteStream> recv_message;
    grpc_metadata_batch recv_trailing_metadata;
    grpc_transport_stream_stats collect_stats;
    grpc_closure recv_trailing_metadata_ready;
    size_t started_send_message_count = 0;
    size_t completed_send_message_count = 0;
    size_t started_recv_message_count = 0;
    size_t completed_recv_message_count = 0;
    bool started_send_initial_metadata : 1;
    bool completed_send_initial_metadata : 1;
    bool started_send_trailing_metadata : 1;
    bool completed_send_trailing_metadata : 1;
    bool started_recv_initial_metadata : 1;
    bool completed_recv_initial_metadata : 1;
    bool started_recv_trailing_metadata : 1;
    bool completed_recv_trailing_metadata : 1;
    SubchannelCallBatchData* recv_initial_metadata_ready_deferred_batch =
        nullptr;
    grpc_error* recv_initial_metadata_error = GRPC_ERROR_NONE;
    SubchannelCallBatchData* recv_message_ready_deferred_batch = nullptr;
    grpc_error* recv_message_error = GRPC_ERROR_NONE;
    SubchannelCallBatchData* recv_trailing_metadata_internal_batch = nullptr;
    bool retry_dispatched : 1;
  };
  struct PendingBatch {
    grpc_transport_stream_op_batch* batch;
    bool send_ops_cached;
  };
  CallData(grpc_call_element* elem, const ChannelData& chand,
           const grpc_call_element_args& args);
  ~CallData();
  void MaybeCacheSendOpsForBatch(PendingBatch* pending);
  void FreeCachedSendInitialMetadata(ChannelData* chand);
  void FreeCachedSendMessage(ChannelData* chand, size_t idx);
  void FreeCachedSendTrailingMetadata(ChannelData* chand);
  void FreeCachedSendOpDataAfterCommit(grpc_call_element* elem,
                                       SubchannelCallRetryState* retry_state);
  void FreeCachedSendOpDataForCompletedBatch(
      grpc_call_element* elem, SubchannelCallBatchData* batch_data,
      SubchannelCallRetryState* retry_state);
  static void RecvTrailingMetadataReadyForLoadBalancingPolicy(
      void* arg, grpc_error* error);
  void MaybeInjectRecvTrailingMetadataReadyForLoadBalancingPolicy(
      grpc_transport_stream_op_batch* batch);
  static size_t GetBatchIndex(grpc_transport_stream_op_batch* batch);
  void PendingBatchesAdd(grpc_call_element* elem,
                         grpc_transport_stream_op_batch* batch);
  void PendingBatchClear(PendingBatch* pending);
  void MaybeClearPendingBatch(grpc_call_element* elem, PendingBatch* pending);
  static void FailPendingBatchInCallCombiner(void* arg, grpc_error* error);
  typedef bool (*YieldCallCombinerPredicate)(
      const CallCombinerClosureList& closures);
  static bool YieldCallCombiner(const CallCombinerClosureList& ) {
    return true;
  }
  static bool NoYieldCallCombiner(const CallCombinerClosureList& ) {
    return false;
  }
  static bool YieldCallCombinerIfPendingBatchesFound(
      const CallCombinerClosureList& closures) {
    return closures.size() > 0;
  }
  void PendingBatchesFail(
      grpc_call_element* elem, grpc_error* error,
      YieldCallCombinerPredicate yield_call_combiner_predicate);
  static void ResumePendingBatchInCallCombiner(void* arg, grpc_error* ignored);
  void PendingBatchesResume(grpc_call_element* elem);
  void RetryCommit(grpc_call_element* elem,
                   SubchannelCallRetryState* retry_state);
  void DoRetry(grpc_call_element* elem, SubchannelCallRetryState* retry_state,
               grpc_millis server_pushback_ms);
  bool MaybeRetry(grpc_call_element* elem, SubchannelCallBatchData* batch_data,
                  grpc_status_code status, grpc_mdelem* server_pushback_md);
  static void InvokeRecvInitialMetadataCallback(void* arg, grpc_error* error);
  static void RecvInitialMetadataReady(void* arg, grpc_error* error);
  static void InvokeRecvMessageCallback(void* arg, grpc_error* error);
  static void RecvMessageReady(void* arg, grpc_error* error);
  void GetCallStatus(grpc_metadata_batch* md_batch, grpc_error* error,
                     grpc_status_code* status,
                     grpc_mdelem** server_pushback_md);
  void AddClosureForRecvTrailingMetadataReady(
      grpc_call_element* elem, SubchannelCallBatchData* batch_data,
      grpc_error* error, CallCombinerClosureList* closures);
  static void AddClosuresForDeferredRecvCallbacks(
      SubchannelCallBatchData* batch_data,
      SubchannelCallRetryState* retry_state, CallCombinerClosureList* closures);
  bool PendingBatchIsUnstarted(PendingBatch* pending,
                               SubchannelCallRetryState* retry_state);
  void AddClosuresToFailUnstartedPendingBatches(
      grpc_call_element* elem, SubchannelCallRetryState* retry_state,
      grpc_error* error, CallCombinerClosureList* closures);
  void RunClosuresForCompletedCall(SubchannelCallBatchData* batch_data,
                                   grpc_error* error);
  static void RecvTrailingMetadataReady(void* arg, grpc_error* error);
  void AddClosuresForCompletedPendingBatch(grpc_call_element* elem,
                                           SubchannelCallBatchData* batch_data,
                                           grpc_error* error,
                                           CallCombinerClosureList* closures);
  void AddClosuresForReplayOrPendingSendOps(
      grpc_call_element* elem, SubchannelCallBatchData* batch_data,
      SubchannelCallRetryState* retry_state, CallCombinerClosureList* closures);
  static void OnComplete(void* arg, grpc_error* error);
  static void StartBatchInCallCombiner(void* arg, grpc_error* ignored);
  void AddClosureForSubchannelBatch(grpc_call_element* elem,
                                    grpc_transport_stream_op_batch* batch,
                                    CallCombinerClosureList* closures);
  void AddRetriableSendInitialMetadataOp(SubchannelCallRetryState* retry_state,
                                         SubchannelCallBatchData* batch_data);
  void AddRetriableSendMessageOp(grpc_call_element* elem,
                                 SubchannelCallRetryState* retry_state,
                                 SubchannelCallBatchData* batch_data);
  void AddRetriableSendTrailingMetadataOp(SubchannelCallRetryState* retry_state,
                                          SubchannelCallBatchData* batch_data);
  void AddRetriableRecvInitialMetadataOp(SubchannelCallRetryState* retry_state,
                                         SubchannelCallBatchData* batch_data);
  void AddRetriableRecvMessageOp(SubchannelCallRetryState* retry_state,
                                 SubchannelCallBatchData* batch_data);
  void AddRetriableRecvTrailingMetadataOp(SubchannelCallRetryState* retry_state,
                                          SubchannelCallBatchData* batch_data);
  void StartInternalRecvTrailingMetadata(grpc_call_element* elem);
  SubchannelCallBatchData* MaybeCreateSubchannelBatchForReplay(
      grpc_call_element* elem, SubchannelCallRetryState* retry_state);
  void AddSubchannelBatchesForPendingBatches(
      grpc_call_element* elem, SubchannelCallRetryState* retry_state,
      CallCombinerClosureList* closures);
  static void StartRetriableSubchannelBatches(void* arg, grpc_error* ignored);
  void CreateSubchannelCall(grpc_call_element* elem);
  static void PickDone(void* arg, grpc_error* error);
  void MaybeRemoveCallFromQueuedPicksLocked(grpc_call_element* elem);
  void MaybeAddCallToQueuedPicksLocked(grpc_call_element* elem);
  grpc_deadline_state deadline_state_;
grpc_slice path_;
  gpr_cycle_counter call_start_time_;
  grpc_millis deadline_;
  Arena* arena_;
  grpc_call_stack* owning_call_;
  CallCombiner* call_combiner_;
  grpc_call_context_element* call_context_;
  RefCountedPtr<ServerRetryThrottleData> retry_throttle_data_;
  const ClientChannelMethodParsedConfig* method_params_ = nullptr;
  std::map<const char*, absl::string_view> call_attributes_;
  std::function<void()> on_call_committed_;
  RefCountedPtr<SubchannelCall> subchannel_call_;
  grpc_error* cancel_error_ = GRPC_ERROR_NONE;
  ChannelData::QueuedPick pick_;
  bool pick_queued_ = false;
  bool service_config_applied_ = false;
  QueuedPickCanceller* pick_canceller_ = nullptr;
  LbCallState lb_call_state_;
  const LoadBalancingPolicy::BackendMetricData* backend_metric_data_ = nullptr;
  RefCountedPtr<ConnectedSubchannel> connected_subchannel_;
  std::function<void(grpc_error*, LoadBalancingPolicy::MetadataInterface*,
                     LoadBalancingPolicy::CallState*)>
      lb_recv_trailing_metadata_ready_;
  grpc_closure pick_closure_;
  grpc_metadata_batch* recv_trailing_metadata_ = nullptr;
  grpc_closure recv_trailing_metadata_ready_;
  grpc_closure* original_recv_trailing_metadata_ready_ = nullptr;
  grpc_polling_entity* pollent_ = nullptr;
  PendingBatch pending_batches_[MAX_PENDING_BATCHES] = {};
  bool pending_send_initial_metadata_ : 1;
  bool pending_send_message_ : 1;
  bool pending_send_trailing_metadata_ : 1;
  bool enable_retries_ : 1;
  bool retry_committed_ : 1;
  bool last_attempt_got_server_pushback_ : 1;
  int num_attempts_completed_ = 0;
  size_t bytes_buffered_for_retry_ = 0;
  ManualConstructor<BackOff> retry_backoff_;
  grpc_timer retry_timer_;
  int num_pending_retriable_subchannel_send_batches_ = 0;
  bool seen_send_initial_metadata_ = false;
  grpc_linked_mdelem* send_initial_metadata_storage_ = nullptr;
  grpc_metadata_batch send_initial_metadata_;
  uint32_t send_initial_metadata_flags_;
  gpr_atm* peer_string_;
  absl::InlinedVector<ByteStreamCache*, 3> send_messages_;
  bool seen_send_trailing_metadata_ = false;
  grpc_linked_mdelem* send_trailing_metadata_storage_ = nullptr;
  grpc_metadata_batch send_trailing_metadata_;
  grpc_error* ApplyServiceConfigToCallLocked(
      grpc_call_element* elem, grpc_metadata_batch* initial_metadata);
  void MaybeInvokeConfigSelectorCommitCallback();
};
class ChannelData::SubchannelWrapper : public SubchannelInterface {
public:
  SubchannelWrapper(ChannelData* chand, Subchannel* subchannel,
                    grpc_core::UniquePtr<char> health_check_service_name)
      : SubchannelInterface(&grpc_client_channel_routing_trace),
        chand_(chand),
        subchannel_(subchannel),
        health_check_service_name_(std::move(health_check_service_name)) {
    if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_routing_trace)) {
      gpr_log(GPR_INFO,
              "chand=%p: creating subchannel wrapper %p for subchannel %p",
              chand, this, subchannel_);
    }
    GRPC_CHANNEL_STACK_REF(chand_->owning_stack_, "SubchannelWrapper");
    auto* subchannel_node = subchannel_->channelz_node();
    if (subchannel_node != nullptr) {
      auto it = chand_->subchannel_refcount_map_.find(subchannel_);
      if (it == chand_->subchannel_refcount_map_.end()) {
        chand_->channelz_node_->AddChildSubchannel(subchannel_node->uuid());
        it = chand_->subchannel_refcount_map_.emplace(subchannel_, 0).first;
      }
      ++it->second;
    }
    chand_->subchannel_wrappers_.insert(this);
  }
  ~SubchannelWrapper() {
    if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_routing_trace)) {
      gpr_log(GPR_INFO,
              "chand=%p: destroying subchannel wrapper %p for subchannel %p",
              chand_, this, subchannel_);
    }
    chand_->subchannel_wrappers_.erase(this);
    auto* subchannel_node = subchannel_->channelz_node();
    if (subchannel_node != nullptr) {
      auto it = chand_->subchannel_refcount_map_.find(subchannel_);
      GPR_ASSERT(it != chand_->subchannel_refcount_map_.end());
      --it->second;
      if (it->second == 0) {
        chand_->channelz_node_->RemoveChildSubchannel(subchannel_node->uuid());
        chand_->subchannel_refcount_map_.erase(it);
      }
    }
    GRPC_SUBCHANNEL_UNREF(subchannel_, "unref from LB");
    GRPC_CHANNEL_STACK_UNREF(chand_->owning_stack_, "SubchannelWrapper");
  }
  grpc_connectivity_state CheckConnectivityState() override {
    RefCountedPtr<ConnectedSubchannel> connected_subchannel;
    grpc_connectivity_state connectivity_state =
        subchannel_->CheckConnectivityState(health_check_service_name_.get(),
                                            &connected_subchannel);
    MaybeUpdateConnectedSubchannel(std::move(connected_subchannel));
    return connectivity_state;
  }
  void WatchConnectivityState(
      grpc_connectivity_state initial_state,
      std::unique_ptr<ConnectivityStateWatcherInterface> watcher) override {
    auto& watcher_wrapper = watcher_map_[watcher.get()];
    GPR_ASSERT(watcher_wrapper == nullptr);
    watcher_wrapper = new WatcherWrapper(std::move(watcher),
                                         Ref(DEBUG_LOCATION, "WatcherWrapper"),
                                         initial_state);
    subchannel_->WatchConnectivityState(
        initial_state,
        grpc_core::UniquePtr<char>(
            gpr_strdup(health_check_service_name_.get())),
        RefCountedPtr<Subchannel::ConnectivityStateWatcherInterface>(
            watcher_wrapper));
  }
  void CancelConnectivityStateWatch(
      ConnectivityStateWatcherInterface* watcher) override {
    auto it = watcher_map_.find(watcher);
    GPR_ASSERT(it != watcher_map_.end());
    subchannel_->CancelConnectivityStateWatch(health_check_service_name_.get(),
                                              it->second);
    watcher_map_.erase(it);
  }
  void AttemptToConnect() override { subchannel_->AttemptToConnect(){ subchannel_->AttemptToConnect(); }
  void ResetBackoff() override { subchannel_->ResetBackoff(){ subchannel_->ResetBackoff(); }
  const grpc_channel_args* channel_args() override {
    return subchannel_->channel_args();
  }
  void UpdateHealthCheckServiceName(
      grpc_core::UniquePtr<char> health_check_service_name) {
    if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_routing_trace)) {
      gpr_log(GPR_INFO,
              "chand=%p: subchannel wrapper %p: updating health check service "
              "name from \"%s\" to \"%s\"",
              chand_, this, health_check_service_name_.get(),
              health_check_service_name.get());
    }
    for (auto& p : watcher_map_) {
      WatcherWrapper*& watcher_wrapper = p.second;
      WatcherWrapper* replacement = watcher_wrapper->MakeReplacement();
      subchannel_->CancelConnectivityStateWatch(
          health_check_service_name_.get(), watcher_wrapper);
      watcher_wrapper = replacement;
      subchannel_->WatchConnectivityState(
          replacement->last_seen_state(),
          grpc_core::UniquePtr<char>(
              gpr_strdup(health_check_service_name.get())),
          RefCountedPtr<Subchannel::ConnectivityStateWatcherInterface>(
              replacement));
    }
    health_check_service_name_ = std::move(health_check_service_name);
  }
  ConnectedSubchannel* connected_subchannel() const {
    return connected_subchannel_.get();
  }
  ConnectedSubchannel* connected_subchannel_in_data_plane() const {
    return connected_subchannel_in_data_plane_.get();
  }
  void set_connected_subchannel_in_data_plane(
      RefCountedPtr<ConnectedSubchannel> connected_subchannel) {
    connected_subchannel_in_data_plane_ = std::move(connected_subchannel);
  }
private:
  class WatcherWrapper : public Subchannel::ConnectivityStateWatcherInterface {
   public:
    WatcherWrapper(
        std::unique_ptr<SubchannelInterface::ConnectivityStateWatcherInterface>
            watcher,
        RefCountedPtr<SubchannelWrapper> parent,
        grpc_connectivity_state initial_state)
        : watcher_(std::move(watcher)),
          parent_(std::move(parent)),
          last_seen_state_(initial_state) {}
    ~WatcherWrapper() {
      auto* parent = parent_.release();
      parent->chand_->work_serializer_->Run(
          [parent]() { parent->Unref(DEBUG_LOCATION, "WatcherWrapper"); },
          DEBUG_LOCATION);
    }
    void OnConnectivityStateChange() override {
      if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_routing_trace)) {
        gpr_log(GPR_INFO,
                "chand=%p: connectivity change for subchannel wrapper %p "
                "subchannel %p; hopping into work_serializer",
                parent_->chand_, parent_.get(), parent_->subchannel_);
      }
      Ref().release();
      parent_->chand_->work_serializer_->Run(
          [this]() {
            ApplyUpdateInControlPlaneWorkSerializer();
            Unref();
          },
          DEBUG_LOCATION);
    }
    grpc_pollset_set* interested_parties() override {
      SubchannelInterface::ConnectivityStateWatcherInterface* watcher =
          watcher_.get();
      if (watcher_ == nullptr) watcher = replacement_->watcher_.get();
      return watcher->interested_parties();
    }
    WatcherWrapper* MakeReplacement() {
      auto* replacement =
          new WatcherWrapper(std::move(watcher_), parent_, last_seen_state_);
      replacement_ = replacement;
      return replacement;
    }
    grpc_connectivity_state last_seen_state() const { return last_seen_state_; }
   private:
    void ApplyUpdateInControlPlaneWorkSerializer() {
      if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_routing_trace)) {
        gpr_log(GPR_INFO,
                "chand=%p: processing connectivity change in work serializer "
                "for subchannel wrapper %p subchannel %p "
                "watcher=%p",
                parent_->chand_, parent_.get(), parent_->subchannel_,
                watcher_.get());
      }
      ConnectivityStateChange state_change = PopConnectivityStateChange();
      if (watcher_ != nullptr) {
        last_seen_state_ = state_change.state;
        parent_->MaybeUpdateConnectedSubchannel(
            std::move(state_change.connected_subchannel));
        watcher_->OnConnectivityStateChange(state_change.state);
      }
    }
    std::unique_ptr<SubchannelInterface::ConnectivityStateWatcherInterface>
        watcher_;
    RefCountedPtr<SubchannelWrapper> parent_;
    grpc_connectivity_state last_seen_state_;
    WatcherWrapper* replacement_ = nullptr;
  };
  void MaybeUpdateConnectedSubchannel(
      RefCountedPtr<ConnectedSubchannel> connected_subchannel) {
    grpc_error* disconnect_error = chand_->disconnect_error();
    if (disconnect_error != GRPC_ERROR_NONE) return;
    if (connected_subchannel_ != connected_subchannel) {
      connected_subchannel_ = std::move(connected_subchannel);
      chand_->pending_subchannel_updates_[Ref(
          DEBUG_LOCATION, "ConnectedSubchannelUpdate")] = connected_subchannel_;
    }
  }
  ChannelData* chand_;
  Subchannel* subchannel_;
  grpc_core::UniquePtr<char> health_check_service_name_;
  std::map<ConnectivityStateWatcherInterface*, WatcherWrapper*> watcher_map_;
  RefCountedPtr<ConnectedSubchannel> connected_subchannel_;
  RefCountedPtr<ConnectedSubchannel> connected_subchannel_in_data_plane_;
};
ChannelData::ExternalConnectivityWatcher::ExternalConnectivityWatcher(
    ChannelData* chand, grpc_polling_entity pollent,
    grpc_connectivity_state* state, grpc_closure* on_complete,
    grpc_closure* watcher_timer_init)
    : chand_(chand),
      pollent_(pollent),
      initial_state_(*state),
      state_(state),
      on_complete_(on_complete),
      watcher_timer_init_(watcher_timer_init) {
  grpc_polling_entity_add_to_pollset_set(&pollent_,
                                         chand_->interested_parties_);
  GRPC_CHANNEL_STACK_REF(chand_->owning_stack_, "ExternalConnectivityWatcher");
  {
    MutexLock lock(&chand_->external_watchers_mu_);
    GPR_ASSERT(chand->external_watchers_[on_complete] {
  grpc_polling_entity_add_to_pollset_set(&pollent_,
                                         chand_->interested_parties_);
  GRPC_CHANNEL_STACK_REF(chand_->owning_stack_, "ExternalConnectivityWatcher");
  {
    MutexLock lock(&chand_->external_watchers_mu_);
    GPR_ASSERT(chand->external_watchers_[on_complete] == nullptr);
    chand->external_watchers_[on_complete] =
        Ref(DEBUG_LOCATION, "AddWatcherToExternalWatchersMapLocked");
  }
  chand_->work_serializer_->Run(
      [this]() {
        AddWatcherLocked();
      },
      DEBUG_LOCATION);
}
ChannelData::ExternalConnectivityWatcher::~ExternalConnectivityWatcher() {
  grpc_polling_entity_del_from_pollset_set(&pollent_,
                                           chand_->interested_parties_);
  GRPC_CHANNEL_STACK_UNREF(chand_->owning_stack_,
                           "ExternalConnectivityWatcher");
}
void ChannelData::ExternalConnectivityWatcher::
    RemoveWatcherFromExternalWatchersMap(ChannelData* chand, grpc_closure* on_complete, bool cancel) {
  RefCountedPtr<ExternalConnectivityWatcher> watcher;
  {
    MutexLock lock(&chand->external_watchers_mu_);
    auto it = chand->external_watchers_.find(on_complete);
    if (it != chand->external_watchers_.end()) {
      watcher = std::move(it->second);
      chand->external_watchers_.erase(it);
    }
  }
  if (watcher != nullptr && cancel) watcher->Cancel();
}
void ChannelData::ExternalConnectivityWatcher::Notify(
    grpc_connectivity_state state) {
  bool done = false;
  if (!done_.CompareExchangeStrong(&done, true, MemoryOrder::RELAXED,
                                   MemoryOrder::RELAXED)) {
    return;
  }
  chand_->RemoveExternalConnectivityWatcher(on_complete_, false);
  *state_ = state;
  ExecCtx::Run(DEBUG_LOCATION, on_complete_, GRPC_ERROR_NONE);
  if (state != GRPC_CHANNEL_SHUTDOWN) {
    chand_->work_serializer_->Run([this]() { RemoveWatcherLocked(); },
                                  DEBUG_LOCATION);
  }
}
void ChannelData::ExternalConnectivityWatcher::Cancel() {
  bool done = false;
  if (!done_.CompareExchangeStrong(&done, true, MemoryOrder::RELAXED,
                                   MemoryOrder::RELAXED)) {
    return;
  }
  ExecCtx::Run(DEBUG_LOCATION, on_complete_, GRPC_ERROR_CANCELLED);
  chand_->work_serializer_->Run([this]() { RemoveWatcherLocked(); },
                                DEBUG_LOCATION);
}
void ChannelData::ExternalConnectivityWatcher::AddWatcherLocked() {
  Closure::Run(DEBUG_LOCATION, watcher_timer_init_, GRPC_ERROR_NONE);
  chand_->state_tracker_.AddWatcher(
      initial_state_, OrphanablePtr<ConnectivityStateWatcherInterface>(this));
}
void ChannelData::ExternalConnectivityWatcher::RemoveWatcherLocked() {
  chand_->state_tracker_.RemoveWatcher(this);
}
class ChannelData::ConnectivityWatcherAdder {
public:
  ConnectivityWatcherAdder(
      ChannelData* chand, grpc_connectivity_state initial_state,
      OrphanablePtr<AsyncConnectivityStateWatcherInterface> watcher)
      : chand_(chand),
        initial_state_(initial_state),
        watcher_(std::move(watcher)) {
    GRPC_CHANNEL_STACK_REF(chand_->owning_stack_, "ConnectivityWatcherAdder");
    chand_->work_serializer_->Run([this]() { AddWatcherLocked(); },
                                  DEBUG_LOCATION);
  }
private:
  void AddWatcherLocked() {
    chand_->state_tracker_.AddWatcher(initial_state_, std::move(watcher_));
    GRPC_CHANNEL_STACK_UNREF(chand_->owning_stack_, "ConnectivityWatcherAdder");
    delete this;
  }
  ChannelData* chand_;
  grpc_connectivity_state initial_state_;
  OrphanablePtr<AsyncConnectivityStateWatcherInterface> watcher_;
};
class ChannelData::ConnectivityWatcherRemover {
public:
  ConnectivityWatcherRemover(ChannelData* chand,
                             AsyncConnectivityStateWatcherInterface* watcher)
      : chand_(chand), watcher_(watcher) {
    GRPC_CHANNEL_STACK_REF(chand_->owning_stack_, "ConnectivityWatcherRemover");
    chand_->work_serializer_->Run([this]() { RemoveWatcherLocked(); },
                                  DEBUG_LOCATION);
  }
private:
  void RemoveWatcherLocked() {
    chand_->state_tracker_.RemoveWatcher(watcher_);
    GRPC_CHANNEL_STACK_UNREF(chand_->owning_stack_,
                             "ConnectivityWatcherRemover");
    delete this;
  }
  ChannelData* chand_;
  AsyncConnectivityStateWatcherInterface* watcher_;
};
class ChannelData::ClientChannelControlHelper
    : public LoadBalancingPolicy::ChannelControlHelper {
public:
  explicit ClientChannelControlHelper(ChannelData* chand) : chand_(chand) {
    GRPC_CHANNEL_STACK_REF(chand_->owning_stack_, "ClientChannelControlHelper");
  }
  () = delete;{
    GRPC_CHANNEL_STACK_UNREF(chand_->owning_stack_,
                             "ClientChannelControlHelper");
  }
  () = delete;{
    GRPC_CHANNEL_STACK_UNREF(chand_->owning_stack_,
                             "ClientChannelControlHelper");
  }
  RefCountedPtr<SubchannelInterface> CreateSubchannel(
      const grpc_channel_args& args) override {
    bool inhibit_health_checking = grpc_channel_arg_get_bool(
        grpc_channel_args_find(&args, GRPC_ARG_INHIBIT_HEALTH_CHECKING), false);
    grpc_core::UniquePtr<char> health_check_service_name;
    if (!inhibit_health_checking) {
      health_check_service_name.reset(
          gpr_strdup(chand_->health_check_service_name_.get()));
    }
    static const char* args_to_remove[] = {
        GRPC_ARG_INHIBIT_HEALTH_CHECKING,
        GRPC_ARG_CHANNELZ_CHANNEL_NODE,
    };
    grpc_arg arg = SubchannelPoolInterface::CreateChannelArg(
        chand_->subchannel_pool_.get());
    grpc_channel_args* new_args = grpc_channel_args_copy_and_add_and_remove(
        &args, args_to_remove, GPR_ARRAY_SIZE(args_to_remove), &arg, 1);
    Subchannel* subchannel =
        chand_->client_channel_factory_->CreateSubchannel(new_args);
    grpc_channel_args_destroy(new_args);
    if (subchannel == nullptr) return nullptr;
    return MakeRefCounted<SubchannelWrapper>(
        chand_, subchannel, std::move(health_check_service_name));
  }
  void UpdateState(
      grpc_connectivity_state state,
      std::unique_ptr<LoadBalancingPolicy::SubchannelPicker> picker) override {
    grpc_error* disconnect_error = chand_->disconnect_error();
    if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_routing_trace)) {
      const char* extra = disconnect_error == GRPC_ERROR_NONE
                              ? ""
                              : " (ignoring -- channel shutting down)";
      gpr_log(GPR_INFO, "chand=%p: update: state=%s picker=%p%s", chand_,
              ConnectivityStateName(state), picker.get(), extra);
    }
    if (disconnect_error == GRPC_ERROR_NONE) {
      chand_->UpdateStateAndPickerLocked(state, "helper", std::move(picker));
    }
  }
  void RequestReresolution() override {}
  void AddTraceEvent(TraceSeverity severity,
                     absl::string_view message) override {
    if (chand_->channelz_node_ != nullptr) {
      chand_->channelz_node_->AddTraceEvent(
          ConvertSeverityEnum(severity),
          grpc_slice_from_copied_buffer(message.data(), message.size()));
    }
  }
private:
  static channelz::ChannelTrace::Severity ConvertSeverityEnum(
      TraceSeverity severity) {
    if (severity == TRACE_INFO) return channelz::ChannelTrace::Info;
    if (severity == TRACE_WARNING) return channelz::ChannelTrace::Warning;
    return channelz::ChannelTrace::Error;
  }
  ChannelData* chand_;
};
ChannelData::ChannelConfigHelper::ApplyServiceConfigResultChannelData::ChannelConfigHelper::ApplyServiceConfig(const Resolver::Result& result) {
  ApplyServiceConfigResult service_config_result;
  RefCountedPtr<ServiceConfig> service_config;
  if (result.service_config_error != GRPC_ERROR_NONE) {
    if (chand_->saved_service_config_ != nullptr) {
      if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_routing_trace)) {
        gpr_log(GPR_INFO,
                "chand=%p: resolver returned invalid service config. "
                "Continuing to use previous service config.",
                chand_);
      }
      service_config = chand_->saved_service_config_;
    } else if (chand_->default_service_config_ != nullptr) {
      if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_routing_trace)) {
        gpr_log(GPR_INFO,
                "chand=%p: resolver returned invalid service config. Using "
                "default service config provided by client API.",
                chand_);
      }
      service_config = chand_->default_service_config_;
    }
  } else if (result.service_config == nullptr) {
    if (chand_->default_service_config_ != nullptr) {
      if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_routing_trace)) {
        gpr_log(GPR_INFO,
                "chand=%p: resolver returned no service config. Using default "
                "service config provided by client API.",
                chand_);
      }
      service_config = chand_->default_service_config_;
    }
  } else {
    service_config = result.service_config;
  }
  service_config_result.service_config_error =
      GRPC_ERROR_REF(result.service_config_error);
  if (service_config == nullptr &&
      result.service_config_error != GRPC_ERROR_NONE) {
    service_config_result.no_valid_service_config = true;
    return service_config_result;
  }
  grpc_core::UniquePtr<char> service_config_json;
  const internal::ClientChannelGlobalParsedConfig* parsed_service_config =
      nullptr;
  if (service_config != nullptr) {
    parsed_service_config =
        static_cast<const internal::ClientChannelGlobalParsedConfig*>(
            service_config->GetGlobalParsedConfig(
                internal::ClientChannelServiceConfigParser::ParserIndex()));
  }
  service_config_result.service_config_changed =
      ((service_config == nullptr) !=
       (chand_->saved_service_config_ == nullptr)) ||
      (service_config != nullptr &&
       service_config->json_string() !=
           chand_->saved_service_config_->json_string());
  if (service_config_result.service_config_changed) {
    service_config_json.reset(gpr_strdup(
        service_config != nullptr ? service_config->json_string().c_str()
                                  : ""));
    if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_routing_trace)) {
      gpr_log(GPR_INFO,
              "chand=%p: resolver returned updated service config: \"%s\"",
              chand_, service_config_json.get());
    }
    if (service_config != nullptr) {
      chand_->health_check_service_name_.reset(
          gpr_strdup(parsed_service_config->health_check_service_name()));
    } else {
      chand_->health_check_service_name_.reset();
    }
    for (auto* subchannel_wrapper : chand_->subchannel_wrappers_) {
      subchannel_wrapper->UpdateHealthCheckServiceName(
          grpc_core::UniquePtr<char>(
              gpr_strdup(chand_->health_check_service_name_.get())));
    }
    chand_->saved_service_config_ = std::move(service_config);
  }
  ProcessLbPolicy(result, parsed_service_config,
                  &service_config_result.lb_policy_config);
  grpc_core::UniquePtr<char> lb_policy_name(
      gpr_strdup((service_config_result.lb_policy_config)->name()));
  {
    MutexLock lock(&chand_->info_mu_);
    chand_->info_lb_policy_name_ = std::move(lb_policy_name);
    if (service_config_json != nullptr) {
      chand_->info_service_config_json_ = std::move(service_config_json);
    }
  }
  return service_config_result;
}
void ChannelData::ChannelConfigHelper::ApplyConfigSelector(bool service_config_changed, RefCountedPtr<ConfigSelector> config_selector) {
  chand_->UpdateServiceConfigInDataPlaneLocked(service_config_changed,
                                               std::move(config_selector));
}
void ChannelData::ChannelConfigHelper::ResolverTransientFailure(grpc_error* error) {
  MutexLock lock(&chand_->data_plane_mu_);
  GRPC_ERROR_UNREF(chand_->resolver_transient_failure_error_);
  chand_->resolver_transient_failure_error_ = error;
}
grpc_error* ChannelData::Init(grpc_channel_element* elem,
                              grpc_channel_element_args* args) {
  GPR_ASSERT(args->is_last);
  GPR_ASSERT(elem->filter == &grpc_client_channel_filter);
  grpc_error* error = GRPC_ERROR_NONE;
  new (elem->channel_data) ChannelData(args, &error);
  return error;
}
void ChannelData::Destroy(grpc_channel_element* elem) {
  ChannelData* chand = static_cast<ChannelData*>(elem->channel_data);
  chand->~ChannelData();
}
bool GetEnableRetries(const grpc_channel_args* args) {
  return grpc_channel_arg_get_bool(
      grpc_channel_args_find(args, GRPC_ARG_ENABLE_RETRIES), true);
}
size_t GetMaxPerRpcRetryBufferSize(const grpc_channel_args* args) {
  return static_cast<size_t>(grpc_channel_arg_get_integer(
      grpc_channel_args_find(args, GRPC_ARG_PER_RPC_RETRY_BUFFER_SIZE),
      {DEFAULT_PER_RPC_RETRY_BUFFER_SIZE, 0, INT_MAX}));
}
RefCountedPtr<SubchannelPoolInterface> GetSubchannelPool(
    const grpc_channel_args* args) {
  const bool use_local_subchannel_pool = grpc_channel_arg_get_bool(
      grpc_channel_args_find(args, GRPC_ARG_USE_LOCAL_SUBCHANNEL_POOL), false);
  if (use_local_subchannel_pool) {
    return MakeRefCounted<LocalSubchannelPool>();
  }
  return GlobalSubchannelPool::instance();
}
channelz::ChannelNode* GetChannelzNode(const grpc_channel_args* args) {
  const grpc_arg* arg =
      grpc_channel_args_find(args, GRPC_ARG_CHANNELZ_CHANNEL_NODE);
  if (arg != nullptr && arg->type == GRPC_ARG_POINTER) {
    return static_cast<channelz::ChannelNode*>(arg->value.pointer.p);
  }
  return nullptr;
}
ChannelData::ChannelData(grpc_channel_element_args* args, grpc_error** error)
    : deadline_checking_enabled_(
          grpc_deadline_checking_enabled(args->channel_args)),
      enable_retries_(GetEnableRetries(args->channel_args)),
      per_rpc_retry_buffer_size_(
          GetMaxPerRpcRetryBufferSize(args->channel_args)),
      owning_stack_(args->channel_stack),
      client_channel_factory_(
          ClientChannelFactory::GetFromChannelArgs(args->channel_args)),
      channelz_node_(GetChannelzNode(args->channel_args)),
      channel_config_helper_(this),
      work_serializer_(std::make_shared<WorkSerializer>()),
      interested_parties_(grpc_pollset_set_create()),
      subchannel_pool_(GetSubchannelPool(args->channel_args)),
      state_tracker_("client_channel", GRPC_CHANNEL_IDLE),
      disconnect_error_(GRPC_ERROR_NONE) {
  if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_routing_trace)) {
    gpr_log(GPR_INFO, "chand=%p: creating client_channel for channel stack %p",
            this, owning_stack_);
  }
  gpr_mu_init(&info_mu_);
  grpc_client_channel_start_backup_polling(interested_parties_);
  if (client_channel_factory_ == nullptr) {
    *error = GRPC_ERROR_CREATE_FROM_STATIC_STRING(
        "Missing client channel factory in args for client channel filter");
    return;
  }
  const char* server_uri = grpc_channel_arg_get_string(
      grpc_channel_args_find(args->channel_args, GRPC_ARG_SERVER_URI));
  if (server_uri == nullptr) {
    *error = GRPC_ERROR_CREATE_FROM_STATIC_STRING(
        "server URI channel arg missing or wrong type in client channel "
        "filter");
    return;
  }
  const char* service_config_json = grpc_channel_arg_get_string(
      grpc_channel_args_find(args->channel_args, GRPC_ARG_SERVICE_CONFIG));
  if (service_config_json != nullptr) {
    *error = GRPC_ERROR_NONE;
    default_service_config_ = ServiceConfig::Create(service_config_json, error);
    if (*error != GRPC_ERROR_NONE) {
      default_service_config_.reset();
      return;
    }
  }
  grpc_uri* uri = grpc_uri_parse(server_uri, true);
  if (uri != nullptr && uri->path[0] != '\0') {
    server_name_.reset(
        gpr_strdup(uri->path[0] == '/' ? uri->path + 1 : uri->path));
  }
  grpc_uri_destroy(uri);
  char* proxy_name = nullptr;
  grpc_channel_args* new_args = nullptr;
  ProxyMapperRegistry::MapName(server_uri, args->channel_args, &proxy_name,
                               &new_args);
  target_uri_.reset(proxy_name != nullptr ? proxy_name
                                          : gpr_strdup(server_uri));
  channel_args_ = new_args != nullptr
                      ? new_args
                      : grpc_channel_args_copy(args->channel_args);
  if (!ResolverRegistry::IsValidTarget(target_uri_.get())) {
    *error =
        GRPC_ERROR_CREATE_FROM_STATIC_STRING("the target uri is not valid.");
    return;
  }
  *error = GRPC_ERROR_NONE;
}
ChannelData::~ChannelData() {
  if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_routing_trace)) {
    gpr_log(GPR_INFO, "chand=%p: destroying channel", this);
  }
  DestroyResolvingLoadBalancingPolicyLocked();
  grpc_channel_args_destroy(channel_args_);
  GRPC_ERROR_UNREF(resolver_transient_failure_error_);
  grpc_client_channel_stop_backup_polling(interested_parties_);
  grpc_pollset_set_destroy(interested_parties_);
  GRPC_ERROR_UNREF(disconnect_error_.Load(MemoryOrder::RELAXED));
  gpr_mu_destroy(&info_mu_);
}
void ChannelData::UpdateStateAndPickerLocked(
    grpc_connectivity_state state, const char* reason,
    std::unique_ptr<LoadBalancingPolicy::SubchannelPicker> picker) {
  if (picker_ == nullptr) {
    health_check_service_name_.reset();
    saved_service_config_.reset();
    saved_config_selector_.reset();
    received_first_resolver_result_ = false;
  }
  state_tracker_.SetState(state, reason);
  if (channelz_node_ != nullptr) {
    channelz_node_->SetConnectivityState(state);
    channelz_node_->AddTraceEvent(
        channelz::ChannelTrace::Severity::Info,
        grpc_slice_from_static_string(
            channelz::ChannelNode::GetChannelConnectivityStateChangeString(
                state)));
  }
  RefCountedPtr<ServerRetryThrottleData> retry_throttle_data_to_unref;
  RefCountedPtr<ServiceConfig> service_config_to_unref;
  RefCountedPtr<ConfigSelector> config_selector_to_unref;
  {
    MutexLock lock(&data_plane_mu_);
    for (auto& p : pending_subchannel_updates_) {
      if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_routing_trace)) {
        gpr_log(GPR_INFO,
                "chand=%p: updating subchannel wrapper %p data plane "
                "connected_subchannel to %p",
                this, p.first.get(), p.second.get());
      }
      p.first->set_connected_subchannel_in_data_plane(std::move(p.second));
    }
    picker_.swap(picker);
    if (picker_ == nullptr) {
      received_service_config_data_ = false;
      retry_throttle_data_to_unref = std::move(retry_throttle_data_);
      service_config_to_unref = std::move(service_config_);
      config_selector_to_unref = std::move(config_selector_);
    }
    for (QueuedPick* pick = queued_picks_; pick != nullptr; pick = pick->next) {
      grpc_call_element* elem = pick->elem;
      CallData* calld = static_cast<CallData*>(elem->call_data);
      grpc_error* error = GRPC_ERROR_NONE;
      if (calld->PickSubchannelLocked(elem, &error)) {
        calld->AsyncPickDone(elem, error);
      }
    }
  }
  pending_subchannel_updates_.clear();
}
void ChannelData::UpdateServiceConfigInDataPlaneLocked(bool service_config_changed, RefCountedPtr<ConfigSelector> config_selector) {
  const bool config_selector_changed =
      saved_config_selector_ != config_selector;
  saved_config_selector_ = config_selector;
  if (!service_config_changed && !config_selector_changed &&
      received_first_resolver_result_) {
    return;
  }
  received_first_resolver_result_ = true;
  RefCountedPtr<ServerRetryThrottleData> retry_throttle_data;
  if (saved_service_config_ != nullptr) {
    const internal::ClientChannelGlobalParsedConfig* parsed_service_config =
        static_cast<const internal::ClientChannelGlobalParsedConfig*>(
            saved_service_config_->GetGlobalParsedConfig(
                internal::ClientChannelServiceConfigParser::ParserIndex()));
    if (parsed_service_config != nullptr) {
      absl::optional<internal::ClientChannelGlobalParsedConfig::RetryThrottling>
          retry_throttle_config = parsed_service_config->retry_throttling();
      if (retry_throttle_config.has_value()) {
        retry_throttle_data =
            internal::ServerRetryThrottleMap::GetDataForServer(
                server_name_.get(),
                retry_throttle_config.value().max_milli_tokens,
                retry_throttle_config.value().milli_token_ratio);
      }
    }
  }
  if (config_selector == nullptr) {
    config_selector =
        MakeRefCounted<DefaultConfigSelector>(saved_service_config_);
  }
  RefCountedPtr<ServiceConfig> service_config_to_unref = saved_service_config_;
  RefCountedPtr<ConfigSelector> config_selector_to_unref =
      std::move(config_selector);
  {
    MutexLock lock(&data_plane_mu_);
    GRPC_ERROR_UNREF(resolver_transient_failure_error_);
    resolver_transient_failure_error_ = GRPC_ERROR_NONE;
    received_service_config_data_ = true;
    retry_throttle_data_.swap(retry_throttle_data);
    service_config_.swap(service_config_to_unref);
    config_selector_.swap(config_selector_to_unref);
    for (QueuedPick* pick = queued_picks_; pick != nullptr; pick = pick->next) {
      grpc_call_element* elem = pick->elem;
      CallData* calld = static_cast<CallData*>(elem->call_data);
      grpc_error* error = GRPC_ERROR_NONE;
      if (calld->PickSubchannelLocked(elem, &error)) {
        calld->AsyncPickDone(elem, error);
      }
    }
  }
}
void ChannelData::CreateResolvingLoadBalancingPolicyLocked() {
  LoadBalancingPolicy::Args lb_args;
  lb_args.work_serializer = work_serializer_;
  lb_args.channel_control_helper =
      absl::make_unique<ClientChannelControlHelper>(this);
  lb_args.args = channel_args_;
  grpc_core::UniquePtr<char> target_uri(gpr_strdup(target_uri_.get()));
  resolving_lb_policy_.reset(new ResolvingLoadBalancingPolicy(
      std::move(lb_args), &grpc_client_channel_routing_trace,
      std::move(target_uri), &channel_config_helper_));
  grpc_pollset_set_add_pollset_set(resolving_lb_policy_->interested_parties(),
                                   interested_parties_);
  if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_routing_trace)) {
    gpr_log(GPR_INFO, "chand=%p: created resolving_lb_policy=%p", this,
            resolving_lb_policy_.get());
  }
}
void ChannelData::DestroyResolvingLoadBalancingPolicyLocked() {
  if (resolving_lb_policy_ != nullptr) {
    grpc_pollset_set_del_pollset_set(resolving_lb_policy_->interested_parties(),
                                     interested_parties_);
    resolving_lb_policy_.reset();
  }
}
void ChannelData::ChannelConfigHelper::ProcessLbPolicy(
    const Resolver::Result& resolver_result,
    const internal::ClientChannelGlobalParsedConfig* parsed_service_config,
    RefCountedPtr<LoadBalancingPolicy::Config>* lb_policy_config) {
  if (parsed_service_config != nullptr &&
      parsed_service_config->parsed_lb_config() != nullptr) {
    *lb_policy_config = parsed_service_config->parsed_lb_config();
    return;
  }
  const char* policy_name = nullptr;
  if (parsed_service_config != nullptr &&
      !parsed_service_config->parsed_deprecated_lb_policy().empty()) {
    policy_name = parsed_service_config->parsed_deprecated_lb_policy().c_str();
  } else {
    const grpc_arg* channel_arg =
        grpc_channel_args_find(resolver_result.args, GRPC_ARG_LB_POLICY_NAME);
    policy_name = grpc_channel_arg_get_string(channel_arg);
  }
  if (policy_name == nullptr) policy_name = "pick_first";
  Json config_json = Json::Array{Json::Object{
      {policy_name, Json::Object{}},
  }};
  grpc_error* parse_error = GRPC_ERROR_NONE;
  *lb_policy_config = LoadBalancingPolicyRegistry::ParseLoadBalancingConfig(
      config_json, &parse_error);
  GPR_ASSERT(*lb_policy_config != nullptr);
  GPR_ASSERT(parse_error == GRPC_ERROR_NONE);
}
grpc_error* ChannelData::DoPingLocked(grpc_transport_op* op) {
  if (state_tracker_.state() != GRPC_CHANNEL_READY) {
    return GRPC_ERROR_CREATE_FROM_STATIC_STRING("channel not connected");
  }
  LoadBalancingPolicy::PickResult result =
      picker_->Pick(LoadBalancingPolicy::PickArgs());
  ConnectedSubchannel* connected_subchannel = nullptr;
  if (result.subchannel != nullptr) {
    SubchannelWrapper* subchannel =
        static_cast<SubchannelWrapper*>(result.subchannel.get());
    connected_subchannel = subchannel->connected_subchannel();
  }
  if (connected_subchannel != nullptr) {
    connected_subchannel->Ping(op->send_ping.on_initiate, op->send_ping.on_ack);
  } else {
    if (result.error == GRPC_ERROR_NONE) {
      result.error = GRPC_ERROR_CREATE_FROM_STATIC_STRING(
          "LB policy dropped call on ping");
    }
  }
  return result.error;
}
void ChannelData::StartTransportOpLocked(grpc_transport_op* op) {
  if (op->start_connectivity_watch != nullptr) {
    state_tracker_.AddWatcher(op->start_connectivity_watch_state,
                              std::move(op->start_connectivity_watch));
  }
  if (op->stop_connectivity_watch != nullptr) {
    state_tracker_.RemoveWatcher(op->stop_connectivity_watch);
  }
  if (op->send_ping.on_initiate != nullptr || op->send_ping.on_ack != nullptr) {
    grpc_error* error = DoPingLocked(op);
    if (error != GRPC_ERROR_NONE) {
      ExecCtx::Run(DEBUG_LOCATION, op->send_ping.on_initiate,
                   GRPC_ERROR_REF(error));
      ExecCtx::Run(DEBUG_LOCATION, op->send_ping.on_ack, error);
    }
    op->bind_pollset = nullptr;
    op->send_ping.on_initiate = nullptr;
    op->send_ping.on_ack = nullptr;
  }
  if (op->reset_connect_backoff) {
    if (resolving_lb_policy_ != nullptr) {
      resolving_lb_policy_->ResetBackoffLocked();
    }
  }
  if (op->disconnect_with_error != GRPC_ERROR_NONE) {
    if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_call_trace)) {
      gpr_log(GPR_INFO, "chand=%p: disconnect_with_error: %s", this,
              grpc_error_string(op->disconnect_with_error));
    }
    DestroyResolvingLoadBalancingPolicyLocked();
    intptr_t value;
    if (grpc_error_get_int(op->disconnect_with_error,
                           GRPC_ERROR_INT_CHANNEL_CONNECTIVITY_STATE, &value) &&
        static_cast<grpc_connectivity_state>(value) == GRPC_CHANNEL_IDLE) {
      if (disconnect_error() == GRPC_ERROR_NONE) {
        UpdateStateAndPickerLocked(GRPC_CHANNEL_IDLE, "channel entering IDLE",
                                   nullptr);
      }
      GRPC_ERROR_UNREF(op->disconnect_with_error);
    } else {
      GPR_ASSERT(disconnect_error_.Load(MemoryOrder::RELAXED) ==
                 GRPC_ERROR_NONE);
      disconnect_error_.Store(op->disconnect_with_error, MemoryOrder::RELEASE);
      UpdateStateAndPickerLocked(
          GRPC_CHANNEL_SHUTDOWN, "shutdown from API",
          absl::make_unique<LoadBalancingPolicy::TransientFailurePicker>(
              GRPC_ERROR_REF(op->disconnect_with_error)));
    }
  }
  GRPC_CHANNEL_STACK_UNREF(owning_stack_, "start_transport_op");
  ExecCtx::Run(DEBUG_LOCATION, op->on_consumed, GRPC_ERROR_NONE);
}
void ChannelData::StartTransportOp(grpc_channel_element* elem,
                                   grpc_transport_op* op) {
  ChannelData* chand = static_cast<ChannelData*>(elem->channel_data);
  GPR_ASSERT(op->set_accept_stream == false);
  if (op->bind_pollset != nullptr) {
    grpc_pollset_set_add_pollset(chand->interested_parties_, op->bind_pollset);
  }
  GRPC_CHANNEL_STACK_REF(chand->owning_stack_, "start_transport_op");
  chand->work_serializer_->Run(
      [chand, op]() { chand->StartTransportOpLocked(op); }, DEBUG_LOCATION);
}
void ChannelData::GetChannelInfo(grpc_channel_element* elem,
                                 const grpc_channel_info* info) {
  ChannelData* chand = static_cast<ChannelData*>(elem->channel_data);
  MutexLock lock(&chand->info_mu_);
  if (info->lb_policy_name != nullptr) {
    *info->lb_policy_name = gpr_strdup(chand->info_lb_policy_name_.get());
  }
  if (info->service_config_json != nullptr) {
    *info->service_config_json =
        gpr_strdup(chand->info_service_config_json_.get());
  }
}
void ChannelData::AddQueuedPick(QueuedPick* pick,
                                grpc_polling_entity* pollent) {
  pick->next = queued_picks_;
  queued_picks_ = pick;
  grpc_polling_entity_add_to_pollset_set(pollent, interested_parties_);
}
void ChannelData::RemoveQueuedPick(QueuedPick* to_remove,
                                   grpc_polling_entity* pollent) {
  grpc_polling_entity_del_from_pollset_set(pollent, interested_parties_);
  for (QueuedPick** pick = &queued_picks_; *pick != nullptr;
       pick = &(*pick)->next) {
    if (*pick == to_remove) {
      *pick = to_remove->next;
      return;
    }
  }
}
RefCountedPtr<ConnectedSubchannel>
ChannelData::GetConnectedSubchannelInDataPlane(
    SubchannelInterface* subchannel) const {
  SubchannelWrapper* subchannel_wrapper =
      static_cast<SubchannelWrapper*>(subchannel);
  ConnectedSubchannel* connected_subchannel =
      subchannel_wrapper->connected_subchannel_in_data_plane();
  if (connected_subchannel == nullptr) return nullptr;
  return connected_subchannel->Ref();
}
void ChannelData::TryToConnectLocked() {
  if (resolving_lb_policy_ != nullptr) {
    resolving_lb_policy_->ExitIdleLocked();
  } else {
    CreateResolvingLoadBalancingPolicyLocked();
  }
  GRPC_CHANNEL_STACK_UNREF(owning_stack_, "TryToConnect");
}
grpc_connectivity_state ChannelData::CheckConnectivityState(
    bool try_to_connect) {
  grpc_connectivity_state out = state_tracker_.state();
  if (out == GRPC_CHANNEL_IDLE && try_to_connect) {
    GRPC_CHANNEL_STACK_REF(owning_stack_, "TryToConnect");
    work_serializer_->Run([this]() { TryToConnectLocked(); }, DEBUG_LOCATION);
  }
  return out;
}
void ChannelData::AddConnectivityWatcher(
    grpc_connectivity_state initial_state,
    OrphanablePtr<AsyncConnectivityStateWatcherInterface> watcher) {
  new ConnectivityWatcherAdder(this, initial_state, std::move(watcher));
}
void ChannelData::RemoveConnectivityWatcher(
    AsyncConnectivityStateWatcherInterface* watcher) {
  new ConnectivityWatcherRemover(this, watcher);
}
CallData::CallData(grpc_call_element* elem, const ChannelData& chand,
                   const grpc_call_element_args& args)
    : deadline_state_(elem, args.call_stack, args.call_combiner,
                      GPR_LIKELY(chand.deadline_checking_enabled())
                          ? args.deadline
                          : GRPC_MILLIS_INF_FUTURE),
      path_(grpc_slice_ref_internal(args.path)),
      call_start_time_(args.start_time),
      deadline_(args.deadline),
      arena_(args.arena),
      owning_call_(args.call_stack),
      call_combiner_(args.call_combiner),
      call_context_(args.context),
      lb_call_state_(this),
      pending_send_initial_metadata_(false),
      pending_send_message_(false),
      pending_send_trailing_metadata_(false),
      enable_retries_(chand.enable_retries()),
      retry_committed_(false),
      last_attempt_got_server_pushback_(false) {}
CallData::~CallData() {
  grpc_slice_unref_internal(path_);
  GRPC_ERROR_UNREF(cancel_error_);
  if (backend_metric_data_ != nullptr) {
    backend_metric_data_
        ->LoadBalancingPolicy::BackendMetricData::~BackendMetricData();
  }
  for (size_t i = 0; i < GPR_ARRAY_SIZE(pending_batches_); ++i) {
    GPR_ASSERT(pending_batches_[i].batch == nullptr);
  }
}
grpc_error* CallData::Init(grpc_call_element* elem,
                           const grpc_call_element_args* args) {
  ChannelData* chand = static_cast<ChannelData*>(elem->channel_data);
  new (elem->call_data) CallData(elem, *chand, *args);
  return GRPC_ERROR_NONE;
}
void CallData::Destroy(grpc_call_element* elem,
                       const grpc_call_final_info* ,
                       grpc_closure* then_schedule_closure) {
  CallData* calld = static_cast<CallData*>(elem->call_data);
  if (GPR_LIKELY(calld->subchannel_call_ != nullptr)) {
    calld->subchannel_call_->SetAfterCallStackDestroy(then_schedule_closure);
    then_schedule_closure = nullptr;
  }
  calld->~CallData();
  ExecCtx::Run(DEBUG_LOCATION, then_schedule_closure, GRPC_ERROR_NONE);
}
void CallData::StartTransportStreamOpBatch(
    grpc_call_element* elem, grpc_transport_stream_op_batch* batch) {
  GPR_TIMER_SCOPE("cc_start_transport_stream_op_batch", 0);
  CallData* calld = static_cast<CallData*>(elem->call_data);
  ChannelData* chand = static_cast<ChannelData*>(elem->channel_data);
  if (GPR_LIKELY(chand->deadline_checking_enabled())) {
    grpc_deadline_state_client_start_transport_stream_op_batch(elem, batch);
  }
  if (GPR_UNLIKELY(calld->cancel_error_ != GRPC_ERROR_NONE)) {
    if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_call_trace)) {
      gpr_log(GPR_INFO, "chand=%p calld=%p: failing batch with error: %s",
              chand, calld, grpc_error_string(calld->cancel_error_));
    }
    grpc_transport_stream_op_batch_finish_with_failure(
        batch, GRPC_ERROR_REF(calld->cancel_error_), calld->call_combiner_);
    return;
  }
  if (GPR_UNLIKELY(batch->cancel_stream)) {
    GRPC_ERROR_UNREF(calld->cancel_error_);
    calld->cancel_error_ =
        GRPC_ERROR_REF(batch->payload->cancel_stream.cancel_error);
    if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_call_trace)) {
      gpr_log(GPR_INFO, "chand=%p calld=%p: recording cancel_error=%s", chand,
              calld, grpc_error_string(calld->cancel_error_));
    }
    if (calld->subchannel_call_ == nullptr) {
      calld->PendingBatchesFail(elem, GRPC_ERROR_REF(calld->cancel_error_),
                                NoYieldCallCombiner);
      grpc_transport_stream_op_batch_finish_with_failure(
          batch, GRPC_ERROR_REF(calld->cancel_error_), calld->call_combiner_);
    } else {
      calld->subchannel_call_->StartTransportStreamOpBatch(batch);
    }
    return;
  }
  calld->PendingBatchesAdd(elem, batch);
  if (calld->subchannel_call_ != nullptr) {
    if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_call_trace)) {
      gpr_log(GPR_INFO,
              "chand=%p calld=%p: starting batch on subchannel_call=%p", chand,
              calld, calld->subchannel_call_.get());
    }
    calld->PendingBatchesResume(elem);
    return;
  }
  if (GPR_LIKELY(batch->send_initial_metadata)) {
    if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_call_trace)) {
      gpr_log(GPR_INFO,
              "chand=%p calld=%p: grabbing data plane mutex to perform pick",
              chand, calld);
    }
    PickSubchannel(elem, GRPC_ERROR_NONE);
  } else {
    if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_call_trace)) {
      gpr_log(GPR_INFO,
              "chand=%p calld=%p: saved batch, yielding call combiner", chand,
              calld);
    }
    GRPC_CALL_COMBINER_STOP(calld->call_combiner_,
                            "batch does not include send_initial_metadata");
  }
}
void CallData::SetPollent(grpc_call_element* elem,
                          grpc_polling_entity* pollent) {
  CallData* calld = static_cast<CallData*>(elem->call_data);
  calld->pollent_ = pollent;
}
void CallData::MaybeCacheSendOpsForBatch(PendingBatch* pending) {
  if (pending->send_ops_cached) return;
  pending->send_ops_cached = true;
  grpc_transport_stream_op_batch* batch = pending->batch;
  if (batch->send_initial_metadata) {
    seen_send_initial_metadata_ = true;
    GPR_ASSERT(send_initial_metadata_storage_ == nullptr);
    grpc_metadata_batch* send_initial_metadata =
        batch->payload->send_initial_metadata.send_initial_metadata;
    send_initial_metadata_storage_ = (grpc_linked_mdelem*)arena_->Alloc(
        sizeof(grpc_linked_mdelem) * send_initial_metadata->list.count);
    grpc_metadata_batch_copy(send_initial_metadata, &send_initial_metadata_,
                             send_initial_metadata_storage_);
    send_initial_metadata_flags_ =
        batch->payload->send_initial_metadata.send_initial_metadata_flags;
    peer_string_ = batch->payload->send_initial_metadata.peer_string;
  }
  if (batch->send_message) {
    ByteStreamCache* cache = arena_->New<ByteStreamCache>(
        std::move(batch->payload->send_message.send_message));
    send_messages_.push_back(cache);
  }
  if (batch->send_trailing_metadata) {
    seen_send_trailing_metadata_ = true;
    GPR_ASSERT(send_trailing_metadata_storage_ == nullptr);
    grpc_metadata_batch* send_trailing_metadata =
        batch->payload->send_trailing_metadata.send_trailing_metadata;
    send_trailing_metadata_storage_ = (grpc_linked_mdelem*)arena_->Alloc(
        sizeof(grpc_linked_mdelem) * send_trailing_metadata->list.count);
    grpc_metadata_batch_copy(send_trailing_metadata, &send_trailing_metadata_,
                             send_trailing_metadata_storage_);
  }
}
void CallData::FreeCachedSendInitialMetadata(ChannelData* chand) {
  if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_call_trace)) {
    gpr_log(GPR_INFO,
            "chand=%p calld=%p: destroying calld->send_initial_metadata", chand,
            this);
  }
  grpc_metadata_batch_destroy(&send_initial_metadata_);
}
void CallData::FreeCachedSendMessage(ChannelData* chand, size_t idx) {
  if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_call_trace)) {
    gpr_log(GPR_INFO,
            "chand=%p calld=%p: destroying calld->send_messages[%" PRIuPTR "]",
            chand, this, idx);
  }
  send_messages_[idx]->Destroy();
}
void CallData::FreeCachedSendTrailingMetadata(ChannelData* chand) {
  if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_call_trace)) {
    gpr_log(GPR_INFO,
            "chand=%p calld=%p: destroying calld->send_trailing_metadata",
            chand, this);
  }
  grpc_metadata_batch_destroy(&send_trailing_metadata_);
}
void CallData::FreeCachedSendOpDataAfterCommit(
    grpc_call_element* elem, SubchannelCallRetryState* retry_state) {
  ChannelData* chand = static_cast<ChannelData*>(elem->channel_data);
  if (retry_state->completed_send_initial_metadata) {
    FreeCachedSendInitialMetadata(chand);
  }
  for (size_t i = 0; i < retry_state->completed_send_message_count; ++i) {
    FreeCachedSendMessage(chand, i);
  }
  if (retry_state->completed_send_trailing_metadata) {
    FreeCachedSendTrailingMetadata(chand);
  }
}
void CallData::FreeCachedSendOpDataForCompletedBatch(
    grpc_call_element* elem, SubchannelCallBatchData* batch_data,
    SubchannelCallRetryState* retry_state) {
  ChannelData* chand = static_cast<ChannelData*>(elem->channel_data);
  if (batch_data->batch.send_initial_metadata) {
    FreeCachedSendInitialMetadata(chand);
  }
  if (batch_data->batch.send_message) {
    FreeCachedSendMessage(chand, retry_state->completed_send_message_count - 1);
  }
  if (batch_data->batch.send_trailing_metadata) {
    FreeCachedSendTrailingMetadata(chand);
  }
}
void CallData::RecvTrailingMetadataReadyForLoadBalancingPolicy(
    void* arg, grpc_error* error) {
  CallData* calld = static_cast<CallData*>(arg);
  grpc_error* error_for_lb = GRPC_ERROR_NONE;
  if (error != GRPC_ERROR_NONE) {
    error_for_lb = error;
  } else {
    const auto& fields = calld->recv_trailing_metadata_->idx.named;
    GPR_ASSERT(fields.grpc_status != nullptr);
    grpc_status_code status =
        grpc_get_status_code_from_metadata(fields.grpc_status->md);
    std::string msg;
    if (status != GRPC_STATUS_OK) {
      error_for_lb = grpc_error_set_int(
          GRPC_ERROR_CREATE_FROM_STATIC_STRING("call failed"),
          GRPC_ERROR_INT_GRPC_STATUS, status);
      if (fields.grpc_message != nullptr) {
        error_for_lb = grpc_error_set_str(
            error_for_lb, GRPC_ERROR_STR_GRPC_MESSAGE,
            grpc_slice_ref_internal(GRPC_MDVALUE(fields.grpc_message->md)));
      }
    }
  }
  Metadata trailing_metadata(calld, calld->recv_trailing_metadata_);
  calld->lb_recv_trailing_metadata_ready_(error_for_lb, &trailing_metadata,
                                          &calld->lb_call_state_);
  if (error == GRPC_ERROR_NONE) GRPC_ERROR_UNREF(error_for_lb);
  Closure::Run(DEBUG_LOCATION, calld->original_recv_trailing_metadata_ready_,
               GRPC_ERROR_REF(error));
}
void CallData::MaybeInjectRecvTrailingMetadataReadyForLoadBalancingPolicy(
    grpc_transport_stream_op_batch* batch) {
  if (lb_recv_trailing_metadata_ready_ != nullptr) {
    recv_trailing_metadata_ =
        batch->payload->recv_trailing_metadata.recv_trailing_metadata;
    original_recv_trailing_metadata_ready_ =
        batch->payload->recv_trailing_metadata.recv_trailing_metadata_ready;
    GRPC_CLOSURE_INIT(&recv_trailing_metadata_ready_,
                      RecvTrailingMetadataReadyForLoadBalancingPolicy, this,
                      grpc_schedule_on_exec_ctx);
    batch->payload->recv_trailing_metadata.recv_trailing_metadata_ready =
        &recv_trailing_metadata_ready_;
  }
}
size_t CallData::GetBatchIndex(grpc_transport_stream_op_batch* batch) {
  if (batch->send_initial_metadata) return 0;
  if (batch->send_message) return 1;
  if (batch->send_trailing_metadata) return 2;
  if (batch->recv_initial_metadata) return 3;
  if (batch->recv_message) return 4;
  if (batch->recv_trailing_metadata) return 5;
  GPR_UNREACHABLE_CODE(return (size_t)-1);
}
void CallData::PendingBatchesAdd(grpc_call_element* elem,
                                 grpc_transport_stream_op_batch* batch) {
  ChannelData* chand = static_cast<ChannelData*>(elem->channel_data);
  const size_t idx = GetBatchIndex(batch);
  if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_call_trace)) {
    gpr_log(GPR_INFO,
            "chand=%p calld=%p: adding pending batch at index %" PRIuPTR, chand,
            this, idx);
  }
  PendingBatch* pending = &pending_batches_[idx];
  GPR_ASSERT(pending->batch == nullptr);
  pending->batch = batch;
  pending->send_ops_cached = false;
  if (enable_retries_) {
    if (batch->send_initial_metadata) {
      pending_send_initial_metadata_ = true;
      bytes_buffered_for_retry_ += grpc_metadata_batch_size(
          batch->payload->send_initial_metadata.send_initial_metadata);
    }
    if (batch->send_message) {
      pending_send_message_ = true;
      bytes_buffered_for_retry_ +=
          batch->payload->send_message.send_message->length();
    }
    if (batch->send_trailing_metadata) {
      pending_send_trailing_metadata_ = true;
    }
    if (GPR_UNLIKELY(bytes_buffered_for_retry_ >
                     chand->per_rpc_retry_buffer_size())) {
      if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_call_trace)) {
        gpr_log(GPR_INFO,
                "chand=%p calld=%p: exceeded retry buffer size, committing",
                chand, this);
      }
      SubchannelCallRetryState* retry_state =
          subchannel_call_ == nullptr ? nullptr
                                      : static_cast<SubchannelCallRetryState*>(
                                            subchannel_call_->GetParentData());
      RetryCommit(elem, retry_state);
      if (num_attempts_completed_ == 0) {
        if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_call_trace)) {
          gpr_log(GPR_INFO,
                  "chand=%p calld=%p: disabling retries before first attempt",
                  chand, this);
        }
        enable_retries_ = false;
      }
    }
  }
}
void CallData::PendingBatchClear(PendingBatch* pending) {
  if (enable_retries_) {
    if (pending->batch->send_initial_metadata) {
      pending_send_initial_metadata_ = false;
    }
    if (pending->batch->send_message) {
      pending_send_message_ = false;
    }
    if (pending->batch->send_trailing_metadata) {
      pending_send_trailing_metadata_ = false;
    }
  }
  pending->batch = nullptr;
}
void CallData::MaybeClearPendingBatch(grpc_call_element* elem,
                                      PendingBatch* pending) {
  ChannelData* chand = static_cast<ChannelData*>(elem->channel_data);
  grpc_transport_stream_op_batch* batch = pending->batch;
  if (batch->on_complete == nullptr &&
      (!batch->recv_initial_metadata ||
       batch->payload->recv_initial_metadata.recv_initial_metadata_ready ==
           nullptr) &&
      (!batch->recv_message ||
       batch->payload->recv_message.recv_message_ready == nullptr) &&
      (!batch->recv_trailing_metadata ||
       batch->payload->recv_trailing_metadata.recv_trailing_metadata_ready ==
           nullptr)) {
    if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_call_trace)) {
      gpr_log(GPR_INFO, "chand=%p calld=%p: clearing pending batch", chand,
              this);
    }
    PendingBatchClear(pending);
  }
}
void CallData::FailPendingBatchInCallCombiner(void* arg, grpc_error* error) {
  grpc_transport_stream_op_batch* batch =
      static_cast<grpc_transport_stream_op_batch*>(arg);
  CallData* calld = static_cast<CallData*>(batch->handler_private.extra_arg);
  grpc_transport_stream_op_batch_finish_with_failure(
      batch, GRPC_ERROR_REF(error), calld->call_combiner_);
}
void CallData::PendingBatchesFail(
    grpc_call_element* elem, grpc_error* error,
    YieldCallCombinerPredicate yield_call_combiner_predicate) {
  GPR_ASSERT(error != GRPC_ERROR_NONE);
  if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_call_trace)) {
    size_t num_batches = 0;
    for (size_t i = 0; i < GPR_ARRAY_SIZE(pending_batches_); ++i) {
      if (pending_batches_[i].batch != nullptr) ++num_batches;
    }
    gpr_log(GPR_INFO,
            "chand=%p calld=%p: failing %" PRIuPTR " pending batches: %s",
            elem->channel_data, this, num_batches, grpc_error_string(error));
  }
  CallCombinerClosureList closures;
  for (size_t i = 0; i < GPR_ARRAY_SIZE(pending_batches_); ++i) {
    PendingBatch* pending = &pending_batches_[i];
    grpc_transport_stream_op_batch* batch = pending->batch;
    if (batch != nullptr) {
      if (batch->recv_trailing_metadata) {
        MaybeInjectRecvTrailingMetadataReadyForLoadBalancingPolicy(batch);
      }
      batch->handler_private.extra_arg = this;
      GRPC_CLOSURE_INIT(&batch->handler_private.closure,
                        FailPendingBatchInCallCombiner, batch,
                        grpc_schedule_on_exec_ctx);
      closures.Add(&batch->handler_private.closure, GRPC_ERROR_REF(error),
                   "PendingBatchesFail");
      PendingBatchClear(pending);
    }
  }
  if (yield_call_combiner_predicate(closures)) {
    closures.RunClosures(call_combiner_);
  } else {
    closures.RunClosuresWithoutYielding(call_combiner_);
  }
  GRPC_ERROR_UNREF(error);
}
void CallData::ResumePendingBatchInCallCombiner(void* arg,
                                                grpc_error* ) {
  grpc_transport_stream_op_batch* batch =
      static_cast<grpc_transport_stream_op_batch*>(arg);
  SubchannelCall* subchannel_call =
      static_cast<SubchannelCall*>(batch->handler_private.extra_arg);
  subchannel_call->StartTransportStreamOpBatch(batch);
}
void CallData::PendingBatchesResume(grpc_call_element* elem) {
  ChannelData* chand = static_cast<ChannelData*>(elem->channel_data);
  if (enable_retries_) {
    StartRetriableSubchannelBatches(elem, GRPC_ERROR_NONE);
    return;
  }
  if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_call_trace)) {
    size_t num_batches = 0;
    for (size_t i = 0; i < GPR_ARRAY_SIZE(pending_batches_); ++i) {
      if (pending_batches_[i].batch != nullptr) ++num_batches;
    }
    gpr_log(GPR_INFO,
            "chand=%p calld=%p: starting %" PRIuPTR
            " pending batches on subchannel_call=%p",
            chand, this, num_batches, subchannel_call_.get());
  }
  CallCombinerClosureList closures;
  for (size_t i = 0; i < GPR_ARRAY_SIZE(pending_batches_); ++i) {
    PendingBatch* pending = &pending_batches_[i];
    grpc_transport_stream_op_batch* batch = pending->batch;
    if (batch != nullptr) {
      if (batch->recv_trailing_metadata) {
        MaybeInjectRecvTrailingMetadataReadyForLoadBalancingPolicy(batch);
      }
      batch->handler_private.extra_arg = subchannel_call_.get();
      GRPC_CLOSURE_INIT(&batch->handler_private.closure,
                        ResumePendingBatchInCallCombiner, batch,
                        grpc_schedule_on_exec_ctx);
      closures.Add(&batch->handler_private.closure, GRPC_ERROR_NONE,
                   "PendingBatchesResume");
      PendingBatchClear(pending);
    }
  }
  closures.RunClosures(call_combiner_);
}
template <typename Predicate>
CallData::PendingBatch* CallData::PendingBatchFind(grpc_call_element* elem,
                                                   const char* log_message,
                                                   Predicate predicate) {
  ChannelData* chand = static_cast<ChannelData*>(elem->channel_data);
  for (size_t i = 0; i < GPR_ARRAY_SIZE(pending_batches_); ++i) {
    PendingBatch* pending = &pending_batches_[i];
    grpc_transport_stream_op_batch* batch = pending->batch;
    if (batch != nullptr && predicate(batch)) {
      if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_call_trace)) {
        gpr_log(GPR_INFO,
                "chand=%p calld=%p: %s pending batch at index %" PRIuPTR, chand,
                this, log_message, i);
      }
      return pending;
    }
  }
  return nullptr;
}
void CallData::RetryCommit(grpc_call_element* elem,
                           SubchannelCallRetryState* retry_state) {
  ChannelData* chand = static_cast<ChannelData*>(elem->channel_data);
  if (retry_committed_) return;
  retry_committed_ = true;
  if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_call_trace)) {
    gpr_log(GPR_INFO, "chand=%p calld=%p: committing retries", chand, this);
  }
  if (retry_state != nullptr) {
    FreeCachedSendOpDataAfterCommit(elem, retry_state);
  }
}
void CallData::DoRetry(grpc_call_element* elem,
                       SubchannelCallRetryState* retry_state,
                       grpc_millis server_pushback_ms) {
  ChannelData* chand = static_cast<ChannelData*>(elem->channel_data);
  GPR_ASSERT(method_params_ != nullptr);
  const auto* retry_policy = method_params_->retry_policy();
  GPR_ASSERT(retry_policy != nullptr);
  subchannel_call_.reset();
  grpc_millis next_attempt_time;
  if (server_pushback_ms >= 0) {
    next_attempt_time = ExecCtx::Get()->Now() + server_pushback_ms;
    last_attempt_got_server_pushback_ = true;
  } else {
    if (num_attempts_completed_ == 1 || last_attempt_got_server_pushback_) {
      retry_backoff_.Init(
          BackOff::Options()
              .set_initial_backoff(retry_policy->initial_backoff)
              .set_multiplier(retry_policy->backoff_multiplier)
              .set_jitter(RETRY_BACKOFF_JITTER)
              .set_max_backoff(retry_policy->max_backoff));
      last_attempt_got_server_pushback_ = false;
    }
    next_attempt_time = retry_backoff_->NextAttemptTime();
  }
  if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_call_trace)) {
    gpr_log(GPR_INFO,
            "chand=%p calld=%p: retrying failed call in %" PRId64 " ms", chand,
            this, next_attempt_time - ExecCtx::Get()->Now());
  }
  GRPC_CLOSURE_INIT(&pick_closure_, PickSubchannel, elem,
                    grpc_schedule_on_exec_ctx);
  grpc_timer_init(&retry_timer_, next_attempt_time, &pick_closure_);
  if (retry_state != nullptr) retry_state->retry_dispatched = true;
}
bool CallData::MaybeRetry(grpc_call_element* elem,
                          SubchannelCallBatchData* batch_data,
                          grpc_status_code status,
                          grpc_mdelem* server_pushback_md) {
  ChannelData* chand = static_cast<ChannelData*>(elem->channel_data);
  if (method_params_ == nullptr) return false;
  const auto* retry_policy = method_params_->retry_policy();
  if (retry_policy == nullptr) return false;
  SubchannelCallRetryState* retry_state = nullptr;
  if (batch_data != nullptr) {
    retry_state = static_cast<SubchannelCallRetryState*>(
        batch_data->subchannel_call->GetParentData());
    if (retry_state->retry_dispatched) {
      if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_call_trace)) {
        gpr_log(GPR_INFO, "chand=%p calld=%p: retry already dispatched", chand,
                this);
      }
      return true;
    }
  }
  if (GPR_LIKELY(status == GRPC_STATUS_OK)) {
    if (retry_throttle_data_ != nullptr) {
      retry_throttle_data_->RecordSuccess();
    }
    if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_call_trace)) {
      gpr_log(GPR_INFO, "chand=%p calld=%p: call succeeded", chand, this);
    }
    return false;
  }
  if (!retry_policy->retryable_status_codes.Contains(status)) {
    if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_call_trace)) {
      gpr_log(GPR_INFO,
              "chand=%p calld=%p: status %s not configured as retryable", chand,
              this, grpc_status_code_to_string(status));
    }
    return false;
  }
  if (retry_throttle_data_ != nullptr &&
      !retry_throttle_data_->RecordFailure()) {
    if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_call_trace)) {
      gpr_log(GPR_INFO, "chand=%p calld=%p: retries throttled", chand, this);
    }
    return false;
  }
  if (retry_committed_) {
    if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_call_trace)) {
      gpr_log(GPR_INFO, "chand=%p calld=%p: retries already committed", chand,
              this);
    }
    return false;
  }
  ++num_attempts_completed_;
  if (num_attempts_completed_ >= retry_policy->max_attempts) {
    if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_call_trace)) {
      gpr_log(GPR_INFO, "chand=%p calld=%p: exceeded %d retry attempts", chand,
              this, retry_policy->max_attempts);
    }
    return false;
  }
  if (cancel_error_ != GRPC_ERROR_NONE) {
    if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_call_trace)) {
      gpr_log(GPR_INFO,
              "chand=%p calld=%p: call cancelled from surface, not retrying",
              chand, this);
    }
    return false;
  }
  grpc_millis server_pushback_ms = -1;
  if (server_pushback_md != nullptr) {
    uint32_t ms;
    if (!grpc_parse_slice_to_uint32(GRPC_MDVALUE(*server_pushback_md), &ms)) {
      if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_call_trace)) {
        gpr_log(GPR_INFO,
                "chand=%p calld=%p: not retrying due to server push-back",
                chand, this);
      }
      return false;
    } else {
      if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_call_trace)) {
        gpr_log(GPR_INFO, "chand=%p calld=%p: server push-back: retry in %u ms",
                chand, this, ms);
      }
      server_pushback_ms = (grpc_millis)ms;
    }
  }
  DoRetry(elem, retry_state, server_pushback_ms);
  return true;
}
CallData::SubchannelCallBatchData* CallData::SubchannelCallBatchData::Create(
    grpc_call_element* elem, int refcount, bool set_on_complete) {
  CallData* calld = static_cast<CallData*>(elem->call_data);
  return calld->arena_->New<SubchannelCallBatchData>(elem, calld, refcount,
                                                     set_on_complete);
}
CallData::SubchannelCallBatchData::SubchannelCallBatchData(
    grpc_call_element* elem, CallData* calld, int refcount,
    bool set_on_complete)
    : elem(elem), subchannel_call(calld->subchannel_call_) {
  SubchannelCallRetryState* retry_state =
      static_cast<SubchannelCallRetryState*>(
          calld->subchannel_call_->GetParentData());
  batch.payload = &retry_state->batch_payload;
  gpr_ref_init(&refs, refcount);
  if (set_on_complete) {
    GRPC_CLOSURE_INIT(&on_complete, CallData::OnComplete, this,
                      grpc_schedule_on_exec_ctx);
    batch.on_complete = &on_complete;
  }
  GRPC_CALL_STACK_REF(calld->owning_call_, "batch_data");
}
void CallData::SubchannelCallBatchData::Destroy() {
  SubchannelCallRetryState* retry_state =
      static_cast<SubchannelCallRetryState*>(subchannel_call->GetParentData());
  if (batch.send_initial_metadata) {
    grpc_metadata_batch_destroy(&retry_state->send_initial_metadata);
  }
  if (batch.send_trailing_metadata) {
    grpc_metadata_batch_destroy(&retry_state->send_trailing_metadata);
  }
  if (batch.recv_initial_metadata) {
    grpc_metadata_batch_destroy(&retry_state->recv_initial_metadata);
  }
  if (batch.recv_trailing_metadata) {
    grpc_metadata_batch_destroy(&retry_state->recv_trailing_metadata);
  }
  subchannel_call.reset();
  CallData* calld = static_cast<CallData*>(elem->call_data);
  GRPC_CALL_STACK_UNREF(calld->owning_call_, "batch_data");
}
void CallData::InvokeRecvInitialMetadataCallback(void* arg, grpc_error* error) {
  SubchannelCallBatchData* batch_data =
      static_cast<SubchannelCallBatchData*>(arg);
  CallData* calld = static_cast<CallData*>(batch_data->elem->call_data);
  PendingBatch* pending = calld->PendingBatchFind(
      batch_data->elem, "invoking recv_initial_metadata_ready for",
      [](grpc_transport_stream_op_batch* batch) {
        return batch->recv_initial_metadata &&
               batch->payload->recv_initial_metadata
                       .recv_initial_metadata_ready != nullptr;
      });
  GPR_ASSERT(pending != nullptr);
  SubchannelCallRetryState* retry_state =
      static_cast<SubchannelCallRetryState*>(
          batch_data->subchannel_call->GetParentData());
  grpc_metadata_batch_move(
      &retry_state->recv_initial_metadata,
      pending->batch->payload->recv_initial_metadata.recv_initial_metadata);
  grpc_closure* recv_initial_metadata_ready =
      pending->batch->payload->recv_initial_metadata
          .recv_initial_metadata_ready;
  pending->batch->payload->recv_initial_metadata.recv_initial_metadata_ready =
      nullptr;
  calld->MaybeClearPendingBatch(batch_data->elem, pending);
  batch_data->Unref();
  Closure::Run(DEBUG_LOCATION, recv_initial_metadata_ready,
               GRPC_ERROR_REF(error));
}
void CallData::RecvInitialMetadataReady(void* arg, grpc_error* error) {
  SubchannelCallBatchData* batch_data =
      static_cast<SubchannelCallBatchData*>(arg);
  grpc_call_element* elem = batch_data->elem;
  ChannelData* chand = static_cast<ChannelData*>(elem->channel_data);
  CallData* calld = static_cast<CallData*>(elem->call_data);
  if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_call_trace)) {
    gpr_log(GPR_INFO,
            "chand=%p calld=%p: got recv_initial_metadata_ready, error=%s",
            chand, calld, grpc_error_string(error));
  }
  SubchannelCallRetryState* retry_state =
      static_cast<SubchannelCallRetryState*>(
          batch_data->subchannel_call->GetParentData());
  retry_state->completed_recv_initial_metadata = true;
  if (retry_state->retry_dispatched) {
    GRPC_CALL_COMBINER_STOP(
        calld->call_combiner_,
        "recv_initial_metadata_ready after retry dispatched");
    return;
  }
  if (GPR_UNLIKELY((retry_state->trailing_metadata_available ||
                    error != GRPC_ERROR_NONE) &&
                   !retry_state->completed_recv_trailing_metadata)) {
    if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_call_trace)) {
      gpr_log(GPR_INFO,
              "chand=%p calld=%p: deferring recv_initial_metadata_ready "
              "(Trailers-Only)",
              chand, calld);
    }
    retry_state->recv_initial_metadata_ready_deferred_batch = batch_data;
    retry_state->recv_initial_metadata_error = GRPC_ERROR_REF(error);
    if (!retry_state->started_recv_trailing_metadata) {
      calld->StartInternalRecvTrailingMetadata(elem);
    } else {
      GRPC_CALL_COMBINER_STOP(
          calld->call_combiner_,
          "recv_initial_metadata_ready trailers-only or error");
    }
    return;
  }
  calld->RetryCommit(elem, retry_state);
  calld->MaybeInvokeConfigSelectorCommitCallback();
  calld->InvokeRecvInitialMetadataCallback(batch_data, error);
}
void CallData::InvokeRecvMessageCallback(void* arg, grpc_error* error) {
  SubchannelCallBatchData* batch_data =
      static_cast<SubchannelCallBatchData*>(arg);
  CallData* calld = static_cast<CallData*>(batch_data->elem->call_data);
  PendingBatch* pending = calld->PendingBatchFind(
      batch_data->elem, "invoking recv_message_ready for",
      [](grpc_transport_stream_op_batch* batch) {
        return batch->recv_message &&
               batch->payload->recv_message.recv_message_ready != nullptr;
      });
  GPR_ASSERT(pending != nullptr);
  SubchannelCallRetryState* retry_state =
      static_cast<SubchannelCallRetryState*>(
          batch_data->subchannel_call->GetParentData());
  *pending->batch->payload->recv_message.recv_message =
      std::move(retry_state->recv_message);
  grpc_closure* recv_message_ready =
      pending->batch->payload->recv_message.recv_message_ready;
  pending->batch->payload->recv_message.recv_message_ready = nullptr;
  calld->MaybeClearPendingBatch(batch_data->elem, pending);
  batch_data->Unref();
  Closure::Run(DEBUG_LOCATION, recv_message_ready, GRPC_ERROR_REF(error));
}
void CallData::RecvMessageReady(void* arg, grpc_error* error) {
  SubchannelCallBatchData* batch_data =
      static_cast<SubchannelCallBatchData*>(arg);
  grpc_call_element* elem = batch_data->elem;
  ChannelData* chand = static_cast<ChannelData*>(elem->channel_data);
  CallData* calld = static_cast<CallData*>(elem->call_data);
  if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_call_trace)) {
    gpr_log(GPR_INFO, "chand=%p calld=%p: got recv_message_ready, error=%s",
            chand, calld, grpc_error_string(error));
  }
  SubchannelCallRetryState* retry_state =
      static_cast<SubchannelCallRetryState*>(
          batch_data->subchannel_call->GetParentData());
  ++retry_state->completed_recv_message_count;
  if (retry_state->retry_dispatched) {
    GRPC_CALL_COMBINER_STOP(calld->call_combiner_,
                            "recv_message_ready after retry dispatched");
    return;
  }
  if (GPR_UNLIKELY(
          (retry_state->recv_message == nullptr || error != GRPC_ERROR_NONE) &&
          !retry_state->completed_recv_trailing_metadata)) {
    if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_call_trace)) {
      gpr_log(GPR_INFO,
              "chand=%p calld=%p: deferring recv_message_ready (nullptr "
              "message and recv_trailing_metadata pending)",
              chand, calld);
    }
    retry_state->recv_message_ready_deferred_batch = batch_data;
    retry_state->recv_message_error = GRPC_ERROR_REF(error);
    if (!retry_state->started_recv_trailing_metadata) {
      calld->StartInternalRecvTrailingMetadata(elem);
    } else {
      GRPC_CALL_COMBINER_STOP(calld->call_combiner_, "recv_message_ready null");
    }
    return;
  }
  calld->RetryCommit(elem, retry_state);
  calld->MaybeInvokeConfigSelectorCommitCallback();
  calld->InvokeRecvMessageCallback(batch_data, error);
}
void CallData::GetCallStatus(grpc_metadata_batch* md_batch, grpc_error* error,
                             grpc_status_code* status,
                             grpc_mdelem** server_pushback_md) {
  if (error != GRPC_ERROR_NONE) {
    grpc_error_get_status(error, deadline_, status, nullptr, nullptr, nullptr);
  } else {
    GPR_ASSERT(md_batch->idx.named.grpc_status != nullptr);
    *status =
        grpc_get_status_code_from_metadata(md_batch->idx.named.grpc_status->md);
    if (server_pushback_md != nullptr &&
        md_batch->idx.named.grpc_retry_pushback_ms != nullptr) {
      *server_pushback_md = &md_batch->idx.named.grpc_retry_pushback_ms->md;
    }
  }
  GRPC_ERROR_UNREF(error);
}
void CallData::AddClosureForRecvTrailingMetadataReady(
    grpc_call_element* elem, SubchannelCallBatchData* batch_data,
    grpc_error* error, CallCombinerClosureList* closures) {
  PendingBatch* pending = PendingBatchFind(
      elem, "invoking recv_trailing_metadata for",
      [](grpc_transport_stream_op_batch* batch) {
        return batch->recv_trailing_metadata &&
               batch->payload->recv_trailing_metadata
                       .recv_trailing_metadata_ready != nullptr;
      });
  if (pending == nullptr) {
    GRPC_ERROR_UNREF(error);
    return;
  }
  SubchannelCallRetryState* retry_state =
      static_cast<SubchannelCallRetryState*>(
          batch_data->subchannel_call->GetParentData());
  grpc_metadata_batch_move(
      &retry_state->recv_trailing_metadata,
      pending->batch->payload->recv_trailing_metadata.recv_trailing_metadata);
  closures->Add(pending->batch->payload->recv_trailing_metadata
                    .recv_trailing_metadata_ready,
                error, "recv_trailing_metadata_ready for pending batch");
  pending->batch->payload->recv_trailing_metadata.recv_trailing_metadata_ready =
      nullptr;
  MaybeClearPendingBatch(elem, pending);
}
void CallData::AddClosuresForDeferredRecvCallbacks(
    SubchannelCallBatchData* batch_data, SubchannelCallRetryState* retry_state,
    CallCombinerClosureList* closures) {
  if (batch_data->batch.recv_trailing_metadata) {
    if (GPR_UNLIKELY(retry_state->recv_initial_metadata_ready_deferred_batch !=
                     nullptr)) {
      GRPC_CLOSURE_INIT(&retry_state->recv_initial_metadata_ready,
                        InvokeRecvInitialMetadataCallback,
                        retry_state->recv_initial_metadata_ready_deferred_batch,
                        grpc_schedule_on_exec_ctx);
      closures->Add(&retry_state->recv_initial_metadata_ready,
                    retry_state->recv_initial_metadata_error,
                    "resuming recv_initial_metadata_ready");
      retry_state->recv_initial_metadata_ready_deferred_batch = nullptr;
    }
    if (GPR_UNLIKELY(retry_state->recv_message_ready_deferred_batch !=
                     nullptr)) {
      GRPC_CLOSURE_INIT(&retry_state->recv_message_ready,
                        InvokeRecvMessageCallback,
                        retry_state->recv_message_ready_deferred_batch,
                        grpc_schedule_on_exec_ctx);
      closures->Add(&retry_state->recv_message_ready,
                    retry_state->recv_message_error,
                    "resuming recv_message_ready");
      retry_state->recv_message_ready_deferred_batch = nullptr;
    }
  }
}
bool CallData::PendingBatchIsUnstarted(PendingBatch* pending,
                                       SubchannelCallRetryState* retry_state) {
  if (pending->batch == nullptr || pending->batch->on_complete == nullptr) {
    return false;
  }
  if (pending->batch->send_initial_metadata &&
      !retry_state->started_send_initial_metadata) {
    return true;
  }
  if (pending->batch->send_message &&
      retry_state->started_send_message_count < send_messages_.size()) {
    return true;
  }
  if (pending->batch->send_trailing_metadata &&
      !retry_state->started_send_trailing_metadata) {
    return true;
  }
  return false;
}
void CallData::AddClosuresToFailUnstartedPendingBatches(
    grpc_call_element* elem, SubchannelCallRetryState* retry_state,
    grpc_error* error, CallCombinerClosureList* closures) {
  ChannelData* chand = static_cast<ChannelData*>(elem->channel_data);
  for (size_t i = 0; i < GPR_ARRAY_SIZE(pending_batches_); ++i) {
    PendingBatch* pending = &pending_batches_[i];
    if (PendingBatchIsUnstarted(pending, retry_state)) {
      if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_call_trace)) {
        gpr_log(GPR_INFO,
                "chand=%p calld=%p: failing unstarted pending batch at index "
                "%" PRIuPTR,
                chand, this, i);
      }
      closures->Add(pending->batch->on_complete, GRPC_ERROR_REF(error),
                    "failing on_complete for pending batch");
      pending->batch->on_complete = nullptr;
      MaybeClearPendingBatch(elem, pending);
    }
  }
  GRPC_ERROR_UNREF(error);
}
void CallData::RunClosuresForCompletedCall(SubchannelCallBatchData* batch_data,
                                           grpc_error* error) {
  grpc_call_element* elem = batch_data->elem;
  SubchannelCallRetryState* retry_state =
      static_cast<SubchannelCallRetryState*>(
          batch_data->subchannel_call->GetParentData());
  CallCombinerClosureList closures;
  AddClosureForRecvTrailingMetadataReady(elem, batch_data,
                                         GRPC_ERROR_REF(error), &closures);
  AddClosuresForDeferredRecvCallbacks(batch_data, retry_state, &closures);
  AddClosuresToFailUnstartedPendingBatches(elem, retry_state,
                                           GRPC_ERROR_REF(error), &closures);
  batch_data->Unref();
  closures.RunClosures(call_combiner_);
  GRPC_ERROR_UNREF(error);
}
void CallData::RecvTrailingMetadataReady(void* arg, grpc_error* error) {
  SubchannelCallBatchData* batch_data =
      static_cast<SubchannelCallBatchData*>(arg);
  grpc_call_element* elem = batch_data->elem;
  ChannelData* chand = static_cast<ChannelData*>(elem->channel_data);
  CallData* calld = static_cast<CallData*>(elem->call_data);
  if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_call_trace)) {
    gpr_log(GPR_INFO,
            "chand=%p calld=%p: got recv_trailing_metadata_ready, error=%s",
            chand, calld, grpc_error_string(error));
  }
  SubchannelCallRetryState* retry_state =
      static_cast<SubchannelCallRetryState*>(
          batch_data->subchannel_call->GetParentData());
  retry_state->completed_recv_trailing_metadata = true;
  grpc_status_code status = GRPC_STATUS_OK;
  grpc_mdelem* server_pushback_md = nullptr;
  grpc_metadata_batch* md_batch =
      batch_data->batch.payload->recv_trailing_metadata.recv_trailing_metadata;
  calld->GetCallStatus(md_batch, GRPC_ERROR_REF(error), &status,
                       &server_pushback_md);
  if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_call_trace)) {
    gpr_log(GPR_INFO, "chand=%p calld=%p: call finished, status=%s", chand,
            calld, grpc_status_code_to_string(status));
  }
  if (calld->MaybeRetry(elem, batch_data, status, server_pushback_md)) {
    if (retry_state->recv_initial_metadata_ready_deferred_batch != nullptr) {
      batch_data->Unref();
      GRPC_ERROR_UNREF(retry_state->recv_initial_metadata_error);
    }
    if (retry_state->recv_message_ready_deferred_batch != nullptr) {
      batch_data->Unref();
      GRPC_ERROR_UNREF(retry_state->recv_message_error);
    }
    batch_data->Unref();
    return;
  }
  calld->RetryCommit(elem, retry_state);
  calld->MaybeInvokeConfigSelectorCommitCallback();
  calld->RunClosuresForCompletedCall(batch_data, GRPC_ERROR_REF(error));
}
void CallData::AddClosuresForCompletedPendingBatch(
    grpc_call_element* elem, SubchannelCallBatchData* batch_data,
    grpc_error* error, CallCombinerClosureList* closures) {
  PendingBatch* pending = PendingBatchFind(
      elem, "completed", [batch_data](grpc_transport_stream_op_batch* batch) {
        return batch->on_complete != nullptr &&
               batch_data->batch.send_initial_metadata ==
                   batch->send_initial_metadata &&
               batch_data->batch.send_message == batch->send_message &&
               batch_data->batch.send_trailing_metadata ==
                   batch->send_trailing_metadata;
      });
  if (pending == nullptr) {
    GRPC_ERROR_UNREF(error);
    return;
  }
  closures->Add(pending->batch->on_complete, error,
                "on_complete for pending batch");
  pending->batch->on_complete = nullptr;
  MaybeClearPendingBatch(elem, pending);
}
void CallData::AddClosuresForReplayOrPendingSendOps(
    grpc_call_element* elem, SubchannelCallBatchData* batch_data,
    SubchannelCallRetryState* retry_state, CallCombinerClosureList* closures) {
  ChannelData* chand = static_cast<ChannelData*>(elem->channel_data);
  bool have_pending_send_message_ops =
      retry_state->started_send_message_count < send_messages_.size();
  bool have_pending_send_trailing_metadata_op =
      seen_send_trailing_metadata_ &&
      !retry_state->started_send_trailing_metadata;
  if (!have_pending_send_message_ops &&
      !have_pending_send_trailing_metadata_op) {
    for (size_t i = 0; i < GPR_ARRAY_SIZE(pending_batches_); ++i) {
      PendingBatch* pending = &pending_batches_[i];
      grpc_transport_stream_op_batch* batch = pending->batch;
      if (batch == nullptr || pending->send_ops_cached) continue;
      if (batch->send_message) have_pending_send_message_ops = true;
      if (batch->send_trailing_metadata) {
        have_pending_send_trailing_metadata_op = true;
      }
    }
  }
  if (have_pending_send_message_ops || have_pending_send_trailing_metadata_op) {
    if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_call_trace)) {
      gpr_log(GPR_INFO,
              "chand=%p calld=%p: starting next batch for pending send op(s)",
              chand, this);
    }
    GRPC_CLOSURE_INIT(&batch_data->batch.handler_private.closure,
                      StartRetriableSubchannelBatches, elem,
                      grpc_schedule_on_exec_ctx);
    closures->Add(&batch_data->batch.handler_private.closure, GRPC_ERROR_NONE,
                  "starting next batch for send_* op(s)");
  }
}
void CallData::OnComplete(void* arg, grpc_error* error) {
  SubchannelCallBatchData* batch_data =
      static_cast<SubchannelCallBatchData*>(arg);
  grpc_call_element* elem = batch_data->elem;
  ChannelData* chand = static_cast<ChannelData*>(elem->channel_data);
  CallData* calld = static_cast<CallData*>(elem->call_data);
  if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_call_trace)) {
    gpr_log(GPR_INFO, "chand=%p calld=%p: got on_complete, error=%s, batch=%s",
            chand, calld, grpc_error_string(error),
            grpc_transport_stream_op_batch_string(&batch_data->batch).c_str());
  }
  SubchannelCallRetryState* retry_state =
      static_cast<SubchannelCallRetryState*>(
          batch_data->subchannel_call->GetParentData());
  if (batch_data->batch.send_initial_metadata) {
    retry_state->completed_send_initial_metadata = true;
  }
  if (batch_data->batch.send_message) {
    ++retry_state->completed_send_message_count;
  }
  if (batch_data->batch.send_trailing_metadata) {
    retry_state->completed_send_trailing_metadata = true;
  }
  if (calld->retry_committed_) {
    calld->FreeCachedSendOpDataForCompletedBatch(elem, batch_data, retry_state);
  }
  CallCombinerClosureList closures;
  if (!retry_state->retry_dispatched) {
    calld->AddClosuresForCompletedPendingBatch(
        elem, batch_data, GRPC_ERROR_REF(error), &closures);
    if (!retry_state->completed_recv_trailing_metadata) {
      calld->AddClosuresForReplayOrPendingSendOps(elem, batch_data, retry_state,
                                                  &closures);
    }
  }
  --calld->num_pending_retriable_subchannel_send_batches_;
  const bool last_send_batch_complete =
      calld->num_pending_retriable_subchannel_send_batches_ == 0;
  batch_data->Unref();
  closures.RunClosures(calld->call_combiner_);
  if (last_send_batch_complete) {
    GRPC_CALL_STACK_UNREF(calld->owning_call_, "subchannel_send_batches");
  }
}
void CallData::StartBatchInCallCombiner(void* arg, grpc_error* ) {
  grpc_transport_stream_op_batch* batch =
      static_cast<grpc_transport_stream_op_batch*>(arg);
  SubchannelCall* subchannel_call =
      static_cast<SubchannelCall*>(batch->handler_private.extra_arg);
  subchannel_call->StartTransportStreamOpBatch(batch);
}
void CallData::AddClosureForSubchannelBatch(
    grpc_call_element* elem, grpc_transport_stream_op_batch* batch,
    CallCombinerClosureList* closures) {
  ChannelData* chand = static_cast<ChannelData*>(elem->channel_data);
  batch->handler_private.extra_arg = subchannel_call_.get();
  GRPC_CLOSURE_INIT(&batch->handler_private.closure, StartBatchInCallCombiner,
                    batch, grpc_schedule_on_exec_ctx);
  if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_call_trace)) {
    gpr_log(GPR_INFO, "chand=%p calld=%p: starting subchannel batch: %s", chand,
            this, grpc_transport_stream_op_batch_string(batch).c_str());
  }
  closures->Add(&batch->handler_private.closure, GRPC_ERROR_NONE,
                "start_subchannel_batch");
}
void CallData::AddRetriableSendInitialMetadataOp(
    SubchannelCallRetryState* retry_state,
    SubchannelCallBatchData* batch_data) {
  const grpc_slice* retry_count_strings[] = {&GRPC_MDSTR_1, &GRPC_MDSTR_2,
                                             &GRPC_MDSTR_3, &GRPC_MDSTR_4};
  retry_state->send_initial_metadata_storage =
      static_cast<grpc_linked_mdelem*>(arena_->Alloc(
          sizeof(grpc_linked_mdelem) *
          (send_initial_metadata_.list.count + (num_attempts_completed_ > 0))));
  grpc_metadata_batch_copy(&send_initial_metadata_,
                           &retry_state->send_initial_metadata,
                           retry_state->send_initial_metadata_storage);
  if (GPR_UNLIKELY(retry_state->send_initial_metadata.idx.named
                       .grpc_previous_rpc_attempts != nullptr)) {
    grpc_metadata_batch_remove(&retry_state->send_initial_metadata,
                               GRPC_BATCH_GRPC_PREVIOUS_RPC_ATTEMPTS);
  }
  if (GPR_UNLIKELY(num_attempts_completed_ > 0)) {
    grpc_mdelem retry_md = grpc_mdelem_create(
        GRPC_MDSTR_GRPC_PREVIOUS_RPC_ATTEMPTS,
        *retry_count_strings[num_attempts_completed_ - 1], nullptr);
    grpc_error* error = grpc_metadata_batch_add_tail(
        &retry_state->send_initial_metadata,
        &retry_state
             ->send_initial_metadata_storage[send_initial_metadata_.list.count],
        retry_md, GRPC_BATCH_GRPC_PREVIOUS_RPC_ATTEMPTS);
    if (GPR_UNLIKELY(error != GRPC_ERROR_NONE)) {
      gpr_log(GPR_ERROR, "error adding retry metadata: %s",
              grpc_error_string(error));
      GPR_ASSERT(false);
    }
  }
  retry_state->started_send_initial_metadata = true;
  batch_data->batch.send_initial_metadata = true;
  batch_data->batch.payload->send_initial_metadata.send_initial_metadata =
      &retry_state->send_initial_metadata;
  batch_data->batch.payload->send_initial_metadata.send_initial_metadata_flags =
      send_initial_metadata_flags_;
  batch_data->batch.payload->send_initial_metadata.peer_string = peer_string_;
}
void CallData::AddRetriableSendMessageOp(grpc_call_element* elem,
                                         SubchannelCallRetryState* retry_state,
                                         SubchannelCallBatchData* batch_data) {
  ChannelData* chand = static_cast<ChannelData*>(elem->channel_data);
  if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_call_trace)) {
    gpr_log(GPR_INFO,
            "chand=%p calld=%p: starting calld->send_messages[%" PRIuPTR "]",
            chand, this, retry_state->started_send_message_count);
  }
  ByteStreamCache* cache =
      send_messages_[retry_state->started_send_message_count];
  ++retry_state->started_send_message_count;
  retry_state->send_message.Init(cache);
  batch_data->batch.send_message = true;
  batch_data->batch.payload->send_message.send_message.reset(
      retry_state->send_message.get());
}
void CallData::AddRetriableSendTrailingMetadataOp(
    SubchannelCallRetryState* retry_state,
    SubchannelCallBatchData* batch_data) {
  retry_state->send_trailing_metadata_storage =
      static_cast<grpc_linked_mdelem*>(arena_->Alloc(
          sizeof(grpc_linked_mdelem) * send_trailing_metadata_.list.count));
  grpc_metadata_batch_copy(&send_trailing_metadata_,
                           &retry_state->send_trailing_metadata,
                           retry_state->send_trailing_metadata_storage);
  retry_state->started_send_trailing_metadata = true;
  batch_data->batch.send_trailing_metadata = true;
  batch_data->batch.payload->send_trailing_metadata.send_trailing_metadata =
      &retry_state->send_trailing_metadata;
}
void CallData::AddRetriableRecvInitialMetadataOp(
    SubchannelCallRetryState* retry_state,
    SubchannelCallBatchData* batch_data) {
  retry_state->started_recv_initial_metadata = true;
  batch_data->batch.recv_initial_metadata = true;
  grpc_metadata_batch_init(&retry_state->recv_initial_metadata);
  batch_data->batch.payload->recv_initial_metadata.recv_initial_metadata =
      &retry_state->recv_initial_metadata;
  batch_data->batch.payload->recv_initial_metadata.trailing_metadata_available =
      &retry_state->trailing_metadata_available;
  GRPC_CLOSURE_INIT(&retry_state->recv_initial_metadata_ready,
                    RecvInitialMetadataReady, batch_data,
                    grpc_schedule_on_exec_ctx);
  batch_data->batch.payload->recv_initial_metadata.recv_initial_metadata_ready =
      &retry_state->recv_initial_metadata_ready;
}
void CallData::AddRetriableRecvMessageOp(SubchannelCallRetryState* retry_state,
                                         SubchannelCallBatchData* batch_data) {
  ++retry_state->started_recv_message_count;
  batch_data->batch.recv_message = true;
  batch_data->batch.payload->recv_message.recv_message =
      &retry_state->recv_message;
  GRPC_CLOSURE_INIT(&retry_state->recv_message_ready, RecvMessageReady,
                    batch_data, grpc_schedule_on_exec_ctx);
  batch_data->batch.payload->recv_message.recv_message_ready =
      &retry_state->recv_message_ready;
}
void CallData::AddRetriableRecvTrailingMetadataOp(
    SubchannelCallRetryState* retry_state,
    SubchannelCallBatchData* batch_data) {
  retry_state->started_recv_trailing_metadata = true;
  batch_data->batch.recv_trailing_metadata = true;
  grpc_metadata_batch_init(&retry_state->recv_trailing_metadata);
  batch_data->batch.payload->recv_trailing_metadata.recv_trailing_metadata =
      &retry_state->recv_trailing_metadata;
  batch_data->batch.payload->recv_trailing_metadata.collect_stats =
      &retry_state->collect_stats;
  GRPC_CLOSURE_INIT(&retry_state->recv_trailing_metadata_ready,
                    RecvTrailingMetadataReady, batch_data,
                    grpc_schedule_on_exec_ctx);
  batch_data->batch.payload->recv_trailing_metadata
      .recv_trailing_metadata_ready =
      &retry_state->recv_trailing_metadata_ready;
  MaybeInjectRecvTrailingMetadataReadyForLoadBalancingPolicy(
      &batch_data->batch);
}
void CallData::StartInternalRecvTrailingMetadata(grpc_call_element* elem) {
  ChannelData* chand = static_cast<ChannelData*>(elem->channel_data);
  if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_call_trace)) {
    gpr_log(GPR_INFO,
            "chand=%p calld=%p: call failed but recv_trailing_metadata not "
            "started; starting it internally",
            chand, this);
  }
  SubchannelCallRetryState* retry_state =
      static_cast<SubchannelCallRetryState*>(subchannel_call_->GetParentData());
  SubchannelCallBatchData* batch_data =
      SubchannelCallBatchData::Create(elem, 2, false );
  AddRetriableRecvTrailingMetadataOp(retry_state, batch_data);
  retry_state->recv_trailing_metadata_internal_batch = batch_data;
  subchannel_call_->StartTransportStreamOpBatch(&batch_data->batch);
}
CallData::SubchannelCallBatchData*
CallData::MaybeCreateSubchannelBatchForReplay(
    grpc_call_element* elem, SubchannelCallRetryState* retry_state) {
  ChannelData* chand = static_cast<ChannelData*>(elem->channel_data);
  SubchannelCallBatchData* replay_batch_data = nullptr;
  if (seen_send_initial_metadata_ &&
      !retry_state->started_send_initial_metadata &&
      !pending_send_initial_metadata_) {
    if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_call_trace)) {
      gpr_log(GPR_INFO,
              "chand=%p calld=%p: replaying previously completed "
              "send_initial_metadata op",
              chand, this);
    }
    replay_batch_data =
        SubchannelCallBatchData::Create(elem, 1, true );
    AddRetriableSendInitialMetadataOp(retry_state, replay_batch_data);
  }
  if (retry_state->started_send_message_count < send_messages_.size() &&
      retry_state->started_send_message_count ==
          retry_state->completed_send_message_count &&
      !pending_send_message_) {
    if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_call_trace)) {
      gpr_log(GPR_INFO,
              "chand=%p calld=%p: replaying previously completed "
              "send_message op",
              chand, this);
    }
    if (replay_batch_data == nullptr) {
      replay_batch_data =
          SubchannelCallBatchData::Create(elem, 1, true );
    }
    AddRetriableSendMessageOp(elem, retry_state, replay_batch_data);
  }
  if (seen_send_trailing_metadata_ &&
      retry_state->started_send_message_count == send_messages_.size() &&
      !retry_state->started_send_trailing_metadata &&
      !pending_send_trailing_metadata_) {
    if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_call_trace)) {
      gpr_log(GPR_INFO,
              "chand=%p calld=%p: replaying previously completed "
              "send_trailing_metadata op",
              chand, this);
    }
    if (replay_batch_data == nullptr) {
      replay_batch_data =
          SubchannelCallBatchData::Create(elem, 1, true );
    }
    AddRetriableSendTrailingMetadataOp(retry_state, replay_batch_data);
  }
  return replay_batch_data;
}
void CallData::AddSubchannelBatchesForPendingBatches(
    grpc_call_element* elem, SubchannelCallRetryState* retry_state,
    CallCombinerClosureList* closures) {
  for (size_t i = 0; i < GPR_ARRAY_SIZE(pending_batches_); ++i) {
    PendingBatch* pending = &pending_batches_[i];
    grpc_transport_stream_op_batch* batch = pending->batch;
    if (batch == nullptr) continue;
    if (batch->send_initial_metadata &&
        retry_state->started_send_initial_metadata) {
      continue;
    }
    if (batch->send_message && retry_state->completed_send_message_count <
                                   retry_state->started_send_message_count) {
      continue;
    }
    if (batch->send_trailing_metadata &&
        (retry_state->started_send_message_count + batch->send_message <
             send_messages_.size() ||
         retry_state->started_send_trailing_metadata)) {
      continue;
    }
    if (batch->recv_initial_metadata &&
        retry_state->started_recv_initial_metadata) {
      continue;
    }
    if (batch->recv_message && retry_state->completed_recv_message_count <
                                   retry_state->started_recv_message_count) {
      continue;
    }
    if (batch->recv_trailing_metadata &&
        retry_state->started_recv_trailing_metadata) {
      if (GPR_UNLIKELY((retry_state->recv_trailing_metadata_internal_batch !=
                        nullptr))) {
        if (retry_state->completed_recv_trailing_metadata) {
          closures->Add(
              &retry_state->recv_trailing_metadata_ready, GRPC_ERROR_NONE,
              "re-executing recv_trailing_metadata_ready to propagate "
              "internally triggered result");
        } else {
          retry_state->recv_trailing_metadata_internal_batch->Unref();
        }
        retry_state->recv_trailing_metadata_internal_batch = nullptr;
      }
      continue;
    }
    if (method_params_ == nullptr ||
        method_params_->retry_policy() == nullptr || retry_committed_) {
      AddClosureForSubchannelBatch(elem, batch, closures);
      PendingBatchClear(pending);
      continue;
    }
    const bool has_send_ops = batch->send_initial_metadata ||
                              batch->send_message ||
                              batch->send_trailing_metadata;
    const int num_callbacks = has_send_ops + batch->recv_initial_metadata +
                              batch->recv_message +
                              batch->recv_trailing_metadata;
    SubchannelCallBatchData* batch_data = SubchannelCallBatchData::Create(
        elem, num_callbacks, has_send_ops );
    MaybeCacheSendOpsForBatch(pending);
    if (batch->send_initial_metadata) {
      AddRetriableSendInitialMetadataOp(retry_state, batch_data);
    }
    if (batch->send_message) {
      AddRetriableSendMessageOp(elem, retry_state, batch_data);
    }
    if (batch->send_trailing_metadata) {
      AddRetriableSendTrailingMetadataOp(retry_state, batch_data);
    }
    if (batch->recv_initial_metadata) {
      GPR_ASSERT(batch->payload->recv_initial_metadata.recv_flags == nullptr);
      AddRetriableRecvInitialMetadataOp(retry_state, batch_data);
    }
    if (batch->recv_message) {
      AddRetriableRecvMessageOp(retry_state, batch_data);
    }
    if (batch->recv_trailing_metadata) {
      AddRetriableRecvTrailingMetadataOp(retry_state, batch_data);
    }
    AddClosureForSubchannelBatch(elem, &batch_data->batch, closures);
    if (batch->send_initial_metadata || batch->send_message ||
        batch->send_trailing_metadata) {
      if (num_pending_retriable_subchannel_send_batches_ == 0) {
        GRPC_CALL_STACK_REF(owning_call_, "subchannel_send_batches");
      }
      ++num_pending_retriable_subchannel_send_batches_;
    }
  }
}
void CallData::StartRetriableSubchannelBatches(void* arg,
                                               grpc_error* ) {
  grpc_call_element* elem = static_cast<grpc_call_element*>(arg);
  ChannelData* chand = static_cast<ChannelData*>(elem->channel_data);
  CallData* calld = static_cast<CallData*>(elem->call_data);
  if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_call_trace)) {
    gpr_log(GPR_INFO, "chand=%p calld=%p: constructing retriable batches",
            chand, calld);
  }
  SubchannelCallRetryState* retry_state =
      static_cast<SubchannelCallRetryState*>(
          calld->subchannel_call_->GetParentData());
  CallCombinerClosureList closures;
  SubchannelCallBatchData* replay_batch_data =
      calld->MaybeCreateSubchannelBatchForReplay(elem, retry_state);
  if (replay_batch_data != nullptr) {
    calld->AddClosureForSubchannelBatch(elem, &replay_batch_data->batch,
                                        &closures);
    if (calld->num_pending_retriable_subchannel_send_batches_ == 0) {
      GRPC_CALL_STACK_REF(calld->owning_call_, "subchannel_send_batches");
    }
    ++calld->num_pending_retriable_subchannel_send_batches_;
  }
  calld->AddSubchannelBatchesForPendingBatches(elem, retry_state, &closures);
  if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_call_trace)) {
    gpr_log(GPR_INFO,
            "chand=%p calld=%p: starting %" PRIuPTR
            " retriable batches on subchannel_call=%p",
            chand, calld, closures.size(), calld->subchannel_call_.get());
  }
  closures.RunClosures(calld->call_combiner_);
}
void CallData::CreateSubchannelCall(grpc_call_element* elem) {
  ChannelData* chand = static_cast<ChannelData*>(elem->channel_data);
  const size_t parent_data_size =
      enable_retries_ ? sizeof(SubchannelCallRetryState) : 0;
  SubchannelCall::Args call_args = {
      std::move(connected_subchannel_), pollent_, path_, call_start_time_,
      deadline_, arena_,
      call_context_, call_combiner_, parent_data_size};
  grpc_error* error = GRPC_ERROR_NONE;
  subchannel_call_ = SubchannelCall::Create(std::move(call_args), &error);
  if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_routing_trace)) {
    gpr_log(GPR_INFO, "chand=%p calld=%p: create subchannel_call=%p: error=%s",
            chand, this, subchannel_call_.get(), grpc_error_string(error));
  }
  if (GPR_UNLIKELY(error != GRPC_ERROR_NONE)) {
    PendingBatchesFail(elem, error, YieldCallCombiner);
  } else {
    if (parent_data_size > 0) {
      new (subchannel_call_->GetParentData())
          SubchannelCallRetryState(call_context_);
    }
    PendingBatchesResume(elem);
  }
}
void CallData::AsyncPickDone(grpc_call_element* elem, grpc_error* error) {
  GRPC_CLOSURE_INIT(&pick_closure_, PickDone, elem, grpc_schedule_on_exec_ctx);
  ExecCtx::Run(DEBUG_LOCATION, &pick_closure_, error);
}
void CallData::PickDone(void* arg, grpc_error* error) {
  grpc_call_element* elem = static_cast<grpc_call_element*>(arg);
  ChannelData* chand = static_cast<ChannelData*>(elem->channel_data);
  CallData* calld = static_cast<CallData*>(elem->call_data);
  if (error != GRPC_ERROR_NONE) {
    if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_routing_trace)) {
      gpr_log(GPR_INFO,
              "chand=%p calld=%p: failed to pick subchannel: error=%s", chand,
              calld, grpc_error_string(error));
    }
    calld->PendingBatchesFail(elem, GRPC_ERROR_REF(error), YieldCallCombiner);
    return;
  }
  calld->CreateSubchannelCall(elem);
}
class CallData::QueuedPickCanceller {
public:
  explicit QueuedPickCanceller(grpc_call_element* elem) : elem_(elem) {
    auto* calld = static_cast<CallData*>(elem->call_data);
    GRPC_CALL_STACK_REF(calld->owning_call_, "QueuedPickCanceller");
    GRPC_CLOSURE_INIT(&closure_, &CancelLocked, this,
                      grpc_schedule_on_exec_ctx);
    calld->call_combiner_->SetNotifyOnCancel(&closure_);
  }
private:
  static void CancelLocked(void* arg, grpc_error* error) {
    auto* self = static_cast<QueuedPickCanceller*>(arg);
    auto* chand = static_cast<ChannelData*>(self->elem_->channel_data);
    auto* calld = static_cast<CallData*>(self->elem_->call_data);
    MutexLock lock(chand->data_plane_mu());
    if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_routing_trace)) {
      gpr_log(GPR_INFO,
              "chand=%p calld=%p: cancelling queued pick: "
              "error=%s self=%p calld->pick_canceller=%p",
              chand, calld, grpc_error_string(error), self,
              calld->pick_canceller_);
    }
    if (calld->pick_canceller_ == self && error != GRPC_ERROR_NONE) {
      calld->MaybeRemoveCallFromQueuedPicksLocked(self->elem_);
      calld->PendingBatchesFail(self->elem_, GRPC_ERROR_REF(error),
                                YieldCallCombinerIfPendingBatchesFound);
    }
    GRPC_CALL_STACK_UNREF(calld->owning_call_, "QueuedPickCanceller");
    delete self;
  }
  grpc_call_element* elem_;
  grpc_closure closure_;
};
void CallData::MaybeRemoveCallFromQueuedPicksLocked(grpc_call_element* elem) {
  if (!pick_queued_) return;
  auto* chand = static_cast<ChannelData*>(elem->channel_data);
  if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_routing_trace)) {
    gpr_log(GPR_INFO, "chand=%p calld=%p: removing from queued picks list",
            chand, this);
  }
  chand->RemoveQueuedPick(&pick_, pollent_);
  pick_queued_ = false;
  pick_canceller_ = nullptr;
}
void CallData::MaybeAddCallToQueuedPicksLocked(grpc_call_element* elem) {
  if (pick_queued_) return;
  auto* chand = static_cast<ChannelData*>(elem->channel_data);
  if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_routing_trace)) {
    gpr_log(GPR_INFO, "chand=%p calld=%p: adding to queued picks list", chand,
            this);
  }
  pick_queued_ = true;
  pick_.elem = elem;
  chand->AddQueuedPick(&pick_, pollent_);
  pick_canceller_ = new QueuedPickCanceller(elem);
}
const char* PickResultTypeName(
    LoadBalancingPolicy::PickResult::ResultType type) {
  switch (type) {
    case LoadBalancingPolicy::PickResult::PICK_COMPLETE:
      return "COMPLETE";
    case LoadBalancingPolicy::PickResult::PICK_QUEUE:
      return "QUEUE";
    case LoadBalancingPolicy::PickResult::PICK_FAILED:
      return "FAILED";
  }
  GPR_UNREACHABLE_CODE(return "UNKNOWN");
}
void CallData::PickSubchannel(void* arg, grpc_error* error) {
  grpc_call_element* elem = static_cast<grpc_call_element*>(arg);
  CallData* calld = static_cast<CallData*>(elem->call_data);
  ChannelData* chand = static_cast<ChannelData*>(elem->channel_data);
  bool pick_complete;
  {
    MutexLock lock(chand->data_plane_mu());
    pick_complete = calld->PickSubchannelLocked(elem, &error);
  }
  if (pick_complete) {
    PickDone(elem, error);
    GRPC_ERROR_UNREF(error);
  }
}
bool CallData::PickSubchannelLocked(grpc_call_element* elem,
                                    grpc_error** error) {
  ChannelData* chand = static_cast<ChannelData*>(elem->channel_data);
  GPR_ASSERT(connected_subchannel_ == nullptr);
  GPR_ASSERT(subchannel_call_ == nullptr);
  if (chand->picker() == nullptr) {
    GRPC_CHANNEL_STACK_REF(chand->owning_stack(), "PickSubchannelLocked");
    ExecCtx::Run(
        DEBUG_LOCATION,
        GRPC_CLOSURE_CREATE(
            [](void* arg, grpc_error* ) {
              auto* chand = static_cast<ChannelData*>(arg);
              chand->work_serializer()->Run(
                  [chand]() {
                    chand->CheckConnectivityState( true);
                    GRPC_CHANNEL_STACK_UNREF(chand->owning_stack(),
                                             "PickSubchannelLocked");
                  },
                  DEBUG_LOCATION);
            },
            chand, nullptr),
        GRPC_ERROR_NONE);
    MaybeAddCallToQueuedPicksLocked(elem);
    return false;
  }
  grpc_metadata_batch* initial_metadata_batch =
      seen_send_initial_metadata_
          ? &send_initial_metadata_
          : pending_batches_[0]
                .batch->payload->send_initial_metadata.send_initial_metadata;
  const uint32_t send_initial_metadata_flags =
      seen_send_initial_metadata_ ? send_initial_metadata_flags_
                                  : pending_batches_[0]
                                        .batch->payload->send_initial_metadata
                                        .send_initial_metadata_flags;
  if (GPR_UNLIKELY(!chand->received_service_config_data())) {
    grpc_error* resolver_error = chand->resolver_transient_failure_error();
    if (resolver_error != GRPC_ERROR_NONE &&
        (send_initial_metadata_flags & GRPC_INITIAL_METADATA_WAIT_FOR_READY) ==
            0) {
      MaybeRemoveCallFromQueuedPicksLocked(elem);
      *error = GRPC_ERROR_REF(resolver_error);
      return true;
    }
    MaybeAddCallToQueuedPicksLocked(elem);
    return false;
  }
  if (GPR_LIKELY(!service_config_applied_)) {
    service_config_applied_ = true;
    *error = ApplyServiceConfigToCallLocked(elem, initial_metadata_batch);
    if (*error != GRPC_ERROR_NONE) return true;
  }
  LoadBalancingPolicy::PickArgs pick_args;
  pick_args.path = StringViewFromSlice(path_);
  pick_args.call_state = &lb_call_state_;
  Metadata initial_metadata(this, initial_metadata_batch);
  pick_args.initial_metadata = &initial_metadata;
  auto result = chand->picker()->Pick(pick_args);
  if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_routing_trace)) {
    gpr_log(GPR_INFO,
            "chand=%p calld=%p: LB pick returned %s (subchannel=%p, error=%s)",
            chand, this, PickResultTypeName(result.type),
            result.subchannel.get(), grpc_error_string(result.error));
  }
  switch (result.type) {
    case LoadBalancingPolicy::PickResult::PICK_FAILED: {
      grpc_error* disconnect_error = chand->disconnect_error();
      if (disconnect_error != GRPC_ERROR_NONE) {
        GRPC_ERROR_UNREF(result.error);
        MaybeRemoveCallFromQueuedPicksLocked(elem);
        MaybeInvokeConfigSelectorCommitCallback();
        *error = GRPC_ERROR_REF(disconnect_error);
        return true;
      }
      if ((send_initial_metadata_flags &
           GRPC_INITIAL_METADATA_WAIT_FOR_READY) == 0) {
        grpc_status_code status = GRPC_STATUS_OK;
        grpc_error_get_status(result.error, deadline_, &status, nullptr,
                              nullptr, nullptr);
        const bool retried = enable_retries_ &&
                             MaybeRetry(elem, nullptr , status,
                                        nullptr );
        if (!retried) {
          grpc_error* new_error =
              GRPC_ERROR_CREATE_REFERENCING_FROM_STATIC_STRING(
                  "Failed to pick subchannel", &result.error, 1);
          GRPC_ERROR_UNREF(result.error);
          *error = new_error;
          MaybeInvokeConfigSelectorCommitCallback();
        }
        MaybeRemoveCallFromQueuedPicksLocked(elem);
        return !retried;
      }
      GRPC_ERROR_UNREF(result.error);
    }
    case LoadBalancingPolicy::PickResult::PICK_QUEUE:
      MaybeAddCallToQueuedPicksLocked(elem);
      return false;
    default:
      MaybeRemoveCallFromQueuedPicksLocked(elem);
      if (GPR_UNLIKELY(result.subchannel == nullptr)) {
        result.error = grpc_error_set_int(
            GRPC_ERROR_CREATE_FROM_STATIC_STRING(
                "Call dropped by load balancing policy"),
            GRPC_ERROR_INT_GRPC_STATUS, GRPC_STATUS_UNAVAILABLE);
        MaybeInvokeConfigSelectorCommitCallback();
      } else {
        connected_subchannel_ =
            chand->GetConnectedSubchannelInDataPlane(result.subchannel.get());
        GPR_ASSERT(connected_subchannel_ != nullptr);
        if (retry_committed_) MaybeInvokeConfigSelectorCommitCallback();
      }
      lb_recv_trailing_metadata_ready_ = result.recv_trailing_metadata_ready;
      *error = result.error;
      return true;
  }
}
grpc_error* CallData::ApplyServiceConfigToCallLocked(grpc_call_element* elem, grpc_metadata_batch* initial_metadata) {
  ChannelData* chand = static_cast<ChannelData*>(elem->channel_data);
  if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_routing_trace)) {
    gpr_log(GPR_INFO, "chand=%p calld=%p: applying service config to call",
            chand, this);
  }
  ConfigSelector* config_selector = chand->config_selector();
  auto service_config = chand->service_config();
  if (service_config != nullptr) {
    ConfigSelector::CallConfig call_config =
        config_selector->GetCallConfig({&path_, initial_metadata, arena_});
    if (call_config.error != GRPC_ERROR_NONE) return call_config.error;
    call_attributes_ = std::move(call_config.call_attributes);
    on_call_committed_ = std::move(call_config.on_call_committed);
    auto* service_config_call_data = arena_->New<ServiceConfigCallData>(
        std::move(service_config), call_config.method_configs, call_context_);
    method_params_ = static_cast<ClientChannelMethodParsedConfig*>(
        service_config_call_data->GetMethodParsedConfig(
            internal::ClientChannelServiceConfigParser::ParserIndex()));
    if (method_params_ != nullptr) {
      if (chand->deadline_checking_enabled() &&
          method_params_->timeout() != 0) {
        const grpc_millis per_method_deadline =
            grpc_cycle_counter_to_millis_round_up(call_start_time_) +
            method_params_->timeout();
        if (per_method_deadline < deadline_) {
          deadline_ = per_method_deadline;
          grpc_deadline_state_reset(elem, deadline_);
        }
      }
      uint32_t* send_initial_metadata_flags =
          &pending_batches_[0]
               .batch->payload->send_initial_metadata
               .send_initial_metadata_flags;
      if (method_params_->wait_for_ready().has_value() &&
          !(*send_initial_metadata_flags &
            GRPC_INITIAL_METADATA_WAIT_FOR_READY_EXPLICITLY_SET)) {
        if (method_params_->wait_for_ready().value()) {
          *send_initial_metadata_flags |= GRPC_INITIAL_METADATA_WAIT_FOR_READY;
        } else {
          *send_initial_metadata_flags &= ~GRPC_INITIAL_METADATA_WAIT_FOR_READY;
        }
      }
    }
    retry_throttle_data_ = chand->retry_throttle_data();
  }
  if (method_params_ == nullptr || method_params_->retry_policy() == nullptr) {
    enable_retries_ = false;
  }
  return GRPC_ERROR_NONE;
}
void CallData::MaybeInvokeConfigSelectorCommitCallback() {
  if (on_call_committed_ != nullptr) {
    on_call_committed_();
    on_call_committed_ = nullptr;
  }
}
}
}
using grpc_core::CallData;
using grpc_core::ChannelData;
const grpc_channel_filter grpc_client_channel_filter = {
    CallData::StartTransportStreamOpBatch,
    ChannelData::StartTransportOp,
    sizeof(CallData),
    CallData::Init,
    CallData::SetPollent,
    CallData::Destroy,
    sizeof(ChannelData),
    ChannelData::Init,
    ChannelData::Destroy,
    ChannelData::GetChannelInfo,
    "client-channel",
};
grpc_connectivity_state grpc_client_channel_check_connectivity_state(
    grpc_channel_element* elem, int try_to_connect) {
  auto* chand = static_cast<ChannelData*>(elem->channel_data);
  return chand->CheckConnectivityState(try_to_connect);
}
int grpc_client_channel_num_external_connectivity_watchers(
    grpc_channel_element* elem) {
  auto* chand = static_cast<ChannelData*>(elem->channel_data);
  return chand->NumExternalConnectivityWatchers();
}
void grpc_client_channel_watch_connectivity_state(
    grpc_channel_element* elem, grpc_polling_entity pollent,
    grpc_connectivity_state* state, grpc_closure* closure,
    grpc_closure* watcher_timer_init) {
  auto* chand = static_cast<ChannelData*>(elem->channel_data);
  if (state == nullptr) {
    GPR_ASSERT(watcher_timer_init == nullptr);
    chand->RemoveExternalConnectivityWatcher(closure, true);
    return;
  }
  return chand->AddExternalConnectivityWatcher(pollent, state, closure,
                                               watcher_timer_init);
}
void grpc_client_channel_start_connectivity_watch(
    grpc_channel_element* elem, grpc_connectivity_state initial_state,
    grpc_core::OrphanablePtr<grpc_core::AsyncConnectivityStateWatcherInterface>
        watcher) {
  auto* chand = static_cast<ChannelData*>(elem->channel_data);
  chand->AddConnectivityWatcher(initial_state, std::move(watcher));
}
void grpc_client_channel_stop_connectivity_watch(
    grpc_channel_element* elem,
    grpc_core::AsyncConnectivityStateWatcherInterface* watcher) {
  auto* chand = static_cast<ChannelData*>(elem->channel_data);
  chand->RemoveConnectivityWatcher(watcher);
}
grpc_core::RefCountedPtr<grpc_core::SubchannelCall>
grpc_client_channel_get_subchannel_call(grpc_call_element* elem) {
  auto* calld = static_cast<CallData*>(elem->call_data);
  return calld->subchannel_call();
}
