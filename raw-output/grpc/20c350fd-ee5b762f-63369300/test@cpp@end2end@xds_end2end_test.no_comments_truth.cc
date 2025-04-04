#include <deque>
#include <memory>
#include <mutex>
#include <numeric>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include "absl/memory/memory.h"
#include "absl/strings/str_cat.h"
#include "absl/types/optional.h"
#include <grpc/grpc.h>
#include <grpc/support/alloc.h>
#include <grpc/support/log.h>
#include <grpc/support/time.h>
#include <grpcpp/channel.h>
#include <grpcpp/client_context.h>
#include <grpcpp/create_channel.h>
#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>
#include "src/core/ext/filters/client_channel/backup_poller.h"
#include "src/core/ext/filters/client_channel/resolver/fake/fake_resolver.h"
#include "src/core/ext/filters/client_channel/server_address.h"
#include "src/core/ext/xds/xds_api.h"
#include "src/core/ext/xds/xds_channel_args.h"
#include "src/core/ext/xds/xds_client.h"
#include "src/core/lib/channel/channel_args.h"
#include "src/core/lib/gpr/env.h"
#include "src/core/lib/gpr/tmpfile.h"
#include "src/core/lib/gprpp/map.h"
#include "src/core/lib/gprpp/ref_counted_ptr.h"
#include "src/core/lib/gprpp/sync.h"
#include "src/core/lib/iomgr/parse_address.h"
#include "src/core/lib/iomgr/sockaddr.h"
#include "src/core/lib/security/credentials/fake/fake_credentials.h"
#include "src/cpp/client/secure_credentials.h"
#include "src/cpp/server/secure_server_credentials.h"
#include "test/core/util/port.h"
#include "test/core/util/resolve_localhost_ip46.h"
#include "test/core/util/test_config.h"
#include "test/cpp/end2end/test_service_impl.h"
#include "src/proto/grpc/testing/echo.grpc.pb.h"
#include "src/proto/grpc/testing/xds/ads_for_test.grpc.pb.h"
#include "src/proto/grpc/testing/xds/cds_for_test.grpc.pb.h"
#include "src/proto/grpc/testing/xds/eds_for_test.grpc.pb.h"
#include "src/proto/grpc/testing/xds/lds_rds_for_test.grpc.pb.h"
#include "src/proto/grpc/testing/xds/lrs_for_test.grpc.pb.h"
#include "src/proto/grpc/testing/xds/v3/ads.grpc.pb.h"
#include "src/proto/grpc/testing/xds/v3/cluster.grpc.pb.h"
#include "src/proto/grpc/testing/xds/v3/discovery.grpc.pb.h"
#include "src/proto/grpc/testing/xds/v3/endpoint.grpc.pb.h"
#include "src/proto/grpc/testing/xds/v3/http_connection_manager.grpc.pb.h"
#include "src/proto/grpc/testing/xds/v3/listener.grpc.pb.h"
#include "src/proto/grpc/testing/xds/v3/lrs.grpc.pb.h"
#include "src/proto/grpc/testing/xds/v3/route.grpc.pb.h"
namespace grpc {
namespace testing {
namespace {
using std::chrono::system_clock;
using ::envoy::config::cluster::v3::CircuitBreakers;
using ::envoy::config::cluster::v3::Cluster;
using ::envoy::config::cluster::v3::RoutingPriority;
using ::envoy::config::endpoint::v3::ClusterLoadAssignment;
using ::envoy::config::endpoint::v3::HealthStatus;
using ::envoy::config::listener::v3::Listener;
using ::envoy::config::route::v3::RouteConfiguration;
using ::envoy::extensions::filters::network::http_connection_manager::v3::
    HttpConnectionManager;
using ::envoy::type::v3::FractionalPercent;
constexpr char kLdsTypeUrl[] =
    "type.googleapis.com/envoy.config.listener.v3.Listener";
constexpr char kRdsTypeUrl[] =
    "type.googleapis.com/envoy.config.route.v3.RouteConfiguration";
constexpr char kCdsTypeUrl[] =
    "type.googleapis.com/envoy.config.cluster.v3.Cluster";
constexpr char kEdsTypeUrl[] =
    "type.googleapis.com/envoy.config.endpoint.v3.ClusterLoadAssignment";
constexpr char kLdsV2TypeUrl[] = "type.googleapis.com/envoy.api.v2.Listener";
constexpr char kRdsV2TypeUrl[] =
    "type.googleapis.com/envoy.api.v2.RouteConfiguration";
constexpr char kCdsV2TypeUrl[] = "type.googleapis.com/envoy.api.v2.Cluster";
constexpr char kEdsV2TypeUrl[] =
    "type.googleapis.com/envoy.api.v2.ClusterLoadAssignment";
constexpr char kDefaultLocalityRegion[] = "xds_default_locality_region";
constexpr char kDefaultLocalityZone[] = "xds_default_locality_zone";
constexpr char kLbDropType[] = "lb";
constexpr char kThrottleDropType[] = "throttle";
constexpr char kServerName[] = "server.example.com";
constexpr char kDefaultRouteConfigurationName[] = "route_config_name";
constexpr char kDefaultClusterName[] = "cluster_name";
constexpr char kDefaultEdsServiceName[] = "eds_service_name";
constexpr int kDefaultLocalityWeight = 3;
constexpr int kDefaultLocalityPriority = 0;
constexpr char kRequestMessage[] = "Live long and prosper.";
constexpr char kDefaultServiceConfig[] =
    "{\n"
    "  \"loadBalancingConfig\":[\n"
    "    { \"does_not_exist\":{} },\n"
    "    { \"eds_experimental\":{\n"
    "      \"clusterName\": \"server.example.com\",\n"
    "      \"lrsLoadReportingServerName\": \"\"\n"
    "    } }\n"
    "  ]\n"
    "}";
constexpr char kDefaultServiceConfigWithoutLoadReporting[] =
    "{\n"
    "  \"loadBalancingConfig\":[\n"
    "    { \"does_not_exist\":{} },\n"
    "    { \"eds_experimental\":{\n"
    "      \"clusterName\": \"server.example.com\"\n"
    "    } }\n"
    "  ]\n"
    "}";
constexpr char kBootstrapFileV3[] =
    "{\n"
    "  \"xds_servers\": [\n"
    "    {\n"
    "      \"server_uri\": \"fake:///xds_server\",\n"
    "      \"channel_creds\": [\n"
    "        {\n"
    "          \"type\": \"fake\"\n"
    "        }\n"
    "      ],\n"
    "      \"server_features\": [\"xds_v3\"]\n"
    "    }\n"
    "  ],\n"
    "  \"node\": {\n"
    "    \"id\": \"xds_end2end_test\",\n"
    "    \"cluster\": \"test\",\n"
    "    \"metadata\": {\n"
    "      \"foo\": \"bar\"\n"
    "    },\n"
    "    \"locality\": {\n"
    "      \"region\": \"corp\",\n"
    "      \"zone\": \"svl\",\n"
    "      \"subzone\": \"mp3\"\n"
    "    }\n"
    "  }\n"
    "}\n";
constexpr char kBootstrapFileV2[] =
    "{\n"
    "  \"xds_servers\": [\n"
    "    {\n"
    "      \"server_uri\": \"fake:///xds_server\",\n"
    "      \"channel_creds\": [\n"
    "        {\n"
    "          \"type\": \"fake\"\n"
    "        }\n"
    "      ]\n"
    "    }\n"
    "  ],\n"
    "  \"node\": {\n"
    "    \"id\": \"xds_end2end_test\",\n"
    "    \"cluster\": \"test\",\n"
    "    \"metadata\": {\n"
    "      \"foo\": \"bar\"\n"
    "    },\n"
    "    \"locality\": {\n"
    "      \"region\": \"corp\",\n"
    "      \"zone\": \"svl\",\n"
    "      \"subzone\": \"mp3\"\n"
    "    }\n"
    "  }\n"
    "}\n";
char* g_bootstrap_file_v3;
char* g_bootstrap_file_v2;
void WriteBootstrapFiles() {
  char* bootstrap_file;
  FILE* out = gpr_tmpfile("xds_bootstrap_v3", &bootstrap_file);
  fputs(kBootstrapFileV3, out);
  fclose(out);
  g_bootstrap_file_v3 = bootstrap_file;
  out = gpr_tmpfile("xds_bootstrap_v2", &bootstrap_file);
  fputs(kBootstrapFileV2, out);
  fclose(out);
  g_bootstrap_file_v2 = bootstrap_file;
}
class PortSaver {
 public:
  int GetPort() {
    if (idx_ >= ports_.size()) {
      ports_.push_back(grpc_pick_unused_port_or_die());
    }
    return ports_[idx_++];
  }
  void Reset() { idx_ = 0; }
 private:
  std::vector<int> ports_;
  size_t idx_ = 0;
};
PortSaver* g_port_saver = nullptr;
template <typename ServiceType>
class CountedService : public ServiceType {
 public:
  size_t request_count() {
    grpc_core::MutexLock lock(&mu_);
    return request_count_;
  }
  size_t response_count() {
    grpc_core::MutexLock lock(&mu_);
    return response_count_;
  }
  void IncreaseResponseCount() {
    grpc_core::MutexLock lock(&mu_);
    ++response_count_;
  }
  void IncreaseRequestCount() {
    grpc_core::MutexLock lock(&mu_);
    ++request_count_;
  }
  void ResetCounters() {
    grpc_core::MutexLock lock(&mu_);
    request_count_ = 0;
    response_count_ = 0;
  }
 private:
  grpc_core::Mutex mu_;
  size_t request_count_ = 0;
  size_t response_count_ = 0;
};
const char g_kCallCredsMdKey[] = "Balancer should not ...";
const char g_kCallCredsMdValue[] = "... receive me";
template <typename RpcService>
class BackendServiceImpl
    : public CountedService<TestMultipleServiceImpl<RpcService>> {
 public:
  BackendServiceImpl() {}
  Status Echo(ServerContext* context, const EchoRequest* request,
              EchoResponse* response) override {
    auto call_credentials_entry =
        context->client_metadata().find(g_kCallCredsMdKey);
    EXPECT_NE(call_credentials_entry, context->client_metadata().end());
    if (call_credentials_entry != context->client_metadata().end()) {
      EXPECT_EQ(call_credentials_entry->second, g_kCallCredsMdValue);
    }
    CountedService<TestMultipleServiceImpl<RpcService>>::IncreaseRequestCount();
    const auto status =
        TestMultipleServiceImpl<RpcService>::Echo(context, request, response);
    CountedService<
        TestMultipleServiceImpl<RpcService>>::IncreaseResponseCount();
    AddClient(context->peer());
    return status;
  }
  Status Echo1(ServerContext* context, const EchoRequest* request,
               EchoResponse* response) override {
    return Echo(context, request, response);
  }
  Status Echo2(ServerContext* context, const EchoRequest* request,
               EchoResponse* response) override {
    return Echo(context, request, response);
  }
  void Start() {}
  void Shutdown() {}
  std::set<std::string> clients() {
    grpc_core::MutexLock lock(&clients_mu_);
    return clients_;
  }
 private:
  void AddClient(const std::string& client) {
    grpc_core::MutexLock lock(&clients_mu_);
    clients_.insert(client);
  }
  grpc_core::Mutex clients_mu_;
  std::set<std::string> clients_;
};
class ClientStats {
 public:
  struct LocalityStats {
    LocalityStats() {}
    template <class UpstreamLocalityStats>
    LocalityStats(const UpstreamLocalityStats& upstream_locality_stats)
        : total_successful_requests(
              upstream_locality_stats.total_successful_requests()),
          total_requests_in_progress(
              upstream_locality_stats.total_requests_in_progress()),
          total_error_requests(upstream_locality_stats.total_error_requests()),
          total_issued_requests(
              upstream_locality_stats.total_issued_requests()) {}
    LocalityStats& operator+=(const LocalityStats& other) {
      total_successful_requests += other.total_successful_requests;
      total_requests_in_progress += other.total_requests_in_progress;
      total_error_requests += other.total_error_requests;
      total_issued_requests += other.total_issued_requests;
      return *this;
    }
    uint64_t total_successful_requests = 0;
    uint64_t total_requests_in_progress = 0;
    uint64_t total_error_requests = 0;
    uint64_t total_issued_requests = 0;
  };
  ClientStats() {}
  template <class ClusterStats>
  explicit ClientStats(const ClusterStats& cluster_stats)
      : cluster_name_(cluster_stats.cluster_name()),
        total_dropped_requests_(cluster_stats.total_dropped_requests()) {
    for (const auto& input_locality_stats :
         cluster_stats.upstream_locality_stats()) {
      locality_stats_.emplace(input_locality_stats.locality().sub_zone(),
                              LocalityStats(input_locality_stats));
    }
    for (const auto& input_dropped_requests :
         cluster_stats.dropped_requests()) {
      dropped_requests_.emplace(input_dropped_requests.category(),
                                input_dropped_requests.dropped_count());
    }
  }
  const std::string& cluster_name() const { return cluster_name_; }
  const std::map<std::string, LocalityStats>& locality_stats() const {
    return locality_stats_;
  }
  uint64_t total_successful_requests() const {
    uint64_t sum = 0;
    for (auto& p : locality_stats_) {
      sum += p.second.total_successful_requests;
    }
    return sum;
  }
  uint64_t total_requests_in_progress() const {
    uint64_t sum = 0;
    for (auto& p : locality_stats_) {
      sum += p.second.total_requests_in_progress;
    }
    return sum;
  }
  uint64_t total_error_requests() const {
    uint64_t sum = 0;
    for (auto& p : locality_stats_) {
      sum += p.second.total_error_requests;
    }
    return sum;
  }
  uint64_t total_issued_requests() const {
    uint64_t sum = 0;
    for (auto& p : locality_stats_) {
      sum += p.second.total_issued_requests;
    }
    return sum;
  }
  uint64_t total_dropped_requests() const { return total_dropped_requests_; }
  uint64_t dropped_requests(const std::string& category) const {
    auto iter = dropped_requests_.find(category);
    GPR_ASSERT(iter != dropped_requests_.end());
    return iter->second;
  }
  ClientStats& operator+=(const ClientStats& other) {
    for (const auto& p : other.locality_stats_) {
      locality_stats_[p.first] += p.second;
    }
    total_dropped_requests_ += other.total_dropped_requests_;
    for (const auto& p : other.dropped_requests_) {
      dropped_requests_[p.first] += p.second;
    }
    return *this;
  }
 private:
  std::string cluster_name_;
  std::map<std::string, LocalityStats> locality_stats_;
  uint64_t total_dropped_requests_ = 0;
  std::map<std::string, uint64_t> dropped_requests_;
};
class AdsServiceImpl : public std::enable_shared_from_this<AdsServiceImpl> {
 public:
  struct ResponseState {
    enum State { NOT_SENT, SENT, ACKED, NACKED };
    State state = NOT_SENT;
    std::string error_message;
  };
  struct EdsResourceArgs {
    struct Locality {
      Locality(std::string sub_zone, std::vector<int> ports,
               int lb_weight = kDefaultLocalityWeight,
               int priority = kDefaultLocalityPriority,
               std::vector<HealthStatus> health_statuses = {})
          : sub_zone(std::move(sub_zone)),
            ports(std::move(ports)),
            lb_weight(lb_weight),
            priority(priority),
            health_statuses(std::move(health_statuses)) {}
      const std::string sub_zone;
      std::vector<int> ports;
      int lb_weight;
      int priority;
      std::vector<HealthStatus> health_statuses;
    };
    EdsResourceArgs() = default;
    explicit EdsResourceArgs(std::vector<Locality> locality_list)
        : locality_list(std::move(locality_list)) {}
    std::vector<Locality> locality_list;
    std::map<std::string, uint32_t> drop_categories;
    FractionalPercent::DenominatorType drop_denominator =
        FractionalPercent::MILLION;
  };
  AdsServiceImpl()
      : v2_rpc_service_(this, true),
        v3_rpc_service_(this, false) {}
  bool seen_v2_client() const { return seen_v2_client_; }
  bool seen_v3_client() const { return seen_v3_client_; }
  ::envoy::service::discovery::v2::AggregatedDiscoveryService::Service*
  v2_rpc_service() {
    return &v2_rpc_service_;
  }
  ::envoy::service::discovery::v3::AggregatedDiscoveryService::Service*
  v3_rpc_service() {
    return &v3_rpc_service_;
  }
  ResponseState lds_response_state() {
    grpc_core::MutexLock lock(&ads_mu_);
    return resource_type_response_state_[kLdsTypeUrl];
  }
  ResponseState rds_response_state() {
    grpc_core::MutexLock lock(&ads_mu_);
    return resource_type_response_state_[kRdsTypeUrl];
  }
  ResponseState cds_response_state() {
    grpc_core::MutexLock lock(&ads_mu_);
    return resource_type_response_state_[kCdsTypeUrl];
  }
  ResponseState eds_response_state() {
    grpc_core::MutexLock lock(&ads_mu_);
    return resource_type_response_state_[kEdsTypeUrl];
  }
  void SetResourceIgnore(const std::string& type_url) {
    grpc_core::MutexLock lock(&ads_mu_);
    resource_types_to_ignore_.emplace(type_url);
  }
  void SetResourceMinVersion(const std::string& type_url, int version) {
    grpc_core::MutexLock lock(&ads_mu_);
    resource_type_min_versions_[type_url] = version;
  }
  void UnsetResource(const std::string& type_url, const std::string& name) {
    grpc_core::MutexLock lock(&ads_mu_);
    ResourceTypeState& resource_type_state = resource_map_[type_url];
    ++resource_type_state.resource_type_version;
    ResourceState& resource_state = resource_type_state.resource_name_map[name];
    resource_state.resource_type_version =
        resource_type_state.resource_type_version;
    resource_state.resource.reset();
    gpr_log(GPR_INFO,
            "ADS[%p]: Unsetting %s resource %s; resource_type_version now %u",
            this, type_url.c_str(), name.c_str(),
            resource_type_state.resource_type_version);
    for (SubscriptionState* subscription : resource_state.subscriptions) {
      subscription->update_queue->emplace_back(type_url, name);
    }
  }
  void SetResource(google::protobuf::Any resource, const std::string& type_url,
                   const std::string& name) {
    grpc_core::MutexLock lock(&ads_mu_);
    ResourceTypeState& resource_type_state = resource_map_[type_url];
    ++resource_type_state.resource_type_version;
    ResourceState& resource_state = resource_type_state.resource_name_map[name];
    resource_state.resource_type_version =
        resource_type_state.resource_type_version;
    resource_state.resource = std::move(resource);
    gpr_log(GPR_INFO,
            "ADS[%p]: Updating %s resource %s; resource_type_version now %u",
            this, type_url.c_str(), name.c_str(),
            resource_type_state.resource_type_version);
    for (SubscriptionState* subscription : resource_state.subscriptions) {
      subscription->update_queue->emplace_back(type_url, name);
    }
  }
  void SetLdsResource(const Listener& listener) {
    google::protobuf::Any resource;
    resource.PackFrom(listener);
    SetResource(std::move(resource), kLdsTypeUrl, listener.name());
  }
  void SetRdsResource(const RouteConfiguration& route) {
    google::protobuf::Any resource;
    resource.PackFrom(route);
    SetResource(std::move(resource), kRdsTypeUrl, route.name());
  }
  void SetCdsResource(const Cluster& cluster) {
    google::protobuf::Any resource;
    resource.PackFrom(cluster);
    SetResource(std::move(resource), kCdsTypeUrl, cluster.name());
  }
  void SetEdsResource(const ClusterLoadAssignment& assignment) {
    google::protobuf::Any resource;
    resource.PackFrom(assignment);
    SetResource(std::move(resource), kEdsTypeUrl, assignment.cluster_name());
  }
  void Start() {
    grpc_core::MutexLock lock(&ads_mu_);
    ads_done_ = false;
  }
  void Shutdown() {
    {
      grpc_core::MutexLock lock(&ads_mu_);
      NotifyDoneWithAdsCallLocked();
      resource_type_response_state_.clear();
    }
    gpr_log(GPR_INFO, "ADS[%p]: shut down", this);
  }
  void NotifyDoneWithAdsCall() {
    grpc_core::MutexLock lock(&ads_mu_);
    NotifyDoneWithAdsCallLocked();
  }
  void NotifyDoneWithAdsCallLocked() {
    if (!ads_done_) {
      ads_done_ = true;
      ads_cond_.Broadcast();
    }
  }
  std::set<std::string> clients() {
    grpc_core::MutexLock lock(&clients_mu_);
    return clients_;
  }
 private:
  using UpdateQueue = std::deque<
      std::pair<std::string , std::string >>;
  struct SubscriptionState {
    UpdateQueue* update_queue;
  };
  using SubscriptionNameMap =
      std::map<std::string , SubscriptionState>;
  using SubscriptionMap =
      std::map<std::string , SubscriptionNameMap>;
  struct SentState {
    int nonce = 0;
    int resource_type_version = 0;
  };
  struct ResourceState {
    absl::optional<google::protobuf::Any> resource;
    int resource_type_version = 0;
    std::set<SubscriptionState*> subscriptions;
  };
  using ResourceNameMap =
      std::map<std::string , ResourceState>;
  struct ResourceTypeState {
    int resource_type_version = 0;
    ResourceNameMap resource_name_map;
  };
  using ResourceMap = std::map<std::string , ResourceTypeState>;
  template <class RpcApi, class DiscoveryRequest, class DiscoveryResponse>
  class RpcService : public RpcApi::Service {
   public:
    using Stream = ServerReaderWriter<DiscoveryResponse, DiscoveryRequest>;
    RpcService(AdsServiceImpl* parent, bool is_v2)
        : parent_(parent), is_v2_(is_v2) {}
    Status StreamAggregatedResources(ServerContext* context,
                                     Stream* stream) override {
      gpr_log(GPR_INFO, "ADS[%p]: StreamAggregatedResources starts", this);
      parent_->AddClient(context->peer());
      if (is_v2_) {
        parent_->seen_v2_client_ = true;
      } else {
        parent_->seen_v3_client_ = true;
      }
      EXPECT_EQ(context->client_metadata().find(g_kCallCredsMdKey),
                context->client_metadata().end());
      std::shared_ptr<AdsServiceImpl> ads_service_impl =
          parent_->shared_from_this();
      UpdateQueue update_queue;
      SubscriptionMap subscription_map;
      std::map<std::string , SentState> sent_state_map;
      std::deque<DiscoveryRequest> requests;
      bool stream_closed = false;
      std::thread reader(std::bind(&RpcService::BlockingRead, this, stream,
                                   &requests, &stream_closed));
      while (true) {
        bool did_work = false;
        absl::optional<DiscoveryResponse> response;
        {
          grpc_core::MutexLock lock(&parent_->ads_mu_);
          if (stream_closed || parent_->ads_done_) break;
          if (!requests.empty()) {
            DiscoveryRequest request = std::move(requests.front());
            requests.pop_front();
            did_work = true;
            gpr_log(GPR_INFO,
                    "ADS[%p]: Received request for type %s with content %s",
                    this, request.type_url().c_str(),
                    request.DebugString().c_str());
            const std::string v3_resource_type =
                TypeUrlToV3(request.type_url());
            SentState& sent_state = sent_state_map[v3_resource_type];
            ProcessRequest(request, v3_resource_type, &update_queue,
                           &subscription_map, &sent_state, &response);
          }
        }
        if (response.has_value()) {
          gpr_log(GPR_INFO, "ADS[%p]: Sending response: %s", this,
                  response->DebugString().c_str());
          stream->Write(response.value());
        }
        response.reset();
        {
          grpc_core::MutexLock lock(&parent_->ads_mu_);
          if (!update_queue.empty()) {
            const std::string resource_type =
                std::move(update_queue.front().first);
            const std::string resource_name =
                std::move(update_queue.front().second);
            update_queue.pop_front();
            did_work = true;
            SentState& sent_state = sent_state_map[resource_type];
            ProcessUpdate(resource_type, resource_name, &subscription_map,
                          &sent_state, &response);
          }
        }
        if (response.has_value()) {
          gpr_log(GPR_INFO, "ADS[%p]: Sending update response: %s", this,
                  response->DebugString().c_str());
          stream->Write(response.value());
        }
        gpr_timespec deadline =
            grpc_timeout_milliseconds_to_deadline(did_work ? 0 : 10);
        {
          grpc_core::MutexLock lock(&parent_->ads_mu_);
          if (!parent_->ads_cond_.WaitUntil(
                  &parent_->ads_mu_, [this] { return parent_->ads_done_; },
                  deadline)) {
            break;
          }
        }
      }
      reader.join();
      {
        grpc_core::MutexLock lock(&parent_->ads_mu_);
        for (auto& p : subscription_map) {
          const std::string& type_url = p.first;
          SubscriptionNameMap& subscription_name_map = p.second;
          for (auto& q : subscription_name_map) {
            const std::string& resource_name = q.first;
            SubscriptionState& subscription_state = q.second;
            ResourceNameMap& resource_name_map =
                parent_->resource_map_[type_url].resource_name_map;
            ResourceState& resource_state = resource_name_map[resource_name];
            resource_state.subscriptions.erase(&subscription_state);
          }
        }
      }
      gpr_log(GPR_INFO, "ADS[%p]: StreamAggregatedResources done", this);
      parent_->RemoveClient(context->peer());
      return Status::OK;
    }
   private:
    void ProcessRequest(const DiscoveryRequest& request,
                        const std::string& v3_resource_type,
                        UpdateQueue* update_queue,
                        SubscriptionMap* subscription_map,
                        SentState* sent_state,
                        absl::optional<DiscoveryResponse>* response) {
      int client_resource_type_version = 0;
      if (!request.version_info().empty()) {
        GPR_ASSERT(absl::SimpleAtoi(request.version_info(),
                                    &client_resource_type_version));
      }
      if (request.response_nonce().empty()) {
        EXPECT_GE(client_resource_type_version,
                  parent_->resource_type_min_versions_[v3_resource_type])
            << "resource_type: " << v3_resource_type;
      } else {
        int client_nonce;
        GPR_ASSERT(absl::SimpleAtoi(request.response_nonce(), &client_nonce));
        if (client_nonce < sent_state->nonce) return;
        auto it = parent_->resource_type_response_state_.find(v3_resource_type);
        if (it != parent_->resource_type_response_state_.end()) {
          if (client_resource_type_version ==
              sent_state->resource_type_version) {
            it->second.state = ResponseState::ACKED;
            it->second.error_message.clear();
            gpr_log(GPR_INFO,
                    "ADS[%p]: client ACKed resource_type=%s version=%s", this,
                    request.type_url().c_str(), request.version_info().c_str());
          } else {
            it->second.state = ResponseState::NACKED;
            EXPECT_EQ(request.error_detail().code(),
                      GRPC_STATUS_INVALID_ARGUMENT);
            it->second.error_message = request.error_detail().message();
            gpr_log(GPR_INFO,
                    "ADS[%p]: client NACKed resource_type=%s version=%s: %s",
                    this, request.type_url().c_str(),
                    request.version_info().c_str(),
                    it->second.error_message.c_str());
          }
        }
      }
      if (parent_->resource_types_to_ignore_.find(v3_resource_type) !=
          parent_->resource_types_to_ignore_.end()) {
        return;
      }
      auto& subscription_name_map = (*subscription_map)[v3_resource_type];
      auto& resource_type_state = parent_->resource_map_[v3_resource_type];
      auto& resource_name_map = resource_type_state.resource_name_map;
      std::set<std::string> resources_in_current_request;
      std::set<std::string> resources_added_to_response;
      for (const std::string& resource_name : request.resource_names()) {
        resources_in_current_request.emplace(resource_name);
        auto& subscription_state = subscription_name_map[resource_name];
        auto& resource_state = resource_name_map[resource_name];
        if (parent_->MaybeSubscribe(v3_resource_type, resource_name,
                                    &subscription_state, &resource_state,
                                    update_queue) ||
            ClientNeedsResourceUpdate(resource_type_state, resource_state,
                                      client_resource_type_version,
                                      &subscription_state)) {
          gpr_log(GPR_INFO, "ADS[%p]: Sending update for type=%s name=%s", this,
                  request.type_url().c_str(), resource_name.c_str());
          resources_added_to_response.emplace(resource_name);
          if (!response->has_value()) response->emplace();
          if (resource_state.resource.has_value()) {
            auto* resource = (*response)->add_resources();
            resource->CopyFrom(resource_state.resource.value());
            if (is_v2_) {
              resource->set_type_url(request.type_url());
            }
          }
        } else {
          gpr_log(GPR_INFO,
                  "ADS[%p]: client does not need update for type=%s name=%s",
                  this, request.type_url().c_str(), resource_name.c_str());
        }
      }
      parent_->ProcessUnsubscriptions(
          v3_resource_type, resources_in_current_request,
          &subscription_name_map, &resource_name_map);
      if (!resources_added_to_response.empty()) {
        CompleteBuildingDiscoveryResponse(
            v3_resource_type, request.type_url(),
            resource_type_state.resource_type_version, subscription_name_map,
            resources_added_to_response, sent_state, &response->value());
      }
    }
    void ProcessUpdate(const std::string& resource_type,
                       const std::string& resource_name,
                       SubscriptionMap* subscription_map, SentState* sent_state,
                       absl::optional<DiscoveryResponse>* response) {
      const std::string v2_resource_type = TypeUrlToV2(resource_type);
      gpr_log(GPR_INFO, "ADS[%p]: Received update for type=%s name=%s", this,
              resource_type.c_str(), resource_name.c_str());
      auto& subscription_name_map = (*subscription_map)[resource_type];
      auto& resource_type_state = parent_->resource_map_[resource_type];
      auto& resource_name_map = resource_type_state.resource_name_map;
      auto it = subscription_name_map.find(resource_name);
      if (it != subscription_name_map.end()) {
        SubscriptionState& subscription_state = it->second;
        ResourceState& resource_state = resource_name_map[resource_name];
        if (ClientNeedsResourceUpdate(resource_type_state, resource_state,
                                      sent_state->resource_type_version,
                                      &subscription_state)) {
          gpr_log(GPR_INFO, "ADS[%p]: Sending update for type=%s name=%s", this,
                  resource_type.c_str(), resource_name.c_str());
          response->emplace();
          if (resource_state.resource.has_value()) {
            auto* resource = (*response)->add_resources();
            resource->CopyFrom(resource_state.resource.value());
            if (is_v2_) {
              resource->set_type_url(v2_resource_type);
            }
          }
          CompleteBuildingDiscoveryResponse(
              resource_type, v2_resource_type,
              resource_type_state.resource_type_version, subscription_name_map,
              {resource_name}, sent_state, &response->value());
        }
      }
    }
    void BlockingRead(Stream* stream, std::deque<DiscoveryRequest>* requests,
                      bool* stream_closed) {
      DiscoveryRequest request;
      bool seen_first_request = false;
      while (stream->Read(&request)) {
        if (!seen_first_request) {
          EXPECT_TRUE(request.has_node());
          ASSERT_FALSE(request.node().client_features().empty());
          EXPECT_EQ(request.node().client_features(0),
                    "envoy.lb.does_not_support_overprovisioning");
          CheckBuildVersion(request);
          seen_first_request = true;
        }
        {
          grpc_core::MutexLock lock(&parent_->ads_mu_);
          requests->emplace_back(std::move(request));
        }
      }
      gpr_log(GPR_INFO, "ADS[%p]: Null read, stream closed", this);
      grpc_core::MutexLock lock(&parent_->ads_mu_);
      *stream_closed = true;
    }
    void CompleteBuildingDiscoveryResponse(
        const std::string& resource_type, const std::string& v2_resource_type,
        const int version, const SubscriptionNameMap& subscription_name_map,
        const std::set<std::string>& resources_added_to_response,
        SentState* sent_state, DiscoveryResponse* response) {
      auto& response_state =
          parent_->resource_type_response_state_[resource_type];
      if (response_state.state == ResponseState::NOT_SENT) {
        response_state.state = ResponseState::SENT;
      }
      response->set_type_url(is_v2_ ? v2_resource_type : resource_type);
      response->set_version_info(std::to_string(version));
      response->set_nonce(std::to_string(++sent_state->nonce));
      if (resource_type == kLdsTypeUrl || resource_type == kCdsTypeUrl) {
        for (const auto& p : subscription_name_map) {
          const std::string& resource_name = p.first;
          if (resources_added_to_response.find(resource_name) ==
              resources_added_to_response.end()) {
            ResourceNameMap& resource_name_map =
                parent_->resource_map_[resource_type].resource_name_map;
            const ResourceState& resource_state =
                resource_name_map[resource_name];
            if (resource_state.resource.has_value()) {
              auto* resource = response->add_resources();
              resource->CopyFrom(resource_state.resource.value());
              if (is_v2_) {
                resource->set_type_url(v2_resource_type);
              }
            }
          }
        }
      }
      sent_state->resource_type_version = version;
    }
    static std::string TypeUrlToV2(const std::string& resource_type) {
      if (resource_type == kLdsTypeUrl) return kLdsV2TypeUrl;
      if (resource_type == kRdsTypeUrl) return kRdsV2TypeUrl;
      if (resource_type == kCdsTypeUrl) return kCdsV2TypeUrl;
      if (resource_type == kEdsTypeUrl) return kEdsV2TypeUrl;
      return resource_type;
    }
    static std::string TypeUrlToV3(const std::string& resource_type) {
      if (resource_type == kLdsV2TypeUrl) return kLdsTypeUrl;
      if (resource_type == kRdsV2TypeUrl) return kRdsTypeUrl;
      if (resource_type == kCdsV2TypeUrl) return kCdsTypeUrl;
      if (resource_type == kEdsV2TypeUrl) return kEdsTypeUrl;
      return resource_type;
    }
    static void CheckBuildVersion(
        const ::envoy::api::v2::DiscoveryRequest& request) {
      EXPECT_FALSE(request.node().build_version().empty());
    }
    static void CheckBuildVersion(
        const ::envoy::service::discovery::v3::DiscoveryRequest& request) {}
    AdsServiceImpl* parent_;
    const bool is_v2_;
  };
  static bool ClientNeedsResourceUpdate(
      const ResourceTypeState& resource_type_state,
      const ResourceState& resource_state, int client_resource_type_version,
      SubscriptionState* subscription_state) {
    return client_resource_type_version <
               resource_type_state.resource_type_version &&
           resource_state.resource_type_version <=
               resource_type_state.resource_type_version;
  }
  bool MaybeSubscribe(const std::string& resource_type,
                      const std::string& resource_name,
                      SubscriptionState* subscription_state,
                      ResourceState* resource_state,
                      UpdateQueue* update_queue) {
    if (subscription_state->update_queue != nullptr) return false;
    subscription_state->update_queue = update_queue;
    resource_state->subscriptions.emplace(subscription_state);
    gpr_log(GPR_INFO, "ADS[%p]: subscribe to resource type %s name %s state %p",
            this, resource_type.c_str(), resource_name.c_str(),
            &subscription_state);
    return true;
  }
  void ProcessUnsubscriptions(
      const std::string& resource_type,
      const std::set<std::string>& resources_in_current_request,
      SubscriptionNameMap* subscription_name_map,
      ResourceNameMap* resource_name_map) {
    for (auto it = subscription_name_map->begin();
         it != subscription_name_map->end();) {
      const std::string& resource_name = it->first;
      SubscriptionState& subscription_state = it->second;
      if (resources_in_current_request.find(resource_name) !=
          resources_in_current_request.end()) {
        ++it;
        continue;
      }
      gpr_log(GPR_INFO, "ADS[%p]: Unsubscribe to type=%s name=%s state=%p",
              this, resource_type.c_str(), resource_name.c_str(),
              &subscription_state);
      auto resource_it = resource_name_map->find(resource_name);
      GPR_ASSERT(resource_it != resource_name_map->end());
      auto& resource_state = resource_it->second;
      resource_state.subscriptions.erase(&subscription_state);
      if (resource_state.subscriptions.empty() &&
          !resource_state.resource.has_value()) {
        resource_name_map->erase(resource_it);
      }
      it = subscription_name_map->erase(it);
    }
  }
  void AddClient(const std::string& client) {
    grpc_core::MutexLock lock(&clients_mu_);
    clients_.insert(client);
  }
  void RemoveClient(const std::string& client) {
    grpc_core::MutexLock lock(&clients_mu_);
    clients_.erase(client);
  }
  RpcService<::envoy::service::discovery::v2::AggregatedDiscoveryService,
             ::envoy::api::v2::DiscoveryRequest,
             ::envoy::api::v2::DiscoveryResponse>
      v2_rpc_service_;
  RpcService<::envoy::service::discovery::v3::AggregatedDiscoveryService,
             ::envoy::service::discovery::v3::DiscoveryRequest,
             ::envoy::service::discovery::v3::DiscoveryResponse>
      v3_rpc_service_;
  std::atomic_bool seen_v2_client_{false};
  std::atomic_bool seen_v3_client_{false};
  grpc_core::CondVar ads_cond_;
  grpc_core::Mutex ads_mu_;
  bool ads_done_ = false;
  std::map<std::string , ResponseState>
      resource_type_response_state_;
  std::set<std::string > resource_types_to_ignore_;
  std::map<std::string , int> resource_type_min_versions_;
  ResourceMap resource_map_;
  grpc_core::Mutex clients_mu_;
  std::set<std::string> clients_;
};
class LrsServiceImpl : public std::enable_shared_from_this<LrsServiceImpl> {
 public:
  explicit LrsServiceImpl(int client_load_reporting_interval_seconds)
      : v2_rpc_service_(this),
        v3_rpc_service_(this),
        client_load_reporting_interval_seconds_(
            client_load_reporting_interval_seconds),
        cluster_names_({kDefaultClusterName}) {}
  ::envoy::service::load_stats::v2::LoadReportingService::Service*
  v2_rpc_service() {
    return &v2_rpc_service_;
  }
  ::envoy::service::load_stats::v3::LoadReportingService::Service*
  v3_rpc_service() {
    return &v3_rpc_service_;
  }
  size_t request_count() {
    return v2_rpc_service_.request_count() + v3_rpc_service_.request_count();
  }
  size_t response_count() {
    return v2_rpc_service_.response_count() + v3_rpc_service_.response_count();
  }
  void set_send_all_clusters(bool send_all_clusters) {
    send_all_clusters_ = send_all_clusters;
  }
  void set_cluster_names(const std::set<std::string>& cluster_names) {
    cluster_names_ = cluster_names;
  }
  void Start() {
    lrs_done_ = false;
    result_queue_.clear();
  }
  void Shutdown() {
    {
      grpc_core::MutexLock lock(&lrs_mu_);
      NotifyDoneWithLrsCallLocked();
    }
    gpr_log(GPR_INFO, "LRS[%p]: shut down", this);
  }
  std::vector<ClientStats> WaitForLoadReport() {
    grpc_core::MutexLock lock(&load_report_mu_);
    grpc_core::CondVar cv;
    if (result_queue_.empty()) {
      load_report_cond_ = &cv;
      load_report_cond_->WaitUntil(&load_report_mu_,
                                   [this] { return !result_queue_.empty(); });
      load_report_cond_ = nullptr;
    }
    std::vector<ClientStats> result = std::move(result_queue_.front());
    result_queue_.pop_front();
    return result;
  }
  void NotifyDoneWithLrsCall() {
    grpc_core::MutexLock lock(&lrs_mu_);
    NotifyDoneWithLrsCallLocked();
  }
 private:
  template <class RpcApi, class LoadStatsRequest, class LoadStatsResponse>
  class RpcService : public CountedService<typename RpcApi::Service> {
   public:
    using Stream = ServerReaderWriter<LoadStatsResponse, LoadStatsRequest>;
    explicit RpcService(LrsServiceImpl* parent) : parent_(parent) {}
    Status StreamLoadStats(ServerContext* ,
                           Stream* stream) override {
      gpr_log(GPR_INFO, "LRS[%p]: StreamLoadStats starts", this);
      EXPECT_GT(parent_->client_load_reporting_interval_seconds_, 0);
      std::shared_ptr<LrsServiceImpl> lrs_service_impl =
          parent_->shared_from_this();
      LoadStatsRequest request;
      if (stream->Read(&request)) {
        CountedService<typename RpcApi::Service>::IncreaseRequestCount();
        EXPECT_THAT(
            request.node().client_features(),
            ::testing::Contains("envoy.lrs.supports_send_all_clusters"));
        LoadStatsResponse response;
        if (parent_->send_all_clusters_) {
          response.set_send_all_clusters(true);
        } else {
          for (const std::string& cluster_name : parent_->cluster_names_) {
            response.add_clusters(cluster_name);
          }
        }
        response.mutable_load_reporting_interval()->set_seconds(
            parent_->client_load_reporting_interval_seconds_);
        stream->Write(response);
        CountedService<typename RpcApi::Service>::IncreaseResponseCount();
        request.Clear();
        while (stream->Read(&request)) {
          gpr_log(GPR_INFO, "LRS[%p]: received client load report message: %s",
                  this, request.DebugString().c_str());
          std::vector<ClientStats> stats;
          for (const auto& cluster_stats : request.cluster_stats()) {
            stats.emplace_back(cluster_stats);
          }
          grpc_core::MutexLock lock(&parent_->load_report_mu_);
          parent_->result_queue_.emplace_back(std::move(stats));
          if (parent_->load_report_cond_ != nullptr) {
            parent_->load_report_cond_->Signal();
          }
        }
        grpc_core::MutexLock lock(&parent_->lrs_mu_);
        parent_->lrs_cv_.WaitUntil(&parent_->lrs_mu_,
                                   [this] { return parent_->lrs_done_; });
      }
      gpr_log(GPR_INFO, "LRS[%p]: StreamLoadStats done", this);
      return Status::OK;
    }
   private:
    LrsServiceImpl* parent_;
  };
  void NotifyDoneWithLrsCallLocked() {
    if (!lrs_done_) {
      lrs_done_ = true;
      lrs_cv_.Broadcast();
    }
  }
  RpcService<::envoy::service::load_stats::v2::LoadReportingService,
             ::envoy::service::load_stats::v2::LoadStatsRequest,
             ::envoy::service::load_stats::v2::LoadStatsResponse>
      v2_rpc_service_;
  RpcService<::envoy::service::load_stats::v3::LoadReportingService,
             ::envoy::service::load_stats::v3::LoadStatsRequest,
             ::envoy::service::load_stats::v3::LoadStatsResponse>
      v3_rpc_service_;
  const int client_load_reporting_interval_seconds_;
  bool send_all_clusters_ = false;
  std::set<std::string> cluster_names_;
  grpc_core::CondVar lrs_cv_;
  grpc_core::Mutex lrs_mu_;
  bool lrs_done_ = false;
  grpc_core::Mutex load_report_mu_;
  grpc_core::CondVar* load_report_cond_ = nullptr;
  std::deque<std::vector<ClientStats>> result_queue_;
};
class TestType {
 public:
  TestType(bool use_xds_resolver, bool enable_load_reporting,
           bool enable_rds_testing = false, bool use_v2 = false)
      : use_xds_resolver_(use_xds_resolver),
        enable_load_reporting_(enable_load_reporting),
        enable_rds_testing_(enable_rds_testing),
        use_v2_(use_v2) {}
  bool use_xds_resolver() const { return use_xds_resolver_; }
  bool enable_load_reporting() const { return enable_load_reporting_; }
  bool enable_rds_testing() const { return enable_rds_testing_; }
  bool use_v2() const { return use_v2_; }
  std::string AsString() const {
    std::string retval = (use_xds_resolver_ ? "XdsResolver" : "FakeResolver");
    retval += (use_v2_ ? "V2" : "V3");
    if (enable_load_reporting_) retval += "WithLoadReporting";
    if (enable_rds_testing_) retval += "Rds";
    return retval;
  }
 private:
  const bool use_xds_resolver_;
  const bool enable_load_reporting_;
  const bool enable_rds_testing_;
  const bool use_v2_;
};
class XdsEnd2endTest : public ::testing::TestWithParam<TestType> {
 protected:
  XdsEnd2endTest(size_t num_backends, size_t num_balancers,
                 int client_load_reporting_interval_seconds = 100)
      : num_backends_(num_backends),
        num_balancers_(num_balancers),
        client_load_reporting_interval_seconds_(
            client_load_reporting_interval_seconds) {}
  static void SetUpTestCase() {
    GPR_GLOBAL_CONFIG_SET(grpc_client_channel_backup_poll_interval_ms, 1);
#if TARGET_OS_IPHONE
    gpr_setenv("grpc_cfstream", "0");
#endif
    grpc_init();
  }
  static void TearDownTestCase() { grpc_shutdown(); }
  void SetUp() override {
    gpr_setenv("GRPC_XDS_EXPERIMENTAL_V3_SUPPORT", "true");
    gpr_setenv("GRPC_XDS_BOOTSTRAP",
               GetParam().use_v2() ? g_bootstrap_file_v2 : g_bootstrap_file_v3);
    g_port_saver->Reset();
    bool localhost_resolves_to_ipv4 = false;
    bool localhost_resolves_to_ipv6 = false;
    grpc_core::LocalhostResolves(&localhost_resolves_to_ipv4,
                                 &localhost_resolves_to_ipv6);
    ipv6_only_ = !localhost_resolves_to_ipv4 && localhost_resolves_to_ipv6;
    response_generator_ =
        grpc_core::MakeRefCounted<grpc_core::FakeResolverResponseGenerator>();
    lb_channel_response_generator_ =
        grpc_core::MakeRefCounted<grpc_core::FakeResolverResponseGenerator>();
    xds_channel_args_to_add_.emplace_back(
        grpc_core::FakeResolverResponseGenerator::MakeChannelArg(
            lb_channel_response_generator_.get()));
    if (xds_resource_does_not_exist_timeout_ms_ > 0) {
      xds_channel_args_to_add_.emplace_back(grpc_channel_arg_integer_create(
          const_cast<char*>(GRPC_ARG_XDS_RESOURCE_DOES_NOT_EXIST_TIMEOUT_MS),
          xds_resource_does_not_exist_timeout_ms_));
    }
    xds_channel_args_.num_args = xds_channel_args_to_add_.size();
    xds_channel_args_.args = xds_channel_args_to_add_.data();
    grpc_core::internal::SetXdsChannelArgsForTest(&xds_channel_args_);
    grpc_core::internal::UnsetGlobalXdsClientForTest();
    default_listener_.set_name(kServerName);
    default_route_config_.set_name(kDefaultRouteConfigurationName);
    auto* virtual_host = default_route_config_.add_virtual_hosts();
    virtual_host->add_domains("*");
    auto* route = virtual_host->add_routes();
    route->mutable_match()->set_prefix("");
    route->mutable_route()->set_cluster(kDefaultClusterName);
    default_cluster_.set_name(kDefaultClusterName);
    default_cluster_.set_type(Cluster::EDS);
    auto* eds_config = default_cluster_.mutable_eds_cluster_config();
    eds_config->mutable_eds_config()->mutable_ads();
    eds_config->set_service_name(kDefaultEdsServiceName);
    default_cluster_.set_lb_policy(Cluster::ROUND_ROBIN);
    if (GetParam().enable_load_reporting()) {
      default_cluster_.mutable_lrs_server()->mutable_self();
    }
    for (size_t i = 0; i < num_backends_; ++i) {
      backends_.emplace_back(new BackendServerThread);
      backends_.back()->Start();
    }
    for (size_t i = 0; i < num_balancers_; ++i) {
      balancers_.emplace_back(
          new BalancerServerThread(GetParam().enable_load_reporting()
                                       ? client_load_reporting_interval_seconds_
                                       : 0));
      balancers_.back()->Start();
      SetListenerAndRouteConfiguration(i, default_listener_,
                                       default_route_config_);
      balancers_.back()->ads_service()->SetCdsResource(default_cluster_);
    }
    ResetStub();
  }
  const char* DefaultEdsServiceName() const {
    return GetParam().use_xds_resolver() ? kDefaultEdsServiceName : kServerName;
  }
  void TearDown() override {
    ShutdownAllBackends();
    for (auto& balancer : balancers_) balancer->Shutdown();
    grpc_core::internal::SetXdsChannelArgsForTest(nullptr);
  }
  void StartAllBackends() {
    for (auto& backend : backends_) backend->Start();
  }
  void StartBackend(size_t index) { backends_[index]->Start(); }
  void ShutdownAllBackends() {
    for (auto& backend : backends_) backend->Shutdown();
  }
  void ShutdownBackend(size_t index) { backends_[index]->Shutdown(); }
  void ResetStub(int failover_timeout = 0) {
    channel_ = CreateChannel(failover_timeout);
    stub_ = grpc::testing::EchoTestService::NewStub(channel_);
    stub1_ = grpc::testing::EchoTest1Service::NewStub(channel_);
    stub2_ = grpc::testing::EchoTest2Service::NewStub(channel_);
  }
  std::shared_ptr<Channel> CreateChannel(
      int failover_timeout = 0, const char* server_name = kServerName,
      grpc_core::FakeResolverResponseGenerator* response_generator = nullptr) {
    ChannelArguments args;
    if (failover_timeout > 0) {
      args.SetInt(GRPC_ARG_PRIORITY_FAILOVER_TIMEOUT_MS, failover_timeout);
    }
    if (!GetParam().use_xds_resolver()) {
      if (response_generator == nullptr) {
        response_generator = response_generator_.get();
      }
      args.SetPointer(GRPC_ARG_FAKE_RESOLVER_RESPONSE_GENERATOR,
                      response_generator);
    }
    std::string uri = absl::StrCat(
        GetParam().use_xds_resolver() ? "xds" : "fake", ":///", server_name);
    grpc_channel_credentials* channel_creds =
        grpc_fake_transport_security_credentials_create();
    grpc_call_credentials* call_creds = grpc_md_only_test_credentials_create(
        g_kCallCredsMdKey, g_kCallCredsMdValue, false);
    std::shared_ptr<ChannelCredentials> creds(
        new SecureChannelCredentials(grpc_composite_channel_credentials_create(
            channel_creds, call_creds, nullptr)));
    call_creds->Unref();
    channel_creds->Unref();
    return ::grpc::CreateCustomChannel(uri, creds, args);
  }
  enum RpcService {
    SERVICE_ECHO,
    SERVICE_ECHO1,
    SERVICE_ECHO2,
  };
  enum RpcMethod {
    METHOD_ECHO,
    METHOD_ECHO1,
    METHOD_ECHO2,
  };
  struct RpcOptions {
    RpcService service = SERVICE_ECHO;
    RpcMethod method = METHOD_ECHO;
    int timeout_ms = 1000;
    bool wait_for_ready = false;
    bool server_fail = false;
    std::vector<std::pair<std::string, std::string>> metadata;
    RpcOptions() {}
    RpcOptions& set_rpc_service(RpcService rpc_service) {
      service = rpc_service;
      return *this;
    }
    RpcOptions& set_rpc_method(RpcMethod rpc_method) {
      method = rpc_method;
      return *this;
    }
    RpcOptions& set_timeout_ms(int rpc_timeout_ms) {
      timeout_ms = rpc_timeout_ms;
      return *this;
    }
    RpcOptions& set_wait_for_ready(bool rpc_wait_for_ready) {
      wait_for_ready = rpc_wait_for_ready;
      return *this;
    }
    RpcOptions& set_server_fail(bool rpc_server_fail) {
      server_fail = rpc_server_fail;
      return *this;
    }
    RpcOptions& set_metadata(
        std::vector<std::pair<std::string, std::string>> rpc_metadata) {
      metadata = std::move(rpc_metadata);
      return *this;
    }
  };
  template <typename Stub>
  Status SendRpcMethod(Stub* stub, const RpcOptions& rpc_options,
                       ClientContext* context, EchoRequest& request,
                       EchoResponse* response) {
    switch (rpc_options.method) {
      case METHOD_ECHO:
        return (*stub)->Echo(context, request, response);
      case METHOD_ECHO1:
        return (*stub)->Echo1(context, request, response);
      case METHOD_ECHO2:
        return (*stub)->Echo2(context, request, response);
    }
  }
  void ResetBackendCounters(size_t start_index = 0, size_t stop_index = 0) {
    if (stop_index == 0) stop_index = backends_.size();
    for (size_t i = start_index; i < stop_index; ++i) {
      backends_[i]->backend_service()->ResetCounters();
      backends_[i]->backend_service1()->ResetCounters();
      backends_[i]->backend_service2()->ResetCounters();
    }
  }
  bool SeenAllBackends(size_t start_index = 0, size_t stop_index = 0,
                       const RpcOptions& rpc_options = RpcOptions()) {
    if (stop_index == 0) stop_index = backends_.size();
    for (size_t i = start_index; i < stop_index; ++i) {
      switch (rpc_options.service) {
        case SERVICE_ECHO:
          if (backends_[i]->backend_service()->request_count() == 0) {
            return false;
          }
          break;
        case SERVICE_ECHO1:
          if (backends_[i]->backend_service1()->request_count() == 0) {
            return false;
          }
          break;
        case SERVICE_ECHO2:
          if (backends_[i]->backend_service2()->request_count() == 0) {
            return false;
          }
          break;
      }
    }
    return true;
  }
  void SendRpcAndCount(int* num_total, int* num_ok, int* num_failure,
                       int* num_drops,
                       const RpcOptions& rpc_options = RpcOptions()) {
    const Status status = SendRpc(rpc_options);
    if (status.ok()) {
      ++*num_ok;
    } else {
      if (status.error_message() == "Call dropped by load balancing policy") {
        ++*num_drops;
      } else {
        ++*num_failure;
      }
    }
    ++*num_total;
  }
  std::tuple<int, int, int> WaitForAllBackends(
      size_t start_index = 0, size_t stop_index = 0, bool reset_counters = true,
      const RpcOptions& rpc_options = RpcOptions(),
      bool allow_failures = false) {
    int num_ok = 0;
    int num_failure = 0;
    int num_drops = 0;
    int num_total = 0;
    while (!SeenAllBackends(start_index, stop_index, rpc_options)) {
      SendRpcAndCount(&num_total, &num_ok, &num_failure, &num_drops,
                      rpc_options);
    }
    if (reset_counters) ResetBackendCounters();
    gpr_log(GPR_INFO,
            "Performed %d warm up requests against the backends. "
            "%d succeeded, %d failed, %d dropped.",
            num_total, num_ok, num_failure, num_drops);
    if (!allow_failures) EXPECT_EQ(num_failure, 0);
    return std::make_tuple(num_ok, num_failure, num_drops);
  }
  void WaitForBackend(size_t backend_idx, bool reset_counters = true,
                      bool require_success = false) {
    gpr_log(GPR_INFO, "========= WAITING FOR BACKEND %lu ==========",
            static_cast<unsigned long>(backend_idx));
    do {
      Status status = SendRpc();
      if (require_success) {
        EXPECT_TRUE(status.ok()) << "code=" << status.error_code()
                                 << " message=" << status.error_message();
      }
    } while (backends_[backend_idx]->backend_service()->request_count() == 0);
    if (reset_counters) ResetBackendCounters();
    gpr_log(GPR_INFO, "========= BACKEND %lu READY ==========",
            static_cast<unsigned long>(backend_idx));
  }
  grpc_core::ServerAddressList CreateAddressListFromPortList(
      const std::vector<int>& ports) {
    grpc_core::ServerAddressList addresses;
    for (int port : ports) {
      std::string lb_uri_str =
          absl::StrCat(ipv6_only_ ? "ipv6:[::1]:" : "ipv4:127.0.0.1:", port);
      grpc_uri* lb_uri = grpc_uri_parse(lb_uri_str.c_str(), true);
      GPR_ASSERT(lb_uri != nullptr);
      grpc_resolved_address address;
      GPR_ASSERT(grpc_parse_uri(lb_uri, &address));
      addresses.emplace_back(address.addr, address.len, nullptr);
      grpc_uri_destroy(lb_uri);
    }
    return addresses;
  }
  void SetNextResolution(
      const std::vector<int>& ports,
      grpc_core::FakeResolverResponseGenerator* response_generator = nullptr) {
    if (GetParam().use_xds_resolver()) return;
    grpc_core::ExecCtx exec_ctx;
    grpc_core::Resolver::Result result;
    result.addresses = CreateAddressListFromPortList(ports);
    grpc_error* error = GRPC_ERROR_NONE;
    const char* service_config_json =
        GetParam().enable_load_reporting()
            ? kDefaultServiceConfig
            : kDefaultServiceConfigWithoutLoadReporting;
    result.service_config =
        grpc_core::ServiceConfig::Create(nullptr, service_config_json, &error);
    ASSERT_EQ(error, GRPC_ERROR_NONE) << grpc_error_string(error);
    ASSERT_NE(result.service_config.get(), nullptr);
    if (response_generator == nullptr) {
      response_generator = response_generator_.get();
    }
    response_generator->SetResponse(std::move(result));
  }
  void SetNextResolutionForLbChannelAllBalancers(
      const char* service_config_json = nullptr,
      const char* expected_targets = nullptr) {
    std::vector<int> ports;
    for (size_t i = 0; i < balancers_.size(); ++i) {
      ports.emplace_back(balancers_[i]->port());
    }
    SetNextResolutionForLbChannel(ports, service_config_json, expected_targets);
  }
  void SetNextResolutionForLbChannel(const std::vector<int>& ports,
                                     const char* service_config_json = nullptr,
                                     const char* expected_targets = nullptr) {
    grpc_core::ExecCtx exec_ctx;
    grpc_core::Resolver::Result result;
    result.addresses = CreateAddressListFromPortList(ports);
    if (service_config_json != nullptr) {
      grpc_error* error = GRPC_ERROR_NONE;
      result.service_config = grpc_core::ServiceConfig::Create(
          nullptr, service_config_json, &error);
      ASSERT_NE(result.service_config.get(), nullptr);
      ASSERT_EQ(error, GRPC_ERROR_NONE) << grpc_error_string(error);
    }
    if (expected_targets != nullptr) {
      grpc_arg expected_targets_arg = grpc_channel_arg_string_create(
          const_cast<char*>(GRPC_ARG_FAKE_SECURITY_EXPECTED_TARGETS),
          const_cast<char*>(expected_targets));
      result.args =
          grpc_channel_args_copy_and_add(nullptr, &expected_targets_arg, 1);
    }
    lb_channel_response_generator_->SetResponse(std::move(result));
  }
  void SetNextReresolutionResponse(const std::vector<int>& ports) {
    grpc_core::ExecCtx exec_ctx;
    grpc_core::Resolver::Result result;
    result.addresses = CreateAddressListFromPortList(ports);
    response_generator_->SetReresolutionResponse(std::move(result));
  }
  const std::vector<int> GetBackendPorts(size_t start_index = 0,
                                         size_t stop_index = 0) const {
    if (stop_index == 0) stop_index = backends_.size();
    std::vector<int> backend_ports;
    for (size_t i = start_index; i < stop_index; ++i) {
      backend_ports.push_back(backends_[i]->port());
    }
    return backend_ports;
  }
  Status SendRpc(const RpcOptions& rpc_options = RpcOptions(),
                 EchoResponse* response = nullptr) {
    const bool local_response = (response == nullptr);
    if (local_response) response = new EchoResponse;
    EchoRequest request;
    ClientContext context;
    for (const auto& metadata : rpc_options.metadata) {
      context.AddMetadata(metadata.first, metadata.second);
    }
    if (rpc_options.timeout_ms != 0) {
      context.set_deadline(
          grpc_timeout_milliseconds_to_deadline(rpc_options.timeout_ms));
    }
    if (rpc_options.wait_for_ready) context.set_wait_for_ready(true);
    request.set_message(kRequestMessage);
    if (rpc_options.server_fail) {
      request.mutable_param()->mutable_expected_error()->set_code(
          GRPC_STATUS_FAILED_PRECONDITION);
    }
    Status status;
    switch (rpc_options.service) {
      case SERVICE_ECHO:
        status =
            SendRpcMethod(&stub_, rpc_options, &context, request, response);
        break;
      case SERVICE_ECHO1:
        status =
            SendRpcMethod(&stub1_, rpc_options, &context, request, response);
        break;
      case SERVICE_ECHO2:
        status =
            SendRpcMethod(&stub2_, rpc_options, &context, request, response);
        break;
    }
    if (local_response) delete response;
    return status;
  }
  void CheckRpcSendOk(const size_t times = 1,
                      const RpcOptions& rpc_options = RpcOptions()) {
    for (size_t i = 0; i < times; ++i) {
      EchoResponse response;
      const Status status = SendRpc(rpc_options, &response);
      EXPECT_TRUE(status.ok()) << "code=" << status.error_code()
                               << " message=" << status.error_message();
      EXPECT_EQ(response.message(), kRequestMessage);
    }
  }
  void CheckRpcSendFailure(
      const size_t times = 1, const RpcOptions& rpc_options = RpcOptions(),
      const StatusCode expected_error_code = StatusCode::OK) {
    for (size_t i = 0; i < times; ++i) {
      const Status status = SendRpc(rpc_options);
      EXPECT_FALSE(status.ok());
      if (expected_error_code != StatusCode::OK) {
        EXPECT_EQ(expected_error_code, status.error_code());
      }
    }
  }
  static Listener BuildListener(const RouteConfiguration& route_config) {
    HttpConnectionManager http_connection_manager;
    *(http_connection_manager.mutable_route_config()) = route_config;
    Listener listener;
    listener.set_name(kServerName);
    listener.mutable_api_listener()->mutable_api_listener()->PackFrom(
        http_connection_manager);
    return listener;
  }
  ClusterLoadAssignment BuildEdsResource(
      const AdsServiceImpl::EdsResourceArgs& args,
      const char* eds_service_name = kDefaultEdsServiceName) {
    ClusterLoadAssignment assignment;
    assignment.set_cluster_name(eds_service_name);
    for (const auto& locality : args.locality_list) {
      auto* endpoints = assignment.add_endpoints();
      endpoints->mutable_load_balancing_weight()->set_value(locality.lb_weight);
      endpoints->set_priority(locality.priority);
      endpoints->mutable_locality()->set_region(kDefaultLocalityRegion);
      endpoints->mutable_locality()->set_zone(kDefaultLocalityZone);
      endpoints->mutable_locality()->set_sub_zone(locality.sub_zone);
      for (size_t i = 0; i < locality.ports.size(); ++i) {
        const int& port = locality.ports[i];
        auto* lb_endpoints = endpoints->add_lb_endpoints();
        if (locality.health_statuses.size() > i &&
            locality.health_statuses[i] != HealthStatus::UNKNOWN) {
          lb_endpoints->set_health_status(locality.health_statuses[i]);
        }
        auto* endpoint = lb_endpoints->mutable_endpoint();
        auto* address = endpoint->mutable_address();
        auto* socket_address = address->mutable_socket_address();
        socket_address->set_address(ipv6_only_ ? "::1" : "127.0.0.1");
        socket_address->set_port_value(port);
      }
    }
    if (!args.drop_categories.empty()) {
      auto* policy = assignment.mutable_policy();
      for (const auto& p : args.drop_categories) {
        const std::string& name = p.first;
        const uint32_t parts_per_million = p.second;
        auto* drop_overload = policy->add_drop_overloads();
        drop_overload->set_category(name);
        auto* drop_percentage = drop_overload->mutable_drop_percentage();
        drop_percentage->set_numerator(parts_per_million);
        drop_percentage->set_denominator(args.drop_denominator);
      }
    }
    return assignment;
  }
  void SetListenerAndRouteConfiguration(
      int idx, Listener listener, const RouteConfiguration& route_config) {
    auto* api_listener =
        listener.mutable_api_listener()->mutable_api_listener();
    HttpConnectionManager http_connection_manager;
    api_listener->UnpackTo(&http_connection_manager);
    if (GetParam().enable_rds_testing()) {
      auto* rds = http_connection_manager.mutable_rds();
      rds->set_route_config_name(kDefaultRouteConfigurationName);
      rds->mutable_config_source()->mutable_ads();
      balancers_[idx]->ads_service()->SetRdsResource(route_config);
    } else {
      *http_connection_manager.mutable_route_config() = route_config;
    }
    api_listener->PackFrom(http_connection_manager);
    balancers_[idx]->ads_service()->SetLdsResource(listener);
  }
  void SetRouteConfiguration(int idx, const RouteConfiguration& route_config) {
    if (GetParam().enable_rds_testing()) {
      balancers_[idx]->ads_service()->SetRdsResource(route_config);
    } else {
      balancers_[idx]->ads_service()->SetLdsResource(
          BuildListener(route_config));
    }
  }
  AdsServiceImpl::ResponseState RouteConfigurationResponseState(int idx) const {
    AdsServiceImpl* ads_service = balancers_[idx]->ads_service();
    if (GetParam().enable_rds_testing()) {
      return ads_service->rds_response_state();
    }
    return ads_service->lds_response_state();
  }
 public:
  void SetEdsResourceWithDelay(size_t i,
                               const ClusterLoadAssignment& assignment,
                               int delay_ms) {
    GPR_ASSERT(delay_ms > 0);
    gpr_sleep_until(grpc_timeout_milliseconds_to_deadline(delay_ms));
    balancers_[i]->ads_service()->SetEdsResource(assignment);
  }
 protected:
  class ServerThread {
   public:
    ServerThread() : port_(g_port_saver->GetPort()) {}
    virtual ~ServerThread(){};
    void Start() {
      gpr_log(GPR_INFO, "starting %s server on port %d", Type(), port_);
      GPR_ASSERT(!running_);
      running_ = true;
      StartAllServices();
      grpc_core::Mutex mu;
      grpc_core::MutexLock lock(&mu);
      grpc_core::CondVar cond;
      thread_ = absl::make_unique<std::thread>(
          std::bind(&ServerThread::Serve, this, &mu, &cond));
      cond.Wait(&mu);
      gpr_log(GPR_INFO, "%s server startup complete", Type());
    }
    void Serve(grpc_core::Mutex* mu, grpc_core::CondVar* cond) {
      grpc_core::MutexLock lock(mu);
      std::ostringstream server_address;
      server_address << "localhost:" << port_;
      ServerBuilder builder;
      std::shared_ptr<ServerCredentials> creds(new SecureServerCredentials(
          grpc_fake_transport_security_server_credentials_create()));
      builder.AddListeningPort(server_address.str(), creds);
      RegisterAllServices(&builder);
      server_ = builder.BuildAndStart();
      cond->Signal();
    }
    void Shutdown() {
      if (!running_) return;
      gpr_log(GPR_INFO, "%s about to shutdown", Type());
      ShutdownAllServices();
      server_->Shutdown(grpc_timeout_milliseconds_to_deadline(0));
      thread_->join();
      gpr_log(GPR_INFO, "%s shutdown completed", Type());
      running_ = false;
    }
    int port() const { return port_; }
   private:
    virtual void RegisterAllServices(ServerBuilder* builder) = 0;
    virtual void StartAllServices() = 0;
    virtual void ShutdownAllServices() = 0;
    virtual const char* Type() = 0;
    const int port_;
    std::unique_ptr<Server> server_;
    std::unique_ptr<std::thread> thread_;
    bool running_ = false;
  };
  class BackendServerThread : public ServerThread {
   public:
    BackendServiceImpl<::grpc::testing::EchoTestService::Service>*
    backend_service() {
      return &backend_service_;
    }
    BackendServiceImpl<::grpc::testing::EchoTest1Service::Service>*
    backend_service1() {
      return &backend_service1_;
    }
    BackendServiceImpl<::grpc::testing::EchoTest2Service::Service>*
    backend_service2() {
      return &backend_service2_;
    }
   private:
    void RegisterAllServices(ServerBuilder* builder) override {
      builder->RegisterService(&backend_service_);
      builder->RegisterService(&backend_service1_);
      builder->RegisterService(&backend_service2_);
    }
    void StartAllServices() override {
      backend_service_.Start();
      backend_service1_.Start();
      backend_service2_.Start();
    }
    void ShutdownAllServices() override {
      backend_service_.Shutdown();
      backend_service1_.Shutdown();
      backend_service2_.Shutdown();
    }
    const char* Type() override { return "Backend"; }
    BackendServiceImpl<::grpc::testing::EchoTestService::Service>
        backend_service_;
    BackendServiceImpl<::grpc::testing::EchoTest1Service::Service>
        backend_service1_;
    BackendServiceImpl<::grpc::testing::EchoTest2Service::Service>
        backend_service2_;
  };
  class BalancerServerThread : public ServerThread {
   public:
    explicit BalancerServerThread(int client_load_reporting_interval = 0)
        : ads_service_(new AdsServiceImpl()),
          lrs_service_(new LrsServiceImpl(client_load_reporting_interval)) {}
    AdsServiceImpl* ads_service() { return ads_service_.get(); }
    LrsServiceImpl* lrs_service() { return lrs_service_.get(); }
   private:
    void RegisterAllServices(ServerBuilder* builder) override {
      builder->RegisterService(ads_service_->v2_rpc_service());
      builder->RegisterService(ads_service_->v3_rpc_service());
      builder->RegisterService(lrs_service_->v2_rpc_service());
      builder->RegisterService(lrs_service_->v3_rpc_service());
    }
    void StartAllServices() override {
      ads_service_->Start();
      lrs_service_->Start();
    }
    void ShutdownAllServices() override {
      ads_service_->Shutdown();
      lrs_service_->Shutdown();
    }
    const char* Type() override { return "Balancer"; }
    std::shared_ptr<AdsServiceImpl> ads_service_;
    std::shared_ptr<LrsServiceImpl> lrs_service_;
  };
  class LongRunningRpc {
   public:
    void StartRpc(grpc::testing::EchoTestService::Stub* stub) {
      sender_thread_ = std::thread([this, stub]() {
        EchoResponse response;
        EchoRequest request;
        request.mutable_param()->set_client_cancel_after_us(1 * 1000 * 1000);
        request.set_message(kRequestMessage);
        (void)stub->Echo(&context_, request, &response);
      });
    }
    void CancelRpc() {
      context_.TryCancel();
      sender_thread_.join();
    }
   private:
    std::thread sender_thread_;
    ClientContext context_;
  };
  const size_t num_backends_;
  const size_t num_balancers_;
  const int client_load_reporting_interval_seconds_;
  bool ipv6_only_ = false;
  std::shared_ptr<Channel> channel_;
  std::unique_ptr<grpc::testing::EchoTestService::Stub> stub_;
  std::unique_ptr<grpc::testing::EchoTest1Service::Stub> stub1_;
  std::unique_ptr<grpc::testing::EchoTest2Service::Stub> stub2_;
  std::vector<std::unique_ptr<BackendServerThread>> backends_;
  std::vector<std::unique_ptr<BalancerServerThread>> balancers_;
  grpc_core::RefCountedPtr<grpc_core::FakeResolverResponseGenerator>
      response_generator_;
  grpc_core::RefCountedPtr<grpc_core::FakeResolverResponseGenerator>
      lb_channel_response_generator_;
  int xds_resource_does_not_exist_timeout_ms_ = 0;
  absl::InlinedVector<grpc_arg, 2> xds_channel_args_to_add_;
  grpc_channel_args xds_channel_args_;
  Listener default_listener_;
  RouteConfiguration default_route_config_;
  Cluster default_cluster_;
};
class BasicTest : public XdsEnd2endTest {
 public:
  BasicTest() : XdsEnd2endTest(4, 1) {}
};
TEST_P(BasicTest, Vanilla) {
  SetNextResolution({});
  SetNextResolutionForLbChannelAllBalancers();
  const size_t kNumRpcsPerAddress = 100;
  AdsServiceImpl::EdsResourceArgs args({
      {"locality0", GetBackendPorts()},
  });
  balancers_[0]->ads_service()->SetEdsResource(
      BuildEdsResource(args, DefaultEdsServiceName()));
  channel_->GetState(true );
  WaitForAllBackends();
  CheckRpcSendOk(kNumRpcsPerAddress * num_backends_);
  for (size_t i = 0; i < backends_.size(); ++i) {
    EXPECT_EQ(kNumRpcsPerAddress,
              backends_[i]->backend_service()->request_count());
  }
  EXPECT_EQ((GetParam().use_xds_resolver() ? "xds_cluster_manager_experimental"
                                           : "eds_experimental"),
            channel_->GetLoadBalancingPolicyName());
}
TEST_P(BasicTest, IgnoresUnhealthyEndpoints) {
  SetNextResolution({});
  SetNextResolutionForLbChannelAllBalancers();
  const size_t kNumRpcsPerAddress = 100;
  AdsServiceImpl::EdsResourceArgs args({
      {"locality0",
       GetBackendPorts(),
       kDefaultLocalityWeight,
       kDefaultLocalityPriority,
       {HealthStatus::DRAINING}},
  });
  balancers_[0]->ads_service()->SetEdsResource(
      BuildEdsResource(args, DefaultEdsServiceName()));
  channel_->GetState(true );
  WaitForAllBackends( 1);
  CheckRpcSendOk(kNumRpcsPerAddress * (num_backends_ - 1));
  for (size_t i = 1; i < backends_.size(); ++i) {
    EXPECT_EQ(kNumRpcsPerAddress,
              backends_[i]->backend_service()->request_count());
  }
}
TEST_P(BasicTest, SameBackendListedMultipleTimes) {
  SetNextResolution({});
  SetNextResolutionForLbChannelAllBalancers();
  std::vector<int> ports(2, backends_[0]->port());
  AdsServiceImpl::EdsResourceArgs args({
      {"locality0", ports},
  });
  const size_t kNumRpcsPerAddress = 10;
  balancers_[0]->ads_service()->SetEdsResource(
      BuildEdsResource(args, DefaultEdsServiceName()));
  WaitForBackend(0);
  CheckRpcSendOk(kNumRpcsPerAddress * ports.size());
  EXPECT_EQ(kNumRpcsPerAddress * ports.size(),
            backends_[0]->backend_service()->request_count());
  EXPECT_EQ(1UL, backends_[0]->backend_service()->clients().size());
}
TEST_P(BasicTest, InitiallyEmptyServerlist) {
  SetNextResolution({});
  SetNextResolutionForLbChannelAllBalancers();
  const int kServerlistDelayMs = 500 * grpc_test_slowdown_factor();
  const int kCallDeadlineMs = kServerlistDelayMs * 2;
  AdsServiceImpl::EdsResourceArgs::Locality empty_locality("locality0", {});
  AdsServiceImpl::EdsResourceArgs args({
      empty_locality,
  });
  balancers_[0]->ads_service()->SetEdsResource(
      BuildEdsResource(args, DefaultEdsServiceName()));
  args = AdsServiceImpl::EdsResourceArgs({
      {"locality0", GetBackendPorts()},
  });
  std::thread delayed_resource_setter(std::bind(
      &BasicTest::SetEdsResourceWithDelay, this, 0,
      BuildEdsResource(args, DefaultEdsServiceName()), kServerlistDelayMs));
  const auto t0 = system_clock::now();
  CheckRpcSendOk(
      1, RpcOptions().set_timeout_ms(kCallDeadlineMs).set_wait_for_ready(true));
  const auto ellapsed_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          system_clock::now() - t0);
  EXPECT_GT(ellapsed_ms.count(), kServerlistDelayMs);
  delayed_resource_setter.join();
}
TEST_P(BasicTest, AllServersUnreachableFailFast) {
  SetNextResolution({});
  SetNextResolutionForLbChannelAllBalancers();
  const size_t kNumUnreachableServers = 5;
  std::vector<int> ports;
  for (size_t i = 0; i < kNumUnreachableServers; ++i) {
    ports.push_back(g_port_saver->GetPort());
  }
  AdsServiceImpl::EdsResourceArgs args({
      {"locality0", ports},
  });
  balancers_[0]->ads_service()->SetEdsResource(
      BuildEdsResource(args, DefaultEdsServiceName()));
  const Status status = SendRpc();
  EXPECT_EQ(StatusCode::UNAVAILABLE, status.error_code());
}
TEST_P(BasicTest, BackendsRestart) {
  SetNextResolution({});
  SetNextResolutionForLbChannelAllBalancers();
  AdsServiceImpl::EdsResourceArgs args({
      {"locality0", GetBackendPorts()},
  });
  balancers_[0]->ads_service()->SetEdsResource(
      BuildEdsResource(args, DefaultEdsServiceName()));
  WaitForAllBackends();
  ShutdownAllBackends();
  CheckRpcSendFailure(num_backends_);
  StartAllBackends();
  CheckRpcSendOk(1, RpcOptions().set_timeout_ms(2000).set_wait_for_ready(true));
}
TEST_P(BasicTest, IgnoresDuplicateUpdates) {
  const size_t kNumRpcsPerAddress = 100;
  SetNextResolution({});
  SetNextResolutionForLbChannelAllBalancers();
  AdsServiceImpl::EdsResourceArgs args({
      {"locality0", GetBackendPorts()},
  });
  balancers_[0]->ads_service()->SetEdsResource(
      BuildEdsResource(args, DefaultEdsServiceName()));
  WaitForAllBackends();
  for (size_t i = 0; i < kNumRpcsPerAddress; ++i) {
    CheckRpcSendOk(2);
    balancers_[0]->ads_service()->SetEdsResource(
        BuildEdsResource(args, DefaultEdsServiceName()));
    CheckRpcSendOk(2);
  }
  for (size_t i = 1; i < backends_.size(); ++i) {
    EXPECT_EQ(kNumRpcsPerAddress,
              backends_[i]->backend_service()->request_count());
  }
}
using XdsResolverOnlyTest = BasicTest;
TEST_P(XdsResolverOnlyTest, ResourceTypeVersionPersistsAcrossStreamRestarts) {
  SetNextResolution({});
  SetNextResolutionForLbChannelAllBalancers();
  AdsServiceImpl::EdsResourceArgs args({
      {"locality0", GetBackendPorts(0, 1)},
  });
  balancers_[0]->ads_service()->SetEdsResource(BuildEdsResource(args));
  WaitForAllBackends(0, 1);
  balancers_[0]->Shutdown();
  balancers_[0]->ads_service()->SetResourceMinVersion(kLdsTypeUrl, 1);
  balancers_[0]->ads_service()->SetResourceMinVersion(kRdsTypeUrl, 1);
  balancers_[0]->ads_service()->SetResourceMinVersion(kCdsTypeUrl, 1);
  balancers_[0]->ads_service()->SetResourceMinVersion(kEdsTypeUrl, 1);
  AdsServiceImpl::EdsResourceArgs args2({
      {"locality0", GetBackendPorts(1, 2)},
  });
  balancers_[0]->ads_service()->SetEdsResource(BuildEdsResource(args2));
  balancers_[0]->Start();
  WaitForAllBackends(1, 2);
}
TEST_P(XdsResolverOnlyTest, ChangeClusters) {
  const char* kNewClusterName = "new_cluster_name";
  const char* kNewEdsServiceName = "new_eds_service_name";
  SetNextResolution({});
  SetNextResolutionForLbChannelAllBalancers();
  AdsServiceImpl::EdsResourceArgs args({
      {"locality0", GetBackendPorts(0, 2)},
  });
  balancers_[0]->ads_service()->SetEdsResource(BuildEdsResource(args));
  WaitForAllBackends(0, 2);
  AdsServiceImpl::EdsResourceArgs args2({
      {"locality0", GetBackendPorts(2, 4)},
  });
  balancers_[0]->ads_service()->SetEdsResource(
      BuildEdsResource(args2, kNewEdsServiceName));
  Cluster new_cluster = default_cluster_;
  new_cluster.set_name(kNewClusterName);
  new_cluster.mutable_eds_cluster_config()->set_service_name(
      kNewEdsServiceName);
  balancers_[0]->ads_service()->SetCdsResource(new_cluster);
  RouteConfiguration new_route_config = default_route_config_;
  new_route_config.mutable_virtual_hosts(0)
      ->mutable_routes(0)
      ->mutable_route()
      ->set_cluster(kNewClusterName);
  SetListenerAndRouteConfiguration(0, default_listener_, new_route_config);
  std::tuple<int, int, int> counts = WaitForAllBackends(2, 4);
  EXPECT_EQ(0, std::get<1>(counts));
}
TEST_P(XdsResolverOnlyTest, ClusterRemoved) {
  SetNextResolution({});
  SetNextResolutionForLbChannelAllBalancers();
  AdsServiceImpl::EdsResourceArgs args({
      {"locality0", GetBackendPorts()},
  });
  balancers_[0]->ads_service()->SetEdsResource(BuildEdsResource(args));
  WaitForAllBackends();
  balancers_[0]->ads_service()->UnsetResource(kCdsTypeUrl, kDefaultClusterName);
  do {
  } while (SendRpc(RpcOptions(), nullptr).ok());
  CheckRpcSendFailure(1000);
  EXPECT_EQ(balancers_[0]->ads_service()->cds_response_state().state,
            AdsServiceImpl::ResponseState::ACKED);
}
TEST_P(XdsResolverOnlyTest, RestartsRequestsUponReconnection) {
  auto listener = default_listener_;
  HttpConnectionManager http_connection_manager;
  auto* rds = http_connection_manager.mutable_rds();
  rds->set_route_config_name(kDefaultRouteConfigurationName);
  rds->mutable_config_source()->mutable_ads();
  listener.mutable_api_listener()->mutable_api_listener()->PackFrom(
      http_connection_manager);
  balancers_[0]->ads_service()->SetLdsResource(listener);
  balancers_[0]->ads_service()->SetRdsResource(default_route_config_);
  const char* kNewClusterName = "new_cluster_name";
  const char* kNewEdsServiceName = "new_eds_service_name";
  SetNextResolution({});
  SetNextResolutionForLbChannelAllBalancers();
  AdsServiceImpl::EdsResourceArgs args({
      {"locality0", GetBackendPorts(0, 2)},
  });
  balancers_[0]->ads_service()->SetEdsResource(BuildEdsResource(args));
  WaitForAllBackends(0, 2);
  balancers_[0]->Shutdown();
  balancers_[0]->Start();
  CheckRpcSendOk(100);
  AdsServiceImpl::EdsResourceArgs args2({
      {"locality0", GetBackendPorts(2, 4)},
  });
  balancers_[0]->ads_service()->SetEdsResource(
      BuildEdsResource(args2, kNewEdsServiceName));
  Cluster new_cluster = default_cluster_;
  new_cluster.set_name(kNewClusterName);
  new_cluster.mutable_eds_cluster_config()->set_service_name(
      kNewEdsServiceName);
  balancers_[0]->ads_service()->SetCdsResource(new_cluster);
  RouteConfiguration new_route_config = default_route_config_;
  new_route_config.mutable_virtual_hosts(0)
      ->mutable_routes(0)
      ->mutable_route()
      ->set_cluster(kNewClusterName);
  balancers_[0]->ads_service()->SetRdsResource(new_route_config);
  std::tuple<int, int, int> counts = WaitForAllBackends(2, 4);
  EXPECT_EQ(0, std::get<1>(counts));
}
TEST_P(XdsResolverOnlyTest, DefaultRouteSpecifiesSlashPrefix) {
  RouteConfiguration route_config = default_route_config_;
  route_config.mutable_virtual_hosts(0)
      ->mutable_routes(0)
      ->mutable_match()
      ->set_prefix("/");
  SetListenerAndRouteConfiguration(0, default_listener_, route_config);
  SetNextResolution({});
  SetNextResolutionForLbChannelAllBalancers();
  AdsServiceImpl::EdsResourceArgs args({
      {"locality0", GetBackendPorts()},
  });
  balancers_[0]->ads_service()->SetEdsResource(BuildEdsResource(args));
  WaitForAllBackends();
}
TEST_P(XdsResolverOnlyTest, CircuitBreaking) {
  gpr_setenv("GRPC_XDS_EXPERIMENTAL_CIRCUIT_BREAKING", "true");
  constexpr size_t kMaxConcurrentRequests = 10;
  SetNextResolution({});
  SetNextResolutionForLbChannelAllBalancers();
  AdsServiceImpl::EdsResourceArgs args({
      {"locality0", GetBackendPorts(0, 1)},
  });
  balancers_[0]->ads_service()->SetEdsResource(BuildEdsResource(args));
  CircuitBreakers circuit_breaks;
  Cluster cluster = default_cluster_;
  auto* threshold = cluster.mutable_circuit_breakers()->add_thresholds();
  threshold->set_priority(RoutingPriority::DEFAULT);
  threshold->mutable_max_requests()->set_value(kMaxConcurrentRequests);
  balancers_[0]->ads_service()->SetCdsResource(cluster);
  LongRunningRpc rpcs[kMaxConcurrentRequests];
  for (size_t i = 0; i < kMaxConcurrentRequests; ++i) {
    rpcs[i].StartRpc(stub_.get());
  }
  while (backends_[0]->backend_service()->RpcsWaitingForClientCancel() <
         kMaxConcurrentRequests) {
    gpr_sleep_until(gpr_time_add(gpr_now(GPR_CLOCK_REALTIME),
                                 gpr_time_from_micros(1 * 1000, GPR_TIMESPAN)));
  }
  Status status = SendRpc();
  EXPECT_FALSE(status.ok());
  EXPECT_EQ(status.error_message(), "Call dropped by load balancing policy");
  rpcs[0].CancelRpc();
  status = SendRpc();
  EXPECT_TRUE(status.ok());
  for (size_t i = 1; i < kMaxConcurrentRequests; ++i) {
    rpcs[i].CancelRpc();
  }
  EXPECT_EQ(kMaxConcurrentRequests + 1,
            backends_[0]->backend_service()->request_count());
  gpr_unsetenv("GRPC_XDS_EXPERIMENTAL_CIRCUIT_BREAKING");
}
TEST_P(XdsResolverOnlyTest, CircuitBreakingMultipleChannelsShareCallCounter) {
  gpr_setenv("GRPC_XDS_EXPERIMENTAL_CIRCUIT_BREAKING", "true");
  constexpr size_t kMaxConcurrentRequests = 10;
  AdsServiceImpl::EdsResourceArgs args({
      {"locality0", GetBackendPorts(0, 1)},
  });
  balancers_[0]->ads_service()->SetEdsResource(BuildEdsResource(args));
  CircuitBreakers circuit_breaks;
  Cluster cluster = default_cluster_;
  auto* threshold = cluster.mutable_circuit_breakers()->add_thresholds();
  threshold->set_priority(RoutingPriority::DEFAULT);
  threshold->mutable_max_requests()->set_value(kMaxConcurrentRequests);
  balancers_[0]->ads_service()->SetCdsResource(cluster);
  auto response_generator2 =
      grpc_core::MakeRefCounted<grpc_core::FakeResolverResponseGenerator>();
  auto channel2 = CreateChannel(
                           0, kServerName,
      response_generator2.get());
  auto stub2 = grpc::testing::EchoTestService::NewStub(channel2);
  SetNextResolution({});
  SetNextResolution({}, response_generator2.get());
  SetNextResolutionForLbChannelAllBalancers();
  LongRunningRpc rpcs[kMaxConcurrentRequests];
  for (size_t i = 0; i < kMaxConcurrentRequests; ++i) {
    rpcs[i].StartRpc(i % 2 == 0 ? stub_.get() : stub2.get());
  }
  while (backends_[0]->backend_service()->RpcsWaitingForClientCancel() <
         kMaxConcurrentRequests) {
    gpr_sleep_until(gpr_time_add(gpr_now(GPR_CLOCK_REALTIME),
                                 gpr_time_from_micros(1 * 1000, GPR_TIMESPAN)));
  }
  Status status = SendRpc();
  EXPECT_FALSE(status.ok());
  EXPECT_EQ(status.error_message(), "Call dropped by load balancing policy");
  rpcs[0].CancelRpc();
  status = SendRpc();
  EXPECT_TRUE(status.ok());
  for (size_t i = 1; i < kMaxConcurrentRequests; ++i) {
    rpcs[i].CancelRpc();
  }
  EXPECT_EQ(kMaxConcurrentRequests + 1,
            backends_[0]->backend_service()->request_count());
  gpr_unsetenv("GRPC_XDS_EXPERIMENTAL_CIRCUIT_BREAKING");
}
TEST_P(XdsResolverOnlyTest, CircuitBreakingDisabled) {
  constexpr size_t kMaxConcurrentRequests = 10;
  SetNextResolution({});
  SetNextResolutionForLbChannelAllBalancers();
  AdsServiceImpl::EdsResourceArgs args({
      {"locality0", GetBackendPorts(0, 1)},
  });
  balancers_[0]->ads_service()->SetEdsResource(BuildEdsResource(args));
  CircuitBreakers circuit_breaks;
  Cluster cluster = default_cluster_;
  auto* threshold = cluster.mutable_circuit_breakers()->add_thresholds();
  threshold->set_priority(RoutingPriority::DEFAULT);
  threshold->mutable_max_requests()->set_value(kMaxConcurrentRequests);
  balancers_[0]->ads_service()->SetCdsResource(cluster);
  LongRunningRpc rpcs[kMaxConcurrentRequests];
  for (size_t i = 0; i < kMaxConcurrentRequests; ++i) {
    rpcs[i].StartRpc(stub_.get());
  }
  while (backends_[0]->backend_service()->RpcsWaitingForClientCancel() <
         kMaxConcurrentRequests) {
    gpr_sleep_until(gpr_time_add(gpr_now(GPR_CLOCK_REALTIME),
                                 gpr_time_from_micros(1 * 1000, GPR_TIMESPAN)));
  }
  Status status = SendRpc();
  EXPECT_TRUE(status.ok());
  for (size_t i = 0; i < kMaxConcurrentRequests; ++i) {
    rpcs[i].CancelRpc();
  }
  EXPECT_EQ(kMaxConcurrentRequests + 1,
            backends_[0]->backend_service()->request_count());
}
TEST_P(XdsResolverOnlyTest, MultipleChannelsShareXdsClient) {
  const char* kNewServerName = "new-server.example.com";
  Listener listener = default_listener_;
  listener.set_name(kNewServerName);
  SetListenerAndRouteConfiguration(0, listener, default_route_config_);
  SetNextResolution({});
  SetNextResolutionForLbChannelAllBalancers();
  AdsServiceImpl::EdsResourceArgs args({
      {"locality0", GetBackendPorts()},
  });
  balancers_[0]->ads_service()->SetEdsResource(BuildEdsResource(args));
  WaitForAllBackends();
  auto channel2 = CreateChannel( 0, kNewServerName);
  channel2->GetState( true);
  ASSERT_TRUE(
      channel2->WaitForConnected(grpc_timeout_milliseconds_to_deadline(100)));
  EXPECT_EQ(1UL, balancers_[0]->ads_service()->clients().size());
}
class XdsResolverLoadReportingOnlyTest : public XdsEnd2endTest {
 public:
  XdsResolverLoadReportingOnlyTest() : XdsEnd2endTest(4, 1, 3) {}
};
TEST_P(XdsResolverLoadReportingOnlyTest, ChangeClusters) {
  const char* kNewClusterName = "new_cluster_name";
  const char* kNewEdsServiceName = "new_eds_service_name";
  balancers_[0]->lrs_service()->set_cluster_names(
      {kDefaultClusterName, kNewClusterName});
  SetNextResolution({});
  SetNextResolutionForLbChannelAllBalancers();
  AdsServiceImpl::EdsResourceArgs args({
      {"locality0", GetBackendPorts(0, 2)},
  });
  balancers_[0]->ads_service()->SetEdsResource(BuildEdsResource(args));
  AdsServiceImpl::EdsResourceArgs args2({
      {"locality1", GetBackendPorts(2, 4)},
  });
  balancers_[0]->ads_service()->SetEdsResource(
      BuildEdsResource(args2, kNewEdsServiceName));
  Cluster new_cluster = default_cluster_;
  new_cluster.set_name(kNewClusterName);
  new_cluster.mutable_eds_cluster_config()->set_service_name(
      kNewEdsServiceName);
  balancers_[0]->ads_service()->SetCdsResource(new_cluster);
  int num_ok = 0;
  int num_failure = 0;
  int num_drops = 0;
  std::tie(num_ok, num_failure, num_drops) = WaitForAllBackends(0, 2);
  std::vector<ClientStats> load_report =
      balancers_[0]->lrs_service()->WaitForLoadReport();
  EXPECT_THAT(
      load_report,
      ::testing::ElementsAre(::testing::AllOf(
          ::testing::Property(&ClientStats::cluster_name, kDefaultClusterName),
          ::testing::Property(
              &ClientStats::locality_stats,
              ::testing::ElementsAre(::testing::Pair(
                  "locality0",
                  ::testing::AllOf(
                      ::testing::Field(&ClientStats::LocalityStats::
                                           total_successful_requests,
                                       num_ok),
                      ::testing::Field(&ClientStats::LocalityStats::
                                           total_requests_in_progress,
                                       0UL),
                      ::testing::Field(
                          &ClientStats::LocalityStats::total_error_requests,
                          num_failure),
                      ::testing::Field(
                          &ClientStats::LocalityStats::total_issued_requests,
                          num_failure + num_ok))))),
          ::testing::Property(&ClientStats::total_dropped_requests,
                              num_drops))));
  RouteConfiguration new_route_config = default_route_config_;
  new_route_config.mutable_virtual_hosts(0)
      ->mutable_routes(0)
      ->mutable_route()
      ->set_cluster(kNewClusterName);
  SetListenerAndRouteConfiguration(0, default_listener_, new_route_config);
  std::tie(num_ok, num_failure, num_drops) = WaitForAllBackends(2, 4);
  load_report = balancers_[0]->lrs_service()->WaitForLoadReport();
  EXPECT_THAT(
      load_report,
      ::testing::ElementsAre(
          ::testing::AllOf(
              ::testing::Property(&ClientStats::cluster_name,
                                  kDefaultClusterName),
              ::testing::Property(
                  &ClientStats::locality_stats,
                  ::testing::ElementsAre(::testing::Pair(
                      "locality0",
                      ::testing::AllOf(
                          ::testing::Field(&ClientStats::LocalityStats::
                                               total_successful_requests,
                                           ::testing::Lt(num_ok)),
                          ::testing::Field(&ClientStats::LocalityStats::
                                               total_requests_in_progress,
                                           0UL),
                          ::testing::Field(
                              &ClientStats::LocalityStats::total_error_requests,
                              ::testing::Le(num_failure)),
                          ::testing::Field(
                              &ClientStats::LocalityStats::
                                  total_issued_requests,
                              ::testing::Le(num_failure + num_ok)))))),
              ::testing::Property(&ClientStats::total_dropped_requests,
                                  num_drops)),
          ::testing::AllOf(
              ::testing::Property(&ClientStats::cluster_name, kNewClusterName),
              ::testing::Property(
                  &ClientStats::locality_stats,
                  ::testing::ElementsAre(::testing::Pair(
                      "locality1",
                      ::testing::AllOf(
                          ::testing::Field(&ClientStats::LocalityStats::
                                               total_successful_requests,
                                           ::testing::Le(num_ok)),
                          ::testing::Field(&ClientStats::LocalityStats::
                                               total_requests_in_progress,
                                           0UL),
                          ::testing::Field(
                              &ClientStats::LocalityStats::total_error_requests,
                              ::testing::Le(num_failure)),
                          ::testing::Field(
                              &ClientStats::LocalityStats::
                                  total_issued_requests,
                              ::testing::Le(num_failure + num_ok)))))),
              ::testing::Property(&ClientStats::total_dropped_requests,
                                  num_drops))));
  int total_ok = 0;
  int total_failure = 0;
  for (const ClientStats& client_stats : load_report) {
    total_ok += client_stats.total_successful_requests();
    total_failure += client_stats.total_error_requests();
  }
  EXPECT_EQ(total_ok, num_ok);
  EXPECT_EQ(total_failure, num_failure);
  EXPECT_EQ(1U, balancers_[0]->lrs_service()->request_count());
  EXPECT_EQ(1U, balancers_[0]->lrs_service()->response_count());
}
using SecureNamingTest = BasicTest;
TEST_P(SecureNamingTest, TargetNameIsExpected) {
  SetNextResolution({});
  SetNextResolutionForLbChannel({balancers_[0]->port()}, nullptr, "xds_server");
  AdsServiceImpl::EdsResourceArgs args({
      {"locality0", GetBackendPorts()},
  });
  balancers_[0]->ads_service()->SetEdsResource(
      BuildEdsResource(args, DefaultEdsServiceName()));
  CheckRpcSendOk();
}
TEST_P(SecureNamingTest, TargetNameIsUnexpected) {
  ::testing::FLAGS_gtest_death_test_style = "threadsafe";
  SetNextResolution({});
  SetNextResolutionForLbChannel({balancers_[0]->port()}, nullptr,
                                "incorrect_server_name");
  AdsServiceImpl::EdsResourceArgs args({
      {"locality0", GetBackendPorts()},
  });
  balancers_[0]->ads_service()->SetEdsResource(
      BuildEdsResource(args, DefaultEdsServiceName()));
  ASSERT_DEATH_IF_SUPPORTED({ CheckRpcSendOk(); }, "");
}
using LdsTest = BasicTest;
TEST_P(LdsTest, NoApiListener) {
  auto listener = default_listener_;
  listener.clear_api_listener();
  balancers_[0]->ads_service()->SetLdsResource(listener);
  SetNextResolution({});
  SetNextResolutionForLbChannelAllBalancers();
  CheckRpcSendFailure();
  const auto& response_state =
      balancers_[0]->ads_service()->lds_response_state();
  EXPECT_EQ(response_state.state, AdsServiceImpl::ResponseState::NACKED);
  EXPECT_EQ(response_state.error_message, "Listener has no ApiListener.");
}
TEST_P(LdsTest, WrongRouteSpecifier) {
  auto listener = default_listener_;
  HttpConnectionManager http_connection_manager;
  http_connection_manager.mutable_scoped_routes();
  listener.mutable_api_listener()->mutable_api_listener()->PackFrom(
      http_connection_manager);
  balancers_[0]->ads_service()->SetLdsResource(listener);
  SetNextResolution({});
  SetNextResolutionForLbChannelAllBalancers();
  CheckRpcSendFailure();
  const auto& response_state =
      balancers_[0]->ads_service()->lds_response_state();
  EXPECT_EQ(response_state.state, AdsServiceImpl::ResponseState::NACKED);
  EXPECT_EQ(response_state.error_message,
            "HttpConnectionManager neither has inlined route_config nor RDS.");
}
TEST_P(LdsTest, RdsMissingConfigSource) {
  auto listener = default_listener_;
  HttpConnectionManager http_connection_manager;
  http_connection_manager.mutable_rds()->set_route_config_name(
      kDefaultRouteConfigurationName);
  listener.mutable_api_listener()->mutable_api_listener()->PackFrom(
      http_connection_manager);
  balancers_[0]->ads_service()->SetLdsResource(listener);
  SetNextResolution({});
  SetNextResolutionForLbChannelAllBalancers();
  CheckRpcSendFailure();
  const auto& response_state =
      balancers_[0]->ads_service()->lds_response_state();
  EXPECT_EQ(response_state.state, AdsServiceImpl::ResponseState::NACKED);
  EXPECT_EQ(response_state.error_message,
            "HttpConnectionManager missing config_source for RDS.");
}
TEST_P(LdsTest, RdsConfigSourceDoesNotSpecifyAds) {
  auto listener = default_listener_;
  HttpConnectionManager http_connection_manager;
  auto* rds = http_connection_manager.mutable_rds();
  rds->set_route_config_name(kDefaultRouteConfigurationName);
  rds->mutable_config_source()->mutable_self();
  listener.mutable_api_listener()->mutable_api_listener()->PackFrom(
      http_connection_manager);
  balancers_[0]->ads_service()->SetLdsResource(listener);
  SetNextResolution({});
  SetNextResolutionForLbChannelAllBalancers();
  CheckRpcSendFailure();
  const auto& response_state =
      balancers_[0]->ads_service()->lds_response_state();
  EXPECT_EQ(response_state.state, AdsServiceImpl::ResponseState::NACKED);
  EXPECT_EQ(response_state.error_message,
            "HttpConnectionManager ConfigSource for RDS does not specify ADS.");
}
using LdsRdsTest = BasicTest;
TEST_P(LdsRdsTest, Vanilla) {
  SetNextResolution({});
  SetNextResolutionForLbChannelAllBalancers();
  (void)SendRpc();
  EXPECT_EQ(RouteConfigurationResponseState(0).state,
            AdsServiceImpl::ResponseState::ACKED);
  EXPECT_EQ(balancers_[0]->ads_service()->seen_v2_client(),
            GetParam().use_v2());
  EXPECT_NE(balancers_[0]->ads_service()->seen_v3_client(),
            GetParam().use_v2());
}
TEST_P(LdsRdsTest, ListenerRemoved) {
  SetNextResolution({});
  SetNextResolutionForLbChannelAllBalancers();
  AdsServiceImpl::EdsResourceArgs args({
      {"locality0", GetBackendPorts()},
  });
  balancers_[0]->ads_service()->SetEdsResource(BuildEdsResource(args));
  WaitForAllBackends();
  balancers_[0]->ads_service()->UnsetResource(kLdsTypeUrl, kServerName);
  do {
  } while (SendRpc(RpcOptions(), nullptr).ok());
  CheckRpcSendFailure(1000);
  EXPECT_EQ(balancers_[0]->ads_service()->lds_response_state().state,
            AdsServiceImpl::ResponseState::ACKED);
}
TEST_P(LdsRdsTest, NoMatchedDomain) {
  RouteConfiguration route_config = default_route_config_;
  route_config.mutable_virtual_hosts(0)->clear_domains();
  route_config.mutable_virtual_hosts(0)->add_domains("unmatched_domain");
  SetRouteConfiguration(0, route_config);
  SetNextResolution({});
  SetNextResolutionForLbChannelAllBalancers();
  CheckRpcSendFailure();
  channel_->WaitForConnected(grpc_timeout_milliseconds_to_deadline(100));
  const auto& response_state = RouteConfigurationResponseState(0);
  EXPECT_EQ(response_state.state, AdsServiceImpl::ResponseState::ACKED);
}
TEST_P(LdsRdsTest, ChooseMatchedDomain) {
  RouteConfiguration route_config = default_route_config_;
  *(route_config.add_virtual_hosts()) = route_config.virtual_hosts(0);
  route_config.mutable_virtual_hosts(0)->clear_domains();
  route_config.mutable_virtual_hosts(0)->add_domains("unmatched_domain");
  SetRouteConfiguration(0, route_config);
  SetNextResolution({});
  SetNextResolutionForLbChannelAllBalancers();
  (void)SendRpc();
  EXPECT_EQ(RouteConfigurationResponseState(0).state,
            AdsServiceImpl::ResponseState::ACKED);
}
TEST_P(LdsRdsTest, ChooseLastRoute) {
  RouteConfiguration route_config = default_route_config_;
  *(route_config.mutable_virtual_hosts(0)->add_routes()) =
      route_config.virtual_hosts(0).routes(0);
  route_config.mutable_virtual_hosts(0)
      ->mutable_routes(0)
      ->mutable_route()
      ->mutable_cluster_header();
  SetRouteConfiguration(0, route_config);
  SetNextResolution({});
  SetNextResolutionForLbChannelAllBalancers();
  (void)SendRpc();
  EXPECT_EQ(RouteConfigurationResponseState(0).state,
            AdsServiceImpl::ResponseState::ACKED);
}
TEST_P(LdsRdsTest, RouteMatchHasQueryParameters) {
  RouteConfiguration route_config = default_route_config_;
  auto* route1 = route_config.mutable_virtual_hosts(0)->mutable_routes(0);
  route1->mutable_match()->set_prefix("/grpc.testing.EchoTest1Service/");
  route1->mutable_match()->add_query_parameters();
  SetRouteConfiguration(0, route_config);
  SetNextResolution({});
  SetNextResolutionForLbChannelAllBalancers();
  CheckRpcSendFailure();
  const auto& response_state = RouteConfigurationResponseState(0);
  EXPECT_EQ(response_state.state, AdsServiceImpl::ResponseState::NACKED);
  EXPECT_EQ(response_state.error_message, "No valid routes specified.");
}
TEST_P(LdsRdsTest, RouteMatchHasValidPrefixEmptyOrSingleSlash) {
  RouteConfiguration route_config = default_route_config_;
  auto* route1 = route_config.mutable_virtual_hosts(0)->mutable_routes(0);
  route1->mutable_match()->set_prefix("");
  auto* default_route = route_config.mutable_virtual_hosts(0)->add_routes();
  default_route->mutable_match()->set_prefix("/");
  default_route->mutable_route()->set_cluster(kDefaultClusterName);
  SetRouteConfiguration(0, route_config);
  SetNextResolution({});
  SetNextResolutionForLbChannelAllBalancers();
  (void)SendRpc();
  const auto& response_state = RouteConfigurationResponseState(0);
  EXPECT_EQ(response_state.state, AdsServiceImpl::ResponseState::ACKED);
}
TEST_P(LdsRdsTest, RouteMatchHasInvalidPrefixNoLeadingSlash) {
  RouteConfiguration route_config = default_route_config_;
  auto* route1 = route_config.mutable_virtual_hosts(0)->mutable_routes(0);
  route1->mutable_match()->set_prefix("grpc.testing.EchoTest1Service/");
  SetRouteConfiguration(0, route_config);
  SetNextResolution({});
  SetNextResolutionForLbChannelAllBalancers();
  CheckRpcSendFailure();
  const auto& response_state = RouteConfigurationResponseState(0);
  EXPECT_EQ(response_state.state, AdsServiceImpl::ResponseState::NACKED);
  EXPECT_EQ(response_state.error_message, "No valid routes specified.");
}
TEST_P(LdsRdsTest, RouteMatchHasInvalidPrefixExtraContent) {
  RouteConfiguration route_config = default_route_config_;
  auto* route1 = route_config.mutable_virtual_hosts(0)->mutable_routes(0);
  route1->mutable_match()->set_prefix("/grpc.testing.EchoTest1Service/Echo1/");
  SetRouteConfiguration(0, route_config);
  SetNextResolution({});
  SetNextResolutionForLbChannelAllBalancers();
  CheckRpcSendFailure();
  const auto& response_state = RouteConfigurationResponseState(0);
  EXPECT_EQ(response_state.state, AdsServiceImpl::ResponseState::NACKED);
  EXPECT_EQ(response_state.error_message, "No valid routes specified.");
}
TEST_P(LdsRdsTest, RouteMatchHasInvalidPrefixDoubleSlash) {
  RouteConfiguration route_config = default_route_config_;
  auto* route1 = route_config.mutable_virtual_hosts(0)->mutable_routes(0);
  route1->mutable_match()->set_prefix("//");
  SetRouteConfiguration(0, route_config);
  SetNextResolution({});
  SetNextResolutionForLbChannelAllBalancers();
  CheckRpcSendFailure();
  const auto& response_state = RouteConfigurationResponseState(0);
  EXPECT_EQ(response_state.state, AdsServiceImpl::ResponseState::NACKED);
  EXPECT_EQ(response_state.error_message, "No valid routes specified.");
}
TEST_P(LdsRdsTest, RouteMatchHasInvalidPathEmptyPath) {
  RouteConfiguration route_config = default_route_config_;
  auto* route1 = route_config.mutable_virtual_hosts(0)->mutable_routes(0);
  route1->mutable_match()->set_path("");
  SetRouteConfiguration(0, route_config);
  SetNextResolution({});
  SetNextResolutionForLbChannelAllBalancers();
  CheckRpcSendFailure();
  const auto& response_state = RouteConfigurationResponseState(0);
  EXPECT_EQ(response_state.state, AdsServiceImpl::ResponseState::NACKED);
  EXPECT_EQ(response_state.error_message, "No valid routes specified.");
}
TEST_P(LdsRdsTest, RouteMatchHasInvalidPathNoLeadingSlash) {
  RouteConfiguration route_config = default_route_config_;
  auto* route1 = route_config.mutable_virtual_hosts(0)->mutable_routes(0);
  route1->mutable_match()->set_path("grpc.testing.EchoTest1Service/Echo1");
  SetRouteConfiguration(0, route_config);
  SetNextResolution({});
  SetNextResolutionForLbChannelAllBalancers();
  CheckRpcSendFailure();
  const auto& response_state = RouteConfigurationResponseState(0);
  EXPECT_EQ(response_state.state, AdsServiceImpl::ResponseState::NACKED);
  EXPECT_EQ(response_state.error_message, "No valid routes specified.");
}
TEST_P(LdsRdsTest, RouteMatchHasInvalidPathTooManySlashes) {
  RouteConfiguration route_config = default_route_config_;
  auto* route1 = route_config.mutable_virtual_hosts(0)->mutable_routes(0);
  route1->mutable_match()->set_path("/grpc.testing.EchoTest1Service/Echo1/");
  SetRouteConfiguration(0, route_config);
  SetNextResolution({});
  SetNextResolutionForLbChannelAllBalancers();
  CheckRpcSendFailure();
  const auto& response_state = RouteConfigurationResponseState(0);
  EXPECT_EQ(response_state.state, AdsServiceImpl::ResponseState::NACKED);
  EXPECT_EQ(response_state.error_message, "No valid routes specified.");
}
TEST_P(LdsRdsTest, RouteMatchHasInvalidPathOnlyOneSlash) {
  RouteConfiguration route_config = default_route_config_;
  auto* route1 = route_config.mutable_virtual_hosts(0)->mutable_routes(0);
  route1->mutable_match()->set_path("/grpc.testing.EchoTest1Service.Echo1");
  SetRouteConfiguration(0, route_config);
  SetNextResolution({});
  SetNextResolutionForLbChannelAllBalancers();
  CheckRpcSendFailure();
  const auto& response_state = RouteConfigurationResponseState(0);
  EXPECT_EQ(response_state.state, AdsServiceImpl::ResponseState::NACKED);
  EXPECT_EQ(response_state.error_message, "No valid routes specified.");
}
TEST_P(LdsRdsTest, RouteMatchHasInvalidPathMissingService) {
  RouteConfiguration route_config = default_route_config_;
  auto* route1 = route_config.mutable_virtual_hosts(0)->mutable_routes(0);
  route1->mutable_match()->set_path("//Echo1");
  SetRouteConfiguration(0, route_config);
  SetNextResolution({});
  SetNextResolutionForLbChannelAllBalancers();
  CheckRpcSendFailure();
  const auto& response_state = RouteConfigurationResponseState(0);
  EXPECT_EQ(response_state.state, AdsServiceImpl::ResponseState::NACKED);
  EXPECT_EQ(response_state.error_message, "No valid routes specified.");
}
TEST_P(LdsRdsTest, RouteMatchHasInvalidPathMissingMethod) {
  RouteConfiguration route_config = default_route_config_;
  auto* route1 = route_config.mutable_virtual_hosts(0)->mutable_routes(0);
  route1->mutable_match()->set_path("/grpc.testing.EchoTest1Service/");
  SetRouteConfiguration(0, route_config);
  SetNextResolution({});
  SetNextResolutionForLbChannelAllBalancers();
  CheckRpcSendFailure();
  const auto& response_state = RouteConfigurationResponseState(0);
  EXPECT_EQ(response_state.state, AdsServiceImpl::ResponseState::NACKED);
  EXPECT_EQ(response_state.error_message, "No valid routes specified.");
}
TEST_P(LdsRdsTest, RouteMatchHasInvalidPathRegex) {
  const char* kNewCluster1Name = "new_cluster_1";
  RouteConfiguration route_config = default_route_config_;
  auto* route1 = route_config.mutable_virtual_hosts(0)->mutable_routes(0);
  route1->mutable_match()->mutable_safe_regex()->set_regex("a[z-a]");
  route1->mutable_route()->set_cluster(kNewCluster1Name);
  SetRouteConfiguration(0, route_config);
  SetNextResolution({});
  SetNextResolutionForLbChannelAllBalancers();
  CheckRpcSendFailure();
  const auto& response_state = RouteConfigurationResponseState(0);
  EXPECT_EQ(response_state.state, AdsServiceImpl::ResponseState::NACKED);
  EXPECT_EQ(response_state.error_message,
            "Invalid regex string specified in path matcher.");
}
TEST_P(LdsRdsTest, RouteHasNoRouteAction) {
  RouteConfiguration route_config = default_route_config_;
  route_config.mutable_virtual_hosts(0)->mutable_routes(0)->mutable_redirect();
  SetRouteConfiguration(0, route_config);
  SetNextResolution({});
  SetNextResolutionForLbChannelAllBalancers();
  CheckRpcSendFailure();
  const auto& response_state = RouteConfigurationResponseState(0);
  EXPECT_EQ(response_state.state, AdsServiceImpl::ResponseState::NACKED);
  EXPECT_EQ(response_state.error_message, "No RouteAction found in route.");
}
TEST_P(LdsRdsTest, RouteActionClusterHasEmptyClusterName) {
  RouteConfiguration route_config = default_route_config_;
  auto* route1 = route_config.mutable_virtual_hosts(0)->mutable_routes(0);
  route1->mutable_match()->set_prefix("/grpc.testing.EchoTest1Service/");
  route1->mutable_route()->set_cluster("");
  auto* default_route = route_config.mutable_virtual_hosts(0)->add_routes();
  default_route->mutable_match()->set_prefix("");
  default_route->mutable_route()->set_cluster(kDefaultClusterName);
  SetRouteConfiguration(0, route_config);
  SetNextResolution({});
  SetNextResolutionForLbChannelAllBalancers();
  CheckRpcSendFailure();
  const auto& response_state = RouteConfigurationResponseState(0);
  EXPECT_EQ(response_state.state, AdsServiceImpl::ResponseState::NACKED);
  EXPECT_EQ(response_state.error_message,
            "RouteAction cluster contains empty cluster name.");
}
TEST_P(LdsRdsTest, RouteActionWeightedTargetHasIncorrectTotalWeightSet) {
  const size_t kWeight75 = 75;
  const char* kNewCluster1Name = "new_cluster_1";
  RouteConfiguration route_config = default_route_config_;
  auto* route1 = route_config.mutable_virtual_hosts(0)->mutable_routes(0);
  route1->mutable_match()->set_prefix("/grpc.testing.EchoTest1Service/");
  auto* weighted_cluster1 =
      route1->mutable_route()->mutable_weighted_clusters()->add_clusters();
  weighted_cluster1->set_name(kNewCluster1Name);
  weighted_cluster1->mutable_weight()->set_value(kWeight75);
  route1->mutable_route()
      ->mutable_weighted_clusters()
      ->mutable_total_weight()
      ->set_value(kWeight75 + 1);
  auto* default_route = route_config.mutable_virtual_hosts(0)->add_routes();
  default_route->mutable_match()->set_prefix("");
  default_route->mutable_route()->set_cluster(kDefaultClusterName);
  SetRouteConfiguration(0, route_config);
  SetNextResolution({});
  SetNextResolutionForLbChannelAllBalancers();
  CheckRpcSendFailure();
  const auto& response_state = RouteConfigurationResponseState(0);
  EXPECT_EQ(response_state.state, AdsServiceImpl::ResponseState::NACKED);
  EXPECT_EQ(response_state.error_message,
            "RouteAction weighted_cluster has incorrect total weight");
}
TEST_P(LdsRdsTest, RouteActionWeightedTargetClusterHasEmptyClusterName) {
  const size_t kWeight75 = 75;
  RouteConfiguration route_config = default_route_config_;
  auto* route1 = route_config.mutable_virtual_hosts(0)->mutable_routes(0);
  route1->mutable_match()->set_prefix("/grpc.testing.EchoTest1Service/");
  auto* weighted_cluster1 =
      route1->mutable_route()->mutable_weighted_clusters()->add_clusters();
  weighted_cluster1->set_name("");
  weighted_cluster1->mutable_weight()->set_value(kWeight75);
  route1->mutable_route()
      ->mutable_weighted_clusters()
      ->mutable_total_weight()
      ->set_value(kWeight75);
  auto* default_route = route_config.mutable_virtual_hosts(0)->add_routes();
  default_route->mutable_match()->set_prefix("");
  default_route->mutable_route()->set_cluster(kDefaultClusterName);
  SetRouteConfiguration(0, route_config);
  SetNextResolution({});
  SetNextResolutionForLbChannelAllBalancers();
  CheckRpcSendFailure();
  const auto& response_state = RouteConfigurationResponseState(0);
  EXPECT_EQ(response_state.state, AdsServiceImpl::ResponseState::NACKED);
  EXPECT_EQ(
      response_state.error_message,
      "RouteAction weighted_cluster cluster contains empty cluster name.");
}
TEST_P(LdsRdsTest, RouteActionWeightedTargetClusterHasNoWeight) {
  const size_t kWeight75 = 75;
  const char* kNewCluster1Name = "new_cluster_1";
  RouteConfiguration route_config = default_route_config_;
  auto* route1 = route_config.mutable_virtual_hosts(0)->mutable_routes(0);
  route1->mutable_match()->set_prefix("/grpc.testing.EchoTest1Service/");
  auto* weighted_cluster1 =
      route1->mutable_route()->mutable_weighted_clusters()->add_clusters();
  weighted_cluster1->set_name(kNewCluster1Name);
  route1->mutable_route()
      ->mutable_weighted_clusters()
      ->mutable_total_weight()
      ->set_value(kWeight75);
  auto* default_route = route_config.mutable_virtual_hosts(0)->add_routes();
  default_route->mutable_match()->set_prefix("");
  default_route->mutable_route()->set_cluster(kDefaultClusterName);
  SetRouteConfiguration(0, route_config);
  SetNextResolution({});
  SetNextResolutionForLbChannelAllBalancers();
  CheckRpcSendFailure();
  const auto& response_state = RouteConfigurationResponseState(0);
  EXPECT_EQ(response_state.state, AdsServiceImpl::ResponseState::NACKED);
  EXPECT_EQ(response_state.error_message,
            "RouteAction weighted_cluster cluster missing weight");
}
TEST_P(LdsRdsTest, RouteHeaderMatchInvalidRegex) {
  const char* kNewCluster1Name = "new_cluster_1";
  RouteConfiguration route_config = default_route_config_;
  auto* route1 = route_config.mutable_virtual_hosts(0)->mutable_routes(0);
  route1->mutable_match()->set_prefix("/grpc.testing.EchoTest1Service/");
  auto* header_matcher1 = route1->mutable_match()->add_headers();
  header_matcher1->set_name("header1");
  header_matcher1->mutable_safe_regex_match()->set_regex("a[z-a]");
  route1->mutable_route()->set_cluster(kNewCluster1Name);
  SetRouteConfiguration(0, route_config);
  SetNextResolution({});
  SetNextResolutionForLbChannelAllBalancers();
  CheckRpcSendFailure();
  const auto& response_state = RouteConfigurationResponseState(0);
  EXPECT_EQ(response_state.state, AdsServiceImpl::ResponseState::NACKED);
  EXPECT_EQ(response_state.error_message,
            "Invalid regex string specified in header matcher.");
}
TEST_P(LdsRdsTest, RouteHeaderMatchInvalidRange) {
  const char* kNewCluster1Name = "new_cluster_1";
  RouteConfiguration route_config = default_route_config_;
  auto* route1 = route_config.mutable_virtual_hosts(0)->mutable_routes(0);
  route1->mutable_match()->set_prefix("/grpc.testing.EchoTest1Service/");
  auto* header_matcher1 = route1->mutable_match()->add_headers();
  header_matcher1->set_name("header1");
  header_matcher1->mutable_range_match()->set_start(1001);
  header_matcher1->mutable_range_match()->set_end(1000);
  route1->mutable_route()->set_cluster(kNewCluster1Name);
  SetRouteConfiguration(0, route_config);
  SetNextResolution({});
  SetNextResolutionForLbChannelAllBalancers();
  CheckRpcSendFailure();
  const auto& response_state = RouteConfigurationResponseState(0);
  EXPECT_EQ(response_state.state, AdsServiceImpl::ResponseState::NACKED);
  EXPECT_EQ(response_state.error_message,
            "Invalid range header matcher specifier specified: end "
            "cannot be smaller than start.");
}
TEST_P(LdsRdsTest, XdsRoutingPathMatching) {
  const char* kNewCluster1Name = "new_cluster_1";
  const char* kNewEdsService1Name = "new_eds_service_name_1";
  const char* kNewCluster2Name = "new_cluster_2";
  const char* kNewEdsService2Name = "new_eds_service_name_2";
  const size_t kNumEcho1Rpcs = 10;
  const size_t kNumEcho2Rpcs = 20;
  const size_t kNumEchoRpcs = 30;
  SetNextResolution({});
  SetNextResolutionForLbChannelAllBalancers();
  AdsServiceImpl::EdsResourceArgs args({
      {"locality0", GetBackendPorts(0, 2)},
  });
  AdsServiceImpl::EdsResourceArgs args1({
      {"locality0", GetBackendPorts(2, 3)},
  });
  AdsServiceImpl::EdsResourceArgs args2({
      {"locality0", GetBackendPorts(3, 4)},
  });
  balancers_[0]->ads_service()->SetEdsResource(BuildEdsResource(args));
  balancers_[0]->ads_service()->SetEdsResource(
      BuildEdsResource(args1, kNewEdsService1Name));
  balancers_[0]->ads_service()->SetEdsResource(
      BuildEdsResource(args2, kNewEdsService2Name));
  Cluster new_cluster1 = default_cluster_;
  new_cluster1.set_name(kNewCluster1Name);
  new_cluster1.mutable_eds_cluster_config()->set_service_name(
      kNewEdsService1Name);
  balancers_[0]->ads_service()->SetCdsResource(new_cluster1);
  Cluster new_cluster2 = default_cluster_;
  new_cluster2.set_name(kNewCluster2Name);
  new_cluster2.mutable_eds_cluster_config()->set_service_name(
      kNewEdsService2Name);
  balancers_[0]->ads_service()->SetCdsResource(new_cluster2);
  RouteConfiguration new_route_config = default_route_config_;
  auto* route1 = new_route_config.mutable_virtual_hosts(0)->mutable_routes(0);
  route1->mutable_match()->set_path("/grpc.testing.EchoTest1Service/Echo1");
  route1->mutable_route()->set_cluster(kNewCluster1Name);
  auto* route2 = new_route_config.mutable_virtual_hosts(0)->add_routes();
  route2->mutable_match()->set_path("/grpc.testing.EchoTest2Service/Echo2");
  route2->mutable_route()->set_cluster(kNewCluster2Name);
  auto* route3 = new_route_config.mutable_virtual_hosts(0)->add_routes();
  route3->mutable_match()->set_path("/grpc.testing.EchoTest3Service/Echo3");
  route3->mutable_route()->set_cluster(kDefaultClusterName);
  auto* default_route = new_route_config.mutable_virtual_hosts(0)->add_routes();
  default_route->mutable_match()->set_prefix("");
  default_route->mutable_route()->set_cluster(kDefaultClusterName);
  SetRouteConfiguration(0, new_route_config);
  WaitForAllBackends(0, 2);
  CheckRpcSendOk(kNumEchoRpcs, RpcOptions().set_wait_for_ready(true));
  CheckRpcSendOk(kNumEcho1Rpcs, RpcOptions()
                                    .set_rpc_service(SERVICE_ECHO1)
                                    .set_rpc_method(METHOD_ECHO1)
                                    .set_wait_for_ready(true));
  CheckRpcSendOk(kNumEcho2Rpcs, RpcOptions()
                                    .set_rpc_service(SERVICE_ECHO2)
                                    .set_rpc_method(METHOD_ECHO2)
                                    .set_wait_for_ready(true));
  for (size_t i = 0; i < 2; ++i) {
    EXPECT_EQ(kNumEchoRpcs / 2,
              backends_[i]->backend_service()->request_count());
    EXPECT_EQ(0, backends_[i]->backend_service1()->request_count());
    EXPECT_EQ(0, backends_[i]->backend_service2()->request_count());
  }
  EXPECT_EQ(0, backends_[2]->backend_service()->request_count());
  EXPECT_EQ(kNumEcho1Rpcs, backends_[2]->backend_service1()->request_count());
  EXPECT_EQ(0, backends_[2]->backend_service2()->request_count());
  EXPECT_EQ(0, backends_[3]->backend_service()->request_count());
  EXPECT_EQ(0, backends_[3]->backend_service1()->request_count());
  EXPECT_EQ(kNumEcho2Rpcs, backends_[3]->backend_service2()->request_count());
}
TEST_P(LdsRdsTest, XdsRoutingPathMatchingCaseInsensitive) {
  const char* kNewCluster1Name = "new_cluster_1";
  const char* kNewEdsService1Name = "new_eds_service_name_1";
  const char* kNewCluster2Name = "new_cluster_2";
  const char* kNewEdsService2Name = "new_eds_service_name_2";
  const size_t kNumEcho1Rpcs = 10;
  const size_t kNumEchoRpcs = 30;
  SetNextResolution({});
  SetNextResolutionForLbChannelAllBalancers();
  AdsServiceImpl::EdsResourceArgs args({
      {"locality0", GetBackendPorts(0, 1)},
  });
  AdsServiceImpl::EdsResourceArgs args1({
      {"locality0", GetBackendPorts(1, 2)},
  });
  AdsServiceImpl::EdsResourceArgs args2({
      {"locality0", GetBackendPorts(2, 3)},
  });
  balancers_[0]->ads_service()->SetEdsResource(BuildEdsResource(args));
  balancers_[0]->ads_service()->SetEdsResource(
      BuildEdsResource(args1, kNewEdsService1Name));
  balancers_[0]->ads_service()->SetEdsResource(
      BuildEdsResource(args2, kNewEdsService2Name));
  Cluster new_cluster1 = default_cluster_;
  new_cluster1.set_name(kNewCluster1Name);
  new_cluster1.mutable_eds_cluster_config()->set_service_name(
      kNewEdsService1Name);
  balancers_[0]->ads_service()->SetCdsResource(new_cluster1);
  Cluster new_cluster2 = default_cluster_;
  new_cluster2.set_name(kNewCluster2Name);
  new_cluster2.mutable_eds_cluster_config()->set_service_name(
      kNewEdsService2Name);
  balancers_[0]->ads_service()->SetCdsResource(new_cluster2);
  RouteConfiguration new_route_config = default_route_config_;
  auto* route1 = new_route_config.mutable_virtual_hosts(0)->mutable_routes(0);
  route1->mutable_match()->set_path("/GrPc.TeStInG.EcHoTeSt1SErViCe/EcHo1");
  route1->mutable_route()->set_cluster(kNewCluster1Name);
  auto* route2 = new_route_config.mutable_virtual_hosts(0)->add_routes();
  route2->mutable_match()->set_path("/GrPc.TeStInG.EcHoTeSt1SErViCe/EcHo1");
  route2->mutable_match()->mutable_case_sensitive()->set_value(false);
  route2->mutable_route()->set_cluster(kNewCluster2Name);
  auto* default_route = new_route_config.mutable_virtual_hosts(0)->add_routes();
  default_route->mutable_match()->set_prefix("");
  default_route->mutable_route()->set_cluster(kDefaultClusterName);
  SetRouteConfiguration(0, new_route_config);
  CheckRpcSendOk(kNumEchoRpcs, RpcOptions().set_wait_for_ready(true));
  CheckRpcSendOk(kNumEcho1Rpcs, RpcOptions()
                                    .set_rpc_service(SERVICE_ECHO1)
                                    .set_rpc_method(METHOD_ECHO1)
                                    .set_wait_for_ready(true));
  EXPECT_EQ(kNumEchoRpcs, backends_[0]->backend_service()->request_count());
  EXPECT_EQ(0, backends_[0]->backend_service1()->request_count());
  EXPECT_EQ(0, backends_[1]->backend_service()->request_count());
  EXPECT_EQ(0, backends_[1]->backend_service1()->request_count());
  EXPECT_EQ(0, backends_[2]->backend_service()->request_count());
  EXPECT_EQ(kNumEcho1Rpcs, backends_[2]->backend_service1()->request_count());
}
TEST_P(LdsRdsTest, XdsRoutingPrefixMatching) {
  const char* kNewCluster1Name = "new_cluster_1";
  const char* kNewEdsService1Name = "new_eds_service_name_1";
  const char* kNewCluster2Name = "new_cluster_2";
  const char* kNewEdsService2Name = "new_eds_service_name_2";
  const size_t kNumEcho1Rpcs = 10;
  const size_t kNumEcho2Rpcs = 20;
  const size_t kNumEchoRpcs = 30;
  SetNextResolution({});
  SetNextResolutionForLbChannelAllBalancers();
  AdsServiceImpl::EdsResourceArgs args({
      {"locality0", GetBackendPorts(0, 2)},
  });
  AdsServiceImpl::EdsResourceArgs args1({
      {"locality0", GetBackendPorts(2, 3)},
  });
  AdsServiceImpl::EdsResourceArgs args2({
      {"locality0", GetBackendPorts(3, 4)},
  });
  balancers_[0]->ads_service()->SetEdsResource(BuildEdsResource(args));
  balancers_[0]->ads_service()->SetEdsResource(
      BuildEdsResource(args1, kNewEdsService1Name));
  balancers_[0]->ads_service()->SetEdsResource(
      BuildEdsResource(args2, kNewEdsService2Name));
  Cluster new_cluster1 = default_cluster_;
  new_cluster1.set_name(kNewCluster1Name);
  new_cluster1.mutable_eds_cluster_config()->set_service_name(
      kNewEdsService1Name);
  balancers_[0]->ads_service()->SetCdsResource(new_cluster1);
  Cluster new_cluster2 = default_cluster_;
  new_cluster2.set_name(kNewCluster2Name);
  new_cluster2.mutable_eds_cluster_config()->set_service_name(
      kNewEdsService2Name);
  balancers_[0]->ads_service()->SetCdsResource(new_cluster2);
  RouteConfiguration new_route_config = default_route_config_;
  auto* route1 = new_route_config.mutable_virtual_hosts(0)->mutable_routes(0);
  route1->mutable_match()->set_prefix("/grpc.testing.EchoTest1Service/");
  route1->mutable_route()->set_cluster(kNewCluster1Name);
  auto* route2 = new_route_config.mutable_virtual_hosts(0)->add_routes();
  route2->mutable_match()->set_prefix("/grpc.testing.EchoTest2Service/");
  route2->mutable_route()->set_cluster(kNewCluster2Name);
  auto* default_route = new_route_config.mutable_virtual_hosts(0)->add_routes();
  default_route->mutable_match()->set_prefix("");
  default_route->mutable_route()->set_cluster(kDefaultClusterName);
  SetRouteConfiguration(0, new_route_config);
  WaitForAllBackends(0, 2);
  CheckRpcSendOk(kNumEchoRpcs, RpcOptions().set_wait_for_ready(true));
  CheckRpcSendOk(
      kNumEcho1Rpcs,
      RpcOptions().set_rpc_service(SERVICE_ECHO1).set_wait_for_ready(true));
  CheckRpcSendOk(
      kNumEcho2Rpcs,
      RpcOptions().set_rpc_service(SERVICE_ECHO2).set_wait_for_ready(true));
  for (size_t i = 0; i < 2; ++i) {
    EXPECT_EQ(kNumEchoRpcs / 2,
              backends_[i]->backend_service()->request_count());
    EXPECT_EQ(0, backends_[i]->backend_service1()->request_count());
    EXPECT_EQ(0, backends_[i]->backend_service2()->request_count());
  }
  EXPECT_EQ(0, backends_[2]->backend_service()->request_count());
  EXPECT_EQ(kNumEcho1Rpcs, backends_[2]->backend_service1()->request_count());
  EXPECT_EQ(0, backends_[2]->backend_service2()->request_count());
  EXPECT_EQ(0, backends_[3]->backend_service()->request_count());
  EXPECT_EQ(0, backends_[3]->backend_service1()->request_count());
  EXPECT_EQ(kNumEcho2Rpcs, backends_[3]->backend_service2()->request_count());
}
TEST_P(LdsRdsTest, XdsRoutingPrefixMatchingCaseInsensitive) {
  const char* kNewCluster1Name = "new_cluster_1";
  const char* kNewEdsService1Name = "new_eds_service_name_1";
  const char* kNewCluster2Name = "new_cluster_2";
  const char* kNewEdsService2Name = "new_eds_service_name_2";
  const size_t kNumEcho1Rpcs = 10;
  const size_t kNumEchoRpcs = 30;
  SetNextResolution({});
  SetNextResolutionForLbChannelAllBalancers();
  AdsServiceImpl::EdsResourceArgs args({
      {"locality0", GetBackendPorts(0, 1)},
  });
  AdsServiceImpl::EdsResourceArgs args1({
      {"locality0", GetBackendPorts(1, 2)},
  });
  AdsServiceImpl::EdsResourceArgs args2({
      {"locality0", GetBackendPorts(2, 3)},
  });
  balancers_[0]->ads_service()->SetEdsResource(BuildEdsResource(args));
  balancers_[0]->ads_service()->SetEdsResource(
      BuildEdsResource(args1, kNewEdsService1Name));
  balancers_[0]->ads_service()->SetEdsResource(
      BuildEdsResource(args2, kNewEdsService2Name));
  Cluster new_cluster1 = default_cluster_;
  new_cluster1.set_name(kNewCluster1Name);
  new_cluster1.mutable_eds_cluster_config()->set_service_name(
      kNewEdsService1Name);
  balancers_[0]->ads_service()->SetCdsResource(new_cluster1);
  Cluster new_cluster2 = default_cluster_;
  new_cluster2.set_name(kNewCluster2Name);
  new_cluster2.mutable_eds_cluster_config()->set_service_name(
      kNewEdsService2Name);
  balancers_[0]->ads_service()->SetCdsResource(new_cluster2);
  RouteConfiguration new_route_config = default_route_config_;
  auto* route1 = new_route_config.mutable_virtual_hosts(0)->mutable_routes(0);
  route1->mutable_match()->set_prefix("/GrPc.TeStInG.EcHoTeSt1SErViCe");
  route1->mutable_route()->set_cluster(kNewCluster1Name);
  auto* route2 = new_route_config.mutable_virtual_hosts(0)->add_routes();
  route2->mutable_match()->set_prefix("/GrPc.TeStInG.EcHoTeSt1SErViCe");
  route2->mutable_match()->mutable_case_sensitive()->set_value(false);
  route2->mutable_route()->set_cluster(kNewCluster2Name);
  auto* default_route = new_route_config.mutable_virtual_hosts(0)->add_routes();
  default_route->mutable_match()->set_prefix("");
  default_route->mutable_route()->set_cluster(kDefaultClusterName);
  SetRouteConfiguration(0, new_route_config);
  CheckRpcSendOk(kNumEchoRpcs, RpcOptions().set_wait_for_ready(true));
  CheckRpcSendOk(kNumEcho1Rpcs, RpcOptions()
                                    .set_rpc_service(SERVICE_ECHO1)
                                    .set_rpc_method(METHOD_ECHO1)
                                    .set_wait_for_ready(true));
  EXPECT_EQ(kNumEchoRpcs, backends_[0]->backend_service()->request_count());
  EXPECT_EQ(0, backends_[0]->backend_service1()->request_count());
  EXPECT_EQ(0, backends_[1]->backend_service()->request_count());
  EXPECT_EQ(0, backends_[1]->backend_service1()->request_count());
  EXPECT_EQ(0, backends_[2]->backend_service()->request_count());
  EXPECT_EQ(kNumEcho1Rpcs, backends_[2]->backend_service1()->request_count());
}
TEST_P(LdsRdsTest, XdsRoutingPathRegexMatching) {
  const char* kNewCluster1Name = "new_cluster_1";
  const char* kNewEdsService1Name = "new_eds_service_name_1";
  const char* kNewCluster2Name = "new_cluster_2";
  const char* kNewEdsService2Name = "new_eds_service_name_2";
  const size_t kNumEcho1Rpcs = 10;
  const size_t kNumEcho2Rpcs = 20;
  const size_t kNumEchoRpcs = 30;
  SetNextResolution({});
  SetNextResolutionForLbChannelAllBalancers();
  AdsServiceImpl::EdsResourceArgs args({
      {"locality0", GetBackendPorts(0, 2)},
  });
  AdsServiceImpl::EdsResourceArgs args1({
      {"locality0", GetBackendPorts(2, 3)},
  });
  AdsServiceImpl::EdsResourceArgs args2({
      {"locality0", GetBackendPorts(3, 4)},
  });
  balancers_[0]->ads_service()->SetEdsResource(BuildEdsResource(args));
  balancers_[0]->ads_service()->SetEdsResource(
      BuildEdsResource(args1, kNewEdsService1Name));
  balancers_[0]->ads_service()->SetEdsResource(
      BuildEdsResource(args2, kNewEdsService2Name));
  Cluster new_cluster1 = default_cluster_;
  new_cluster1.set_name(kNewCluster1Name);
  new_cluster1.mutable_eds_cluster_config()->set_service_name(
      kNewEdsService1Name);
  balancers_[0]->ads_service()->SetCdsResource(new_cluster1);
  Cluster new_cluster2 = default_cluster_;
  new_cluster2.set_name(kNewCluster2Name);
  new_cluster2.mutable_eds_cluster_config()->set_service_name(
      kNewEdsService2Name);
  balancers_[0]->ads_service()->SetCdsResource(new_cluster2);
  RouteConfiguration new_route_config = default_route_config_;
  auto* route1 = new_route_config.mutable_virtual_hosts(0)->mutable_routes(0);
  route1->mutable_match()->mutable_safe_regex()->set_regex(".*1.*");
  route1->mutable_route()->set_cluster(kNewCluster1Name);
  auto* route2 = new_route_config.mutable_virtual_hosts(0)->add_routes();
  route2->mutable_match()->mutable_safe_regex()->set_regex(".*2.*");
  route2->mutable_route()->set_cluster(kNewCluster2Name);
  auto* default_route = new_route_config.mutable_virtual_hosts(0)->add_routes();
  default_route->mutable_match()->set_prefix("");
  default_route->mutable_route()->set_cluster(kDefaultClusterName);
  SetRouteConfiguration(0, new_route_config);
  WaitForAllBackends(0, 2);
  CheckRpcSendOk(kNumEchoRpcs, RpcOptions().set_wait_for_ready(true));
  CheckRpcSendOk(
      kNumEcho1Rpcs,
      RpcOptions().set_rpc_service(SERVICE_ECHO1).set_wait_for_ready(true));
  CheckRpcSendOk(
      kNumEcho2Rpcs,
      RpcOptions().set_rpc_service(SERVICE_ECHO2).set_wait_for_ready(true));
  for (size_t i = 0; i < 2; ++i) {
    EXPECT_EQ(kNumEchoRpcs / 2,
              backends_[i]->backend_service()->request_count());
    EXPECT_EQ(0, backends_[i]->backend_service1()->request_count());
    EXPECT_EQ(0, backends_[i]->backend_service2()->request_count());
  }
  EXPECT_EQ(0, backends_[2]->backend_service()->request_count());
  EXPECT_EQ(kNumEcho1Rpcs, backends_[2]->backend_service1()->request_count());
  EXPECT_EQ(0, backends_[2]->backend_service2()->request_count());
  EXPECT_EQ(0, backends_[3]->backend_service()->request_count());
  EXPECT_EQ(0, backends_[3]->backend_service1()->request_count());
  EXPECT_EQ(kNumEcho2Rpcs, backends_[3]->backend_service2()->request_count());
}
TEST_P(LdsRdsTest, XdsRoutingPathRegexMatchingCaseInsensitive) {
  const char* kNewCluster1Name = "new_cluster_1";
  const char* kNewEdsService1Name = "new_eds_service_name_1";
  const char* kNewCluster2Name = "new_cluster_2";
  const char* kNewEdsService2Name = "new_eds_service_name_2";
  const size_t kNumEcho1Rpcs = 10;
  const size_t kNumEchoRpcs = 30;
  SetNextResolution({});
  SetNextResolutionForLbChannelAllBalancers();
  AdsServiceImpl::EdsResourceArgs args({
      {"locality0", GetBackendPorts(0, 1)},
  });
  AdsServiceImpl::EdsResourceArgs args1({
      {"locality0", GetBackendPorts(1, 2)},
  });
  AdsServiceImpl::EdsResourceArgs args2({
      {"locality0", GetBackendPorts(2, 3)},
  });
  balancers_[0]->ads_service()->SetEdsResource(BuildEdsResource(args));
  balancers_[0]->ads_service()->SetEdsResource(
      BuildEdsResource(args1, kNewEdsService1Name));
  balancers_[0]->ads_service()->SetEdsResource(
      BuildEdsResource(args2, kNewEdsService2Name));
  Cluster new_cluster1 = default_cluster_;
  new_cluster1.set_name(kNewCluster1Name);
  new_cluster1.mutable_eds_cluster_config()->set_service_name(
      kNewEdsService1Name);
  balancers_[0]->ads_service()->SetCdsResource(new_cluster1);
  Cluster new_cluster2 = default_cluster_;
  new_cluster2.set_name(kNewCluster2Name);
  new_cluster2.mutable_eds_cluster_config()->set_service_name(
      kNewEdsService2Name);
  balancers_[0]->ads_service()->SetCdsResource(new_cluster2);
  RouteConfiguration new_route_config = default_route_config_;
  auto* route1 = new_route_config.mutable_virtual_hosts(0)->mutable_routes(0);
  route1->mutable_match()->mutable_safe_regex()->set_regex(
      ".*EcHoTeSt1SErViCe.*");
  route1->mutable_route()->set_cluster(kNewCluster1Name);
  auto* route2 = new_route_config.mutable_virtual_hosts(0)->add_routes();
  route2->mutable_match()->mutable_safe_regex()->set_regex(
      ".*EcHoTeSt1SErViCe.*");
  route2->mutable_match()->mutable_case_sensitive()->set_value(false);
  route2->mutable_route()->set_cluster(kNewCluster2Name);
  auto* default_route = new_route_config.mutable_virtual_hosts(0)->add_routes();
  default_route->mutable_match()->set_prefix("");
  default_route->mutable_route()->set_cluster(kDefaultClusterName);
  SetRouteConfiguration(0, new_route_config);
  CheckRpcSendOk(kNumEchoRpcs, RpcOptions().set_wait_for_ready(true));
  CheckRpcSendOk(kNumEcho1Rpcs, RpcOptions()
                                    .set_rpc_service(SERVICE_ECHO1)
                                    .set_rpc_method(METHOD_ECHO1)
                                    .set_wait_for_ready(true));
  EXPECT_EQ(kNumEchoRpcs, backends_[0]->backend_service()->request_count());
  EXPECT_EQ(0, backends_[0]->backend_service1()->request_count());
  EXPECT_EQ(0, backends_[1]->backend_service()->request_count());
  EXPECT_EQ(0, backends_[1]->backend_service1()->request_count());
  EXPECT_EQ(0, backends_[2]->backend_service()->request_count());
  EXPECT_EQ(kNumEcho1Rpcs, backends_[2]->backend_service1()->request_count());
}
TEST_P(LdsRdsTest, XdsRoutingWeightedCluster) {
  const char* kNewCluster1Name = "new_cluster_1";
  const char* kNewEdsService1Name = "new_eds_service_name_1";
  const char* kNewCluster2Name = "new_cluster_2";
  const char* kNewEdsService2Name = "new_eds_service_name_2";
  const size_t kNumEcho1Rpcs = 1000;
  const size_t kNumEchoRpcs = 10;
  const size_t kWeight75 = 75;
  const size_t kWeight25 = 25;
  SetNextResolution({});
  SetNextResolutionForLbChannelAllBalancers();
  AdsServiceImpl::EdsResourceArgs args({
      {"locality0", GetBackendPorts(0, 1)},
  });
  AdsServiceImpl::EdsResourceArgs args1({
      {"locality0", GetBackendPorts(1, 2)},
  });
  AdsServiceImpl::EdsResourceArgs args2({
      {"locality0", GetBackendPorts(2, 3)},
  });
  balancers_[0]->ads_service()->SetEdsResource(BuildEdsResource(args));
  balancers_[0]->ads_service()->SetEdsResource(
      BuildEdsResource(args1, kNewEdsService1Name));
  balancers_[0]->ads_service()->SetEdsResource(
      BuildEdsResource(args2, kNewEdsService2Name));
  Cluster new_cluster1 = default_cluster_;
  new_cluster1.set_name(kNewCluster1Name);
  new_cluster1.mutable_eds_cluster_config()->set_service_name(
      kNewEdsService1Name);
  balancers_[0]->ads_service()->SetCdsResource(new_cluster1);
  Cluster new_cluster2 = default_cluster_;
  new_cluster2.set_name(kNewCluster2Name);
  new_cluster2.mutable_eds_cluster_config()->set_service_name(
      kNewEdsService2Name);
  balancers_[0]->ads_service()->SetCdsResource(new_cluster2);
  RouteConfiguration new_route_config = default_route_config_;
  auto* route1 = new_route_config.mutable_virtual_hosts(0)->mutable_routes(0);
  route1->mutable_match()->set_prefix("/grpc.testing.EchoTest1Service/");
  auto* weighted_cluster1 =
      route1->mutable_route()->mutable_weighted_clusters()->add_clusters();
  weighted_cluster1->set_name(kNewCluster1Name);
  weighted_cluster1->mutable_weight()->set_value(kWeight75);
  auto* weighted_cluster2 =
      route1->mutable_route()->mutable_weighted_clusters()->add_clusters();
  weighted_cluster2->set_name(kNewCluster2Name);
  weighted_cluster2->mutable_weight()->set_value(kWeight25);
  route1->mutable_route()
      ->mutable_weighted_clusters()
      ->mutable_total_weight()
      ->set_value(kWeight75 + kWeight25);
  auto* default_route = new_route_config.mutable_virtual_hosts(0)->add_routes();
  default_route->mutable_match()->set_prefix("");
  default_route->mutable_route()->set_cluster(kDefaultClusterName);
  SetRouteConfiguration(0, new_route_config);
  WaitForAllBackends(0, 1);
  WaitForAllBackends(1, 3, true, RpcOptions().set_rpc_service(SERVICE_ECHO1));
  CheckRpcSendOk(kNumEchoRpcs);
  CheckRpcSendOk(kNumEcho1Rpcs, RpcOptions().set_rpc_service(SERVICE_ECHO1));
  EXPECT_EQ(kNumEchoRpcs, backends_[0]->backend_service()->request_count());
  EXPECT_EQ(0, backends_[0]->backend_service1()->request_count());
  EXPECT_EQ(0, backends_[1]->backend_service()->request_count());
  const int weight_75_request_count =
      backends_[1]->backend_service1()->request_count();
  EXPECT_EQ(0, backends_[2]->backend_service()->request_count());
  const int weight_25_request_count =
      backends_[2]->backend_service1()->request_count();
  const double kErrorTolerance = 0.2;
  EXPECT_THAT(weight_75_request_count,
              ::testing::AllOf(::testing::Ge(kNumEcho1Rpcs * kWeight75 / 100 *
                                             (1 - kErrorTolerance)),
                               ::testing::Le(kNumEcho1Rpcs * kWeight75 / 100 *
                                             (1 + kErrorTolerance))));
  const double kErrorToleranceSmallLoad = 0.3;
  gpr_log(GPR_INFO, "target_75 received %d rpcs and target_25 received %d rpcs",
          weight_75_request_count, weight_25_request_count);
  EXPECT_THAT(weight_25_request_count,
              ::testing::AllOf(::testing::Ge(kNumEcho1Rpcs * kWeight25 / 100 *
                                             (1 - kErrorToleranceSmallLoad)),
                               ::testing::Le(kNumEcho1Rpcs * kWeight25 / 100 *
                                             (1 + kErrorToleranceSmallLoad))));
}
TEST_P(LdsRdsTest, RouteActionWeightedTargetDefaultRoute) {
  const char* kNewCluster1Name = "new_cluster_1";
  const char* kNewEdsService1Name = "new_eds_service_name_1";
  const char* kNewCluster2Name = "new_cluster_2";
  const char* kNewEdsService2Name = "new_eds_service_name_2";
  const size_t kNumEchoRpcs = 1000;
  const size_t kWeight75 = 75;
  const size_t kWeight25 = 25;
  SetNextResolution({});
  SetNextResolutionForLbChannelAllBalancers();
  AdsServiceImpl::EdsResourceArgs args({
      {"locality0", GetBackendPorts(0, 1)},
  });
  AdsServiceImpl::EdsResourceArgs args1({
      {"locality0", GetBackendPorts(1, 2)},
  });
  AdsServiceImpl::EdsResourceArgs args2({
      {"locality0", GetBackendPorts(2, 3)},
  });
  balancers_[0]->ads_service()->SetEdsResource(BuildEdsResource(args));
  balancers_[0]->ads_service()->SetEdsResource(
      BuildEdsResource(args1, kNewEdsService1Name));
  balancers_[0]->ads_service()->SetEdsResource(
      BuildEdsResource(args2, kNewEdsService2Name));
  Cluster new_cluster1 = default_cluster_;
  new_cluster1.set_name(kNewCluster1Name);
  new_cluster1.mutable_eds_cluster_config()->set_service_name(
      kNewEdsService1Name);
  balancers_[0]->ads_service()->SetCdsResource(new_cluster1);
  Cluster new_cluster2 = default_cluster_;
  new_cluster2.set_name(kNewCluster2Name);
  new_cluster2.mutable_eds_cluster_config()->set_service_name(
      kNewEdsService2Name);
  balancers_[0]->ads_service()->SetCdsResource(new_cluster2);
  RouteConfiguration new_route_config = default_route_config_;
  auto* route1 = new_route_config.mutable_virtual_hosts(0)->mutable_routes(0);
  route1->mutable_match()->set_prefix("");
  auto* weighted_cluster1 =
      route1->mutable_route()->mutable_weighted_clusters()->add_clusters();
  weighted_cluster1->set_name(kNewCluster1Name);
  weighted_cluster1->mutable_weight()->set_value(kWeight75);
  auto* weighted_cluster2 =
      route1->mutable_route()->mutable_weighted_clusters()->add_clusters();
  weighted_cluster2->set_name(kNewCluster2Name);
  weighted_cluster2->mutable_weight()->set_value(kWeight25);
  route1->mutable_route()
      ->mutable_weighted_clusters()
      ->mutable_total_weight()
      ->set_value(kWeight75 + kWeight25);
  SetRouteConfiguration(0, new_route_config);
  WaitForAllBackends(1, 3);
  CheckRpcSendOk(kNumEchoRpcs);
  EXPECT_EQ(0, backends_[0]->backend_service()->request_count());
  const int weight_75_request_count =
      backends_[1]->backend_service()->request_count();
  const int weight_25_request_count =
      backends_[2]->backend_service()->request_count();
  const double kErrorTolerance = 0.2;
  EXPECT_THAT(weight_75_request_count,
              ::testing::AllOf(::testing::Ge(kNumEchoRpcs * kWeight75 / 100 *
                                             (1 - kErrorTolerance)),
                               ::testing::Le(kNumEchoRpcs * kWeight75 / 100 *
                                             (1 + kErrorTolerance))));
  const double kErrorToleranceSmallLoad = 0.3;
  gpr_log(GPR_INFO, "target_75 received %d rpcs and target_25 received %d rpcs",
          weight_75_request_count, weight_25_request_count);
  EXPECT_THAT(weight_25_request_count,
              ::testing::AllOf(::testing::Ge(kNumEchoRpcs * kWeight25 / 100 *
                                             (1 - kErrorToleranceSmallLoad)),
                               ::testing::Le(kNumEchoRpcs * kWeight25 / 100 *
                                             (1 + kErrorToleranceSmallLoad))));
}
TEST_P(LdsRdsTest, XdsRoutingWeightedClusterUpdateWeights) {
  const char* kNewCluster1Name = "new_cluster_1";
  const char* kNewEdsService1Name = "new_eds_service_name_1";
  const char* kNewCluster2Name = "new_cluster_2";
  const char* kNewEdsService2Name = "new_eds_service_name_2";
  const char* kNewCluster3Name = "new_cluster_3";
  const char* kNewEdsService3Name = "new_eds_service_name_3";
  const size_t kNumEcho1Rpcs = 1000;
  const size_t kNumEchoRpcs = 10;
  const size_t kWeight75 = 75;
  const size_t kWeight25 = 25;
  const size_t kWeight50 = 50;
  SetNextResolution({});
  SetNextResolutionForLbChannelAllBalancers();
  AdsServiceImpl::EdsResourceArgs args({
      {"locality0", GetBackendPorts(0, 1)},
  });
  AdsServiceImpl::EdsResourceArgs args1({
      {"locality0", GetBackendPorts(1, 2)},
  });
  AdsServiceImpl::EdsResourceArgs args2({
      {"locality0", GetBackendPorts(2, 3)},
  });
  AdsServiceImpl::EdsResourceArgs args3({
      {"locality0", GetBackendPorts(3, 4)},
  });
  balancers_[0]->ads_service()->SetEdsResource(BuildEdsResource(args));
  balancers_[0]->ads_service()->SetEdsResource(
      BuildEdsResource(args1, kNewEdsService1Name));
  balancers_[0]->ads_service()->SetEdsResource(
      BuildEdsResource(args2, kNewEdsService2Name));
  balancers_[0]->ads_service()->SetEdsResource(
      BuildEdsResource(args3, kNewEdsService3Name));
  Cluster new_cluster1 = default_cluster_;
  new_cluster1.set_name(kNewCluster1Name);
  new_cluster1.mutable_eds_cluster_config()->set_service_name(
      kNewEdsService1Name);
  balancers_[0]->ads_service()->SetCdsResource(new_cluster1);
  Cluster new_cluster2 = default_cluster_;
  new_cluster2.set_name(kNewCluster2Name);
  new_cluster2.mutable_eds_cluster_config()->set_service_name(
      kNewEdsService2Name);
  balancers_[0]->ads_service()->SetCdsResource(new_cluster2);
  Cluster new_cluster3 = default_cluster_;
  new_cluster3.set_name(kNewCluster3Name);
  new_cluster3.mutable_eds_cluster_config()->set_service_name(
      kNewEdsService3Name);
  balancers_[0]->ads_service()->SetCdsResource(new_cluster3);
  RouteConfiguration new_route_config = default_route_config_;
  auto* route1 = new_route_config.mutable_virtual_hosts(0)->mutable_routes(0);
  route1->mutable_match()->set_prefix("/grpc.testing.EchoTest1Service/");
  auto* weighted_cluster1 =
      route1->mutable_route()->mutable_weighted_clusters()->add_clusters();
  weighted_cluster1->set_name(kNewCluster1Name);
  weighted_cluster1->mutable_weight()->set_value(kWeight75);
  auto* weighted_cluster2 =
      route1->mutable_route()->mutable_weighted_clusters()->add_clusters();
  weighted_cluster2->set_name(kNewCluster2Name);
  weighted_cluster2->mutable_weight()->set_value(kWeight25);
  route1->mutable_route()
      ->mutable_weighted_clusters()
      ->mutable_total_weight()
      ->set_value(kWeight75 + kWeight25);
  auto* default_route = new_route_config.mutable_virtual_hosts(0)->add_routes();
  default_route->mutable_match()->set_prefix("");
  default_route->mutable_route()->set_cluster(kDefaultClusterName);
  SetRouteConfiguration(0, new_route_config);
  WaitForAllBackends(0, 1);
  WaitForAllBackends(1, 3, true, RpcOptions().set_rpc_service(SERVICE_ECHO1));
  CheckRpcSendOk(kNumEchoRpcs);
  CheckRpcSendOk(kNumEcho1Rpcs, RpcOptions().set_rpc_service(SERVICE_ECHO1));
  EXPECT_EQ(kNumEchoRpcs, backends_[0]->backend_service()->request_count());
  EXPECT_EQ(0, backends_[0]->backend_service1()->request_count());
  EXPECT_EQ(0, backends_[1]->backend_service()->request_count());
  const int weight_75_request_count =
      backends_[1]->backend_service1()->request_count();
  EXPECT_EQ(0, backends_[1]->backend_service2()->request_count());
  EXPECT_EQ(0, backends_[2]->backend_service()->request_count());
  const int weight_25_request_count =
      backends_[2]->backend_service1()->request_count();
  EXPECT_EQ(0, backends_[3]->backend_service()->request_count());
  EXPECT_EQ(0, backends_[3]->backend_service1()->request_count());
  const double kErrorTolerance = 0.2;
  EXPECT_THAT(weight_75_request_count,
              ::testing::AllOf(::testing::Ge(kNumEcho1Rpcs * kWeight75 / 100 *
                                             (1 - kErrorTolerance)),
                               ::testing::Le(kNumEcho1Rpcs * kWeight75 / 100 *
                                             (1 + kErrorTolerance))));
  const double kErrorToleranceSmallLoad = 0.3;
  gpr_log(GPR_INFO, "target_75 received %d rpcs and target_25 received %d rpcs",
          weight_75_request_count, weight_25_request_count);
  EXPECT_THAT(weight_25_request_count,
              ::testing::AllOf(::testing::Ge(kNumEcho1Rpcs * kWeight25 / 100 *
                                             (1 - kErrorToleranceSmallLoad)),
                               ::testing::Le(kNumEcho1Rpcs * kWeight25 / 100 *
                                             (1 + kErrorToleranceSmallLoad))));
  weighted_cluster1->mutable_weight()->set_value(kWeight50);
  weighted_cluster2->mutable_weight()->set_value(kWeight50);
  default_route->mutable_route()->set_cluster(kNewCluster3Name);
  SetRouteConfiguration(0, new_route_config);
  ResetBackendCounters();
  WaitForAllBackends(3, 4);
  CheckRpcSendOk(kNumEchoRpcs);
  CheckRpcSendOk(kNumEcho1Rpcs, RpcOptions().set_rpc_service(SERVICE_ECHO1));
  EXPECT_EQ(0, backends_[0]->backend_service()->request_count());
  EXPECT_EQ(0, backends_[0]->backend_service1()->request_count());
  EXPECT_EQ(0, backends_[1]->backend_service()->request_count());
  const int weight_50_request_count_1 =
      backends_[1]->backend_service1()->request_count();
  EXPECT_EQ(0, backends_[2]->backend_service()->request_count());
  const int weight_50_request_count_2 =
      backends_[2]->backend_service1()->request_count();
  EXPECT_EQ(kNumEchoRpcs, backends_[3]->backend_service()->request_count());
  EXPECT_EQ(0, backends_[3]->backend_service1()->request_count());
  EXPECT_THAT(weight_50_request_count_1,
              ::testing::AllOf(::testing::Ge(kNumEcho1Rpcs * kWeight50 / 100 *
                                             (1 - kErrorTolerance)),
                               ::testing::Le(kNumEcho1Rpcs * kWeight50 / 100 *
                                             (1 + kErrorTolerance))));
  EXPECT_THAT(weight_50_request_count_2,
              ::testing::AllOf(::testing::Ge(kNumEcho1Rpcs * kWeight50 / 100 *
                                             (1 - kErrorTolerance)),
                               ::testing::Le(kNumEcho1Rpcs * kWeight50 / 100 *
                                             (1 + kErrorTolerance))));
}
TEST_P(LdsRdsTest, XdsRoutingWeightedClusterUpdateClusters) {
  const char* kNewCluster1Name = "new_cluster_1";
  const char* kNewEdsService1Name = "new_eds_service_name_1";
  const char* kNewCluster2Name = "new_cluster_2";
  const char* kNewEdsService2Name = "new_eds_service_name_2";
  const char* kNewCluster3Name = "new_cluster_3";
  const char* kNewEdsService3Name = "new_eds_service_name_3";
  const size_t kNumEcho1Rpcs = 1000;
  const size_t kNumEchoRpcs = 10;
  const size_t kWeight75 = 75;
  const size_t kWeight25 = 25;
  const size_t kWeight50 = 50;
  SetNextResolution({});
  SetNextResolutionForLbChannelAllBalancers();
  AdsServiceImpl::EdsResourceArgs args({
      {"locality0", GetBackendPorts(0, 1)},
  });
  AdsServiceImpl::EdsResourceArgs args1({
      {"locality0", GetBackendPorts(1, 2)},
  });
  AdsServiceImpl::EdsResourceArgs args2({
      {"locality0", GetBackendPorts(2, 3)},
  });
  AdsServiceImpl::EdsResourceArgs args3({
      {"locality0", GetBackendPorts(3, 4)},
  });
  balancers_[0]->ads_service()->SetEdsResource(BuildEdsResource(args));
  balancers_[0]->ads_service()->SetEdsResource(
      BuildEdsResource(args1, kNewEdsService1Name));
  balancers_[0]->ads_service()->SetEdsResource(
      BuildEdsResource(args2, kNewEdsService2Name));
  balancers_[0]->ads_service()->SetEdsResource(
      BuildEdsResource(args3, kNewEdsService3Name));
  Cluster new_cluster1 = default_cluster_;
  new_cluster1.set_name(kNewCluster1Name);
  new_cluster1.mutable_eds_cluster_config()->set_service_name(
      kNewEdsService1Name);
  balancers_[0]->ads_service()->SetCdsResource(new_cluster1);
  Cluster new_cluster2 = default_cluster_;
  new_cluster2.set_name(kNewCluster2Name);
  new_cluster2.mutable_eds_cluster_config()->set_service_name(
      kNewEdsService2Name);
  balancers_[0]->ads_service()->SetCdsResource(new_cluster2);
  Cluster new_cluster3 = default_cluster_;
  new_cluster3.set_name(kNewCluster3Name);
  new_cluster3.mutable_eds_cluster_config()->set_service_name(
      kNewEdsService3Name);
  balancers_[0]->ads_service()->SetCdsResource(new_cluster3);
  RouteConfiguration new_route_config = default_route_config_;
  auto* route1 = new_route_config.mutable_virtual_hosts(0)->mutable_routes(0);
  route1->mutable_match()->set_prefix("/grpc.testing.EchoTest1Service/");
  auto* weighted_cluster1 =
      route1->mutable_route()->mutable_weighted_clusters()->add_clusters();
  weighted_cluster1->set_name(kNewCluster1Name);
  weighted_cluster1->mutable_weight()->set_value(kWeight75);
  auto* weighted_cluster2 =
      route1->mutable_route()->mutable_weighted_clusters()->add_clusters();
  weighted_cluster2->set_name(kDefaultClusterName);
  weighted_cluster2->mutable_weight()->set_value(kWeight25);
  route1->mutable_route()
      ->mutable_weighted_clusters()
      ->mutable_total_weight()
      ->set_value(kWeight75 + kWeight25);
  auto* default_route = new_route_config.mutable_virtual_hosts(0)->add_routes();
  default_route->mutable_match()->set_prefix("");
  default_route->mutable_route()->set_cluster(kDefaultClusterName);
  SetRouteConfiguration(0, new_route_config);
  WaitForAllBackends(0, 1);
  WaitForAllBackends(1, 2, true, RpcOptions().set_rpc_service(SERVICE_ECHO1));
  CheckRpcSendOk(kNumEchoRpcs);
  CheckRpcSendOk(kNumEcho1Rpcs, RpcOptions().set_rpc_service(SERVICE_ECHO1));
  EXPECT_EQ(kNumEchoRpcs, backends_[0]->backend_service()->request_count());
  int weight_25_request_count =
      backends_[0]->backend_service1()->request_count();
  EXPECT_EQ(0, backends_[1]->backend_service()->request_count());
  int weight_75_request_count =
      backends_[1]->backend_service1()->request_count();
  EXPECT_EQ(0, backends_[2]->backend_service()->request_count());
  EXPECT_EQ(0, backends_[2]->backend_service1()->request_count());
  EXPECT_EQ(0, backends_[3]->backend_service()->request_count());
  EXPECT_EQ(0, backends_[3]->backend_service1()->request_count());
  const double kErrorTolerance = 0.2;
  EXPECT_THAT(weight_75_request_count,
              ::testing::AllOf(::testing::Ge(kNumEcho1Rpcs * kWeight75 / 100 *
                                             (1 - kErrorTolerance)),
                               ::testing::Le(kNumEcho1Rpcs * kWeight75 / 100 *
                                             (1 + kErrorTolerance))));
  const double kErrorToleranceSmallLoad = 0.3;
  gpr_log(GPR_INFO, "target_75 received %d rpcs and target_25 received %d rpcs",
          weight_75_request_count, weight_25_request_count);
  EXPECT_THAT(weight_25_request_count,
              ::testing::AllOf(::testing::Ge(kNumEcho1Rpcs * kWeight25 / 100 *
                                             (1 - kErrorToleranceSmallLoad)),
                               ::testing::Le(kNumEcho1Rpcs * kWeight25 / 100 *
                                             (1 + kErrorToleranceSmallLoad))));
  weighted_cluster1->mutable_weight()->set_value(kWeight50);
  weighted_cluster2->set_name(kNewCluster2Name);
  weighted_cluster2->mutable_weight()->set_value(kWeight50);
  SetRouteConfiguration(0, new_route_config);
  ResetBackendCounters();
  WaitForAllBackends(2, 3, true, RpcOptions().set_rpc_service(SERVICE_ECHO1));
  CheckRpcSendOk(kNumEchoRpcs);
  CheckRpcSendOk(kNumEcho1Rpcs, RpcOptions().set_rpc_service(SERVICE_ECHO1));
  EXPECT_EQ(kNumEchoRpcs, backends_[0]->backend_service()->request_count());
  EXPECT_EQ(0, backends_[0]->backend_service1()->request_count());
  EXPECT_EQ(0, backends_[1]->backend_service()->request_count());
  const int weight_50_request_count_1 =
      backends_[1]->backend_service1()->request_count();
  EXPECT_EQ(0, backends_[2]->backend_service()->request_count());
  const int weight_50_request_count_2 =
      backends_[2]->backend_service1()->request_count();
  EXPECT_EQ(0, backends_[3]->backend_service()->request_count());
  EXPECT_EQ(0, backends_[3]->backend_service1()->request_count());
  EXPECT_THAT(weight_50_request_count_1,
              ::testing::AllOf(::testing::Ge(kNumEcho1Rpcs * kWeight50 / 100 *
                                             (1 - kErrorTolerance)),
                               ::testing::Le(kNumEcho1Rpcs * kWeight50 / 100 *
                                             (1 + kErrorTolerance))));
  EXPECT_THAT(weight_50_request_count_2,
              ::testing::AllOf(::testing::Ge(kNumEcho1Rpcs * kWeight50 / 100 *
                                             (1 - kErrorTolerance)),
                               ::testing::Le(kNumEcho1Rpcs * kWeight50 / 100 *
                                             (1 + kErrorTolerance))));
  weighted_cluster1->mutable_weight()->set_value(kWeight75);
  weighted_cluster2->set_name(kNewCluster3Name);
  weighted_cluster2->mutable_weight()->set_value(kWeight25);
  SetRouteConfiguration(0, new_route_config);
  ResetBackendCounters();
  WaitForAllBackends(3, 4, true, RpcOptions().set_rpc_service(SERVICE_ECHO1));
  CheckRpcSendOk(kNumEchoRpcs);
  CheckRpcSendOk(kNumEcho1Rpcs, RpcOptions().set_rpc_service(SERVICE_ECHO1));
  EXPECT_EQ(kNumEchoRpcs, backends_[0]->backend_service()->request_count());
  EXPECT_EQ(0, backends_[0]->backend_service1()->request_count());
  EXPECT_EQ(0, backends_[1]->backend_service()->request_count());
  weight_75_request_count = backends_[1]->backend_service1()->request_count();
  EXPECT_EQ(0, backends_[2]->backend_service()->request_count());
  EXPECT_EQ(0, backends_[2]->backend_service1()->request_count());
  EXPECT_EQ(0, backends_[3]->backend_service()->request_count());
  weight_25_request_count = backends_[3]->backend_service1()->request_count();
  EXPECT_THAT(weight_75_request_count,
              ::testing::AllOf(::testing::Ge(kNumEcho1Rpcs * kWeight75 / 100 *
                                             (1 - kErrorTolerance)),
                               ::testing::Le(kNumEcho1Rpcs * kWeight75 / 100 *
                                             (1 + kErrorTolerance))));
  gpr_log(GPR_INFO, "target_75 received %d rpcs and target_25 received %d rpcs",
          weight_75_request_count, weight_25_request_count);
  EXPECT_THAT(weight_25_request_count,
              ::testing::AllOf(::testing::Ge(kNumEcho1Rpcs * kWeight25 / 100 *
                                             (1 - kErrorToleranceSmallLoad)),
                               ::testing::Le(kNumEcho1Rpcs * kWeight25 / 100 *
                                             (1 + kErrorToleranceSmallLoad))));
}
TEST_P(LdsRdsTest, XdsRoutingClusterUpdateClusters) {
  const char* kNewClusterName = "new_cluster";
  const char* kNewEdsServiceName = "new_eds_service_name";
  const size_t kNumEchoRpcs = 5;
  SetNextResolution({});
  SetNextResolutionForLbChannelAllBalancers();
  AdsServiceImpl::EdsResourceArgs args({
      {"locality0", GetBackendPorts(0, 1)},
  });
  AdsServiceImpl::EdsResourceArgs args1({
      {"locality0", GetBackendPorts(1, 2)},
  });
  balancers_[0]->ads_service()->SetEdsResource(BuildEdsResource(args));
  balancers_[0]->ads_service()->SetEdsResource(
      BuildEdsResource(args1, kNewEdsServiceName));
  Cluster new_cluster = default_cluster_;
  new_cluster.set_name(kNewClusterName);
  new_cluster.mutable_eds_cluster_config()->set_service_name(
      kNewEdsServiceName);
  balancers_[0]->ads_service()->SetCdsResource(new_cluster);
  RouteConfiguration new_route_config = default_route_config_;
  SetRouteConfiguration(0, new_route_config);
  WaitForAllBackends(0, 1);
  CheckRpcSendOk(kNumEchoRpcs);
  EXPECT_EQ(kNumEchoRpcs, backends_[0]->backend_service()->request_count());
  auto* default_route =
      new_route_config.mutable_virtual_hosts(0)->mutable_routes(0);
  default_route->mutable_route()->set_cluster(kNewClusterName);
  SetRouteConfiguration(0, new_route_config);
  WaitForAllBackends(1, 2);
  CheckRpcSendOk(kNumEchoRpcs);
  EXPECT_EQ(kNumEchoRpcs, backends_[1]->backend_service()->request_count());
}
TEST_P(LdsRdsTest, XdsRoutingClusterUpdateClustersWithPickingDelays) {
  const char* kNewClusterName = "new_cluster";
  const char* kNewEdsServiceName = "new_eds_service_name";
  SetNextResolution({});
  SetNextResolutionForLbChannelAllBalancers();
  AdsServiceImpl::EdsResourceArgs args({
      {"locality0", GetBackendPorts(0, 1)},
  });
  AdsServiceImpl::EdsResourceArgs args1({
      {"locality0", GetBackendPorts(1, 2)},
  });
  balancers_[0]->ads_service()->SetEdsResource(BuildEdsResource(args));
  balancers_[0]->ads_service()->SetEdsResource(
      BuildEdsResource(args1, kNewEdsServiceName));
  Cluster new_cluster = default_cluster_;
  new_cluster.set_name(kNewClusterName);
  new_cluster.mutable_eds_cluster_config()->set_service_name(
      kNewEdsServiceName);
  balancers_[0]->ads_service()->SetCdsResource(new_cluster);
  ShutdownBackend(0);
  RouteConfiguration new_route_config = default_route_config_;
  SetRouteConfiguration(0, new_route_config);
  std::thread sending_rpc([this]() {
    CheckRpcSendOk(1, RpcOptions().set_wait_for_ready(true).set_timeout_ms(0));
  });
  const Status status = SendRpc(RpcOptions().set_timeout_ms(0));
  EXPECT_FALSE(status.ok());
  auto* default_route =
      new_route_config.mutable_virtual_hosts(0)->mutable_routes(0);
  default_route->mutable_route()->set_cluster(kNewClusterName);
  SetRouteConfiguration(0, new_route_config);
  WaitForAllBackends(1, 2, false, RpcOptions(), true);
  StartBackend(0);
  sending_rpc.join();
  EXPECT_EQ(1, backends_[0]->backend_service()->request_count());
  EXPECT_EQ(1, backends_[1]->backend_service()->request_count());
}
TEST_P(LdsRdsTest, XdsRoutingApplyXdsTimeout) {
  if (BuiltUnderAsan() || BuiltUnderTsan()) return;
  gpr_setenv("GRPC_XDS_EXPERIMENTAL_ENABLE_TIMEOUT", "true");
  const int64_t kTimeoutNano = 500000000;
  const int64_t kTimeoutGrpcTimeoutHeaderMaxSecond = 1;
  const int64_t kTimeoutMaxStreamDurationSecond = 2;
  const int64_t kTimeoutHttpMaxStreamDurationSecond = 3;
  const int64_t kTimeoutApplicationSecond = 4;
  const char* kNewCluster1Name = "new_cluster_1";
  const char* kNewEdsService1Name = "new_eds_service_name_1";
  const char* kNewCluster2Name = "new_cluster_2";
  const char* kNewEdsService2Name = "new_eds_service_name_2";
  const char* kNewCluster3Name = "new_cluster_3";
  const char* kNewEdsService3Name = "new_eds_service_name_3";
  SetNextResolution({});
  SetNextResolutionForLbChannelAllBalancers();
  AdsServiceImpl::EdsResourceArgs args({
      {"locality0", {g_port_saver->GetPort()}},
  });
  AdsServiceImpl::EdsResourceArgs args1({
      {"locality0", {g_port_saver->GetPort()}},
  });
  AdsServiceImpl::EdsResourceArgs args2({
      {"locality0", {g_port_saver->GetPort()}},
  });
  AdsServiceImpl::EdsResourceArgs args3({
      {"locality0", {g_port_saver->GetPort()}},
  });
  balancers_[0]->ads_service()->SetEdsResource(BuildEdsResource(args));
  balancers_[0]->ads_service()->SetEdsResource(
      BuildEdsResource(args1, kNewEdsService1Name));
  balancers_[0]->ads_service()->SetEdsResource(
      BuildEdsResource(args2, kNewEdsService2Name));
  balancers_[0]->ads_service()->SetEdsResource(
      BuildEdsResource(args3, kNewEdsService3Name));
  Cluster new_cluster1 = default_cluster_;
  new_cluster1.set_name(kNewCluster1Name);
  new_cluster1.mutable_eds_cluster_config()->set_service_name(
      kNewEdsService1Name);
  balancers_[0]->ads_service()->SetCdsResource(new_cluster1);
  Cluster new_cluster2 = default_cluster_;
  new_cluster2.set_name(kNewCluster2Name);
  new_cluster2.mutable_eds_cluster_config()->set_service_name(
      kNewEdsService2Name);
  balancers_[0]->ads_service()->SetCdsResource(new_cluster2);
  Cluster new_cluster3 = default_cluster_;
  new_cluster3.set_name(kNewCluster3Name);
  new_cluster3.mutable_eds_cluster_config()->set_service_name(
      kNewEdsService3Name);
  balancers_[0]->ads_service()->SetCdsResource(new_cluster3);
  auto listener = default_listener_;
  HttpConnectionManager http_connection_manager;
  auto* duration =
      http_connection_manager.mutable_common_http_protocol_options()
          ->mutable_max_stream_duration();
  duration->set_seconds(kTimeoutHttpMaxStreamDurationSecond);
  duration->set_nanos(kTimeoutNano);
  listener.mutable_api_listener()->mutable_api_listener()->PackFrom(
      http_connection_manager);
  RouteConfiguration new_route_config = default_route_config_;
  auto* route1 = new_route_config.mutable_virtual_hosts(0)->mutable_routes(0);
  route1->mutable_match()->set_path("/grpc.testing.EchoTest1Service/Echo1");
  route1->mutable_route()->set_cluster(kNewCluster1Name);
  auto* max_stream_duration =
      route1->mutable_route()->mutable_max_stream_duration();
  duration = max_stream_duration->mutable_max_stream_duration();
  duration->set_seconds(kTimeoutMaxStreamDurationSecond);
  duration->set_nanos(kTimeoutNano);
  duration = max_stream_duration->mutable_grpc_timeout_header_max();
  duration->set_seconds(kTimeoutGrpcTimeoutHeaderMaxSecond);
  duration->set_nanos(kTimeoutNano);
  auto* route2 = new_route_config.mutable_virtual_hosts(0)->add_routes();
  route2->mutable_match()->set_path("/grpc.testing.EchoTest2Service/Echo2");
  route2->mutable_route()->set_cluster(kNewCluster2Name);
  max_stream_duration = route2->mutable_route()->mutable_max_stream_duration();
  duration = max_stream_duration->mutable_max_stream_duration();
  duration->set_seconds(kTimeoutMaxStreamDurationSecond);
  duration->set_nanos(kTimeoutNano);
  auto* route3 = new_route_config.mutable_virtual_hosts(0)->add_routes();
  route3->mutable_match()->set_path("/grpc.testing.EchoTestService/Echo");
  route3->mutable_route()->set_cluster(kNewCluster3Name);
  SetListenerAndRouteConfiguration(0, std::move(listener), new_route_config);
  auto t0 = system_clock::now();
  CheckRpcSendFailure(1,
                      RpcOptions()
                          .set_rpc_service(SERVICE_ECHO1)
                          .set_rpc_method(METHOD_ECHO1)
                          .set_wait_for_ready(true)
                          .set_timeout_ms(kTimeoutApplicationSecond * 1000),
                      StatusCode::DEADLINE_EXCEEDED);
  auto ellapsed_nano_seconds =
      std::chrono::duration_cast<std::chrono::nanoseconds>(system_clock::now() -
                                                           t0);
  EXPECT_GT(ellapsed_nano_seconds.count(),
            kTimeoutGrpcTimeoutHeaderMaxSecond * 1000000000 + kTimeoutNano);
  EXPECT_LT(ellapsed_nano_seconds.count(),
            kTimeoutMaxStreamDurationSecond * 1000000000);
  t0 = system_clock::now();
  CheckRpcSendFailure(1,
                      RpcOptions()
                          .set_rpc_service(SERVICE_ECHO2)
                          .set_rpc_method(METHOD_ECHO2)
                          .set_wait_for_ready(true)
                          .set_timeout_ms(kTimeoutApplicationSecond * 1000),
                      StatusCode::DEADLINE_EXCEEDED);
  ellapsed_nano_seconds = std::chrono::duration_cast<std::chrono::nanoseconds>(
      system_clock::now() - t0);
  EXPECT_GT(ellapsed_nano_seconds.count(),
            kTimeoutMaxStreamDurationSecond * 1000000000 + kTimeoutNano);
  EXPECT_LT(ellapsed_nano_seconds.count(),
            kTimeoutHttpMaxStreamDurationSecond * 1000000000);
  t0 = system_clock::now();
  CheckRpcSendFailure(1,
                      RpcOptions().set_wait_for_ready(true).set_timeout_ms(
                          kTimeoutApplicationSecond * 1000),
                      StatusCode::DEADLINE_EXCEEDED);
  ellapsed_nano_seconds = std::chrono::duration_cast<std::chrono::nanoseconds>(
      system_clock::now() - t0);
  EXPECT_GT(ellapsed_nano_seconds.count(),
            kTimeoutHttpMaxStreamDurationSecond * 1000000000 + kTimeoutNano);
  EXPECT_LT(ellapsed_nano_seconds.count(),
            kTimeoutApplicationSecond * 1000000000);
  gpr_unsetenv("GRPC_XDS_EXPERIMENTAL_ENABLE_TIMEOUT");
}
TEST_P(LdsRdsTest, XdsRoutingXdsTimeoutDisabled) {
  const int64_t kTimeoutMillis = 500;
  const int64_t kTimeoutNano = kTimeoutMillis * 1000000;
  const int64_t kTimeoutGrpcTimeoutHeaderMaxSecond = 1;
  const int64_t kTimeoutApplicationSecond = 4;
  SetNextResolution({});
  SetNextResolutionForLbChannelAllBalancers();
  AdsServiceImpl::EdsResourceArgs args({
      {"locality0", {g_port_saver->GetPort()}},
  });
  balancers_[0]->ads_service()->SetEdsResource(BuildEdsResource(args));
  RouteConfiguration new_route_config = default_route_config_;
  auto* route1 = new_route_config.mutable_virtual_hosts(0)->mutable_routes(0);
  auto* max_stream_duration =
      route1->mutable_route()->mutable_max_stream_duration();
  auto* duration = max_stream_duration->mutable_grpc_timeout_header_max();
  duration->set_seconds(kTimeoutGrpcTimeoutHeaderMaxSecond);
  duration->set_nanos(kTimeoutNano);
  SetRouteConfiguration(0, new_route_config);
  gpr_timespec t0 = gpr_now(GPR_CLOCK_MONOTONIC);
  gpr_timespec est_timeout_time = gpr_time_add(
      t0, gpr_time_from_millis(
              kTimeoutGrpcTimeoutHeaderMaxSecond * 1000 + kTimeoutMillis,
              GPR_TIMESPAN));
  CheckRpcSendFailure(1,
                      RpcOptions()
                          .set_rpc_service(SERVICE_ECHO1)
                          .set_rpc_method(METHOD_ECHO1)
                          .set_wait_for_ready(true)
                          .set_timeout_ms(kTimeoutApplicationSecond * 1000),
                      StatusCode::DEADLINE_EXCEEDED);
  gpr_timespec timeout_time = gpr_now(GPR_CLOCK_MONOTONIC);
  EXPECT_GT(gpr_time_cmp(timeout_time, est_timeout_time), 0);
}
TEST_P(LdsRdsTest, XdsRoutingHttpTimeoutDisabled) {
  const int64_t kTimeoutMillis = 500;
  const int64_t kTimeoutNano = kTimeoutMillis * 1000000;
  const int64_t kTimeoutHttpMaxStreamDurationSecond = 3;
  const int64_t kTimeoutApplicationSecond = 4;
  SetNextResolution({});
  SetNextResolutionForLbChannelAllBalancers();
  AdsServiceImpl::EdsResourceArgs args({
      {"locality0", {g_port_saver->GetPort()}},
  });
  auto listener = default_listener_;
  HttpConnectionManager http_connection_manager;
  auto* duration =
      http_connection_manager.mutable_common_http_protocol_options()
          ->mutable_max_stream_duration();
  duration->set_seconds(kTimeoutHttpMaxStreamDurationSecond);
  duration->set_nanos(kTimeoutNano);
  listener.mutable_api_listener()->mutable_api_listener()->PackFrom(
      http_connection_manager);
  SetListenerAndRouteConfiguration(0, std::move(listener),
                                   default_route_config_);
  auto t0 = gpr_now(GPR_CLOCK_MONOTONIC);
  auto est_timeout_time = gpr_time_add(
      t0, gpr_time_from_millis(
              kTimeoutHttpMaxStreamDurationSecond * 1000 + kTimeoutMillis,
              GPR_TIMESPAN));
  CheckRpcSendFailure(1,
                      RpcOptions().set_wait_for_ready(true).set_timeout_ms(
                          kTimeoutApplicationSecond * 1000),
                      StatusCode::DEADLINE_EXCEEDED);
  auto timeout_time = gpr_now(GPR_CLOCK_MONOTONIC);
  EXPECT_GT(gpr_time_cmp(timeout_time, est_timeout_time), 0);
}
TEST_P(LdsRdsTest, XdsRoutingApplyApplicationTimeoutWhenXdsTimeoutExplicit0) {
  gpr_setenv("GRPC_XDS_EXPERIMENTAL_ENABLE_TIMEOUT", "true");
  const int64_t kTimeoutNano = 500000000;
  const int64_t kTimeoutMaxStreamDurationSecond = 2;
  const int64_t kTimeoutHttpMaxStreamDurationSecond = 3;
  const int64_t kTimeoutApplicationSecond = 4;
  const char* kNewCluster1Name = "new_cluster_1";
  const char* kNewEdsService1Name = "new_eds_service_name_1";
  const char* kNewCluster2Name = "new_cluster_2";
  const char* kNewEdsService2Name = "new_eds_service_name_2";
  SetNextResolution({});
  SetNextResolutionForLbChannelAllBalancers();
  AdsServiceImpl::EdsResourceArgs args({
      {"locality0", {g_port_saver->GetPort()}},
  });
  AdsServiceImpl::EdsResourceArgs args1({
      {"locality0", {g_port_saver->GetPort()}},
  });
  AdsServiceImpl::EdsResourceArgs args2({
      {"locality0", {g_port_saver->GetPort()}},
  });
  balancers_[0]->ads_service()->SetEdsResource(BuildEdsResource(args));
  balancers_[0]->ads_service()->SetEdsResource(
      BuildEdsResource(args1, kNewEdsService1Name));
  balancers_[0]->ads_service()->SetEdsResource(
      BuildEdsResource(args2, kNewEdsService2Name));
  Cluster new_cluster1 = default_cluster_;
  new_cluster1.set_name(kNewCluster1Name);
  new_cluster1.mutable_eds_cluster_config()->set_service_name(
      kNewEdsService1Name);
  balancers_[0]->ads_service()->SetCdsResource(new_cluster1);
  Cluster new_cluster2 = default_cluster_;
  new_cluster2.set_name(kNewCluster2Name);
  new_cluster2.mutable_eds_cluster_config()->set_service_name(
      kNewEdsService2Name);
  balancers_[0]->ads_service()->SetCdsResource(new_cluster2);
  auto listener = default_listener_;
  HttpConnectionManager http_connection_manager;
  auto* duration =
      http_connection_manager.mutable_common_http_protocol_options()
          ->mutable_max_stream_duration();
  duration->set_seconds(kTimeoutHttpMaxStreamDurationSecond);
  duration->set_nanos(kTimeoutNano);
  listener.mutable_api_listener()->mutable_api_listener()->PackFrom(
      http_connection_manager);
  RouteConfiguration new_route_config = default_route_config_;
  auto* route1 = new_route_config.mutable_virtual_hosts(0)->mutable_routes(0);
  route1->mutable_match()->set_path("/grpc.testing.EchoTest1Service/Echo1");
  route1->mutable_route()->set_cluster(kNewCluster1Name);
  auto* max_stream_duration =
      route1->mutable_route()->mutable_max_stream_duration();
  duration = max_stream_duration->mutable_max_stream_duration();
  duration->set_seconds(kTimeoutMaxStreamDurationSecond);
  duration->set_nanos(kTimeoutNano);
  duration = max_stream_duration->mutable_grpc_timeout_header_max();
  duration->set_seconds(0);
  duration->set_nanos(0);
  auto* route2 = new_route_config.mutable_virtual_hosts(0)->add_routes();
  route2->mutable_match()->set_path("/grpc.testing.EchoTest2Service/Echo2");
  route2->mutable_route()->set_cluster(kNewCluster2Name);
  max_stream_duration = route2->mutable_route()->mutable_max_stream_duration();
  duration = max_stream_duration->mutable_max_stream_duration();
  duration->set_seconds(0);
  duration->set_nanos(0);
  SetListenerAndRouteConfiguration(0, std::move(listener), new_route_config);
  auto t0 = system_clock::now();
  CheckRpcSendFailure(1,
                      RpcOptions()
                          .set_rpc_service(SERVICE_ECHO1)
                          .set_rpc_method(METHOD_ECHO1)
                          .set_wait_for_ready(true)
                          .set_timeout_ms(kTimeoutApplicationSecond * 1000),
                      StatusCode::DEADLINE_EXCEEDED);
  auto ellapsed_nano_seconds =
      std::chrono::duration_cast<std::chrono::nanoseconds>(system_clock::now() -
                                                           t0);
  EXPECT_GT(ellapsed_nano_seconds.count(),
            kTimeoutApplicationSecond * 1000000000);
  t0 = system_clock::now();
  CheckRpcSendFailure(1,
                      RpcOptions()
                          .set_rpc_service(SERVICE_ECHO2)
                          .set_rpc_method(METHOD_ECHO2)
                          .set_wait_for_ready(true)
                          .set_timeout_ms(kTimeoutApplicationSecond * 1000),
                      StatusCode::DEADLINE_EXCEEDED);
  ellapsed_nano_seconds = std::chrono::duration_cast<std::chrono::nanoseconds>(
      system_clock::now() - t0);
  EXPECT_GT(ellapsed_nano_seconds.count(),
            kTimeoutApplicationSecond * 1000000000);
  gpr_unsetenv("GRPC_XDS_EXPERIMENTAL_ENABLE_TIMEOUT");
}
TEST_P(LdsRdsTest, XdsRoutingApplyApplicationTimeoutWhenHttpTimeoutExplicit0) {
  gpr_setenv("GRPC_XDS_EXPERIMENTAL_ENABLE_TIMEOUT", "true");
  const int64_t kTimeoutApplicationSecond = 4;
  SetNextResolution({});
  SetNextResolutionForLbChannelAllBalancers();
  AdsServiceImpl::EdsResourceArgs args({
      {"locality0", {g_port_saver->GetPort()}},
  });
  balancers_[0]->ads_service()->SetEdsResource(BuildEdsResource(args));
  HttpConnectionManager http_connection_manager;
  auto* duration =
      http_connection_manager.mutable_common_http_protocol_options()
          ->mutable_max_stream_duration();
  duration->set_seconds(0);
  duration->set_nanos(0);
  auto listener = default_listener_;
  listener.mutable_api_listener()->mutable_api_listener()->PackFrom(
      http_connection_manager);
  SetListenerAndRouteConfiguration(0, std::move(listener),
                                   default_route_config_);
  auto t0 = system_clock::now();
  CheckRpcSendFailure(1,
                      RpcOptions().set_wait_for_ready(true).set_timeout_ms(
                          kTimeoutApplicationSecond * 1000),
                      StatusCode::DEADLINE_EXCEEDED);
  auto ellapsed_nano_seconds =
      std::chrono::duration_cast<std::chrono::nanoseconds>(system_clock::now() -
                                                           t0);
  EXPECT_GT(ellapsed_nano_seconds.count(),
            kTimeoutApplicationSecond * 1000000000);
  gpr_unsetenv("GRPC_XDS_EXPERIMENTAL_ENABLE_TIMEOUT");
}
TEST_P(LdsRdsTest, XdsRoutingWithOnlyApplicationTimeout) {
  gpr_setenv("GRPC_XDS_EXPERIMENTAL_ENABLE_TIMEOUT", "true");
  const int64_t kTimeoutApplicationSecond = 4;
  SetNextResolution({});
  SetNextResolutionForLbChannelAllBalancers();
  AdsServiceImpl::EdsResourceArgs args({
      {"locality0", {g_port_saver->GetPort()}},
  });
  balancers_[0]->ads_service()->SetEdsResource(BuildEdsResource(args));
  auto t0 = system_clock::now();
  CheckRpcSendFailure(1,
                      RpcOptions().set_wait_for_ready(true).set_timeout_ms(
                          kTimeoutApplicationSecond * 1000),
                      StatusCode::DEADLINE_EXCEEDED);
  auto ellapsed_nano_seconds =
      std::chrono::duration_cast<std::chrono::nanoseconds>(system_clock::now() -
                                                           t0);
  EXPECT_GT(ellapsed_nano_seconds.count(),
            kTimeoutApplicationSecond * 1000000000);
  gpr_unsetenv("GRPC_XDS_EXPERIMENTAL_ENABLE_TIMEOUT");
}
TEST_P(LdsRdsTest, XdsRoutingHeadersMatching) {
  const char* kNewClusterName = "new_cluster";
  const char* kNewEdsServiceName = "new_eds_service_name";
  const size_t kNumEcho1Rpcs = 100;
  const size_t kNumEchoRpcs = 5;
  SetNextResolution({});
  SetNextResolutionForLbChannelAllBalancers();
  AdsServiceImpl::EdsResourceArgs args({
      {"locality0", GetBackendPorts(0, 1)},
  });
  AdsServiceImpl::EdsResourceArgs args1({
      {"locality0", GetBackendPorts(1, 2)},
  });
  balancers_[0]->ads_service()->SetEdsResource(BuildEdsResource(args));
  balancers_[0]->ads_service()->SetEdsResource(
      BuildEdsResource(args1, kNewEdsServiceName));
  Cluster new_cluster = default_cluster_;
  new_cluster.set_name(kNewClusterName);
  new_cluster.mutable_eds_cluster_config()->set_service_name(
      kNewEdsServiceName);
  balancers_[0]->ads_service()->SetCdsResource(new_cluster);
  RouteConfiguration route_config = default_route_config_;
  auto* route1 = route_config.mutable_virtual_hosts(0)->mutable_routes(0);
  route1->mutable_match()->set_prefix("/grpc.testing.EchoTest1Service/");
  auto* header_matcher1 = route1->mutable_match()->add_headers();
  header_matcher1->set_name("header1");
  header_matcher1->set_exact_match("POST,PUT,GET");
  auto* header_matcher2 = route1->mutable_match()->add_headers();
  header_matcher2->set_name("header2");
  header_matcher2->mutable_safe_regex_match()->set_regex("[a-z]*");
  auto* header_matcher3 = route1->mutable_match()->add_headers();
  header_matcher3->set_name("header3");
  header_matcher3->mutable_range_match()->set_start(1);
  header_matcher3->mutable_range_match()->set_end(1000);
  auto* header_matcher4 = route1->mutable_match()->add_headers();
  header_matcher4->set_name("header4");
  header_matcher4->set_present_match(false);
  auto* header_matcher5 = route1->mutable_match()->add_headers();
  header_matcher5->set_name("header5");
  header_matcher5->set_prefix_match("/grpc");
  auto* header_matcher6 = route1->mutable_match()->add_headers();
  header_matcher6->set_name("header6");
  header_matcher6->set_suffix_match(".cc");
  header_matcher6->set_invert_match(true);
  route1->mutable_route()->set_cluster(kNewClusterName);
  auto* default_route = route_config.mutable_virtual_hosts(0)->add_routes();
  default_route->mutable_match()->set_prefix("");
  default_route->mutable_route()->set_cluster(kDefaultClusterName);
  SetRouteConfiguration(0, route_config);
  std::vector<std::pair<std::string, std::string>> metadata = {
      {"header1", "POST"}, {"header2", "blah"},
      {"header3", "1"}, {"header5", "/grpc.testing.EchoTest1Service/"},
      {"header1", "PUT"}, {"header6", "grpc.java"},
      {"header1", "GET"},
  };
  const auto header_match_rpc_options = RpcOptions()
                                            .set_rpc_service(SERVICE_ECHO1)
                                            .set_rpc_method(METHOD_ECHO1)
                                            .set_metadata(std::move(metadata));
  WaitForAllBackends(0, 1);
  WaitForAllBackends(1, 2, true, header_match_rpc_options);
  CheckRpcSendOk(kNumEchoRpcs);
  CheckRpcSendOk(kNumEcho1Rpcs, header_match_rpc_options);
  EXPECT_EQ(kNumEchoRpcs, backends_[0]->backend_service()->request_count());
  EXPECT_EQ(0, backends_[0]->backend_service1()->request_count());
  EXPECT_EQ(0, backends_[0]->backend_service2()->request_count());
  EXPECT_EQ(0, backends_[1]->backend_service()->request_count());
  EXPECT_EQ(kNumEcho1Rpcs, backends_[1]->backend_service1()->request_count());
  EXPECT_EQ(0, backends_[1]->backend_service2()->request_count());
  const auto& response_state = RouteConfigurationResponseState(0);
  EXPECT_EQ(response_state.state, AdsServiceImpl::ResponseState::ACKED);
}
TEST_P(LdsRdsTest, XdsRoutingHeadersMatchingSpecialHeaderContentType) {
  const char* kNewClusterName = "new_cluster";
  const char* kNewEdsServiceName = "new_eds_service_name";
  const size_t kNumEchoRpcs = 100;
  SetNextResolution({});
  SetNextResolutionForLbChannelAllBalancers();
  AdsServiceImpl::EdsResourceArgs args({
      {"locality0", GetBackendPorts(0, 1)},
  });
  AdsServiceImpl::EdsResourceArgs args1({
      {"locality0", GetBackendPorts(1, 2)},
  });
  balancers_[0]->ads_service()->SetEdsResource(BuildEdsResource(args));
  balancers_[0]->ads_service()->SetEdsResource(
      BuildEdsResource(args1, kNewEdsServiceName));
  Cluster new_cluster = default_cluster_;
  new_cluster.set_name(kNewClusterName);
  new_cluster.mutable_eds_cluster_config()->set_service_name(
      kNewEdsServiceName);
  balancers_[0]->ads_service()->SetCdsResource(new_cluster);
  RouteConfiguration route_config = default_route_config_;
  auto* route1 = route_config.mutable_virtual_hosts(0)->mutable_routes(0);
  route1->mutable_match()->set_prefix("");
  auto* header_matcher1 = route1->mutable_match()->add_headers();
  header_matcher1->set_name("content-type");
  header_matcher1->set_exact_match("notapplication/grpc");
  route1->mutable_route()->set_cluster(kNewClusterName);
  auto* default_route = route_config.mutable_virtual_hosts(0)->add_routes();
  default_route->mutable_match()->set_prefix("");
  auto* header_matcher2 = default_route->mutable_match()->add_headers();
  header_matcher2->set_name("content-type");
  header_matcher2->set_exact_match("application/grpc");
  default_route->mutable_route()->set_cluster(kDefaultClusterName);
  SetRouteConfiguration(0, route_config);
  WaitForAllBackends(0, 1);
  CheckRpcSendOk(kNumEchoRpcs);
  EXPECT_EQ(kNumEchoRpcs, backends_[0]->backend_service()->request_count());
  EXPECT_EQ(0, backends_[1]->backend_service()->request_count());
  const auto& response_state = RouteConfigurationResponseState(0);
  EXPECT_EQ(response_state.state, AdsServiceImpl::ResponseState::ACKED);
}
TEST_P(LdsRdsTest, XdsRoutingHeadersMatchingSpecialCasesToIgnore) {
  const char* kNewCluster1Name = "new_cluster_1";
  const char* kNewEdsService1Name = "new_eds_service_name_1";
  const char* kNewCluster2Name = "new_cluster_2";
  const char* kNewEdsService2Name = "new_eds_service_name_2";
  const size_t kNumEchoRpcs = 100;
  SetNextResolution({});
  SetNextResolutionForLbChannelAllBalancers();
  AdsServiceImpl::EdsResourceArgs args({
      {"locality0", GetBackendPorts(0, 1)},
  });
  AdsServiceImpl::EdsResourceArgs args1({
      {"locality0", GetBackendPorts(1, 2)},
  });
  AdsServiceImpl::EdsResourceArgs args2({
      {"locality0", GetBackendPorts(2, 3)},
  });
  balancers_[0]->ads_service()->SetEdsResource(BuildEdsResource(args));
  balancers_[0]->ads_service()->SetEdsResource(
      BuildEdsResource(args1, kNewEdsService1Name));
  balancers_[0]->ads_service()->SetEdsResource(
      BuildEdsResource(args2, kNewEdsService2Name));
  Cluster new_cluster1 = default_cluster_;
  new_cluster1.set_name(kNewCluster1Name);
  new_cluster1.mutable_eds_cluster_config()->set_service_name(
      kNewEdsService1Name);
  balancers_[0]->ads_service()->SetCdsResource(new_cluster1);
  Cluster new_cluster2 = default_cluster_;
  new_cluster2.set_name(kNewCluster2Name);
  new_cluster2.mutable_eds_cluster_config()->set_service_name(
      kNewEdsService2Name);
  balancers_[0]->ads_service()->SetCdsResource(new_cluster2);
  RouteConfiguration route_config = default_route_config_;
  auto* route1 = route_config.mutable_virtual_hosts(0)->mutable_routes(0);
  route1->mutable_match()->set_prefix("");
  auto* header_matcher1 = route1->mutable_match()->add_headers();
  header_matcher1->set_name("grpc-foo-bin");
  header_matcher1->set_present_match(true);
  route1->mutable_route()->set_cluster(kNewCluster1Name);
  auto route2 = route_config.mutable_virtual_hosts(0)->add_routes();
  route2->mutable_match()->set_prefix("");
  auto* header_matcher2 = route2->mutable_match()->add_headers();
  header_matcher2->set_name("grpc-previous-rpc-attempts");
  header_matcher2->set_present_match(true);
  route2->mutable_route()->set_cluster(kNewCluster2Name);
  auto* default_route = route_config.mutable_virtual_hosts(0)->add_routes();
  default_route->mutable_match()->set_prefix("");
  default_route->mutable_route()->set_cluster(kDefaultClusterName);
  SetRouteConfiguration(0, route_config);
  std::vector<std::pair<std::string, std::string>> metadata = {
      {"grpc-foo-bin", "grpc-foo-bin"},
      {"grpc-previous-rpc-attempts", "grpc-previous-rpc-attempts"},
  };
  WaitForAllBackends(0, 1);
  CheckRpcSendOk(kNumEchoRpcs, RpcOptions().set_metadata(metadata));
  EXPECT_EQ(kNumEchoRpcs, backends_[0]->backend_service()->request_count());
  EXPECT_EQ(0, backends_[1]->backend_service()->request_count());
  EXPECT_EQ(0, backends_[2]->backend_service()->request_count());
  const auto& response_state = RouteConfigurationResponseState(0);
  EXPECT_EQ(response_state.state, AdsServiceImpl::ResponseState::ACKED);
}
TEST_P(LdsRdsTest, XdsRoutingRuntimeFractionMatching) {
  const char* kNewClusterName = "new_cluster";
  const char* kNewEdsServiceName = "new_eds_service_name";
  const size_t kNumRpcs = 1000;
  SetNextResolution({});
  SetNextResolutionForLbChannelAllBalancers();
  AdsServiceImpl::EdsResourceArgs args({
      {"locality0", GetBackendPorts(0, 1)},
  });
  AdsServiceImpl::EdsResourceArgs args1({
      {"locality0", GetBackendPorts(1, 2)},
  });
  balancers_[0]->ads_service()->SetEdsResource(BuildEdsResource(args));
  balancers_[0]->ads_service()->SetEdsResource(
      BuildEdsResource(args1, kNewEdsServiceName));
  Cluster new_cluster = default_cluster_;
  new_cluster.set_name(kNewClusterName);
  new_cluster.mutable_eds_cluster_config()->set_service_name(
      kNewEdsServiceName);
  balancers_[0]->ads_service()->SetCdsResource(new_cluster);
  RouteConfiguration route_config = default_route_config_;
  auto* route1 = route_config.mutable_virtual_hosts(0)->mutable_routes(0);
  route1->mutable_match()
      ->mutable_runtime_fraction()
      ->mutable_default_value()
      ->set_numerator(25);
  route1->mutable_route()->set_cluster(kNewClusterName);
  auto* default_route = route_config.mutable_virtual_hosts(0)->add_routes();
  default_route->mutable_match()->set_prefix("");
  default_route->mutable_route()->set_cluster(kDefaultClusterName);
  SetRouteConfiguration(0, route_config);
  WaitForAllBackends(0, 2);
  CheckRpcSendOk(kNumRpcs);
  const int default_backend_count =
      backends_[0]->backend_service()->request_count();
  const int matched_backend_count =
      backends_[1]->backend_service()->request_count();
  const double kErrorTolerance = 0.2;
  EXPECT_THAT(default_backend_count,
              ::testing::AllOf(
                  ::testing::Ge(kNumRpcs * 75 / 100 * (1 - kErrorTolerance)),
                  ::testing::Le(kNumRpcs * 75 / 100 * (1 + kErrorTolerance))));
  EXPECT_THAT(matched_backend_count,
              ::testing::AllOf(
                  ::testing::Ge(kNumRpcs * 25 / 100 * (1 - kErrorTolerance)),
                  ::testing::Le(kNumRpcs * 25 / 100 * (1 + kErrorTolerance))));
  const auto& response_state = RouteConfigurationResponseState(0);
  EXPECT_EQ(response_state.state, AdsServiceImpl::ResponseState::ACKED);
}
TEST_P(LdsRdsTest, XdsRoutingHeadersMatchingUnmatchCases) {
  const char* kNewCluster1Name = "new_cluster_1";
  const char* kNewEdsService1Name = "new_eds_service_name_1";
  const char* kNewCluster2Name = "new_cluster_2";
  const char* kNewEdsService2Name = "new_eds_service_name_2";
  const char* kNewCluster3Name = "new_cluster_3";
  const char* kNewEdsService3Name = "new_eds_service_name_3";
  const size_t kNumEcho1Rpcs = 100;
  const size_t kNumEchoRpcs = 5;
  SetNextResolution({});
  SetNextResolutionForLbChannelAllBalancers();
  AdsServiceImpl::EdsResourceArgs args({
      {"locality0", GetBackendPorts(0, 1)},
  });
  AdsServiceImpl::EdsResourceArgs args1({
      {"locality0", GetBackendPorts(1, 2)},
  });
  AdsServiceImpl::EdsResourceArgs args2({
      {"locality0", GetBackendPorts(2, 3)},
  });
  AdsServiceImpl::EdsResourceArgs args3({
      {"locality0", GetBackendPorts(3, 4)},
  });
  balancers_[0]->ads_service()->SetEdsResource(BuildEdsResource(args));
  balancers_[0]->ads_service()->SetEdsResource(
      BuildEdsResource(args1, kNewEdsService1Name));
  balancers_[0]->ads_service()->SetEdsResource(
      BuildEdsResource(args2, kNewEdsService2Name));
  balancers_[0]->ads_service()->SetEdsResource(
      BuildEdsResource(args3, kNewEdsService3Name));
  Cluster new_cluster1 = default_cluster_;
  new_cluster1.set_name(kNewCluster1Name);
  new_cluster1.mutable_eds_cluster_config()->set_service_name(
      kNewEdsService1Name);
  balancers_[0]->ads_service()->SetCdsResource(new_cluster1);
  Cluster new_cluster2 = default_cluster_;
  new_cluster2.set_name(kNewCluster2Name);
  new_cluster2.mutable_eds_cluster_config()->set_service_name(
      kNewEdsService2Name);
  balancers_[0]->ads_service()->SetCdsResource(new_cluster2);
  Cluster new_cluster3 = default_cluster_;
  new_cluster3.set_name(kNewCluster3Name);
  new_cluster3.mutable_eds_cluster_config()->set_service_name(
      kNewEdsService3Name);
  balancers_[0]->ads_service()->SetCdsResource(new_cluster3);
  RouteConfiguration route_config = default_route_config_;
  auto* route1 = route_config.mutable_virtual_hosts(0)->mutable_routes(0);
  route1->mutable_match()->set_prefix("/grpc.testing.EchoTest1Service/");
  auto* header_matcher1 = route1->mutable_match()->add_headers();
  header_matcher1->set_name("header1");
  header_matcher1->set_exact_match("POST");
  route1->mutable_route()->set_cluster(kNewCluster1Name);
  auto route2 = route_config.mutable_virtual_hosts(0)->add_routes();
  route2->mutable_match()->set_prefix("/grpc.testing.EchoTest1Service/");
  auto* header_matcher2 = route2->mutable_match()->add_headers();
  header_matcher2->set_name("header2");
  header_matcher2->mutable_range_match()->set_start(1);
  header_matcher2->mutable_range_match()->set_end(1000);
  route2->mutable_route()->set_cluster(kNewCluster2Name);
  auto route3 = route_config.mutable_virtual_hosts(0)->add_routes();
  route3->mutable_match()->set_prefix("/grpc.testing.EchoTest1Service/");
  auto* header_matcher3 = route3->mutable_match()->add_headers();
  header_matcher3->set_name("header3");
  header_matcher3->mutable_safe_regex_match()->set_regex("[a-z]*");
  route3->mutable_route()->set_cluster(kNewCluster3Name);
  auto* default_route = route_config.mutable_virtual_hosts(0)->add_routes();
  default_route->mutable_match()->set_prefix("");
  default_route->mutable_route()->set_cluster(kDefaultClusterName);
  SetRouteConfiguration(0, route_config);
  std::vector<std::pair<std::string, std::string>> metadata = {
      {"header1", "POST"},
      {"header2", "1000"},
      {"header3", "123"},
      {"header1", "GET"},
  };
  WaitForAllBackends(0, 1);
  CheckRpcSendOk(kNumEchoRpcs, RpcOptions().set_metadata(metadata));
  CheckRpcSendOk(kNumEcho1Rpcs, RpcOptions()
                                    .set_rpc_service(SERVICE_ECHO1)
                                    .set_rpc_method(METHOD_ECHO1)
                                    .set_metadata(metadata));
  for (size_t i = 1; i < 4; ++i) {
    EXPECT_EQ(0, backends_[i]->backend_service()->request_count());
    EXPECT_EQ(0, backends_[i]->backend_service1()->request_count());
    EXPECT_EQ(0, backends_[i]->backend_service2()->request_count());
  }
  EXPECT_EQ(kNumEchoRpcs, backends_[0]->backend_service()->request_count());
  EXPECT_EQ(kNumEcho1Rpcs, backends_[0]->backend_service1()->request_count());
  EXPECT_EQ(0, backends_[0]->backend_service2()->request_count());
  const auto& response_state = RouteConfigurationResponseState(0);
  EXPECT_EQ(response_state.state, AdsServiceImpl::ResponseState::ACKED);
}
TEST_P(LdsRdsTest, XdsRoutingChangeRoutesWithoutChangingClusters) {
  const char* kNewClusterName = "new_cluster";
  const char* kNewEdsServiceName = "new_eds_service_name";
  SetNextResolution({});
  SetNextResolutionForLbChannelAllBalancers();
  AdsServiceImpl::EdsResourceArgs args({
      {"locality0", GetBackendPorts(0, 1)},
  });
  AdsServiceImpl::EdsResourceArgs args1({
      {"locality0", GetBackendPorts(1, 2)},
  });
  balancers_[0]->ads_service()->SetEdsResource(BuildEdsResource(args));
  balancers_[0]->ads_service()->SetEdsResource(
      BuildEdsResource(args1, kNewEdsServiceName));
  Cluster new_cluster = default_cluster_;
  new_cluster.set_name(kNewClusterName);
  new_cluster.mutable_eds_cluster_config()->set_service_name(
      kNewEdsServiceName);
  balancers_[0]->ads_service()->SetCdsResource(new_cluster);
  RouteConfiguration route_config = default_route_config_;
  auto* route1 = route_config.mutable_virtual_hosts(0)->mutable_routes(0);
  route1->mutable_match()->set_prefix("/grpc.testing.EchoTest1Service/");
  route1->mutable_route()->set_cluster(kNewClusterName);
  auto* default_route = route_config.mutable_virtual_hosts(0)->add_routes();
  default_route->mutable_match()->set_prefix("");
  default_route->mutable_route()->set_cluster(kDefaultClusterName);
  SetRouteConfiguration(0, route_config);
  WaitForAllBackends(0, 1, false);
  WaitForAllBackends(1, 2, false, RpcOptions().set_rpc_service(SERVICE_ECHO1));
  WaitForAllBackends(0, 1, false, RpcOptions().set_rpc_service(SERVICE_ECHO2));
  EXPECT_EQ(1, backends_[0]->backend_service()->request_count());
  EXPECT_EQ(0, backends_[0]->backend_service1()->request_count());
  EXPECT_EQ(1, backends_[0]->backend_service2()->request_count());
  EXPECT_EQ(0, backends_[1]->backend_service()->request_count());
  EXPECT_EQ(1, backends_[1]->backend_service1()->request_count());
  EXPECT_EQ(0, backends_[1]->backend_service2()->request_count());
  route1->mutable_match()->set_prefix("/grpc.testing.EchoTest2Service/");
  SetRouteConfiguration(0, route_config);
  WaitForAllBackends(1, 2, true, RpcOptions().set_rpc_service(SERVICE_ECHO2));
  WaitForAllBackends(0, 1, false);
  WaitForAllBackends(0, 1, false, RpcOptions().set_rpc_service(SERVICE_ECHO1));
  WaitForAllBackends(1, 2, false, RpcOptions().set_rpc_service(SERVICE_ECHO2));
  EXPECT_EQ(1, backends_[0]->backend_service()->request_count());
  EXPECT_EQ(1, backends_[0]->backend_service1()->request_count());
  EXPECT_EQ(0, backends_[0]->backend_service2()->request_count());
  EXPECT_EQ(0, backends_[1]->backend_service()->request_count());
  EXPECT_EQ(0, backends_[1]->backend_service1()->request_count());
  EXPECT_EQ(1, backends_[1]->backend_service2()->request_count());
}
using CdsTest = BasicTest;
TEST_P(CdsTest, Vanilla) {
  SetNextResolution({});
  SetNextResolutionForLbChannelAllBalancers();
  (void)SendRpc();
  EXPECT_EQ(balancers_[0]->ads_service()->cds_response_state().state,
            AdsServiceImpl::ResponseState::ACKED);
}
TEST_P(CdsTest, WrongClusterType) {
  auto cluster = default_cluster_;
  cluster.set_type(Cluster::STATIC);
  balancers_[0]->ads_service()->SetCdsResource(cluster);
  SetNextResolution({});
  SetNextResolutionForLbChannelAllBalancers();
  CheckRpcSendFailure();
  const auto& response_state =
      balancers_[0]->ads_service()->cds_response_state();
  EXPECT_EQ(response_state.state, AdsServiceImpl::ResponseState::NACKED);
  EXPECT_EQ(response_state.error_message, "DiscoveryType is not EDS.");
}
TEST_P(CdsTest, WrongEdsConfig) {
  auto cluster = default_cluster_;
  cluster.mutable_eds_cluster_config()->mutable_eds_config()->mutable_self();
  balancers_[0]->ads_service()->SetCdsResource(cluster);
  SetNextResolution({});
  SetNextResolutionForLbChannelAllBalancers();
  CheckRpcSendFailure();
  const auto& response_state =
      balancers_[0]->ads_service()->cds_response_state();
  EXPECT_EQ(response_state.state, AdsServiceImpl::ResponseState::NACKED);
  EXPECT_EQ(response_state.error_message, "EDS ConfigSource is not ADS.");
}
TEST_P(CdsTest, WrongLbPolicy) {
  auto cluster = default_cluster_;
  cluster.set_lb_policy(Cluster::LEAST_REQUEST);
  balancers_[0]->ads_service()->SetCdsResource(cluster);
  SetNextResolution({});
  SetNextResolutionForLbChannelAllBalancers();
  CheckRpcSendFailure();
  const auto& response_state =
      balancers_[0]->ads_service()->cds_response_state();
  EXPECT_EQ(response_state.state, AdsServiceImpl::ResponseState::NACKED);
  EXPECT_EQ(response_state.error_message, "LB policy is not ROUND_ROBIN.");
}
TEST_P(CdsTest, WrongLrsServer) {
  auto cluster = default_cluster_;
  cluster.mutable_lrs_server()->mutable_ads();
  balancers_[0]->ads_service()->SetCdsResource(cluster);
  SetNextResolution({});
  SetNextResolutionForLbChannelAllBalancers();
  CheckRpcSendFailure();
  const auto& response_state =
      balancers_[0]->ads_service()->cds_response_state();
  EXPECT_EQ(response_state.state, AdsServiceImpl::ResponseState::NACKED);
  EXPECT_EQ(response_state.error_message, "LRS ConfigSource is not self.");
}
using EdsTest = BasicTest;
TEST_P(EdsTest, NacksSparsePriorityList) {
  SetNextResolution({});
  SetNextResolutionForLbChannelAllBalancers();
  AdsServiceImpl::EdsResourceArgs args({
      {"locality0", GetBackendPorts(), kDefaultLocalityWeight, 1},
  });
  balancers_[0]->ads_service()->SetEdsResource(BuildEdsResource(args));
  CheckRpcSendFailure();
  const auto& response_state =
      balancers_[0]->ads_service()->eds_response_state();
  EXPECT_EQ(response_state.state, AdsServiceImpl::ResponseState::NACKED);
  EXPECT_EQ(response_state.error_message,
            "EDS update includes sparse priority list");
}
TEST_P(EdsTest, EdsServiceNameDefaultsToClusterName) {
  AdsServiceImpl::EdsResourceArgs args({
      {"locality0", GetBackendPorts()},
  });
  balancers_[0]->ads_service()->SetEdsResource(
      BuildEdsResource(args, kDefaultClusterName));
  Cluster cluster = default_cluster_;
  cluster.mutable_eds_cluster_config()->clear_service_name();
  balancers_[0]->ads_service()->SetCdsResource(cluster);
  SetNextResolution({});
  SetNextResolutionForLbChannelAllBalancers();
  CheckRpcSendOk();
}
class TimeoutTest : public BasicTest {
 protected:
  void SetUp() override {
    xds_resource_does_not_exist_timeout_ms_ = 500;
    BasicTest::SetUp();
  }
};
TEST_P(TimeoutTest, Lds) {
  balancers_[0]->ads_service()->SetResourceIgnore(kLdsTypeUrl);
  SetNextResolution({});
  SetNextResolutionForLbChannelAllBalancers();
  CheckRpcSendFailure();
}
TEST_P(TimeoutTest, Rds) {
  balancers_[0]->ads_service()->SetResourceIgnore(kRdsTypeUrl);
  SetNextResolution({});
  SetNextResolutionForLbChannelAllBalancers();
  CheckRpcSendFailure();
}
TEST_P(TimeoutTest, Cds) {
  balancers_[0]->ads_service()->SetResourceIgnore(kCdsTypeUrl);
  SetNextResolution({});
  SetNextResolutionForLbChannelAllBalancers();
  CheckRpcSendFailure();
}
TEST_P(TimeoutTest, Eds) {
  balancers_[0]->ads_service()->SetResourceIgnore(kEdsTypeUrl);
  SetNextResolution({});
  SetNextResolutionForLbChannelAllBalancers();
  CheckRpcSendFailure();
}
using LocalityMapTest = BasicTest;
TEST_P(LocalityMapTest, WeightedRoundRobin) {
  SetNextResolution({});
  SetNextResolutionForLbChannelAllBalancers();
  const size_t kNumRpcs = 5000;
  const int kLocalityWeight0 = 2;
  const int kLocalityWeight1 = 8;
  const int kTotalLocalityWeight = kLocalityWeight0 + kLocalityWeight1;
  const double kLocalityWeightRate0 =
      static_cast<double>(kLocalityWeight0) / kTotalLocalityWeight;
  const double kLocalityWeightRate1 =
      static_cast<double>(kLocalityWeight1) / kTotalLocalityWeight;
  AdsServiceImpl::EdsResourceArgs args({
      {"locality0", GetBackendPorts(0, 1), kLocalityWeight0},
      {"locality1", GetBackendPorts(1, 2), kLocalityWeight1},
  });
  balancers_[0]->ads_service()->SetEdsResource(
      BuildEdsResource(args, DefaultEdsServiceName()));
  WaitForAllBackends(0, 2);
  CheckRpcSendOk(kNumRpcs);
  const double locality_picked_rate_0 =
      static_cast<double>(backends_[0]->backend_service()->request_count()) /
      kNumRpcs;
  const double locality_picked_rate_1 =
      static_cast<double>(backends_[1]->backend_service()->request_count()) /
      kNumRpcs;
  const double kErrorTolerance = 0.2;
  EXPECT_THAT(locality_picked_rate_0,
              ::testing::AllOf(
                  ::testing::Ge(kLocalityWeightRate0 * (1 - kErrorTolerance)),
                  ::testing::Le(kLocalityWeightRate0 * (1 + kErrorTolerance))));
  EXPECT_THAT(locality_picked_rate_1,
              ::testing::AllOf(
                  ::testing::Ge(kLocalityWeightRate1 * (1 - kErrorTolerance)),
                  ::testing::Le(kLocalityWeightRate1 * (1 + kErrorTolerance))));
}
TEST_P(LocalityMapTest, LocalityContainingNoEndpoints) {
  SetNextResolution({});
  SetNextResolutionForLbChannelAllBalancers();
  const size_t kNumRpcs = 5000;
  AdsServiceImpl::EdsResourceArgs args({
      {"locality0", GetBackendPorts()},
      {"locality1", {}},
  });
  balancers_[0]->ads_service()->SetEdsResource(
      BuildEdsResource(args, DefaultEdsServiceName()));
  WaitForAllBackends();
  CheckRpcSendOk(kNumRpcs);
  EXPECT_EQ(backends_[0]->backend_service()->request_count(),
            kNumRpcs / backends_.size());
  EXPECT_EQ(backends_[1]->backend_service()->request_count(),
            kNumRpcs / backends_.size());
  EXPECT_EQ(backends_[2]->backend_service()->request_count(),
            kNumRpcs / backends_.size());
  EXPECT_EQ(backends_[3]->backend_service()->request_count(),
            kNumRpcs / backends_.size());
}
TEST_P(LocalityMapTest, NoLocalities) {
  SetNextResolution({});
  SetNextResolutionForLbChannelAllBalancers();
  balancers_[0]->ads_service()->SetEdsResource(
      BuildEdsResource({}, DefaultEdsServiceName()));
  Status status = SendRpc();
  EXPECT_FALSE(status.ok());
  EXPECT_EQ(status.error_code(), StatusCode::UNAVAILABLE);
}
TEST_P(LocalityMapTest, StressTest) {
  SetNextResolution({});
  SetNextResolutionForLbChannelAllBalancers();
  const size_t kNumLocalities = 100;
  AdsServiceImpl::EdsResourceArgs args;
  for (size_t i = 0; i < kNumLocalities; ++i) {
    std::string name = absl::StrCat("locality", i);
    AdsServiceImpl::EdsResourceArgs::Locality locality(name,
                                                       {backends_[0]->port()});
    args.locality_list.emplace_back(std::move(locality));
  }
  balancers_[0]->ads_service()->SetEdsResource(
      BuildEdsResource(args, DefaultEdsServiceName()));
  args = AdsServiceImpl::EdsResourceArgs({
      {"locality0", GetBackendPorts(1, 2)},
  });
  std::thread delayed_resource_setter(
      std::bind(&BasicTest::SetEdsResourceWithDelay, this, 0,
                BuildEdsResource(args, DefaultEdsServiceName()), 60 * 1000));
  WaitForBackend(0, false);
  EXPECT_EQ(0U, backends_[1]->backend_service()->request_count());
  WaitForBackend(1);
  delayed_resource_setter.join();
}
TEST_P(LocalityMapTest, UpdateMap) {
  SetNextResolution({});
  SetNextResolutionForLbChannelAllBalancers();
  const size_t kNumRpcs = 3000;
  const std::vector<int> kLocalityWeights0 = {2, 3, 4};
  const double kTotalLocalityWeight0 =
      std::accumulate(kLocalityWeights0.begin(), kLocalityWeights0.end(), 0);
  std::vector<double> locality_weight_rate_0;
  locality_weight_rate_0.reserve(kLocalityWeights0.size());
  for (int weight : kLocalityWeights0) {
    locality_weight_rate_0.push_back(weight / kTotalLocalityWeight0);
  }
  const std::vector<int> kLocalityWeights1 = {3, 2, 6};
  const double kTotalLocalityWeight1 =
      std::accumulate(kLocalityWeights1.begin(), kLocalityWeights1.end(), 0);
  std::vector<double> locality_weight_rate_1 = {
      0 };
  for (int weight : kLocalityWeights1) {
    locality_weight_rate_1.push_back(weight / kTotalLocalityWeight1);
  }
  AdsServiceImpl::EdsResourceArgs args({
      {"locality0", GetBackendPorts(0, 1), 2},
      {"locality1", GetBackendPorts(1, 2), 3},
      {"locality2", GetBackendPorts(2, 3), 4},
  });
  balancers_[0]->ads_service()->SetEdsResource(
      BuildEdsResource(args, DefaultEdsServiceName()));
  WaitForAllBackends(0, 3);
  gpr_log(GPR_INFO, "========= BEFORE FIRST BATCH ==========");
  CheckRpcSendOk(kNumRpcs);
  gpr_log(GPR_INFO, "========= DONE WITH FIRST BATCH ==========");
  std::vector<double> locality_picked_rates;
  for (size_t i = 0; i < 3; ++i) {
    locality_picked_rates.push_back(
        static_cast<double>(backends_[i]->backend_service()->request_count()) /
        kNumRpcs);
  }
  const double kErrorTolerance = 0.2;
  for (size_t i = 0; i < 3; ++i) {
    gpr_log(GPR_INFO, "Locality %" PRIuPTR " rate %f", i,
            locality_picked_rates[i]);
    EXPECT_THAT(
        locality_picked_rates[i],
        ::testing::AllOf(
            ::testing::Ge(locality_weight_rate_0[i] * (1 - kErrorTolerance)),
            ::testing::Le(locality_weight_rate_0[i] * (1 + kErrorTolerance))));
  }
  args = AdsServiceImpl::EdsResourceArgs({
      {"locality1", GetBackendPorts(1, 2), 3},
      {"locality2", GetBackendPorts(2, 3), 2},
      {"locality3", GetBackendPorts(3, 4), 6},
  });
  balancers_[0]->ads_service()->SetEdsResource(
      BuildEdsResource(args, DefaultEdsServiceName()));
  EXPECT_EQ(0U, backends_[3]->backend_service()->request_count());
  WaitForAllBackends(3, 4);
  gpr_log(GPR_INFO, "========= BEFORE SECOND BATCH ==========");
  CheckRpcSendOk(kNumRpcs);
  gpr_log(GPR_INFO, "========= DONE WITH SECOND BATCH ==========");
  EXPECT_EQ(0U, backends_[0]->backend_service()->request_count());
  locality_picked_rates = {0 };
  for (size_t i = 1; i < 4; ++i) {
    locality_picked_rates.push_back(
        static_cast<double>(backends_[i]->backend_service()->request_count()) /
        kNumRpcs);
  }
  for (size_t i = 1; i < 4; ++i) {
    gpr_log(GPR_INFO, "Locality %" PRIuPTR " rate %f", i,
            locality_picked_rates[i]);
    EXPECT_THAT(
        locality_picked_rates[i],
        ::testing::AllOf(
            ::testing::Ge(locality_weight_rate_1[i] * (1 - kErrorTolerance)),
            ::testing::Le(locality_weight_rate_1[i] * (1 + kErrorTolerance))));
  }
}
TEST_P(LocalityMapTest, ReplaceAllLocalitiesInPriority) {
  SetNextResolution({});
  SetNextResolutionForLbChannelAllBalancers();
  AdsServiceImpl::EdsResourceArgs args({
      {"locality0", GetBackendPorts(0, 1)},
  });
  balancers_[0]->ads_service()->SetEdsResource(
      BuildEdsResource(args, DefaultEdsServiceName()));
  args = AdsServiceImpl::EdsResourceArgs({
      {"locality1", GetBackendPorts(1, 2)},
  });
  std::thread delayed_resource_setter(
      std::bind(&BasicTest::SetEdsResourceWithDelay, this, 0,
                BuildEdsResource(args, DefaultEdsServiceName()), 5000));
  WaitForBackend(0);
  WaitForBackend(1, true, true);
  delayed_resource_setter.join();
}
class FailoverTest : public BasicTest {
 public:
  void SetUp() override {
    BasicTest::SetUp();
    ResetStub(500);
  }
};
TEST_P(FailoverTest, ChooseHighestPriority) {
  SetNextResolution({});
  SetNextResolutionForLbChannelAllBalancers();
  AdsServiceImpl::EdsResourceArgs args({
      {"locality0", GetBackendPorts(0, 1), kDefaultLocalityWeight, 1},
      {"locality1", GetBackendPorts(1, 2), kDefaultLocalityWeight, 2},
      {"locality2", GetBackendPorts(2, 3), kDefaultLocalityWeight, 3},
      {"locality3", GetBackendPorts(3, 4), kDefaultLocalityWeight, 0},
  });
  balancers_[0]->ads_service()->SetEdsResource(
      BuildEdsResource(args, DefaultEdsServiceName()));
  WaitForBackend(3, false);
  for (size_t i = 0; i < 3; ++i) {
    EXPECT_EQ(0U, backends_[i]->backend_service()->request_count());
  }
}
TEST_P(FailoverTest, DoesNotUsePriorityWithNoEndpoints) {
  SetNextResolution({});
  SetNextResolutionForLbChannelAllBalancers();
  AdsServiceImpl::EdsResourceArgs args({
      {"locality0", GetBackendPorts(0, 1), kDefaultLocalityWeight, 1},
      {"locality1", GetBackendPorts(1, 2), kDefaultLocalityWeight, 2},
      {"locality2", GetBackendPorts(2, 3), kDefaultLocalityWeight, 3},
      {"locality3", {}, kDefaultLocalityWeight, 0},
  });
  balancers_[0]->ads_service()->SetEdsResource(
      BuildEdsResource(args, DefaultEdsServiceName()));
  WaitForBackend(0, false);
  for (size_t i = 1; i < 3; ++i) {
    EXPECT_EQ(0U, backends_[i]->backend_service()->request_count());
  }
}
TEST_P(FailoverTest, DoesNotUseLocalityWithNoEndpoints) {
  SetNextResolution({});
  SetNextResolutionForLbChannelAllBalancers();
  AdsServiceImpl::EdsResourceArgs args({
      {"locality0", {}, kDefaultLocalityWeight, 0},
      {"locality1", GetBackendPorts(), kDefaultLocalityWeight, 0},
  });
  balancers_[0]->ads_service()->SetEdsResource(
      BuildEdsResource(args, DefaultEdsServiceName()));
  std::tuple<int, int, int> counts = WaitForAllBackends();
  EXPECT_EQ(0, std::get<1>(counts));
}
TEST_P(FailoverTest, Failover) {
  SetNextResolution({});
  SetNextResolutionForLbChannelAllBalancers();
  AdsServiceImpl::EdsResourceArgs args({
      {"locality0", GetBackendPorts(0, 1), kDefaultLocalityWeight, 1},
      {"locality1", GetBackendPorts(1, 2), kDefaultLocalityWeight, 2},
      {"locality2", GetBackendPorts(2, 3), kDefaultLocalityWeight, 3},
      {"locality3", GetBackendPorts(3, 4), kDefaultLocalityWeight, 0},
  });
  ShutdownBackend(3);
  ShutdownBackend(0);
  balancers_[0]->ads_service()->SetEdsResource(
      BuildEdsResource(args, DefaultEdsServiceName()));
  WaitForBackend(1, false);
  for (size_t i = 0; i < 4; ++i) {
    if (i == 1) continue;
    EXPECT_EQ(0U, backends_[i]->backend_service()->request_count());
  }
}
TEST_P(FailoverTest, SwitchBackToHigherPriority) {
  SetNextResolution({});
  SetNextResolutionForLbChannelAllBalancers();
  const size_t kNumRpcs = 100;
  AdsServiceImpl::EdsResourceArgs args({
      {"locality0", GetBackendPorts(0, 1), kDefaultLocalityWeight, 1},
      {"locality1", GetBackendPorts(1, 2), kDefaultLocalityWeight, 2},
      {"locality2", GetBackendPorts(2, 3), kDefaultLocalityWeight, 3},
      {"locality3", GetBackendPorts(3, 4), kDefaultLocalityWeight, 0},
  });
  ShutdownBackend(3);
  ShutdownBackend(0);
  balancers_[0]->ads_service()->SetEdsResource(
      BuildEdsResource(args, DefaultEdsServiceName()));
  WaitForBackend(1, false);
  for (size_t i = 0; i < 4; ++i) {
    if (i == 1) continue;
    EXPECT_EQ(0U, backends_[i]->backend_service()->request_count());
  }
  StartBackend(0);
  WaitForBackend(0);
  CheckRpcSendOk(kNumRpcs);
  EXPECT_EQ(kNumRpcs, backends_[0]->backend_service()->request_count());
}
TEST_P(FailoverTest, UpdateInitialUnavailable) {
  SetNextResolution({});
  SetNextResolutionForLbChannelAllBalancers();
  AdsServiceImpl::EdsResourceArgs args({
      {"locality0", GetBackendPorts(0, 1), kDefaultLocalityWeight, 0},
      {"locality1", GetBackendPorts(1, 2), kDefaultLocalityWeight, 1},
  });
  balancers_[0]->ads_service()->SetEdsResource(
      BuildEdsResource(args, DefaultEdsServiceName()));
  args = AdsServiceImpl::EdsResourceArgs({
      {"locality0", GetBackendPorts(0, 1), kDefaultLocalityWeight, 0},
      {"locality1", GetBackendPorts(1, 2), kDefaultLocalityWeight, 1},
      {"locality2", GetBackendPorts(2, 3), kDefaultLocalityWeight, 2},
      {"locality3", GetBackendPorts(3, 4), kDefaultLocalityWeight, 3},
  });
  ShutdownBackend(0);
  ShutdownBackend(1);
  std::thread delayed_resource_setter(
      std::bind(&BasicTest::SetEdsResourceWithDelay, this, 0,
                BuildEdsResource(args, DefaultEdsServiceName()), 1000));
  gpr_timespec deadline = gpr_time_add(gpr_now(GPR_CLOCK_REALTIME),
                                       gpr_time_from_millis(500, GPR_TIMESPAN));
  do {
    CheckRpcSendFailure();
  } while (gpr_time_cmp(gpr_now(GPR_CLOCK_REALTIME), deadline) < 0);
  WaitForBackend(2, false);
  for (size_t i = 0; i < 4; ++i) {
    if (i == 2) continue;
    EXPECT_EQ(0U, backends_[i]->backend_service()->request_count());
  }
  delayed_resource_setter.join();
}
TEST_P(FailoverTest, UpdatePriority) {
  SetNextResolution({});
  SetNextResolutionForLbChannelAllBalancers();
  const size_t kNumRpcs = 100;
  AdsServiceImpl::EdsResourceArgs args({
      {"locality0", GetBackendPorts(0, 1), kDefaultLocalityWeight, 1},
      {"locality1", GetBackendPorts(1, 2), kDefaultLocalityWeight, 2},
      {"locality2", GetBackendPorts(2, 3), kDefaultLocalityWeight, 3},
      {"locality3", GetBackendPorts(3, 4), kDefaultLocalityWeight, 0},
  });
  balancers_[0]->ads_service()->SetEdsResource(
      BuildEdsResource(args, DefaultEdsServiceName()));
  args = AdsServiceImpl::EdsResourceArgs({
      {"locality0", GetBackendPorts(0, 1), kDefaultLocalityWeight, 2},
      {"locality1", GetBackendPorts(1, 2), kDefaultLocalityWeight, 0},
      {"locality2", GetBackendPorts(2, 3), kDefaultLocalityWeight, 1},
      {"locality3", GetBackendPorts(3, 4), kDefaultLocalityWeight, 3},
  });
  std::thread delayed_resource_setter(
      std::bind(&BasicTest::SetEdsResourceWithDelay, this, 0,
                BuildEdsResource(args, DefaultEdsServiceName()), 1000));
  WaitForBackend(3, false);
  for (size_t i = 0; i < 3; ++i) {
    EXPECT_EQ(0U, backends_[i]->backend_service()->request_count());
  }
  WaitForBackend(1);
  CheckRpcSendOk(kNumRpcs);
  EXPECT_EQ(kNumRpcs, backends_[1]->backend_service()->request_count());
  delayed_resource_setter.join();
}
TEST_P(FailoverTest, MoveAllLocalitiesInCurrentPriorityToHigherPriority) {
  SetNextResolution({});
  SetNextResolutionForLbChannelAllBalancers();
  ShutdownBackend(0);
  AdsServiceImpl::EdsResourceArgs args({
      {"locality0", GetBackendPorts(0, 1), kDefaultLocalityWeight, 0},
      {"locality1", GetBackendPorts(1, 3), kDefaultLocalityWeight, 1},
  });
  balancers_[0]->ads_service()->SetEdsResource(
      BuildEdsResource(args, DefaultEdsServiceName()));
  args = AdsServiceImpl::EdsResourceArgs({
      {"locality0", GetBackendPorts(0, 1), kDefaultLocalityWeight, 0},
      {"locality1", GetBackendPorts(1, 4), kDefaultLocalityWeight, 0},
  });
  std::thread delayed_resource_setter(
      std::bind(&BasicTest::SetEdsResourceWithDelay, this, 0,
                BuildEdsResource(args, DefaultEdsServiceName()), 1000));
  WaitForAllBackends(1, 3, false);
  EXPECT_EQ(0UL, backends_[3]->backend_service()->request_count());
  WaitForBackend(3);
  EXPECT_GT(balancers_[0]->ads_service()->eds_response_state().state,
            AdsServiceImpl::ResponseState::NOT_SENT);
  delayed_resource_setter.join();
}
using DropTest = BasicTest;
TEST_P(DropTest, Vanilla) {
  SetNextResolution({});
  SetNextResolutionForLbChannelAllBalancers();
  const size_t kNumRpcs = 5000;
  const uint32_t kDropPerMillionForLb = 100000;
  const uint32_t kDropPerMillionForThrottle = 200000;
  const double kDropRateForLb = kDropPerMillionForLb / 1000000.0;
  const double kDropRateForThrottle = kDropPerMillionForThrottle / 1000000.0;
  const double KDropRateForLbAndThrottle =
      kDropRateForLb + (1 - kDropRateForLb) * kDropRateForThrottle;
  AdsServiceImpl::EdsResourceArgs args({
      {"locality0", GetBackendPorts()},
  });
  args.drop_categories = {{kLbDropType, kDropPerMillionForLb},
                          {kThrottleDropType, kDropPerMillionForThrottle}};
  balancers_[0]->ads_service()->SetEdsResource(
      BuildEdsResource(args, DefaultEdsServiceName()));
  WaitForAllBackends();
  size_t num_drops = 0;
  for (size_t i = 0; i < kNumRpcs; ++i) {
    EchoResponse response;
    const Status status = SendRpc(RpcOptions(), &response);
    if (!status.ok() &&
        status.error_message() == "Call dropped by load balancing policy") {
      ++num_drops;
    } else {
      EXPECT_TRUE(status.ok()) << "code=" << status.error_code()
                               << " message=" << status.error_message();
      EXPECT_EQ(response.message(), kRequestMessage);
    }
  }
  const double seen_drop_rate = static_cast<double>(num_drops) / kNumRpcs;
  const double kErrorTolerance = 0.2;
  EXPECT_THAT(
      seen_drop_rate,
      ::testing::AllOf(
          ::testing::Ge(KDropRateForLbAndThrottle * (1 - kErrorTolerance)),
          ::testing::Le(KDropRateForLbAndThrottle * (1 + kErrorTolerance))));
}
TEST_P(DropTest, DropPerHundred) {
  SetNextResolution({});
  SetNextResolutionForLbChannelAllBalancers();
  const size_t kNumRpcs = 5000;
  const uint32_t kDropPerHundredForLb = 10;
  const double kDropRateForLb = kDropPerHundredForLb / 100.0;
  AdsServiceImpl::EdsResourceArgs args({
      {"locality0", GetBackendPorts()},
  });
  args.drop_categories = {{kLbDropType, kDropPerHundredForLb}};
  args.drop_denominator = FractionalPercent::HUNDRED;
  balancers_[0]->ads_service()->SetEdsResource(
      BuildEdsResource(args, DefaultEdsServiceName()));
  WaitForAllBackends();
  size_t num_drops = 0;
  for (size_t i = 0; i < kNumRpcs; ++i) {
    EchoResponse response;
    const Status status = SendRpc(RpcOptions(), &response);
    if (!status.ok() &&
        status.error_message() == "Call dropped by load balancing policy") {
      ++num_drops;
    } else {
      EXPECT_TRUE(status.ok()) << "code=" << status.error_code()
                               << " message=" << status.error_message();
      EXPECT_EQ(response.message(), kRequestMessage);
    }
  }
  const double seen_drop_rate = static_cast<double>(num_drops) / kNumRpcs;
  const double kErrorTolerance = 0.2;
  EXPECT_THAT(
      seen_drop_rate,
      ::testing::AllOf(::testing::Ge(kDropRateForLb * (1 - kErrorTolerance)),
                       ::testing::Le(kDropRateForLb * (1 + kErrorTolerance))));
}
TEST_P(DropTest, DropPerTenThousand) {
  SetNextResolution({});
  SetNextResolutionForLbChannelAllBalancers();
  const size_t kNumRpcs = 5000;
  const uint32_t kDropPerTenThousandForLb = 1000;
  const double kDropRateForLb = kDropPerTenThousandForLb / 10000.0;
  AdsServiceImpl::EdsResourceArgs args({
      {"locality0", GetBackendPorts()},
  });
  args.drop_categories = {{kLbDropType, kDropPerTenThousandForLb}};
  args.drop_denominator = FractionalPercent::TEN_THOUSAND;
  balancers_[0]->ads_service()->SetEdsResource(
      BuildEdsResource(args, DefaultEdsServiceName()));
  WaitForAllBackends();
  size_t num_drops = 0;
  for (size_t i = 0; i < kNumRpcs; ++i) {
    EchoResponse response;
    const Status status = SendRpc(RpcOptions(), &response);
    if (!status.ok() &&
        status.error_message() == "Call dropped by load balancing policy") {
      ++num_drops;
    } else {
      EXPECT_TRUE(status.ok()) << "code=" << status.error_code()
                               << " message=" << status.error_message();
      EXPECT_EQ(response.message(), kRequestMessage);
    }
  }
  const double seen_drop_rate = static_cast<double>(num_drops) / kNumRpcs;
  const double kErrorTolerance = 0.2;
  EXPECT_THAT(
      seen_drop_rate,
      ::testing::AllOf(::testing::Ge(kDropRateForLb * (1 - kErrorTolerance)),
                       ::testing::Le(kDropRateForLb * (1 + kErrorTolerance))));
}
TEST_P(DropTest, Update) {
  SetNextResolution({});
  SetNextResolutionForLbChannelAllBalancers();
  const size_t kNumRpcs = 3000;
  const uint32_t kDropPerMillionForLb = 100000;
  const uint32_t kDropPerMillionForThrottle = 200000;
  const double kDropRateForLb = kDropPerMillionForLb / 1000000.0;
  const double kDropRateForThrottle = kDropPerMillionForThrottle / 1000000.0;
  const double KDropRateForLbAndThrottle =
      kDropRateForLb + (1 - kDropRateForLb) * kDropRateForThrottle;
  AdsServiceImpl::EdsResourceArgs args({
      {"locality0", GetBackendPorts()},
  });
  args.drop_categories = {{kLbDropType, kDropPerMillionForLb}};
  balancers_[0]->ads_service()->SetEdsResource(
      BuildEdsResource(args, DefaultEdsServiceName()));
  WaitForAllBackends();
  size_t num_drops = 0;
  gpr_log(GPR_INFO, "========= BEFORE FIRST BATCH ==========");
  for (size_t i = 0; i < kNumRpcs; ++i) {
    EchoResponse response;
    const Status status = SendRpc(RpcOptions(), &response);
    if (!status.ok() &&
        status.error_message() == "Call dropped by load balancing policy") {
      ++num_drops;
    } else {
      EXPECT_TRUE(status.ok()) << "code=" << status.error_code()
                               << " message=" << status.error_message();
      EXPECT_EQ(response.message(), kRequestMessage);
    }
  }
  gpr_log(GPR_INFO, "========= DONE WITH FIRST BATCH ==========");
  double seen_drop_rate = static_cast<double>(num_drops) / kNumRpcs;
  gpr_log(GPR_INFO, "First batch drop rate %f", seen_drop_rate);
  const double kErrorTolerance = 0.3;
  EXPECT_THAT(
      seen_drop_rate,
      ::testing::AllOf(::testing::Ge(kDropRateForLb * (1 - kErrorTolerance)),
                       ::testing::Le(kDropRateForLb * (1 + kErrorTolerance))));
  args.drop_categories = {{kLbDropType, kDropPerMillionForLb},
                          {kThrottleDropType, kDropPerMillionForThrottle}};
  balancers_[0]->ads_service()->SetEdsResource(
      BuildEdsResource(args, DefaultEdsServiceName()));
  const double kDropRateThreshold =
      (kDropRateForLb + KDropRateForLbAndThrottle) / 2;
  size_t num_rpcs = kNumRpcs;
  while (seen_drop_rate < kDropRateThreshold) {
    EchoResponse response;
    const Status status = SendRpc(RpcOptions(), &response);
    ++num_rpcs;
    if (!status.ok() &&
        status.error_message() == "Call dropped by load balancing policy") {
      ++num_drops;
    } else {
      EXPECT_TRUE(status.ok()) << "code=" << status.error_code()
                               << " message=" << status.error_message();
      EXPECT_EQ(response.message(), kRequestMessage);
    }
    seen_drop_rate = static_cast<double>(num_drops) / num_rpcs;
  }
  num_drops = 0;
  gpr_log(GPR_INFO, "========= BEFORE SECOND BATCH ==========");
  for (size_t i = 0; i < kNumRpcs; ++i) {
    EchoResponse response;
    const Status status = SendRpc(RpcOptions(), &response);
    if (!status.ok() &&
        status.error_message() == "Call dropped by load balancing policy") {
      ++num_drops;
    } else {
      EXPECT_TRUE(status.ok()) << "code=" << status.error_code()
                               << " message=" << status.error_message();
      EXPECT_EQ(response.message(), kRequestMessage);
    }
  }
  gpr_log(GPR_INFO, "========= DONE WITH SECOND BATCH ==========");
  seen_drop_rate = static_cast<double>(num_drops) / kNumRpcs;
  gpr_log(GPR_INFO, "Second batch drop rate %f", seen_drop_rate);
  EXPECT_THAT(
      seen_drop_rate,
      ::testing::AllOf(
          ::testing::Ge(KDropRateForLbAndThrottle * (1 - kErrorTolerance)),
          ::testing::Le(KDropRateForLbAndThrottle * (1 + kErrorTolerance))));
}
TEST_P(DropTest, DropAll) {
  SetNextResolution({});
  SetNextResolutionForLbChannelAllBalancers();
  const size_t kNumRpcs = 1000;
  const uint32_t kDropPerMillionForLb = 100000;
  const uint32_t kDropPerMillionForThrottle = 1000000;
  AdsServiceImpl::EdsResourceArgs args;
  args.drop_categories = {{kLbDropType, kDropPerMillionForLb},
                          {kThrottleDropType, kDropPerMillionForThrottle}};
  balancers_[0]->ads_service()->SetEdsResource(
      BuildEdsResource(args, DefaultEdsServiceName()));
  for (size_t i = 0; i < kNumRpcs; ++i) {
    EchoResponse response;
    const Status status = SendRpc(RpcOptions(), &response);
    EXPECT_EQ(status.error_code(), StatusCode::UNAVAILABLE);
    EXPECT_EQ(status.error_message(), "Call dropped by load balancing policy");
  }
}
class BalancerUpdateTest : public XdsEnd2endTest {
 public:
  BalancerUpdateTest() : XdsEnd2endTest(4, 3) {}
};
TEST_P(BalancerUpdateTest, UpdateBalancersButKeepUsingOriginalBalancer) {
  SetNextResolution({});
  SetNextResolutionForLbChannelAllBalancers();
  AdsServiceImpl::EdsResourceArgs args({
      {"locality0", {backends_[0]->port()}},
  });
  balancers_[0]->ads_service()->SetEdsResource(
      BuildEdsResource(args, DefaultEdsServiceName()));
  args = AdsServiceImpl::EdsResourceArgs({
      {"locality0", {backends_[1]->port()}},
  });
  balancers_[1]->ads_service()->SetEdsResource(
      BuildEdsResource(args, DefaultEdsServiceName()));
  WaitForBackend(0);
  gpr_log(GPR_INFO, "========= BEFORE FIRST BATCH ==========");
  CheckRpcSendOk(10);
  gpr_log(GPR_INFO, "========= DONE WITH FIRST BATCH ==========");
  EXPECT_EQ(10U, backends_[0]->backend_service()->request_count());
  EXPECT_GT(balancers_[0]->ads_service()->eds_response_state().state,
            AdsServiceImpl::ResponseState::NOT_SENT);
  EXPECT_EQ(balancers_[1]->ads_service()->eds_response_state().state,
            AdsServiceImpl::ResponseState::NOT_SENT)
      << "Error Message:"
      << balancers_[1]->ads_service()->eds_response_state().error_message;
  EXPECT_EQ(balancers_[2]->ads_service()->eds_response_state().state,
            AdsServiceImpl::ResponseState::NOT_SENT)
      << "Error Message:"
      << balancers_[2]->ads_service()->eds_response_state().error_message;
  gpr_log(GPR_INFO, "========= ABOUT TO UPDATE 1 ==========");
  SetNextResolutionForLbChannel({balancers_[1]->port()});
  gpr_log(GPR_INFO, "========= UPDATE 1 DONE ==========");
  EXPECT_EQ(0U, backends_[1]->backend_service()->request_count());
  gpr_timespec deadline = gpr_time_add(
      gpr_now(GPR_CLOCK_REALTIME), gpr_time_from_millis(10000, GPR_TIMESPAN));
  do {
    CheckRpcSendOk();
  } while (gpr_time_cmp(gpr_now(GPR_CLOCK_REALTIME), deadline) < 0);
  EXPECT_EQ(0U, backends_[1]->backend_service()->request_count());
  EXPECT_GT(balancers_[0]->ads_service()->eds_response_state().state,
            AdsServiceImpl::ResponseState::NOT_SENT);
  EXPECT_EQ(balancers_[1]->ads_service()->eds_response_state().state,
            AdsServiceImpl::ResponseState::NOT_SENT)
      << "Error Message:"
      << balancers_[1]->ads_service()->eds_response_state().error_message;
  EXPECT_EQ(balancers_[2]->ads_service()->eds_response_state().state,
            AdsServiceImpl::ResponseState::NOT_SENT)
      << "Error Message:"
      << balancers_[2]->ads_service()->eds_response_state().error_message;
}
TEST_P(BalancerUpdateTest, Repeated) {
  SetNextResolution({});
  SetNextResolutionForLbChannelAllBalancers();
  AdsServiceImpl::EdsResourceArgs args({
      {"locality0", {backends_[0]->port()}},
  });
  balancers_[0]->ads_service()->SetEdsResource(
      BuildEdsResource(args, DefaultEdsServiceName()));
  args = AdsServiceImpl::EdsResourceArgs({
      {"locality0", {backends_[1]->port()}},
  });
  balancers_[1]->ads_service()->SetEdsResource(
      BuildEdsResource(args, DefaultEdsServiceName()));
  WaitForBackend(0);
  gpr_log(GPR_INFO, "========= BEFORE FIRST BATCH ==========");
  CheckRpcSendOk(10);
  gpr_log(GPR_INFO, "========= DONE WITH FIRST BATCH ==========");
  EXPECT_EQ(10U, backends_[0]->backend_service()->request_count());
  EXPECT_GT(balancers_[0]->ads_service()->eds_response_state().state,
            AdsServiceImpl::ResponseState::NOT_SENT);
  EXPECT_EQ(balancers_[1]->ads_service()->eds_response_state().state,
            AdsServiceImpl::ResponseState::NOT_SENT)
      << "Error Message:"
      << balancers_[1]->ads_service()->eds_response_state().error_message;
  EXPECT_EQ(balancers_[2]->ads_service()->eds_response_state().state,
            AdsServiceImpl::ResponseState::NOT_SENT)
      << "Error Message:"
      << balancers_[2]->ads_service()->eds_response_state().error_message;
  std::vector<int> ports;
  ports.emplace_back(balancers_[0]->port());
  ports.emplace_back(balancers_[1]->port());
  ports.emplace_back(balancers_[2]->port());
  gpr_log(GPR_INFO, "========= ABOUT TO UPDATE 1 ==========");
  SetNextResolutionForLbChannel(ports);
  gpr_log(GPR_INFO, "========= UPDATE 1 DONE ==========");
  EXPECT_EQ(0U, backends_[1]->backend_service()->request_count());
  gpr_timespec deadline = gpr_time_add(
      gpr_now(GPR_CLOCK_REALTIME), gpr_time_from_millis(10000, GPR_TIMESPAN));
  do {
    CheckRpcSendOk();
  } while (gpr_time_cmp(gpr_now(GPR_CLOCK_REALTIME), deadline) < 0);
  EXPECT_EQ(0U, backends_[1]->backend_service()->request_count());
  ports.clear();
  ports.emplace_back(balancers_[0]->port());
  ports.emplace_back(balancers_[1]->port());
  gpr_log(GPR_INFO, "========= ABOUT TO UPDATE 2 ==========");
  SetNextResolutionForLbChannel(ports);
  gpr_log(GPR_INFO, "========= UPDATE 2 DONE ==========");
  EXPECT_EQ(0U, backends_[1]->backend_service()->request_count());
  deadline = gpr_time_add(gpr_now(GPR_CLOCK_REALTIME),
                          gpr_time_from_millis(10000, GPR_TIMESPAN));
  do {
    CheckRpcSendOk();
  } while (gpr_time_cmp(gpr_now(GPR_CLOCK_REALTIME), deadline) < 0);
  EXPECT_EQ(0U, backends_[1]->backend_service()->request_count());
}
TEST_P(BalancerUpdateTest, DeadUpdate) {
  SetNextResolution({});
  SetNextResolutionForLbChannel({balancers_[0]->port()});
  AdsServiceImpl::EdsResourceArgs args({
      {"locality0", {backends_[0]->port()}},
  });
  balancers_[0]->ads_service()->SetEdsResource(
      BuildEdsResource(args, DefaultEdsServiceName()));
  args = AdsServiceImpl::EdsResourceArgs({
      {"locality0", {backends_[1]->port()}},
  });
  balancers_[1]->ads_service()->SetEdsResource(
      BuildEdsResource(args, DefaultEdsServiceName()));
  gpr_log(GPR_INFO, "========= BEFORE FIRST BATCH ==========");
  CheckRpcSendOk(10);
  gpr_log(GPR_INFO, "========= DONE WITH FIRST BATCH ==========");
  EXPECT_EQ(10U, backends_[0]->backend_service()->request_count());
  EXPECT_GT(balancers_[0]->ads_service()->eds_response_state().state,
            AdsServiceImpl::ResponseState::NOT_SENT);
  EXPECT_EQ(balancers_[1]->ads_service()->eds_response_state().state,
            AdsServiceImpl::ResponseState::NOT_SENT)
      << "Error Message:"
      << balancers_[1]->ads_service()->eds_response_state().error_message;
  EXPECT_EQ(balancers_[2]->ads_service()->eds_response_state().state,
            AdsServiceImpl::ResponseState::NOT_SENT)
      << "Error Message:"
      << balancers_[2]->ads_service()->eds_response_state().error_message;
  gpr_log(GPR_INFO, "********** ABOUT TO KILL BALANCER 0 *************");
  balancers_[0]->Shutdown();
  gpr_log(GPR_INFO, "********** KILLED BALANCER 0 *************");
  gpr_log(GPR_INFO, "========= BEFORE SECOND BATCH ==========");
  CheckRpcSendOk(10);
  gpr_log(GPR_INFO, "========= DONE WITH SECOND BATCH ==========");
  EXPECT_EQ(20U, backends_[0]->backend_service()->request_count());
  EXPECT_EQ(0U, backends_[1]->backend_service()->request_count());
  EXPECT_EQ(balancers_[0]->ads_service()->eds_response_state().state,
            AdsServiceImpl::ResponseState::NOT_SENT)
      << "Error Message:"
      << balancers_[0]->ads_service()->eds_response_state().error_message;
  EXPECT_EQ(balancers_[1]->ads_service()->eds_response_state().state,
            AdsServiceImpl::ResponseState::NOT_SENT)
      << "Error Message:"
      << balancers_[1]->ads_service()->eds_response_state().error_message;
  EXPECT_EQ(balancers_[2]->ads_service()->eds_response_state().state,
            AdsServiceImpl::ResponseState::NOT_SENT)
      << "Error Message:"
      << balancers_[2]->ads_service()->eds_response_state().error_message;
  gpr_log(GPR_INFO, "========= ABOUT TO UPDATE 1 ==========");
  SetNextResolutionForLbChannel({balancers_[1]->port()});
  gpr_log(GPR_INFO, "========= UPDATE 1 DONE ==========");
  EXPECT_EQ(0U, backends_[1]->backend_service()->request_count());
  WaitForBackend(1);
  backends_[1]->backend_service()->ResetCounters();
  gpr_log(GPR_INFO, "========= BEFORE THIRD BATCH ==========");
  CheckRpcSendOk(10);
  gpr_log(GPR_INFO, "========= DONE WITH THIRD BATCH ==========");
  EXPECT_EQ(10U, backends_[1]->backend_service()->request_count());
  EXPECT_EQ(balancers_[0]->ads_service()->eds_response_state().state,
            AdsServiceImpl::ResponseState::NOT_SENT)
      << "Error Message:"
      << balancers_[0]->ads_service()->eds_response_state().error_message;
  EXPECT_GT(balancers_[1]->ads_service()->eds_response_state().state,
            AdsServiceImpl::ResponseState::NOT_SENT);
  EXPECT_EQ(balancers_[2]->ads_service()->eds_response_state().state,
            AdsServiceImpl::ResponseState::NOT_SENT)
      << "Error Message:"
      << balancers_[2]->ads_service()->eds_response_state().error_message;
}
class ClientLoadReportingTest : public XdsEnd2endTest {
 public:
  ClientLoadReportingTest() : XdsEnd2endTest(4, 1, 3) {}
};
TEST_P(ClientLoadReportingTest, Vanilla) {
  if (!GetParam().use_xds_resolver()) {
    balancers_[0]->lrs_service()->set_cluster_names({kServerName});
  }
  SetNextResolution({});
  SetNextResolutionForLbChannel({balancers_[0]->port()});
  const size_t kNumRpcsPerAddress = 10;
  const size_t kNumFailuresPerAddress = 3;
  AdsServiceImpl::EdsResourceArgs args({
      {"locality0", GetBackendPorts()},
  });
  balancers_[0]->ads_service()->SetEdsResource(
      BuildEdsResource(args, DefaultEdsServiceName()));
  int num_ok = 0;
  int num_failure = 0;
  int num_drops = 0;
  std::tie(num_ok, num_failure, num_drops) = WaitForAllBackends();
  CheckRpcSendOk(kNumRpcsPerAddress * num_backends_);
  CheckRpcSendFailure(kNumFailuresPerAddress * num_backends_,
                      RpcOptions().set_server_fail(true));
  for (size_t i = 0; i < backends_.size(); ++i) {
    EXPECT_EQ(kNumRpcsPerAddress + kNumFailuresPerAddress,
              backends_[i]->backend_service()->request_count());
  }
  std::vector<ClientStats> load_report =
      balancers_[0]->lrs_service()->WaitForLoadReport();
  ASSERT_EQ(load_report.size(), 1UL);
  ClientStats& client_stats = load_report.front();
  EXPECT_EQ(kNumRpcsPerAddress * num_backends_ + num_ok,
            client_stats.total_successful_requests());
  EXPECT_EQ(0U, client_stats.total_requests_in_progress());
  EXPECT_EQ((kNumRpcsPerAddress + kNumFailuresPerAddress) * num_backends_ +
                num_ok + num_failure,
            client_stats.total_issued_requests());
  EXPECT_EQ(kNumFailuresPerAddress * num_backends_ + num_failure,
            client_stats.total_error_requests());
  EXPECT_EQ(0U, client_stats.total_dropped_requests());
  EXPECT_EQ(1U, balancers_[0]->lrs_service()->request_count());
  EXPECT_EQ(1U, balancers_[0]->lrs_service()->response_count());
}
TEST_P(ClientLoadReportingTest, SendAllClusters) {
  balancers_[0]->lrs_service()->set_send_all_clusters(true);
  SetNextResolution({});
  SetNextResolutionForLbChannel({balancers_[0]->port()});
  const size_t kNumRpcsPerAddress = 10;
  const size_t kNumFailuresPerAddress = 3;
  AdsServiceImpl::EdsResourceArgs args({
      {"locality0", GetBackendPorts()},
  });
  balancers_[0]->ads_service()->SetEdsResource(
      BuildEdsResource(args, DefaultEdsServiceName()));
  int num_ok = 0;
  int num_failure = 0;
  int num_drops = 0;
  std::tie(num_ok, num_failure, num_drops) = WaitForAllBackends();
  CheckRpcSendOk(kNumRpcsPerAddress * num_backends_);
  CheckRpcSendFailure(kNumFailuresPerAddress * num_backends_,
                      RpcOptions().set_server_fail(true));
  for (size_t i = 0; i < backends_.size(); ++i) {
    EXPECT_EQ(kNumRpcsPerAddress + kNumFailuresPerAddress,
              backends_[i]->backend_service()->request_count());
  }
  std::vector<ClientStats> load_report =
      balancers_[0]->lrs_service()->WaitForLoadReport();
  ASSERT_EQ(load_report.size(), 1UL);
  ClientStats& client_stats = load_report.front();
  EXPECT_EQ(kNumRpcsPerAddress * num_backends_ + num_ok,
            client_stats.total_successful_requests());
  EXPECT_EQ(0U, client_stats.total_requests_in_progress());
  EXPECT_EQ((kNumRpcsPerAddress + kNumFailuresPerAddress) * num_backends_ +
                num_ok + num_failure,
            client_stats.total_issued_requests());
  EXPECT_EQ(kNumFailuresPerAddress * num_backends_ + num_failure,
            client_stats.total_error_requests());
  EXPECT_EQ(0U, client_stats.total_dropped_requests());
  EXPECT_EQ(1U, balancers_[0]->lrs_service()->request_count());
  EXPECT_EQ(1U, balancers_[0]->lrs_service()->response_count());
}
TEST_P(ClientLoadReportingTest, HonorsClustersRequestedByLrsServer) {
  balancers_[0]->lrs_service()->set_cluster_names({"bogus"});
  SetNextResolution({});
  SetNextResolutionForLbChannel({balancers_[0]->port()});
  const size_t kNumRpcsPerAddress = 100;
  AdsServiceImpl::EdsResourceArgs args({
      {"locality0", GetBackendPorts()},
  });
  balancers_[0]->ads_service()->SetEdsResource(
      BuildEdsResource(args, DefaultEdsServiceName()));
  int num_ok = 0;
  int num_failure = 0;
  int num_drops = 0;
  std::tie(num_ok, num_failure, num_drops) = WaitForAllBackends();
  CheckRpcSendOk(kNumRpcsPerAddress * num_backends_);
  for (size_t i = 0; i < backends_.size(); ++i) {
    EXPECT_EQ(kNumRpcsPerAddress,
              backends_[i]->backend_service()->request_count());
  }
  EXPECT_EQ(1U, balancers_[0]->lrs_service()->request_count());
  EXPECT_EQ(1U, balancers_[0]->lrs_service()->response_count());
  std::vector<ClientStats> load_report =
      balancers_[0]->lrs_service()->WaitForLoadReport();
  ASSERT_EQ(load_report.size(), 0UL);
}
TEST_P(ClientLoadReportingTest, BalancerRestart) {
  if (!GetParam().use_xds_resolver()) {
    balancers_[0]->lrs_service()->set_cluster_names({kServerName});
  }
  SetNextResolution({});
  SetNextResolutionForLbChannel({balancers_[0]->port()});
  const size_t kNumBackendsFirstPass = backends_.size() / 2;
  const size_t kNumBackendsSecondPass =
      backends_.size() - kNumBackendsFirstPass;
  AdsServiceImpl::EdsResourceArgs args({
      {"locality0", GetBackendPorts(0, kNumBackendsFirstPass)},
  });
  balancers_[0]->ads_service()->SetEdsResource(
      BuildEdsResource(args, DefaultEdsServiceName()));
  int num_ok = 0;
  int num_failure = 0;
  int num_drops = 0;
  std::tie(num_ok, num_failure, num_drops) =
      WaitForAllBackends( 0,
                                          kNumBackendsFirstPass);
  std::vector<ClientStats> load_report =
      balancers_[0]->lrs_service()->WaitForLoadReport();
  ASSERT_EQ(load_report.size(), 1UL);
  ClientStats client_stats = std::move(load_report.front());
  EXPECT_EQ(static_cast<size_t>(num_ok),
            client_stats.total_successful_requests());
  EXPECT_EQ(0U, client_stats.total_requests_in_progress());
  EXPECT_EQ(0U, client_stats.total_error_requests());
  EXPECT_EQ(0U, client_stats.total_dropped_requests());
  balancers_[0]->Shutdown();
  ResetBackendCounters();
  int num_started = std::get<0>(WaitForAllBackends(
                        0, kNumBackendsFirstPass));
  balancers_[0]->Start();
  args = AdsServiceImpl::EdsResourceArgs({
      {"locality0", GetBackendPorts(kNumBackendsFirstPass)},
  });
  balancers_[0]->ads_service()->SetEdsResource(
      BuildEdsResource(args, DefaultEdsServiceName()));
  std::tie(num_ok, num_failure, num_drops) =
      WaitForAllBackends( kNumBackendsFirstPass);
  num_started += num_ok + num_failure + num_drops;
  CheckRpcSendOk(kNumBackendsSecondPass);
  num_started += kNumBackendsSecondPass;
  load_report = balancers_[0]->lrs_service()->WaitForLoadReport();
  ASSERT_EQ(load_report.size(), 1UL);
  client_stats = std::move(load_report.front());
  EXPECT_EQ(num_started, client_stats.total_successful_requests());
  EXPECT_EQ(0U, client_stats.total_requests_in_progress());
  EXPECT_EQ(0U, client_stats.total_error_requests());
  EXPECT_EQ(0U, client_stats.total_dropped_requests());
}
class ClientLoadReportingWithDropTest : public XdsEnd2endTest {
 public:
  ClientLoadReportingWithDropTest() : XdsEnd2endTest(4, 1, 20) {}
};
TEST_P(ClientLoadReportingWithDropTest, Vanilla) {
  if (!GetParam().use_xds_resolver()) {
    balancers_[0]->lrs_service()->set_cluster_names({kServerName});
  }
  SetNextResolution({});
  SetNextResolutionForLbChannelAllBalancers();
  const size_t kNumRpcs = 3000;
  const uint32_t kDropPerMillionForLb = 100000;
  const uint32_t kDropPerMillionForThrottle = 200000;
  const double kDropRateForLb = kDropPerMillionForLb / 1000000.0;
  const double kDropRateForThrottle = kDropPerMillionForThrottle / 1000000.0;
  const double KDropRateForLbAndThrottle =
      kDropRateForLb + (1 - kDropRateForLb) * kDropRateForThrottle;
  AdsServiceImpl::EdsResourceArgs args({
      {"locality0", GetBackendPorts()},
  });
  args.drop_categories = {{kLbDropType, kDropPerMillionForLb},
                          {kThrottleDropType, kDropPerMillionForThrottle}};
  balancers_[0]->ads_service()->SetEdsResource(
      BuildEdsResource(args, DefaultEdsServiceName()));
  int num_ok = 0;
  int num_failure = 0;
  int num_drops = 0;
  std::tie(num_ok, num_failure, num_drops) = WaitForAllBackends();
  const size_t num_warmup = num_ok + num_failure + num_drops;
  for (size_t i = 0; i < kNumRpcs; ++i) {
    EchoResponse response;
    const Status status = SendRpc(RpcOptions(), &response);
    if (!status.ok() &&
        status.error_message() == "Call dropped by load balancing policy") {
      ++num_drops;
    } else {
      EXPECT_TRUE(status.ok()) << "code=" << status.error_code()
                               << " message=" << status.error_message();
      EXPECT_EQ(response.message(), kRequestMessage);
    }
  }
  const double seen_drop_rate = static_cast<double>(num_drops) / kNumRpcs;
  const double kErrorTolerance = 0.2;
  EXPECT_THAT(
      seen_drop_rate,
      ::testing::AllOf(
          ::testing::Ge(KDropRateForLbAndThrottle * (1 - kErrorTolerance)),
          ::testing::Le(KDropRateForLbAndThrottle * (1 + kErrorTolerance))));
  const size_t total_rpc = num_warmup + kNumRpcs;
  ClientStats client_stats;
  do {
    std::vector<ClientStats> load_reports =
        balancers_[0]->lrs_service()->WaitForLoadReport();
    for (const auto& load_report : load_reports) {
      client_stats += load_report;
    }
  } while (client_stats.total_issued_requests() +
               client_stats.total_dropped_requests() <
           total_rpc);
  EXPECT_EQ(num_drops, client_stats.total_dropped_requests());
  EXPECT_THAT(
      client_stats.dropped_requests(kLbDropType),
      ::testing::AllOf(
          ::testing::Ge(total_rpc * kDropRateForLb * (1 - kErrorTolerance)),
          ::testing::Le(total_rpc * kDropRateForLb * (1 + kErrorTolerance))));
  EXPECT_THAT(client_stats.dropped_requests(kThrottleDropType),
              ::testing::AllOf(
                  ::testing::Ge(total_rpc * (1 - kDropRateForLb) *
                                kDropRateForThrottle * (1 - kErrorTolerance)),
                  ::testing::Le(total_rpc * (1 - kDropRateForLb) *
                                kDropRateForThrottle * (1 + kErrorTolerance))));
}
std::string TestTypeName(const ::testing::TestParamInfo<TestType>& info) {
  return info.param.AsString();
}
INSTANTIATE_TEST_SUITE_P(XdsTest, BasicTest,
                         ::testing::Values(TestType(false, true),
                                           TestType(false, false),
                                           TestType(true, false),
                                           TestType(true, true)),
                         &TestTypeName);
INSTANTIATE_TEST_SUITE_P(XdsTest, SecureNamingTest,
                         ::testing::Values(TestType(false, false),
                                           TestType(true, false)),
                         &TestTypeName);
INSTANTIATE_TEST_SUITE_P(XdsTest, LdsTest,
                         ::testing::Values(TestType(true, false),
                                           TestType(true, true)),
                         &TestTypeName);
INSTANTIATE_TEST_SUITE_P(XdsTest, LdsRdsTest,
                         ::testing::Values(TestType(true, false),
                                           TestType(true, true),
                                           TestType(true, false, true),
                                           TestType(true, true, true),
                                           TestType(true, true, true, true)),
                         &TestTypeName);
INSTANTIATE_TEST_SUITE_P(XdsTest, CdsTest,
                         ::testing::Values(TestType(true, false),
                                           TestType(true, true)),
                         &TestTypeName);
INSTANTIATE_TEST_SUITE_P(XdsTest, EdsTest,
                         ::testing::Values(TestType(true, false),
                                           TestType(true, true)),
                         &TestTypeName);
INSTANTIATE_TEST_SUITE_P(XdsTest, TimeoutTest,
                         ::testing::Values(TestType(true, false, true)),
                         &TestTypeName);
INSTANTIATE_TEST_SUITE_P(XdsTest, XdsResolverOnlyTest,
                         ::testing::Values(TestType(true, false),
                                           TestType(true, true)),
                         &TestTypeName);
INSTANTIATE_TEST_SUITE_P(XdsTest, XdsResolverLoadReportingOnlyTest,
                         ::testing::Values(TestType(true, true)),
                         &TestTypeName);
INSTANTIATE_TEST_SUITE_P(XdsTest, LocalityMapTest,
                         ::testing::Values(TestType(false, true),
                                           TestType(false, false),
                                           TestType(true, false),
                                           TestType(true, true)),
                         &TestTypeName);
INSTANTIATE_TEST_SUITE_P(XdsTest, FailoverTest,
                         ::testing::Values(TestType(false, true),
                                           TestType(false, false),
                                           TestType(true, false),
                                           TestType(true, true)),
                         &TestTypeName);
INSTANTIATE_TEST_SUITE_P(XdsTest, DropTest,
                         ::testing::Values(TestType(false, true),
                                           TestType(false, false),
                                           TestType(true, false),
                                           TestType(true, true)),
                         &TestTypeName);
INSTANTIATE_TEST_SUITE_P(XdsTest, BalancerUpdateTest,
                         ::testing::Values(TestType(false, true),
                                           TestType(false, false),
                                           TestType(true, true)),
                         &TestTypeName);
INSTANTIATE_TEST_SUITE_P(XdsTest, ClientLoadReportingTest,
                         ::testing::Values(TestType(false, true),
                                           TestType(true, true)),
                         &TestTypeName);
INSTANTIATE_TEST_SUITE_P(XdsTest, ClientLoadReportingWithDropTest,
                         ::testing::Values(TestType(false, true),
                                           TestType(true, true)),
                         &TestTypeName);
}
}
}
int main(int argc, char** argv) {
  grpc::testing::TestEnvironment env(argc, argv);
  ::testing::InitGoogleTest(&argc, argv);
  grpc::testing::WriteBootstrapFiles();
  grpc::testing::g_port_saver = new grpc::testing::PortSaver();
  const auto result = RUN_ALL_TESTS();
  return result;
}
