#include <grpc/support/port_platform.h>
#include "src/core/ext/filters/client_channel/resolving_lb_policy.h"
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <grpc/support/alloc.h>
#include <grpc/support/log.h>
#include <grpc/support/string_util.h>
#include <grpc/support/sync.h>
#include "src/core/ext/filters/client_channel/backup_poller.h"
#include "src/core/ext/filters/client_channel/http_connect_handshaker.h"
#include "src/core/ext/filters/client_channel/lb_policy_registry.h"
#include "src/core/ext/filters/client_channel/proxy_mapper_registry.h"
#include "src/core/ext/filters/client_channel/resolver_registry.h"
#include "src/core/ext/filters/client_channel/retry_throttle.h"
#include "src/core/ext/filters/client_channel/server_address.h"
#include "src/core/ext/filters/client_channel/service_config.h"
#include "src/core/ext/filters/client_channel/subchannel.h"
#include "src/core/ext/filters/deadline/deadline_filter.h"
#include "src/core/lib/backoff/backoff.h"
#include "src/core/lib/channel/channel_args.h"
#include "src/core/lib/channel/connected_channel.h"
#include "src/core/lib/channel/status_util.h"
#include "src/core/lib/gpr/string.h"
#include "src/core/lib/gprpp/inlined_vector.h"
#include "src/core/lib/gprpp/manual_constructor.h"
#include "src/core/lib/gprpp/sync.h"
#include "src/core/lib/iomgr/iomgr.h"
#include "src/core/lib/iomgr/polling_entity.h"
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
namespace grpc_core {
class ResolvingLoadBalancingPolicy::ResolverResultHandler
    : public Resolver::ResultHandler {
 public:
  explicit ResolverResultHandler(
      RefCountedPtr<ResolvingLoadBalancingPolicy> parent)
      : parent_(std::move(parent)) {}
  ~ResolverResultHandler() {
    if (GRPC_TRACE_FLAG_ENABLED(*(parent_->tracer_))) {
      gpr_log(GPR_INFO, "resolving_lb=%p: resolver shutdown complete",
              parent_.get());
    }
  }
  void ReturnResult(Resolver::Result result) override {
    parent_->OnResolverResultChangedLocked(std::move(result));
  }
  void ReturnError(grpc_error* error) override {
    parent_->OnResolverError(error);
  }
 private:
  RefCountedPtr<ResolvingLoadBalancingPolicy> parent_;
};
class ResolvingLoadBalancingPolicy::ResolvingControlHelper
    : public LoadBalancingPolicy::ChannelControlHelper {
 public:
  explicit ResolvingControlHelper(
      RefCountedPtr<ResolvingLoadBalancingPolicy> parent)
      : parent_(std::move(parent)) {}
  RefCountedPtr<SubchannelInterface> CreateSubchannel(
      const grpc_channel_args& args) override {
    if (parent_->resolver_ == nullptr) return nullptr;
    if (!CalledByCurrentChild() && !CalledByPendingChild()) return nullptr;
    return parent_->channel_control_helper()->CreateSubchannel(args);
  }
  void UpdateState(grpc_connectivity_state state,
                   std::unique_ptr<SubchannelPicker> picker) override {
    if (parent_->resolver_ == nullptr) return;
    if (CalledByPendingChild()) {
      if (GRPC_TRACE_FLAG_ENABLED(*(parent_->tracer_))) {
        gpr_log(GPR_INFO,
                "resolving_lb=%p helper=%p: pending child policy %p reports "
                "state=%s",
                parent_.get(), this, child_, ConnectivityStateName(state));
      }
      if (state != GRPC_CHANNEL_READY) return;
      grpc_pollset_set_del_pollset_set(
          parent_->lb_policy_->interested_parties(),
          parent_->interested_parties());
      parent_->lb_policy_ = std::move(parent_->pending_lb_policy_);
    } else if (!CalledByCurrentChild()) {
      return;
    }
    parent_->channel_control_helper()->UpdateState(state, std::move(picker));
  }
  void RequestReresolution() override {
    if (parent_->pending_lb_policy_ != nullptr && !CalledByPendingChild()) {
      return;
    }
    if (GRPC_TRACE_FLAG_ENABLED(*(parent_->tracer_))) {
      gpr_log(GPR_INFO, "resolving_lb=%p: started name re-resolving",
              parent_.get());
    }
    if (parent_->resolver_ != nullptr) {
      parent_->resolver_->RequestReresolutionLocked();
    }
  }
  void AddTraceEvent(TraceSeverity ,
                     StringView ) override {}
  void set_child(LoadBalancingPolicy* child) { child_ = child; }
 private:
  bool CalledByPendingChild() const {
    GPR_ASSERT(child_ != nullptr);
    return child_ == parent_->pending_lb_policy_.get();
  }
  bool CalledByCurrentChild() const {
    GPR_ASSERT(child_ != nullptr);
    return child_ == parent_->lb_policy_.get();
  };
  RefCountedPtr<ResolvingLoadBalancingPolicy> parent_;
  LoadBalancingPolicy* child_ = nullptr;
};
ResolvingLoadBalancingPolicy::ResolvingLoadBalancingPolicy(
    Args args, TraceFlag* tracer, grpc_core::UniquePtr<char> target_uri,
    ProcessResolverResultCallback process_resolver_result,
    void* process_resolver_result_user_data)
    : LoadBalancingPolicy(std::move(args)),
      tracer_(tracer),
      target_uri_(std::move(target_uri)),
      process_resolver_result_(process_resolver_result),
      process_resolver_result_user_data_(process_resolver_result_user_data) {
  GPR_ASSERT(process_resolver_result != nullptr);
  resolver_ = ResolverRegistry::CreateResolver(
<<<<<<< HEAD
      target_uri_.get(), args.args, interested_parties(), work_serializer(),
      grpc_core::MakeUnique<ResolverResultHandler>(Ref()));
||||||| 660e5360f7
      target_uri_.get(), args.args, interested_parties(), combiner(),
      grpc_core::MakeUnique<ResolverResultHandler>(Ref()));
=======
      target_uri_.get(), args.args, interested_parties(), combiner(),
      absl::make_unique<ResolverResultHandler>(Ref()));
>>>>>>> 0b689cb6
  GPR_ASSERT(resolver_ != nullptr);
  if (GRPC_TRACE_FLAG_ENABLED(*tracer_)) {
    gpr_log(GPR_INFO, "resolving_lb=%p: starting name resolution", this);
  }
  channel_control_helper()->UpdateState(GRPC_CHANNEL_CONNECTING,
                                        absl::make_unique<QueuePicker>(Ref()));
  resolver_->StartLocked();
}
ResolvingLoadBalancingPolicy::~ResolvingLoadBalancingPolicy() {
  GPR_ASSERT(resolver_ == nullptr);
  GPR_ASSERT(lb_policy_ == nullptr);
}
void ResolvingLoadBalancingPolicy::ShutdownLocked() {
  if (resolver_ != nullptr) {
    resolver_.reset();
    if (lb_policy_ != nullptr) {
      if (GRPC_TRACE_FLAG_ENABLED(*tracer_)) {
        gpr_log(GPR_INFO, "resolving_lb=%p: shutting down lb_policy=%p", this,
                lb_policy_.get());
      }
      grpc_pollset_set_del_pollset_set(lb_policy_->interested_parties(),
                                       interested_parties());
      lb_policy_.reset();
    }
    if (pending_lb_policy_ != nullptr) {
      if (GRPC_TRACE_FLAG_ENABLED(*tracer_)) {
        gpr_log(GPR_INFO, "resolving_lb=%p: shutting down pending lb_policy=%p",
                this, pending_lb_policy_.get());
      }
      grpc_pollset_set_del_pollset_set(pending_lb_policy_->interested_parties(),
                                       interested_parties());
      pending_lb_policy_.reset();
    }
  }
}
void ResolvingLoadBalancingPolicy::ExitIdleLocked() {
  if (lb_policy_ != nullptr) {
    lb_policy_->ExitIdleLocked();
    if (pending_lb_policy_ != nullptr) pending_lb_policy_->ExitIdleLocked();
  }
}
void ResolvingLoadBalancingPolicy::ResetBackoffLocked() {
  if (resolver_ != nullptr) {
    resolver_->ResetBackoffLocked();
    resolver_->RequestReresolutionLocked();
  }
  if (lb_policy_ != nullptr) lb_policy_->ResetBackoffLocked();
  if (pending_lb_policy_ != nullptr) pending_lb_policy_->ResetBackoffLocked();
}
void ResolvingLoadBalancingPolicy::OnResolverError(grpc_error* error) {
  if (resolver_ == nullptr) {
    GRPC_ERROR_UNREF(error);
    return;
  }
  if (GRPC_TRACE_FLAG_ENABLED(*tracer_)) {
    gpr_log(GPR_INFO, "resolving_lb=%p: resolver transient failure: %s", this,
            grpc_error_string(error));
  }
  if (lb_policy_ == nullptr) {
    grpc_error* state_error = GRPC_ERROR_CREATE_REFERENCING_FROM_STATIC_STRING(
        "Resolver transient failure", &error, 1);
    channel_control_helper()->UpdateState(
        GRPC_CHANNEL_TRANSIENT_FAILURE,
        absl::make_unique<TransientFailurePicker>(state_error));
  }
  GRPC_ERROR_UNREF(error);
}
void ResolvingLoadBalancingPolicy::CreateOrUpdateLbPolicyLocked(
    const char* lb_policy_name,
    RefCountedPtr<LoadBalancingPolicy::Config> lb_policy_config,
    Resolver::Result result, TraceStringVector* trace_strings) {
  const bool create_policy =
      lb_policy_ == nullptr ||
      (pending_lb_policy_ == nullptr &&
       strcmp(lb_policy_->name(), lb_policy_name) != 0) ||
      (pending_lb_policy_ != nullptr &&
       strcmp(pending_lb_policy_->name(), lb_policy_name) != 0);
  LoadBalancingPolicy* policy_to_update = nullptr;
  if (create_policy) {
    if (GRPC_TRACE_FLAG_ENABLED(*tracer_)) {
      gpr_log(GPR_INFO, "resolving_lb=%p: Creating new %schild policy %s", this,
              lb_policy_ == nullptr ? "" : "pending ", lb_policy_name);
    }
    auto& lb_policy = lb_policy_ == nullptr ? lb_policy_ : pending_lb_policy_;
    lb_policy =
        CreateLbPolicyLocked(lb_policy_name, *result.args, trace_strings);
    policy_to_update = lb_policy.get();
  } else {
    policy_to_update = pending_lb_policy_ != nullptr ? pending_lb_policy_.get()
                                                     : lb_policy_.get();
  }
  GPR_ASSERT(policy_to_update != nullptr);
  if (GRPC_TRACE_FLAG_ENABLED(*tracer_)) {
    gpr_log(GPR_INFO, "resolving_lb=%p: Updating %schild policy %p", this,
            policy_to_update == pending_lb_policy_.get() ? "pending " : "",
            policy_to_update);
  }
  UpdateArgs update_args;
  update_args.addresses = std::move(result.addresses);
  update_args.config = std::move(lb_policy_config);
  update_args.args = result.args;
  result.args = nullptr;
  policy_to_update->UpdateLocked(std::move(update_args));
}
OrphanablePtr<LoadBalancingPolicy>
ResolvingLoadBalancingPolicy::CreateLbPolicyLocked(
    const char* lb_policy_name, const grpc_channel_args& args,
    TraceStringVector* trace_strings) {
  ResolvingControlHelper* helper = new ResolvingControlHelper(Ref());
  LoadBalancingPolicy::Args lb_policy_args;
  lb_policy_args.work_serializer = work_serializer();
  lb_policy_args.channel_control_helper =
      std::unique_ptr<ChannelControlHelper>(helper);
  lb_policy_args.args = &args;
  OrphanablePtr<LoadBalancingPolicy> lb_policy =
      LoadBalancingPolicyRegistry::CreateLoadBalancingPolicy(
          lb_policy_name, std::move(lb_policy_args));
  if (GPR_UNLIKELY(lb_policy == nullptr)) {
    gpr_log(GPR_ERROR, "could not create LB policy \"%s\"", lb_policy_name);
    char* str;
    gpr_asprintf(&str, "Could not create LB policy \"%s\"", lb_policy_name);
    trace_strings->push_back(str);
    return nullptr;
  }
  helper->set_child(lb_policy.get());
  if (GRPC_TRACE_FLAG_ENABLED(*tracer_)) {
    gpr_log(GPR_INFO, "resolving_lb=%p: created new LB policy \"%s\" (%p)",
            this, lb_policy_name, lb_policy.get());
  }
  char* str;
  gpr_asprintf(&str, "Created new LB policy \"%s\"", lb_policy_name);
  trace_strings->push_back(str);
  grpc_pollset_set_add_pollset_set(lb_policy->interested_parties(),
                                   interested_parties());
  return lb_policy;
}
void ResolvingLoadBalancingPolicy::MaybeAddTraceMessagesForAddressChangesLocked(
    bool resolution_contains_addresses, TraceStringVector* trace_strings) {
  if (!resolution_contains_addresses &&
      previous_resolution_contained_addresses_) {
    trace_strings->push_back(gpr_strdup("Address list became empty"));
  } else if (resolution_contains_addresses &&
             !previous_resolution_contained_addresses_) {
    trace_strings->push_back(gpr_strdup("Address list became non-empty"));
  }
  previous_resolution_contained_addresses_ = resolution_contains_addresses;
}
void ResolvingLoadBalancingPolicy::ConcatenateAndAddChannelTraceLocked(
    TraceStringVector* trace_strings) const {
  if (!trace_strings->empty()) {
    gpr_strvec v;
    gpr_strvec_init(&v);
    gpr_strvec_add(&v, gpr_strdup("Resolution event: "));
    bool is_first = 1;
    for (size_t i = 0; i < trace_strings->size(); ++i) {
      if (!is_first) gpr_strvec_add(&v, gpr_strdup(", "));
      is_first = false;
      gpr_strvec_add(&v, (*trace_strings)[i]);
    }
    size_t len = 0;
    grpc_core::UniquePtr<char> message(gpr_strvec_flatten(&v, &len));
    channel_control_helper()->AddTraceEvent(ChannelControlHelper::TRACE_INFO,
                                            StringView(message.get()));
    gpr_strvec_destroy(&v);
  }
}
void ResolvingLoadBalancingPolicy::OnResolverResultChangedLocked(
    Resolver::Result result) {
  if (resolver_ == nullptr) return;
  if (GRPC_TRACE_FLAG_ENABLED(*tracer_)) {
    gpr_log(GPR_INFO, "resolving_lb=%p: got resolver result", this);
  }
  TraceStringVector trace_strings;
  const bool resolution_contains_addresses = result.addresses.size() > 0;
  const char* lb_policy_name = nullptr;
  RefCountedPtr<LoadBalancingPolicy::Config> lb_policy_config;
  bool service_config_changed = false;
  char* service_config_error_string = nullptr;
  if (process_resolver_result_ != nullptr) {
    grpc_error* service_config_error = GRPC_ERROR_NONE;
    service_config_changed = process_resolver_result_(
        process_resolver_result_user_data_, result, &lb_policy_name,
        &lb_policy_config, &service_config_error);
    if (service_config_error != GRPC_ERROR_NONE) {
      service_config_error_string =
          gpr_strdup(grpc_error_string(service_config_error));
      if (lb_policy_name == nullptr) {
        OnResolverError(service_config_error);
      } else {
        GRPC_ERROR_UNREF(service_config_error);
      }
    }
  } else {
    lb_policy_name = child_policy_name_.get();
    lb_policy_config = child_lb_config_;
  }
  if (lb_policy_name != nullptr) {
    CreateOrUpdateLbPolicyLocked(lb_policy_name, lb_policy_config,
                                 std::move(result), &trace_strings);
  }
  if (service_config_changed) {
    trace_strings.push_back(gpr_strdup("Service config changed"));
  }
  if (service_config_error_string != nullptr) {
    trace_strings.push_back(service_config_error_string);
    service_config_error_string = nullptr;
  }
  MaybeAddTraceMessagesForAddressChangesLocked(resolution_contains_addresses,
                                               &trace_strings);
  ConcatenateAndAddChannelTraceLocked(&trace_strings);
}
}
