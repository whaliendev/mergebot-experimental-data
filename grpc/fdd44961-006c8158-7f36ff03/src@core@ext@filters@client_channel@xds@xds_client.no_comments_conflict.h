#ifndef GRPC_CORE_EXT_FILTERS_CLIENT_CHANNEL_XDS_XDS_CLIENT_H
#define GRPC_CORE_EXT_FILTERS_CLIENT_CHANNEL_XDS_XDS_CLIENT_H 
#include <grpc/support/port_platform.h>
#include <set>
#include "absl/types/optional.h"
#include "src/core/ext/filters/client_channel/service_config.h"
#include "src/core/ext/filters/client_channel/xds/xds_api.h"
#include "src/core/ext/filters/client_channel/xds/xds_bootstrap.h"
#include "src/core/ext/filters/client_channel/xds/xds_client_stats.h"
#include "src/core/lib/gprpp/map.h"
#include "src/core/lib/gprpp/memory.h"
#include "src/core/lib/gprpp/orphanable.h"
#include "src/core/lib/gprpp/ref_counted.h"
#include "src/core/lib/gprpp/ref_counted_ptr.h"
#include "src/core/lib/gprpp/string_view.h"
#include "src/core/lib/iomgr/work_serializer.h"
namespace grpc_core {
extern TraceFlag xds_client_trace;
class XdsClient : public InternallyRefCounted<XdsClient> {
 public:
  class ServiceConfigWatcherInterface {
   public:
    virtual ~ServiceConfigWatcherInterface() = default;
    virtual void OnServiceConfigChanged(
        RefCountedPtr<ServiceConfig> service_config) = 0;
    virtual void OnError(grpc_error* error) = 0;
  };
  class ClusterWatcherInterface {
   public:
    virtual ~ClusterWatcherInterface() = default;
    virtual void OnClusterChanged(XdsApi::CdsUpdate cluster_data) = 0;
    virtual void OnError(grpc_error* error) = 0;
  };
  class EndpointWatcherInterface {
   public:
    virtual ~EndpointWatcherInterface() = default;
    virtual void OnEndpointChanged(XdsApi::EdsUpdate update) = 0;
    virtual void OnError(grpc_error* error) = 0;
  };
  XdsClient(std::shared_ptr<WorkSerializer> work_serializer,
            grpc_pollset_set* interested_parties, StringView server_name,
            std::unique_ptr<ServiceConfigWatcherInterface> watcher,
            const grpc_channel_args& channel_args, grpc_error** error);
  ~XdsClient();
  void Orphan() override;
  void WatchClusterData(StringView cluster_name,
                        std::unique_ptr<ClusterWatcherInterface> watcher);
  void CancelClusterDataWatch(StringView cluster_name,
                              ClusterWatcherInterface* watcher,
                              bool delay_unsubscription = false);
  void WatchEndpointData(StringView eds_service_name,
                         std::unique_ptr<EndpointWatcherInterface> watcher);
  void CancelEndpointDataWatch(StringView eds_service_name,
                               EndpointWatcherInterface* watcher,
                               bool delay_unsubscription = false);
  RefCountedPtr<XdsClusterDropStats> AddClusterDropStats(
      StringView lrs_server, StringView cluster_name,
      StringView eds_service_name);
  void RemoveClusterDropStats(StringView ,
                              StringView cluster_name,
                              StringView eds_service_name,
                              XdsClusterDropStats* cluster_drop_stats);
  RefCountedPtr<XdsClusterLocalityStats> AddClusterLocalityStats(
      StringView lrs_server, StringView cluster_name,
      StringView eds_service_name, RefCountedPtr<XdsLocalityName> locality);
  void RemoveClusterLocalityStats(
      StringView , StringView cluster_name,
      StringView eds_service_name,
      const RefCountedPtr<XdsLocalityName>& locality,
      XdsClusterLocalityStats* cluster_locality_stats);
  void ResetBackoff();
  grpc_arg MakeChannelArg() const;
  static RefCountedPtr<XdsClient> GetFromChannelArgs(
      const grpc_channel_args& args);
 private:
  class ChannelState : public InternallyRefCounted<ChannelState> {
   public:
    template <typename T>
    class RetryableCall;
    class AdsCallState;
    class LrsCallState;
    ChannelState(RefCountedPtr<XdsClient> xds_client, grpc_channel* channel);
    ~ChannelState();
    void Orphan() override;
    grpc_channel* channel() const { return channel_; }
    XdsClient* xds_client() const { return xds_client_.get(); }
    AdsCallState* ads_calld() const;
    LrsCallState* lrs_calld() const;
    void MaybeStartLrsCall();
    void StopLrsCall();
    bool HasActiveAdsCall() const;
    void StartConnectivityWatchLocked();
    void CancelConnectivityWatchLocked();
    void Subscribe(const std::string& type_url, const std::string& name);
    void Unsubscribe(const std::string& type_url, const std::string& name,
                     bool delay_unsubscription);
   private:
    class StateWatcher;
    RefCountedPtr<XdsClient> xds_client_;
    grpc_channel* channel_;
    bool shutting_down_ = false;
    StateWatcher* watcher_ = nullptr;
    OrphanablePtr<RetryableCall<AdsCallState>> ads_calld_;
    OrphanablePtr<RetryableCall<LrsCallState>> lrs_calld_;
  };
  struct ClusterState {
    std::map<ClusterWatcherInterface*, std::unique_ptr<ClusterWatcherInterface>>
        watchers;
    absl::optional<XdsApi::CdsUpdate> update;
  };
  struct EndpointState {
    std::map<EndpointWatcherInterface*,
             std::unique_ptr<EndpointWatcherInterface>>
        watchers;
    absl::optional<XdsApi::EdsUpdate> update;
  };
  struct LoadReportState {
    struct LocalityState {
      std::set<XdsClusterLocalityStats*> locality_stats;
      std::vector<XdsClusterLocalityStats::Snapshot> deleted_locality_stats;
    };
    std::set<XdsClusterDropStats*> drop_stats;
    XdsClusterDropStats::DroppedRequestsMap deleted_drop_stats;
    std::map<RefCountedPtr<XdsLocalityName>, LocalityState,
             XdsLocalityName::Less>
        locality_stats;
    grpc_millis last_report_time = ExecCtx::Get()->Now();
  };
  void NotifyOnError(grpc_error* error);
  grpc_error* CreateServiceConfig(
      const XdsApi::RdsUpdate& rds_update,
      RefCountedPtr<ServiceConfig>* service_config) const;
  XdsApi::ClusterLoadReportMap BuildLoadReportSnapshot(
      const std::set<std::string>& clusters);
  static void* ChannelArgCopy(void* p);
  static void ChannelArgDestroy(void* p);
  static int ChannelArgCmp(void* p, void* q);
  static const grpc_arg_pointer_vtable kXdsClientVtable;
  const grpc_millis request_timeout_;
<<<<<<< HEAD
  std::shared_ptr<WorkSerializer> work_serializer_;
||||||| 7f36ff039f
  Combiner* combiner_;
=======
  const bool xds_routing_enabled_;
  Combiner* combiner_;
>>>>>>> 006c8158
  grpc_pollset_set* interested_parties_;
  std::unique_ptr<XdsBootstrap> bootstrap_;
  XdsApi api_;
  const std::string server_name_;
  std::unique_ptr<ServiceConfigWatcherInterface> service_config_watcher_;
  OrphanablePtr<ChannelState> chand_;
  absl::optional<XdsApi::LdsUpdate> lds_result_;
  absl::optional<XdsApi::RdsUpdate> rds_result_;
  std::map<std::string , ClusterState> cluster_map_;
  std::map<std::string , EndpointState> endpoint_map_;
  std::map<
      std::pair<std::string , std::string >,
      LoadReportState>
      load_report_map_;
  bool shutting_down_ = false;
};
}
#endif
