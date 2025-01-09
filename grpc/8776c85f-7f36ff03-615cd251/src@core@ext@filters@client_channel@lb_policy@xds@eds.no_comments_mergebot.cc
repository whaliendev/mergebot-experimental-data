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
#include "src/core/ext/filters/client_channel/xds/xds_client.h"
#include "src/core/ext/filters/client_channel/xds/xds_client_stats.h"
#include "src/core/lib/channel/channel_args.h"
#include "src/core/lib/gprpp/orphanable.h"
#include "src/core/lib/gprpp/ref_counted_ptr.h"
#include "src/core/lib/iomgr/combiner.h"
#include "src/core/lib/iomgr/timer.h"
#include "src/core/lib/iomgr/work_serializer.h"
#include "src/core/lib/uri/uri_parser.h"
#define GRPC_EDS_DEFAULT_FALLBACK_TIMEOUT 10000
namespace grpc_core {
TraceFlag grpc_lb_eds_trace(false, "eds_lb");
namespace {
constexpr char kEds[] = "eds_experimental";
class EdsLbConfig : public LoadBalancingPolicy::Config {
 public:
  EdsLbConfig(std::string cluster_name, std::string eds_service_name,
              absl::optional<std::string> lrs_load_reporting_server_name,
              Json locality_picking_policy, Json endpoint_picking_policy)
      : cluster_name_(std::move(cluster_name)),
        eds_service_name_(std::move(eds_service_name)),
        lrs_load_reporting_server_name_(
            std::move(lrs_load_reporting_server_name)),
        locality_picking_policy_(std::move(locality_picking_policy)),
        endpoint_picking_policy_(std::move(endpoint_picking_policy)) {}
  const char* name() const override { return kEds; }
  const std::string& cluster_name() const { return cluster_name_; }
  const std::string& eds_service_name() const { return eds_service_name_; }
  const absl::optional<std::string>& lrs_load_reporting_server_name() const {
    return lrs_load_reporting_server_name_;
  }
  const Json& locality_picking_policy() const {
    return locality_picking_policy_;
  }
  const Json& endpoint_picking_policy() const {
    return endpoint_picking_policy_;
  }
 private:
  std::string cluster_name_;
  std::string eds_service_name_;
  absl::optional<std::string> lrs_load_reporting_server_name_;
  Json locality_picking_policy_;
  Json endpoint_picking_policy_;
};
class EdsLb : public LoadBalancingPolicy {
 public:
  explicit EdsLb(Args args);
  const char* name() const override { return kEds; }
  void UpdateLocked(UpdateArgs args) override;
  void ResetBackoffLocked() override;
 private:
  class EndpointWatcher;
  class ChildPickerWrapper : public RefCounted<ChildPickerWrapper> {
   public:
    explicit ChildPickerWrapper(std::unique_ptr<SubchannelPicker> picker)
        : picker_(std::move(picker)) {}
    PickResult Pick(PickArgs args) { return picker_->Pick(args); }
   private:
    std::unique_ptr<SubchannelPicker> picker_;
  };
  class DropPicker : public SubchannelPicker {
   public:
    explicit DropPicker(EdsLb* eds_policy);
    PickResult Pick(PickArgs args) override;
   private:
    RefCountedPtr<XdsApi::DropConfig> drop_config_;
    RefCountedPtr<XdsClusterDropStats> drop_stats_;
    RefCountedPtr<ChildPickerWrapper> child_picker_;
  };
  class Helper : public ChannelControlHelper {
   public:
    explicit Helper(RefCountedPtr<EdsLb> eds_policy)
        : eds_policy_(std::move(eds_policy)) {}
    ~Helper() { eds_policy_.reset(DEBUG_LOCATION, "Helper"); }
    RefCountedPtr<SubchannelInterface> CreateSubchannel(
        const grpc_channel_args& args) override;
    void UpdateState(grpc_connectivity_state state,
                     std::unique_ptr<SubchannelPicker> picker) override;
    void RequestReresolution() override {}
    void AddTraceEvent(TraceSeverity severity, StringView message) override;
   private:
    RefCountedPtr<EdsLb> eds_policy_;
  };
  ~EdsLb();
  void ShutdownLocked() override;
  void UpdatePriorityList(XdsApi::PriorityListUpdate priority_list_update);
  void UpdateChildPolicyLocked();
  OrphanablePtr<LoadBalancingPolicy> CreateChildPolicyLocked(
      const grpc_channel_args* args);
  ServerAddressList CreateChildPolicyAddressesLocked();
  RefCountedPtr<Config> CreateChildPolicyConfigLocked();
  grpc_channel_args* CreateChildPolicyArgsLocked(
      const grpc_channel_args* args_in);
  void MaybeUpdateDropPickerLocked();
  const StringView GetEdsResourceName() const {
    if (xds_client_from_channel_ == nullptr) return server_name_;
    if (!config_->eds_service_name().empty()) {
      return config_->eds_service_name();
    }
    return config_->cluster_name();
  }
  std::pair<StringView, StringView> GetLrsClusterKey() const {
    if (xds_client_from_channel_ == nullptr) return {server_name_, nullptr};
    return {config_->cluster_name(), config_->eds_service_name()};
  }
  XdsClient* xds_client() const {
    return xds_client_from_channel_ != nullptr ? xds_client_from_channel_.get()
                                               : xds_client_.get();
  }
  std::string server_name_;
  const grpc_channel_args* args_ = nullptr;
  RefCountedPtr<EdsLbConfig> config_;
  bool shutting_down_ = false;
  RefCountedPtr<XdsClient> xds_client_from_channel_;
  OrphanablePtr<XdsClient> xds_client_;
  EndpointWatcher* endpoint_watcher_ = nullptr;
  XdsApi::PriorityListUpdate priority_list_update_;
  std::vector<size_t > priority_child_numbers_;
  RefCountedPtr<XdsApi::DropConfig> drop_config_;
  RefCountedPtr<XdsClusterDropStats> drop_stats_;
  OrphanablePtr<LoadBalancingPolicy> child_policy_;
  grpc_connectivity_state child_state_;
  RefCountedPtr<ChildPickerWrapper> child_picker_;
};
EdsLb::DropPicker::DropPicker(EdsLb* eds_policy)
    : drop_config_(eds_policy->drop_config_),
      drop_stats_(eds_policy->drop_stats_),
      child_picker_(eds_policy->child_picker_) {
  if (GRPC_TRACE_FLAG_ENABLED(grpc_lb_eds_trace)) {
    gpr_log(GPR_INFO, "[edslb %p] constructed new drop picker %p", eds_policy,
            this);
  }
}
EdsLb::PickResult EdsLb::DropPicker::Pick(PickArgs args) {
  const std::string* drop_category;
  if (drop_config_->ShouldDrop(&drop_category)) {
    if (drop_stats_ != nullptr) drop_stats_->AddCallDropped(*drop_category);
    PickResult result;
    result.type = PickResult::PICK_COMPLETE;
    return result;
  }
  if (child_picker_ == nullptr) {
    PickResult result;
    result.type = PickResult::PICK_FAILED;
    result.error =
        grpc_error_set_int(GRPC_ERROR_CREATE_FROM_STATIC_STRING(
                               "eds drop picker not given any child picker"),
                           GRPC_ERROR_INT_GRPC_STATUS, GRPC_STATUS_INTERNAL);
    return result;
  }
  return child_picker_->Pick(args);
}
RefCountedPtr<SubchannelInterface> EdsLb::Helper::CreateSubchannel(
    const grpc_channel_args& args) {
  if (eds_policy_->shutting_down_) return nullptr;
  return eds_policy_->channel_control_helper()->CreateSubchannel(args);
}
void EdsLb::Helper::UpdateState(grpc_connectivity_state state,
                                std::unique_ptr<SubchannelPicker> picker) {
  if (eds_policy_->shutting_down_) return;
  if (GRPC_TRACE_FLAG_ENABLED(grpc_lb_eds_trace)) {
    gpr_log(GPR_INFO, "[edslb %p] child policy updated state=%s picker=%p",
            eds_policy_.get(), ConnectivityStateName(state), picker.get());
  }
  eds_policy_->child_state_ = state;
  eds_policy_->child_picker_ =
      MakeRefCounted<ChildPickerWrapper>(std::move(picker));
  eds_policy_->MaybeUpdateDropPickerLocked();
}
void EdsLb::Helper::AddTraceEvent(TraceSeverity severity, StringView message) {
  if (eds_policy_->shutting_down_) return;
  eds_policy_->channel_control_helper()->AddTraceEvent(severity, message);
}
class EdsLb::EndpointWatcher : public XdsClient::EndpointWatcherInterface {
 public:
  explicit EndpointWatcher(RefCountedPtr<EdsLb> eds_policy)
      : eds_policy_(std::move(eds_policy)) {}
  ~EndpointWatcher() { eds_policy_.reset(DEBUG_LOCATION, "EndpointWatcher"); }
  void OnEndpointChanged(XdsApi::EdsUpdate update) override {
    if (GRPC_TRACE_FLAG_ENABLED(grpc_lb_eds_trace)) {
      gpr_log(GPR_INFO, "[edslb %p] Received EDS update from xds client",
              eds_policy_.get());
    }
    const bool drop_config_changed =
        eds_policy_->drop_config_ == nullptr ||
        *eds_policy_->drop_config_ != *update.drop_config;
    if (drop_config_changed) {
      if (GRPC_TRACE_FLAG_ENABLED(grpc_lb_eds_trace)) {
        gpr_log(GPR_INFO, "[edslb %p] Updating drop config", eds_policy_.get());
      }
      eds_policy_->drop_config_ = std::move(update.drop_config);
      eds_policy_->MaybeUpdateDropPickerLocked();
    } else if (GRPC_TRACE_FLAG_ENABLED(grpc_lb_eds_trace)) {
      gpr_log(GPR_INFO, "[edslb %p] Drop config unchanged, ignoring",
              eds_policy_.get());
    }
    if (eds_policy_->child_policy_ == nullptr ||
        eds_policy_->priority_list_update_ != update.priority_list_update) {
      if (GRPC_TRACE_FLAG_ENABLED(grpc_lb_eds_trace)) {
        gpr_log(GPR_INFO, "[edslb %p] Updating priority list",
                eds_policy_.get());
      }
      eds_policy_->UpdatePriorityList(std::move(update.priority_list_update));
    } else if (GRPC_TRACE_FLAG_ENABLED(grpc_lb_eds_trace)) {
      gpr_log(GPR_INFO, "[edslb %p] Priority list unchanged, ignoring",
              eds_policy_.get());
    }
  }
  void OnError(grpc_error* error) override {
    gpr_log(GPR_ERROR, "[edslb %p] xds watcher reported error: %s",
            eds_policy_.get(), grpc_error_string(error));
    if (eds_policy_->child_policy_ == nullptr) {
      eds_policy_->channel_control_helper()->UpdateState(
          GRPC_CHANNEL_TRANSIENT_FAILURE,
          absl::make_unique<TransientFailurePicker>(error));
    } else {
      GRPC_ERROR_UNREF(error);
    }
  }
 private:
  RefCountedPtr<EdsLb> eds_policy_;
};
EdsLb::EdsLb(Args args)
    : LoadBalancingPolicy(std::move(args)),
      xds_client_from_channel_(XdsClient::GetFromChannelArgs(*args.args)) {
  if (GRPC_TRACE_FLAG_ENABLED(grpc_lb_eds_trace)) {
    gpr_log(GPR_INFO, "[edslb %p] created -- xds client from channel: %p", this,
            xds_client_from_channel_.get());
  }
  const grpc_arg* arg = grpc_channel_args_find(args.args, GRPC_ARG_SERVER_URI);
  const char* server_uri = grpc_channel_arg_get_string(arg);
  GPR_ASSERT(server_uri != nullptr);
  grpc_uri* uri = grpc_uri_parse(server_uri, true);
  GPR_ASSERT(uri->path[0] != '\0');
  server_name_ = uri->path[0] == '/' ? uri->path + 1 : uri->path;
  if (GRPC_TRACE_FLAG_ENABLED(grpc_lb_eds_trace)) {
    gpr_log(GPR_INFO, "[edslb %p] server name from channel: %s", this,
            server_name_.c_str());
  }
  grpc_uri_destroy(uri);
}
EdsLb::~EdsLb() {
  if (GRPC_TRACE_FLAG_ENABLED(grpc_lb_eds_trace)) {
    gpr_log(GPR_INFO, "[edslb %p] destroying xds LB policy", this);
  }
  grpc_channel_args_destroy(args_);
}
void EdsLb::ShutdownLocked() {
  if (GRPC_TRACE_FLAG_ENABLED(grpc_lb_eds_trace)) {
    gpr_log(GPR_INFO, "[edslb %p] shutting down", this);
  }
  shutting_down_ = true;
  child_picker_.reset();
  if (child_policy_ != nullptr) {
    grpc_pollset_set_del_pollset_set(child_policy_->interested_parties(),
                                     interested_parties());
    child_policy_.reset();
  }
  drop_stats_.reset();
  if (xds_client_from_channel_ != nullptr) {
    if (config_ != nullptr) {
      if (GRPC_TRACE_FLAG_ENABLED(grpc_lb_eds_trace)) {
        gpr_log(GPR_INFO, "[edslb %p] cancelling xds watch for %s", this,
                std::string(GetEdsResourceName()).c_str());
      }
      xds_client()->CancelEndpointDataWatch(GetEdsResourceName(),
                                            endpoint_watcher_);
    }
    xds_client_from_channel_.reset();
  }
  xds_client_.reset();
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
  if (is_initial_update) {
    if (xds_client_from_channel_ == nullptr) {
      grpc_error* error = GRPC_ERROR_NONE;
      xds_client_ = MakeOrphanable<XdsClient>(
          work_serializer(), interested_parties(), GetEdsResourceName(),
          nullptr , *args_, &error);
      GPR_ASSERT(error == GRPC_ERROR_NONE);
      if (GRPC_TRACE_FLAG_ENABLED(grpc_lb_eds_trace)) {
        gpr_log(GPR_INFO, "[edslb %p] Created xds client %p", this,
                xds_client_.get());
      }
    }
  }
  if (is_initial_update || config_->lrs_load_reporting_server_name() !=
                               old_config->lrs_load_reporting_server_name()) {
    drop_stats_.reset();
    if (config_->lrs_load_reporting_server_name().has_value()) {
      const auto key = GetLrsClusterKey();
      drop_stats_ = xds_client()->AddClusterDropStats(
          config_->lrs_load_reporting_server_name().value(),
          key.first , key.second );
    }
    MaybeUpdateDropPickerLocked();
  }
  if (child_policy_ != nullptr) UpdateChildPolicyLocked();
  if (is_initial_update) {
    if (GRPC_TRACE_FLAG_ENABLED(grpc_lb_eds_trace)) {
      gpr_log(GPR_INFO, "[edslb %p] starting xds watch for %s", this,
              std::string(GetEdsResourceName()).c_str());
    }
    auto watcher = absl::make_unique<EndpointWatcher>(
        Ref(DEBUG_LOCATION, "EndpointWatcher"));
    endpoint_watcher_ = watcher.get();
    xds_client()->WatchEndpointData(GetEdsResourceName(), std::move(watcher));
  }
}
void EdsLb::ResetBackoffLocked() {
  if (xds_client_ != nullptr) xds_client_->ResetBackoff();
  if (child_policy_ != nullptr) {
    child_policy_->ResetBackoffLocked();
  }
}
void EdsLb::UpdatePriorityList(
    XdsApi::PriorityListUpdate priority_list_update) {
  std::map<XdsLocalityName*, size_t , XdsLocalityName::Less>
      locality_child_map;
  std::map<size_t, std::set<XdsLocalityName*>> child_locality_map;
  for (uint32_t priority = 0; priority < priority_list_update_.size();
       ++priority) {
    auto* locality_map = priority_list_update_.Find(priority);
    GPR_ASSERT(locality_map != nullptr);
    size_t child_number = priority_child_numbers_[priority];
    for (const auto& p : locality_map->localities) {
      XdsLocalityName* locality_name = p.first.get();
      locality_child_map[locality_name] = child_number;
      child_locality_map[child_number].insert(locality_name);
    }
  }
  std::vector<size_t> priority_child_numbers;
  for (uint32_t priority = 0; priority < priority_list_update.size();
       ++priority) {
    auto* locality_map = priority_list_update.Find(priority);
    GPR_ASSERT(locality_map != nullptr);
    absl::optional<size_t> child_number;
    for (const auto& p : locality_map->localities) {
      XdsLocalityName* locality_name = p.first.get();
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
           ++(*child_number))
        ;
      child_locality_map[*child_number];
    }
    priority_child_numbers.push_back(*child_number);
  }
  priority_list_update_ = std::move(priority_list_update);
  priority_child_numbers_ = std::move(priority_child_numbers);
  UpdateChildPolicyLocked();
}
ServerAddressList EdsLb::CreateChildPolicyAddressesLocked() {
  ServerAddressList addresses;
  for (uint32_t priority = 0; priority < priority_list_update_.size();
       ++priority) {
    std::string priority_child_name =
        absl::StrCat("child", priority_child_numbers_[priority]);
    const auto* locality_map = priority_list_update_.Find(priority);
    GPR_ASSERT(locality_map != nullptr);
    for (const auto& p : locality_map->localities) {
      const auto& locality_name = p.first;
      const auto& locality = p.second;
      std::vector<std::string> hierarchical_path = {
          priority_child_name, locality_name->AsHumanReadableString()};
      for (size_t i = 0; i < locality.serverlist.size(); ++i) {
        const ServerAddress& address = locality.serverlist[i];
        grpc_arg new_arg = MakeHierarchicalPathArg(hierarchical_path);
        grpc_channel_args* args =
            grpc_channel_args_copy_and_add(address.args(), &new_arg, 1);
        addresses.emplace_back(address.address(), args);
      }
    }
  }
  return addresses;
}
RefCountedPtr<LoadBalancingPolicy::Config>
EdsLb::CreateChildPolicyConfigLocked() {
  Json::Object priority_children;
  Json::Array priority_priorities;
  for (uint32_t priority = 0; priority < priority_list_update_.size();
       ++priority) {
    const auto* locality_map = priority_list_update_.Find(priority);
    GPR_ASSERT(locality_map != nullptr);
    Json::Object weighted_targets;
    for (const auto& p : locality_map->localities) {
      XdsLocalityName* locality_name = p.first.get();
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
      Json endpoint_picking_policy;
      if (config_->lrs_load_reporting_server_name().has_value()) {
        const auto key = GetLrsClusterKey();
        Json::Object lrs_config = {
            {"clusterName", std::string(key.first)},
            {"locality", std::move(locality_name_json)},
            {"lrsLoadReportingServerName",
             config_->lrs_load_reporting_server_name().value()},
            {"childPolicy", config_->endpoint_picking_policy()},
        };
        if (!key.second.empty()) {
          lrs_config["edsServiceName"] = std::string(key.second);
        }
        endpoint_picking_policy = Json::Array{Json::Object{
            {"lrs_experimental", std::move(lrs_config)},
        }};
      } else {
        endpoint_picking_policy = config_->endpoint_picking_policy();
      }
      weighted_targets[locality_name->AsHumanReadableString()] = Json::Object{
          {"weight", locality.lb_weight},
          {"childPolicy", std::move(endpoint_picking_policy)},
      };
    }
    const size_t child_number = priority_child_numbers_[priority];
    std::string child_name = absl::StrCat("child", child_number);
    priority_priorities.emplace_back(child_name);
    Json locality_picking_config = config_->locality_picking_policy();
    Json::Object& config =
        *(*locality_picking_config.mutable_array())[0].mutable_object();
    auto it = config.begin();
    GPR_ASSERT(it != config.end());
    (*it->second.mutable_object())["targets"] = std::move(weighted_targets);
    priority_children[child_name] = Json::Object{
        {"config", std::move(locality_picking_config)},
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
        GRPC_CHANNEL_TRANSIENT_FAILURE,
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
  absl::InlinedVector<grpc_arg, 3> args_to_add = {
      grpc_channel_arg_integer_create(
          const_cast<char*>(GRPC_ARG_ADDRESS_IS_BACKEND_FROM_XDS_LOAD_BALANCER),
          1),
      grpc_channel_arg_integer_create(
          const_cast<char*>(GRPC_ARG_INHIBIT_HEALTH_CHECKING), 1),
  };
  if (xds_client_from_channel_ == nullptr) {
    args_to_add.emplace_back(xds_client_->MakeChannelArg());
  }
  return grpc_channel_args_copy_and_add(args, args_to_add.data(),
                                        args_to_add.size());
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
void EdsLb::MaybeUpdateDropPickerLocked() {
  if (drop_config_ != nullptr && drop_config_->drop_all()) {
    channel_control_helper()->UpdateState(GRPC_CHANNEL_READY,
                                          absl::make_unique<DropPicker>(this));
    return;
  }
  if (child_picker_ != nullptr) {
    channel_control_helper()->UpdateState(child_state_,
                                          absl::make_unique<DropPicker>(this));
  }
}
class EdsLbFactory : public LoadBalancingPolicyFactory {
 public:
  OrphanablePtr<LoadBalancingPolicy> CreateLoadBalancingPolicy(
      LoadBalancingPolicy::Args args) const override {
    return MakeOrphanable<EdsChildHandler>(std::move(args), &grpc_lb_eds_trace);
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
    if (error_list.empty()) {
      return MakeRefCounted<EdsLbConfig>(
          std::move(cluster_name), std::move(eds_service_name),
          std::move(lrs_load_reporting_server_name),
          std::move(locality_picking_policy),
          std::move(endpoint_picking_policy));
    } else {
      *error = GRPC_ERROR_CREATE_FROM_VECTOR(
          "eds_experimental LB policy config", &error_list);
      return nullptr;
    }
  }
 private:
  class EdsChildHandler : public ChildPolicyHandler {
   public:
    EdsChildHandler(Args args, TraceFlag* tracer)
        : ChildPolicyHandler(std::move(args), tracer) {}
    bool ConfigChangeRequiresNewPolicyInstance(
        LoadBalancingPolicy::Config* old_config,
        LoadBalancingPolicy::Config* new_config) const override {
      GPR_ASSERT(old_config->name() == kEds);
      GPR_ASSERT(new_config->name() == kEds);
      EdsLbConfig* old_eds_config = static_cast<EdsLbConfig*>(old_config);
      EdsLbConfig* new_eds_config = static_cast<EdsLbConfig*>(new_config);
      return old_eds_config->cluster_name() != new_eds_config->cluster_name() ||
             old_eds_config->eds_service_name() !=
                 new_eds_config->eds_service_name();
    }
    OrphanablePtr<LoadBalancingPolicy> CreateLoadBalancingPolicy(
        const char* name, LoadBalancingPolicy::Args args) const override {
      return MakeOrphanable<EdsLb>(std::move(args));
    }
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
