#include <grpc/support/port_platform.h>
#include <inttypes.h>
#include <limits.h>
#include "absl/strings/str_cat.h"
#include "absl/types/optional.h"
#include <grpc/grpc.h>
#include "src/core/ext/filters/client_channel/client_channel.h"
#include "src/core/ext/filters/client_channel/lb_policy.h"
#include "src/core/ext/filters/client_channel/lb_policy/address_filtering.h"
#include "src/core/ext/filters/client_channel/lb_policy/child_policy_handler.h"
#include "src/core/ext/filters/client_channel/lb_policy/xds/xds.h"
#include "src/core/ext/filters/client_channel/lb_policy_factory.h"
#include "src/core/ext/filters/client_channel/lb_policy_registry.h"
#include "src/core/ext/filters/client_channel/server_address.h"
#include "src/core/ext/xds/xds_channel_args.h"
#include "src/core/ext/xds/xds_client.h"
#include "src/core/ext/xds/xds_client_stats.h"
#include "src/core/lib/channel/channel_args.h"
#include "src/core/lib/gpr/env.h"
#include "src/core/lib/gpr/string.h"
#include "src/core/lib/gprpp/orphanable.h"
#include "src/core/lib/gprpp/ref_counted_ptr.h"
#include "src/core/lib/iomgr/work_serializer.h"
#include "src/core/lib/transport/error_utils.h"
#include "src/core/lib/uri/uri_parser.h"
#define GRPC_EDS_DEFAULT_FALLBACK_TIMEOUT 10000
namespace grpc_core {
TraceFlag grpc_lb_eds_trace(false, "eds_lb");
const char* kXdsLocalityNameAttributeKey = "xds_locality_name";
namespace {
constexpr char kEds[] = "eds_experimental";
class EdsLbConfig : public LoadBalancingPolicy::Config {
 public:
  EdsLbConfig(std::string cluster_name, std::string eds_service_name,
              absl::optional<std::string> lrs_load_reporting_server_name,
              Json locality_picking_policy, Json endpoint_picking_policy,
              uint32_t max_concurrent_requests)
      : cluster_name_(std::move(cluster_name)),
        eds_service_name_(std::move(eds_service_name)),
        lrs_load_reporting_server_name_(
            std::move(lrs_load_reporting_server_name)),
        locality_picking_policy_(std::move(locality_picking_policy)),
        endpoint_picking_policy_(std::move(endpoint_picking_policy)),
        max_concurrent_requests_(max_concurrent_requests) {}
  const char* name() const override { return kEds; }
  const std::string& cluster_name() const { return cluster_name_; }
  const std::string& eds_service_name() const { return eds_service_name_; }
  const absl::optional<std::string>& lrs_load_reporting_server_name() const {
    return lrs_load_reporting_server_name_;
  };
  const Json& locality_picking_policy() const {
    return locality_picking_policy_;
  }
  const Json& endpoint_picking_policy() const {
    return endpoint_picking_policy_;
  }
  const uint32_t max_concurrent_requests() const {
    return max_concurrent_requests_;
  }
 private:
  std::string cluster_name_;
  std::string eds_service_name_;
  absl::optional<std::string> lrs_load_reporting_server_name_;
  Json locality_picking_policy_;
  Json endpoint_picking_policy_;
  uint32_t max_concurrent_requests_;
};
class EdsLb : public LoadBalancingPolicy {
 public:
  EdsLb(RefCountedPtr<XdsClient> xds_client, Args args);
  const char* name() const override { return kEds; }
  void UpdateLocked(UpdateArgs args) override;
  void ResetBackoffLocked() override;
 private:
  class EndpointWatcher : public XdsClient::EndpointWatcherInterface {
   public:
    explicit EndpointWatcher(RefCountedPtr<EdsLb> parent)
        : parent_(std::move(parent)) {}
    void OnEndpointChanged(XdsApi::EdsUpdate update) override {
      new Notifier(parent_, std::move(update));
    }
    void OnError(grpc_error* error) override { new Notifier(parent_, error); }
    void OnResourceDoesNotExist() override { new Notifier(parent_); }
   private:
    class Notifier {
     public:
      Notifier(RefCountedPtr<EdsLb> parent, XdsApi::EdsUpdate update);
      Notifier(RefCountedPtr<EdsLb> parent, grpc_error* error);
      explicit Notifier(RefCountedPtr<EdsLb> parent);
     private:
      enum Type { kUpdate, kError, kDoesNotExist };
      static void RunInExecCtx(void* arg, grpc_error* error);
      void RunInWorkSerializer(grpc_error* error);
      RefCountedPtr<EdsLb> parent_;
      grpc_closure closure_;
      XdsApi::EdsUpdate update_;
      Type type_;
    };
    RefCountedPtr<EdsLb> parent_;
  };
class Helper : public ChannelControlHelper {
  class Helper : public ChannelControlHelper {
   public:
    explicit Helper(RefCountedPtr<EdsLb> eds_policy)
        : eds_policy_(std::move(eds_policy)) {}
    ~Helper() override { eds_policy_.reset(DEBUG_LOCATION, "Helper"); }
    RefCountedPtr<SubchannelInterface> CreateSubchannel(
        ServerAddress address, const grpc_channel_args& args) override;
    void UpdateState(grpc_connectivity_state state, const absl::Status& status,
                     std::unique_ptr<SubchannelPicker> picker) override;
    void RequestReresolution() override {}
    void AddTraceEvent(TraceSeverity severity,
                       absl::string_view message) override;
   private:
    RefCountedPtr<EdsLb> eds_policy_;
  };
  ~EdsLb() override;
  void ShutdownLocked() override;
  void OnEndpointChanged(XdsApi::EdsUpdate update);
  void OnError(grpc_error* error);
  void OnResourceDoesNotExist();
  void MaybeDestroyChildPolicyLocked();
  void UpdatePriorityList(XdsApi::EdsUpdate::PriorityList priority_list);
  void UpdateChildPolicyLocked();
  OrphanablePtr<LoadBalancingPolicy> CreateChildPolicyLocked(
      const grpc_channel_args* args);
  ServerAddressList CreateChildPolicyAddressesLocked();
  RefCountedPtr<Config> CreateChildPolicyConfigLocked();
  grpc_channel_args* CreateChildPolicyArgsLocked(
      const grpc_channel_args* args_in);
  const absl::string_view GetEdsResourceName() const {
    if (!is_xds_uri_) return server_name_;
    if (!config_->eds_service_name().empty()) {
      return config_->eds_service_name();
    }
    return config_->cluster_name();
  }
  std::pair<absl::string_view, absl::string_view> GetLrsClusterKey() const {
    if (!is_xds_uri_) return {server_name_, nullptr};
    return {config_->cluster_name(), config_->eds_service_name()};
  }
  std::string server_name_;
  bool is_xds_uri_;
  const grpc_channel_args* args_ = nullptr;
  RefCountedPtr<EdsLbConfig> config_;
  bool shutting_down_ = false;
  RefCountedPtr<XdsClient> xds_client_;
  EndpointWatcher* endpoint_watcher_ = nullptr;
  XdsApi::EdsUpdate::PriorityList priority_list_;
  std::vector<size_t > priority_child_numbers_;
  RefCountedPtr<XdsApi::EdsUpdate::DropConfig> drop_config_;
  OrphanablePtr<LoadBalancingPolicy> child_policy_;
};
RefCountedPtr<SubchannelInterface> EdsLb::Helper::CreateSubchannel(
    ServerAddress address, const grpc_channel_args& args) {
  if (eds_policy_->shutting_down_) return nullptr;
  return eds_policy_->channel_control_helper()->CreateSubchannel(
      std::move(address), args);
}
void EdsLb::Helper::UpdateState(grpc_connectivity_state state,
                                const absl::Status& status,
                                std::unique_ptr<SubchannelPicker> picker) {
  if (eds_policy_->shutting_down_ || eds_policy_->child_policy_ == nullptr) {
    return;
  }
  if (GRPC_TRACE_FLAG_ENABLED(grpc_lb_eds_trace)) {
    gpr_log(GPR_INFO, "[edslb %p] child policy updated state=%s (%s) picker=%p",
            eds_policy_.get(), ConnectivityStateName(state),
            status.ToString().c_str(), picker.get());
  }
  eds_policy_->channel_control_helper()->UpdateState(state, status,
                                                     std::move(picker));
}
void EdsLb::Helper::AddTraceEvent(TraceSeverity severity,
                                  absl::string_view message) {
  if (eds_policy_->shutting_down_) return;
  eds_policy_->channel_control_helper()->AddTraceEvent(severity, message);
}
EdsLb::EndpointWatcher::Notifier::Notifier(RefCountedPtr<EdsLb> parent,
                                           XdsApi::EdsUpdate update)
    : parent_(std::move(parent)), update_(std::move(update)), type_(kUpdate) {
  GRPC_CLOSURE_INIT(&closure_, &RunInExecCtx, this, nullptr);
  ExecCtx::Run(DEBUG_LOCATION, &closure_, GRPC_ERROR_NONE);
}
EdsLb::EndpointWatcher::Notifier::Notifier(RefCountedPtr<EdsLb> parent,
                                           grpc_error* error)
    : parent_(std::move(parent)), type_(kError) {
  GRPC_CLOSURE_INIT(&closure_, &RunInExecCtx, this, nullptr);
  ExecCtx::Run(DEBUG_LOCATION, &closure_, error);
}
EdsLb::EndpointWatcher::Notifier::Notifier(RefCountedPtr<EdsLb> parent)
    : parent_(std::move(parent)), type_(kDoesNotExist) {
  GRPC_CLOSURE_INIT(&closure_, &RunInExecCtx, this, nullptr);
  ExecCtx::Run(DEBUG_LOCATION, &closure_, GRPC_ERROR_NONE);
}
void EdsLb::EndpointWatcher::Notifier::RunInExecCtx(void* arg,
                                                    grpc_error* error) {
  Notifier* self = static_cast<Notifier*>(arg);
  GRPC_ERROR_REF(error);
  self->parent_->work_serializer()->Run(
      [self, error]() { self->RunInWorkSerializer(error); }, DEBUG_LOCATION);
}
void EdsLb::EndpointWatcher::Notifier::RunInWorkSerializer(grpc_error* error) {
  switch (type_) {
    case kUpdate:
      parent_->OnEndpointChanged(std::move(update_));
      break;
    case kError:
      parent_->OnError(error);
      break;
    case kDoesNotExist:
      parent_->OnResourceDoesNotExist();
      break;
  };
  delete this;
}
EdsLb::EdsLb(RefCountedPtr<XdsClient> xds_client, Args args)
    : LoadBalancingPolicy(std::move(args)), xds_client_(std::move(xds_client)) {
  if (GRPC_TRACE_FLAG_ENABLED(grpc_lb_eds_trace)) {
    gpr_log(GPR_INFO, "[edslb %p] created -- using xds client %p", this,
            xds_client_.get());
  }
  const char* server_uri =
      grpc_channel_args_find_string(args.args, GRPC_ARG_SERVER_URI);
  GPR_ASSERT(server_uri != nullptr);
  grpc_uri* uri = grpc_uri_parse(server_uri, true);
  GPR_ASSERT(uri->path[0] != '\0');
  server_name_ = uri->path[0] == '/' ? uri->path + 1 : uri->path;
  is_xds_uri_ = strcmp(uri->scheme, "xds") == 0;
  if (GRPC_TRACE_FLAG_ENABLED(grpc_lb_eds_trace)) {
    gpr_log(GPR_INFO, "[edslb %p] server name from channel (is_xds_uri=%d): %s",
            this, is_xds_uri_, server_name_.c_str());
  }
  grpc_uri_destroy(uri);
  if (!is_xds_uri_) {
    channelz::ChannelNode* parent_channelz_node =
        grpc_channel_args_find_pointer<channelz::ChannelNode>(
            args.args, GRPC_ARG_CHANNELZ_CHANNEL_NODE);
    if (parent_channelz_node != nullptr) {
      xds_client_->AddChannelzLinkage(parent_channelz_node);
    }
    grpc_pollset_set_add_pollset_set(xds_client_->interested_parties(),
                                     interested_parties());
  }
}
EdsLb::~EdsLb() {
  if (GRPC_TRACE_FLAG_ENABLED(grpc_lb_eds_trace)) {
    gpr_log(GPR_INFO, "[edslb %p] destroying eds LB policy", this);
  }
}
void EdsLb::ShutdownLocked() {
  if (GRPC_TRACE_FLAG_ENABLED(grpc_lb_eds_trace)) {
    gpr_log(GPR_INFO, "[edslb %p] shutting down", this);
  }
  shutting_down_ = true;
  MaybeDestroyChildPolicyLocked();
  if (endpoint_watcher_ != nullptr) {
    if (GRPC_TRACE_FLAG_ENABLED(grpc_lb_eds_trace)) {
      gpr_log(GPR_INFO, "[edslb %p] cancelling xds watch for %s", this,
              std::string(GetEdsResourceName()).c_str());
    }
    xds_client_->CancelEndpointDataWatch(GetEdsResourceName(),
                                         endpoint_watcher_);
  }
  if (!is_xds_uri_) {
    channelz::ChannelNode* parent_channelz_node =
        grpc_channel_args_find_pointer<channelz::ChannelNode>(
            args_, GRPC_ARG_CHANNELZ_CHANNEL_NODE);
    if (parent_channelz_node != nullptr) {
      xds_client_->RemoveChannelzLinkage(parent_channelz_node);
    }
    grpc_pollset_set_del_pollset_set(xds_client_->interested_parties(),
                                     interested_parties());
  }
  xds_client_.reset(DEBUG_LOCATION, "EdsLb");
  grpc_channel_args_destroy(args_);
  args_ = nullptr;
}
void EdsLb::MaybeDestroyChildPolicyLocked() {
  if (child_policy_ != nullptr) {
    grpc_pollset_set_del_pollset_set(child_policy_->interested_parties(),
                                     interested_parties());
    child_policy_.reset();
  }
}
void EdsLb::UpdateLocked(UpdateArgs args) {
  if (GRPC_TRACE_FLAG_ENABLED(grpc_lb_eds_trace)) {
    gpr_log(GPR_INFO, "[edslb %p] Received update", this);
  }
  const bool is_initial_update = args_ == nullptr;
  auto old_config = std::move(config_);
  config_ = std::move(args.config);
  grpc_channel_args_destroy(args_);
  args_ = args.args;
  args.args = nullptr;
  if (child_policy_ != nullptr) UpdateChildPolicyLocked();
  if (is_initial_update) {
    if (GRPC_TRACE_FLAG_ENABLED(grpc_lb_eds_trace)) {
      gpr_log(GPR_INFO, "[edslb %p] starting xds watch for %s", this,
              std::string(GetEdsResourceName()).c_str());
    }
    auto watcher = absl::make_unique<EndpointWatcher>(
        Ref(DEBUG_LOCATION, "EndpointWatcher"));
    endpoint_watcher_ = watcher.get();
    xds_client_->WatchEndpointData(GetEdsResourceName(), std::move(watcher));
  }
}
void EdsLb::ResetBackoffLocked() {
  if (!is_xds_uri_ && xds_client_ != nullptr) xds_client_->ResetBackoff();
  if (child_policy_ != nullptr) {
    child_policy_->ResetBackoffLocked();
  }
}
void EdsLb::OnEndpointChanged(XdsApi::EdsUpdate update) {
  if (GRPC_TRACE_FLAG_ENABLED(grpc_lb_eds_trace)) {
    gpr_log(GPR_INFO, "[edslb %p] Received EDS update from xds client", this);
  }
  drop_config_ = std::move(update.drop_config);
  if (update.priorities.empty()) update.priorities.emplace_back();
  UpdatePriorityList(std::move(update.priorities));
}
void EdsLb::OnError(grpc_error* error) {
  gpr_log(GPR_ERROR, "[edslb %p] xds watcher reported error: %s", this,
          grpc_error_string(error));
  if (child_policy_ == nullptr) {
    channel_control_helper()->UpdateState(
        GRPC_CHANNEL_TRANSIENT_FAILURE, grpc_error_to_absl_status(error),
        absl::make_unique<TransientFailurePicker>(error));
  } else {
    GRPC_ERROR_UNREF(error);
  }
}
void EdsLb::OnResourceDoesNotExist() {
  gpr_log(
      GPR_ERROR,
      "[edslb %p] EDS resource does not exist -- reporting TRANSIENT_FAILURE",
      this);
  grpc_error* error = grpc_error_set_int(
      GRPC_ERROR_CREATE_FROM_STATIC_STRING("EDS resource does not exist"),
      GRPC_ERROR_INT_GRPC_STATUS, GRPC_STATUS_UNAVAILABLE);
  channel_control_helper()->UpdateState(
      GRPC_CHANNEL_TRANSIENT_FAILURE, grpc_error_to_absl_status(error),
      absl::make_unique<TransientFailurePicker>(error));
  MaybeDestroyChildPolicyLocked();
}
void EdsLb::UpdatePriorityList(XdsApi::EdsUpdate::PriorityList priority_list) {
  std::map<XdsLocalityName*, size_t , XdsLocalityName::Less>
      locality_child_map;
  std::map<size_t, std::set<XdsLocalityName*>> child_locality_map;
  for (size_t priority = 0; priority < priority_list_.size(); ++priority) {
    size_t child_number = priority_child_numbers_[priority];
    const auto& localities = priority_list_[priority].localities;
    for (const auto& p : localities) {
      XdsLocalityName* locality_name = p.first;
      locality_child_map[locality_name] = child_number;
      child_locality_map[child_number].insert(locality_name);
    }
  }
  std::vector<size_t> priority_child_numbers;
  for (size_t priority = 0; priority < priority_list.size(); ++priority) {
    const auto& localities = priority_list[priority].localities;
    absl::optional<size_t> child_number;
    for (const auto& p : localities) {
      XdsLocalityName* locality_name = p.first;
      if (!child_number.has_value()) {
        auto it = locality_child_map.find(locality_name);
        if (it != locality_child_map.end()) {
          child_number = it->second;
          locality_child_map.erase(it);
          for (XdsLocalityName* old_locality :
               child_locality_map[*child_number]) {
            locality_child_map.erase(old_locality);
          }
        }
      } else {
        locality_child_map.erase(locality_name);
      }
    }
    if (!child_number.has_value()) {
      for (child_number = 0;
           child_locality_map.find(*child_number) != child_locality_map.end();
           ++(*child_number)) {
      }
      child_locality_map[*child_number];
    }
    priority_child_numbers.push_back(*child_number);
  }
  priority_list_ = std::move(priority_list);
  priority_child_numbers_ = std::move(priority_child_numbers);
  UpdateChildPolicyLocked();
}
ServerAddressList EdsLb::CreateChildPolicyAddressesLocked() {
  ServerAddressList addresses;
  for (size_t priority = 0; priority < priority_list_.size(); ++priority) {
    const auto& localities = priority_list_[priority].localities;
    std::string priority_child_name =
        absl::StrCat("child", priority_child_numbers_[priority]);
    for (const auto& p : localities) {
      const auto& locality_name = p.first;
      const auto& locality = p.second;
      std::vector<std::string> hierarchical_path = {
          priority_child_name, locality_name->AsHumanReadableString()};
      for (const auto& endpoint : locality.endpoints) {
        addresses.emplace_back(
            endpoint
                .WithAttribute(kHierarchicalPathAttributeKey,
                               MakeHierarchicalPathAttribute(hierarchical_path))
                .WithAttribute(kXdsLocalityNameAttributeKey,
                               absl::make_unique<XdsLocalityAttribute>(
                                   locality_name->Ref())));
      }
    }
  }
  return addresses;
}
RefCountedPtr<LoadBalancingPolicy::Config>
EdsLb::CreateChildPolicyConfigLocked() {
  const auto lrs_key = GetLrsClusterKey();
  Json::Object priority_children;
  Json::Array priority_priorities;
  for (size_t priority = 0; priority < priority_list_.size(); ++priority) {
    const auto& localities = priority_list_[priority].localities;
    Json::Object weighted_targets;
    for (const auto& p : localities) {
      XdsLocalityName* locality_name = p.first;
      const auto& locality = p.second;
      Json::Object locality_name_json;
      if (!locality_name->region().empty()) {
        locality_name_json["region"] = locality_name->region();
      }
      if (!locality_name->zone().empty()) {
        locality_name_json["zone"] = locality_name->zone();
      }
      if (!locality_name->sub_zone().empty()) {
        locality_name_json["subzone"] = locality_name->sub_zone();
      }
      weighted_targets[locality_name->AsHumanReadableString()] = Json::Object{
          {"weight", locality.lb_weight},
          {"childPolicy", config_->endpoint_picking_policy()},
      };
    }
    Json locality_picking_config = config_->locality_picking_policy();
    Json::Object& config =
        *(*locality_picking_config.mutable_array())[0].mutable_object();
    auto it = config.begin();
    GPR_ASSERT(it != config.end());
    (*it->second.mutable_object())["targets"] = std::move(weighted_targets);
    Json::Array drop_categories;
    for (const auto& category : drop_config_->drop_category_list()) {
      drop_categories.push_back(Json::Object{
          {"category", category.name},
          {"requests_per_million", category.parts_per_million},
      });
    }
    Json::Object xds_cluster_impl_config = {
        {"clusterName", std::string(lrs_key.first)},
        {"childPolicy", std::move(locality_picking_config)},
        {"dropCategories", std::move(drop_categories)},
        {"maxConcurrentRequests", config_->max_concurrent_requests()},
    };
    if (!lrs_key.second.empty()) {
      xds_cluster_impl_config["edsServiceName"] = std::string(lrs_key.second);
    }
    if (config_->lrs_load_reporting_server_name().has_value()) {
      xds_cluster_impl_config["lrsLoadReportingServerName"] =
          config_->lrs_load_reporting_server_name().value();
    }
    Json locality_picking_policy = Json::Array{Json::Object{
        {"xds_cluster_impl_experimental", std::move(xds_cluster_impl_config)},
    }};
    const size_t child_number = priority_child_numbers_[priority];
    std::string child_name = absl::StrCat("child", child_number);
    priority_priorities.emplace_back(child_name);
    priority_children[child_name] = Json::Object{
        {"config", std::move(locality_picking_policy)},
        {"ignore_reresolution_requests", true},
    };
  }
  Json json = Json::Array{Json::Object{
      {"priority_experimental",
       Json::Object{
           {"children", std::move(priority_children)},
           {"priorities", std::move(priority_priorities)},
       }},
  }};
  if (GRPC_TRACE_FLAG_ENABLED(grpc_lb_eds_trace)) {
    std::string json_str = json.Dump( 1);
    gpr_log(GPR_INFO, "[edslb %p] generated config for child policy: %s", this,
            json_str.c_str());
  }
  grpc_error* error = GRPC_ERROR_NONE;
  RefCountedPtr<LoadBalancingPolicy::Config> config =
      LoadBalancingPolicyRegistry::ParseLoadBalancingConfig(json, &error);
  if (error != GRPC_ERROR_NONE) {
    gpr_log(GPR_ERROR,
            "[edslb %p] error parsing generated child policy config -- "
            "will put channel in TRANSIENT_FAILURE: %s",
            this, grpc_error_string(error));
    error = grpc_error_set_int(
        grpc_error_add_child(
            GRPC_ERROR_CREATE_FROM_STATIC_STRING(
                "eds LB policy: error parsing generated child policy config"),
            error),
        GRPC_ERROR_INT_GRPC_STATUS, GRPC_STATUS_INTERNAL);
    channel_control_helper()->UpdateState(
        GRPC_CHANNEL_TRANSIENT_FAILURE, grpc_error_to_absl_status(error),
        absl::make_unique<TransientFailurePicker>(error));
    return nullptr;
  }
  return config;
}
void EdsLb::UpdateChildPolicyLocked() {
  if (shutting_down_) return;
  UpdateArgs update_args;
  update_args.config = CreateChildPolicyConfigLocked();
  if (update_args.config == nullptr) return;
  update_args.addresses = CreateChildPolicyAddressesLocked();
  update_args.args = CreateChildPolicyArgsLocked(args_);
  if (child_policy_ == nullptr) {
    child_policy_ = CreateChildPolicyLocked(update_args.args);
  }
  if (GRPC_TRACE_FLAG_ENABLED(grpc_lb_eds_trace)) {
    gpr_log(GPR_INFO, "[edslb %p] Updating child policy %p", this,
            child_policy_.get());
  }
  child_policy_->UpdateLocked(std::move(update_args));
}
grpc_channel_args* EdsLb::CreateChildPolicyArgsLocked(
    const grpc_channel_args* args) {
  grpc_arg args_to_add[] = {
      grpc_channel_arg_integer_create(
          const_cast<char*>(GRPC_ARG_ADDRESS_IS_BACKEND_FROM_XDS_LOAD_BALANCER),
          1),
      grpc_channel_arg_integer_create(
          const_cast<char*>(GRPC_ARG_INHIBIT_HEALTH_CHECKING), 1),
  };
  return grpc_channel_args_copy_and_add(args, args_to_add,
                                        GPR_ARRAY_SIZE(args_to_add));
}
OrphanablePtr<LoadBalancingPolicy> EdsLb::CreateChildPolicyLocked(
    const grpc_channel_args* args) {
  LoadBalancingPolicy::Args lb_policy_args;
  lb_policy_args.work_serializer = work_serializer();
  lb_policy_args.args = args;
  lb_policy_args.channel_control_helper =
      absl::make_unique<Helper>(Ref(DEBUG_LOCATION, "Helper"));
  OrphanablePtr<LoadBalancingPolicy> lb_policy =
      LoadBalancingPolicyRegistry::CreateLoadBalancingPolicy(
          "priority_experimental", std::move(lb_policy_args));
  if (GPR_UNLIKELY(lb_policy == nullptr)) {
    gpr_log(GPR_ERROR, "[edslb %p] failure creating child policy", this);
    return nullptr;
  }
  if (GRPC_TRACE_FLAG_ENABLED(grpc_lb_eds_trace)) {
    gpr_log(GPR_INFO, "[edslb %p]: Created new child policy %p", this,
            lb_policy.get());
  }
  grpc_pollset_set_add_pollset_set(lb_policy->interested_parties(),
                                   interested_parties());
  return lb_policy;
}
class EdsLbFactory : public LoadBalancingPolicyFactory {
 public:
  OrphanablePtr<LoadBalancingPolicy> CreateLoadBalancingPolicy(
      LoadBalancingPolicy::Args args) const override {
    grpc_error* error = GRPC_ERROR_NONE;
    RefCountedPtr<XdsClient> xds_client = XdsClient::GetOrCreate(&error);
    if (error != GRPC_ERROR_NONE) {
      gpr_log(GPR_ERROR,
              "cannot get XdsClient to instantiate eds LB policy: %s",
              grpc_error_string(error));
      GRPC_ERROR_UNREF(error);
      return nullptr;
    }
    return MakeOrphanable<EdsChildHandler>(std::move(xds_client),
                                           std::move(args));
  }
  const char* name() const override { return kEds; }
  RefCountedPtr<LoadBalancingPolicy::Config> ParseLoadBalancingConfig(
      const Json& json, grpc_error** error) const override {
    GPR_DEBUG_ASSERT(error != nullptr && *error == GRPC_ERROR_NONE);
    if (json.type() == Json::Type::JSON_NULL) {
      *error = GRPC_ERROR_CREATE_FROM_STATIC_STRING(
          "field:loadBalancingPolicy error:eds policy requires configuration. "
          "Please use loadBalancingConfig field of service config instead.");
      return nullptr;
    }
    std::vector<grpc_error*> error_list;
    std::string eds_service_name;
    auto it = json.object_value().find("edsServiceName");
    if (it != json.object_value().end()) {
      if (it->second.type() != Json::Type::STRING) {
        error_list.push_back(GRPC_ERROR_CREATE_FROM_STATIC_STRING(
            "field:edsServiceName error:type should be string"));
      } else {
        eds_service_name = it->second.string_value();
      }
    }
    std::string cluster_name;
    it = json.object_value().find("clusterName");
    if (it == json.object_value().end()) {
      error_list.push_back(GRPC_ERROR_CREATE_FROM_STATIC_STRING(
          "field:clusterName error:required field missing"));
    } else if (it->second.type() != Json::Type::STRING) {
      error_list.push_back(GRPC_ERROR_CREATE_FROM_STATIC_STRING(
          "field:clusterName error:type should be string"));
    } else {
      cluster_name = it->second.string_value();
    }
    absl::optional<std::string> lrs_load_reporting_server_name;
    it = json.object_value().find("lrsLoadReportingServerName");
    if (it != json.object_value().end()) {
      if (it->second.type() != Json::Type::STRING) {
        error_list.push_back(GRPC_ERROR_CREATE_FROM_STATIC_STRING(
            "field:lrsLoadReportingServerName error:type should be string"));
      } else {
        lrs_load_reporting_server_name.emplace(it->second.string_value());
      }
    }
    Json locality_picking_policy;
    it = json.object_value().find("localityPickingPolicy");
    if (it == json.object_value().end()) {
      locality_picking_policy = Json::Array{
          Json::Object{
              {"weighted_target_experimental",
               Json::Object{
                   {"targets", Json::Object()},
               }},
          },
      };
    } else {
      locality_picking_policy = it->second;
    }
    grpc_error* parse_error = GRPC_ERROR_NONE;
    if (LoadBalancingPolicyRegistry::ParseLoadBalancingConfig(
            locality_picking_policy, &parse_error) == nullptr) {
      GPR_DEBUG_ASSERT(parse_error != GRPC_ERROR_NONE);
      error_list.push_back(GRPC_ERROR_CREATE_REFERENCING_FROM_STATIC_STRING(
          "localityPickingPolicy", &parse_error, 1));
      GRPC_ERROR_UNREF(parse_error);
    }
    Json endpoint_picking_policy;
    it = json.object_value().find("endpointPickingPolicy");
    if (it == json.object_value().end()) {
      endpoint_picking_policy = Json::Array{
          Json::Object{
              {"round_robin", Json::Object()},
          },
      };
    } else {
      endpoint_picking_policy = it->second;
    }
    parse_error = GRPC_ERROR_NONE;
    if (LoadBalancingPolicyRegistry::ParseLoadBalancingConfig(
            endpoint_picking_policy, &parse_error) == nullptr) {
      GPR_DEBUG_ASSERT(parse_error != GRPC_ERROR_NONE);
      error_list.push_back(GRPC_ERROR_CREATE_REFERENCING_FROM_STATIC_STRING(
          "endpointPickingPolicy", &parse_error, 1));
      GRPC_ERROR_UNREF(parse_error);
    }
    uint32_t max_concurrent_requests = 1024;
    it = json.object_value().find("max_concurrent_requests");
    if (it != json.object_value().end()) {
      if (it->second.type() != Json::Type::NUMBER) {
        error_list.push_back(GRPC_ERROR_CREATE_FROM_STATIC_STRING(
            "field:max_concurrent_requests error:must be of type number"));
      } else {
        max_concurrent_requests =
            gpr_parse_nonnegative_int(it->second.string_value().c_str());
      }
    }
    if (error_list.empty()) {
      return MakeRefCounted<EdsLbConfig>(
          std::move(cluster_name), std::move(eds_service_name),
          std::move(lrs_load_reporting_server_name),
          std::move(locality_picking_policy),
          std::move(endpoint_picking_policy), max_concurrent_requests);
    } else {
      *error = GRPC_ERROR_CREATE_FROM_VECTOR(
          "eds_experimental LB policy config", &error_list);
      return nullptr;
    }
  }
 private:
  class EdsChildHandler : public ChildPolicyHandler {
   public:
    EdsChildHandler(RefCountedPtr<XdsClient> xds_client, Args args)
        : ChildPolicyHandler(std::move(args), &grpc_lb_eds_trace),
          xds_client_(std::move(xds_client)) {}
    bool ConfigChangeRequiresNewPolicyInstance(
        LoadBalancingPolicy::Config* old_config,
        LoadBalancingPolicy::Config* new_config) const override {
      GPR_ASSERT(old_config->name() == kEds);
      GPR_ASSERT(new_config->name() == kEds);
      EdsLbConfig* old_eds_config = static_cast<EdsLbConfig*>(old_config);
      EdsLbConfig* new_eds_config = static_cast<EdsLbConfig*>(new_config);
      return old_eds_config->cluster_name() != new_eds_config->cluster_name() ||
             old_eds_config->eds_service_name() !=
                 new_eds_config->eds_service_name() ||
             old_eds_config->lrs_load_reporting_server_name() !=
                 new_eds_config->lrs_load_reporting_server_name();
    }
    OrphanablePtr<LoadBalancingPolicy> CreateLoadBalancingPolicy(
        const char* name, LoadBalancingPolicy::Args args) const override {
      return MakeOrphanable<EdsLb>(xds_client_, std::move(args));
    }
   private:
    RefCountedPtr<XdsClient> xds_client_;
  };
};
}
}
void grpc_lb_policy_eds_init() {
  grpc_core::LoadBalancingPolicyRegistry::Builder::
      RegisterLoadBalancingPolicyFactory(
          absl::make_unique<grpc_core::EdsLbFactory>());
}
void grpc_lb_policy_eds_shutdown() {}
