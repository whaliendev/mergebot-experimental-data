/*
 *
 * Copyright 2018 gRPC authors.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#ifndef GRPC_CORE_EXT_FILTERS_CLIENT_CHANNEL_XDS_XDS_CLIENT_STATS_H
#define GRPC_CORE_EXT_FILTERS_CLIENT_CHANNEL_XDS_XDS_CLIENT_STATS_H

#include <grpc/support/port_platform.h>

#include <grpc/support/string_util.h>

#include "src/core/lib/gprpp/atomic.h"
#include "src/core/lib/gprpp/inlined_vector.h"
#include "src/core/lib/gprpp/map.h"
#include "src/core/lib/gprpp/memory.h"
#include "src/core/lib/gprpp/ref_counted.h"
#include "src/core/lib/gprpp/sync.h"
#include "src/core/lib/iomgr/exec_ctx.h"

namespace grpc_core {

// Forward declaration to avoid circular dependency.
class XdsClient;

// Locality name.
class XdsLocalityName : public RefCounted<XdsLocalityName> {
 public:
  struct Less {
    bool operator()(const XdsLocalityName* lhs,
                    const XdsLocalityName* rhs) const {
      int cmp_result = lhs->region_.compare(rhs->region_);
      if (cmp_result != 0) return cmp_result < 0;
      cmp_result = lhs->zone_.compare(rhs->zone_);
      if (cmp_result != 0) return cmp_result < 0;
      return lhs->sub_zone_.compare(rhs->sub_zone_) < 0;
    }

    bool operator()(const RefCountedPtr<XdsLocalityName>& lhs,
                    const RefCountedPtr<XdsLocalityName>& rhs) const {
      return (*this)(lhs.get(), rhs.get());
    }
  };

  XdsLocalityName(std::string region, std::string zone, std::string subzone)
      : region_(std::move(region)),
        zone_(std::move(zone)),
        sub_zone_(std::move(subzone)) {}

  bool operator==(const XdsLocalityName& other) const {
    return region_ == other.region_ && zone_ == other.zone_ &&
           sub_zone_ == other.sub_zone_;
  }

  const std::string& region() const { return region_; }
  const std::string& zone() const { return zone_; }
  const std::string& sub_zone() const { return sub_zone_; }

  const char* AsHumanReadableString() {
    if (human_readable_string_ == nullptr) {
      char* tmp;
      gpr_asprintf(&tmp, "{region=\"%s\", zone=\"%s\", sub_zone=\"%s\"}",
                   region_.c_str(), zone_.c_str(), sub_zone_.c_str());
      human_readable_string_.reset(tmp);
    }
    return human_readable_string_.get();
  }

 private:
  std::string region_;
  std::string zone_;
  std::string sub_zone_;
  UniquePtr<char> human_readable_string_;
};

// Drop stats for an xds cluster.
class XdsClusterDropStats : public RefCounted<XdsClusterDropStats> {
 public:
  using DroppedRequestsMap = std::map<std::string /* category */, uint64_t>;

  XdsClusterDropStats(RefCountedPtr<XdsClient> xds_client,
                      StringView lrs_server_name, StringView cluster_name,
                      StringView eds_service_name);
  ~XdsClusterDropStats();

  // Returns a snapshot of this instance and resets all the counters.
  DroppedRequestsMap GetSnapshotAndReset();

<<<<<<< HEAD
     private:
      uint64_t num_requests_finished_with_metric_{0};
      double total_metric_value_{0};
    };

    using LoadMetricMap = std::map<std::string, LoadMetric>;
    using LoadMetricSnapshotMap = std::map<std::string, LoadMetric::Snapshot>;

    struct Snapshot {
      // TODO(juanlishen): Change this to const method when const_iterator is
      // added to Map<>.
      bool IsAllZero();

      uint64_t total_successful_requests;
      uint64_t total_requests_in_progress;
      uint64_t total_error_requests;
      uint64_t total_issued_requests;
      LoadMetricSnapshotMap load_metric_stats;
    };

    // Returns a snapshot of this instance and reset all the accumulative
    // counters.
    Snapshot GetSnapshotAndReset();

    // Each XdsLb::PickerWrapper holds a ref to the perspective LocalityStats.
    // If the refcount is 0, there won't be new calls recorded to the
    // LocalityStats, so the LocalityStats can be safely deleted when all the
    // in-progress calls have finished.
    // Only be called from the control plane work_serializer.
    void RefByPicker() { picker_refcount_.FetchAdd(1, MemoryOrder::ACQ_REL); }
    // Might be called from the control plane work_serializer or the data plane
    // mutex.
    // TODO(juanlishen): Once https://github.com/grpc/grpc/pull/19390 is merged,
    //  this method will also only be invoked in the control plane
    //  work_serializer. We may then be able to simplify the LocalityStats'
    //  lifetime by making it RefCounted<> and populating the protobuf in its
    //  dtor.
    void UnrefByPicker() { picker_refcount_.FetchSub(1, MemoryOrder::ACQ_REL); }
    // Only be called from the control plane work_serializer.
    // The only place where the picker_refcount_ can be increased is
    // RefByPicker(), which also can only be called from the control plane
    // work_serializer. Also, if the picker_refcount_ is 0,
    // total_requests_in_progress_ can't be increased from 0. So it's safe to
    // delete the LocalityStats right after this method returns true.
    bool IsSafeToDelete() {
      return picker_refcount_.FetchAdd(0, MemoryOrder::ACQ_REL) == 0 &&
             total_requests_in_progress_.FetchAdd(0, MemoryOrder::ACQ_REL) == 0;
    }

    void AddCallStarted();
    void AddCallFinished(bool fail = false);

   private:
    Atomic<uint64_t> total_successful_requests_{0};
    Atomic<uint64_t> total_requests_in_progress_{0};
    // Requests that were issued (not dropped) but failed.
    Atomic<uint64_t> total_error_requests_{0};
    Atomic<uint64_t> total_issued_requests_{0};
    // Protects load_metric_stats_. A mutex is necessary because the length of
    // load_metric_stats_ can be accessed by both the callback intercepting the
    // call's recv_trailing_metadata (not from any work_serializer) and the load
    // reporting thread (from the control plane work_serializer).
    Mutex load_metric_stats_mu_;
    LoadMetricMap load_metric_stats_;
    // Can be accessed from either the control plane work_serializer or the data
    // plane mutex.
    Atomic<uint8_t> picker_refcount_{0};
  };

  // TODO(juanlishen): The value type of Map<> must be movable in current
  // implementation. To avoid making LocalityStats movable, we wrap it by
  // std::unique_ptr<>. We should remove this wrapper if the value type of Map<>
  // doesn't have to be movable.
  using LocalityStatsMap =
      std::map<RefCountedPtr<XdsLocalityName>, RefCountedPtr<LocalityStats>,
               XdsLocalityName::Less>;
  using LocalityStatsSnapshotMap =
      std::map<RefCountedPtr<XdsLocalityName>, LocalityStats::Snapshot,
               XdsLocalityName::Less>;
  using DroppedRequestsMap = std::map<std::string, uint64_t>;
  using DroppedRequestsSnapshotMap = DroppedRequestsMap;

  struct Snapshot {
    // TODO(juanlishen): Change this to const method when const_iterator is
    // added to Map<>.
    bool IsAllZero();

    LocalityStatsSnapshotMap upstream_locality_stats;
    uint64_t total_dropped_requests;
    DroppedRequestsSnapshotMap dropped_requests;
    // The actual load report interval.
    grpc_millis load_report_interval;
  };

  // Returns a snapshot of this instance and reset all the accumulative
  // counters.
  Snapshot GetSnapshotAndReset();

  void MaybeInitLastReportTime();
  RefCountedPtr<LocalityStats> FindLocalityStats(
      const RefCountedPtr<XdsLocalityName>& locality_name);
  void PruneLocalityStats();
||||||| 660e5360f7
     private:
      uint64_t num_requests_finished_with_metric_{0};
      double total_metric_value_{0};
    };

    using LoadMetricMap = std::map<std::string, LoadMetric>;
    using LoadMetricSnapshotMap = std::map<std::string, LoadMetric::Snapshot>;

    struct Snapshot {
      // TODO(juanlishen): Change this to const method when const_iterator is
      // added to Map<>.
      bool IsAllZero();

      uint64_t total_successful_requests;
      uint64_t total_requests_in_progress;
      uint64_t total_error_requests;
      uint64_t total_issued_requests;
      LoadMetricSnapshotMap load_metric_stats;
    };

    // Returns a snapshot of this instance and reset all the accumulative
    // counters.
    Snapshot GetSnapshotAndReset();

    // Each XdsLb::PickerWrapper holds a ref to the perspective LocalityStats.
    // If the refcount is 0, there won't be new calls recorded to the
    // LocalityStats, so the LocalityStats can be safely deleted when all the
    // in-progress calls have finished.
    // Only be called from the control plane combiner.
    void RefByPicker() { picker_refcount_.FetchAdd(1, MemoryOrder::ACQ_REL); }
    // Might be called from the control plane combiner or the data plane
    // combiner.
    // TODO(juanlishen): Once https://github.com/grpc/grpc/pull/19390 is merged,
    //  this method will also only be invoked in the control plane combiner.
    //  We may then be able to simplify the LocalityStats' lifetime by making it
    //  RefCounted<> and populating the protobuf in its dtor.
    void UnrefByPicker() { picker_refcount_.FetchSub(1, MemoryOrder::ACQ_REL); }
    // Only be called from the control plane combiner.
    // The only place where the picker_refcount_ can be increased is
    // RefByPicker(), which also can only be called from the control plane
    // combiner. Also, if the picker_refcount_ is 0, total_requests_in_progress_
    // can't be increased from 0. So it's safe to delete the LocalityStats right
    // after this method returns true.
    bool IsSafeToDelete() {
      return picker_refcount_.FetchAdd(0, MemoryOrder::ACQ_REL) == 0 &&
             total_requests_in_progress_.FetchAdd(0, MemoryOrder::ACQ_REL) == 0;
    }

    void AddCallStarted();
    void AddCallFinished(bool fail = false);

   private:
    Atomic<uint64_t> total_successful_requests_{0};
    Atomic<uint64_t> total_requests_in_progress_{0};
    // Requests that were issued (not dropped) but failed.
    Atomic<uint64_t> total_error_requests_{0};
    Atomic<uint64_t> total_issued_requests_{0};
    // Protects load_metric_stats_. A mutex is necessary because the length of
    // load_metric_stats_ can be accessed by both the callback intercepting the
    // call's recv_trailing_metadata (not from any combiner) and the load
    // reporting thread (from the control plane combiner).
    Mutex load_metric_stats_mu_;
    LoadMetricMap load_metric_stats_;
    // Can be accessed from either the control plane combiner or the data plane
    // combiner.
    Atomic<uint8_t> picker_refcount_{0};
  };

  // TODO(juanlishen): The value type of Map<> must be movable in current
  // implementation. To avoid making LocalityStats movable, we wrap it by
  // std::unique_ptr<>. We should remove this wrapper if the value type of Map<>
  // doesn't have to be movable.
  using LocalityStatsMap =
      std::map<RefCountedPtr<XdsLocalityName>, RefCountedPtr<LocalityStats>,
               XdsLocalityName::Less>;
  using LocalityStatsSnapshotMap =
      std::map<RefCountedPtr<XdsLocalityName>, LocalityStats::Snapshot,
               XdsLocalityName::Less>;
  using DroppedRequestsMap = std::map<std::string, uint64_t>;
  using DroppedRequestsSnapshotMap = DroppedRequestsMap;

  struct Snapshot {
    // TODO(juanlishen): Change this to const method when const_iterator is
    // added to Map<>.
    bool IsAllZero();

    LocalityStatsSnapshotMap upstream_locality_stats;
    uint64_t total_dropped_requests;
    DroppedRequestsSnapshotMap dropped_requests;
    // The actual load report interval.
    grpc_millis load_report_interval;
  };

  // Returns a snapshot of this instance and reset all the accumulative
  // counters.
  Snapshot GetSnapshotAndReset();

  void MaybeInitLastReportTime();
  RefCountedPtr<LocalityStats> FindLocalityStats(
      const RefCountedPtr<XdsLocalityName>& locality_name);
  void PruneLocalityStats();
=======
>>>>>>> 0b689cb6
  void AddCallDropped(const std::string& category);

 private:
  RefCountedPtr<XdsClient> xds_client_;
  StringView lrs_server_name_;
  StringView cluster_name_;
  StringView eds_service_name_;
  // Protects dropped_requests_. A mutex is necessary because the length of
  // dropped_requests_ can be accessed by both the picker (from data plane
<<<<<<< HEAD
  // mutex) and the load reporting thread (from the control plane
  // work_serializer).
  Mutex dropped_requests_mu_;
||||||| 660e5360f7
  // combiner) and the load reporting thread (from the control plane combiner).
  Mutex dropped_requests_mu_;
=======
  // mutex) and the load reporting thread (from the control plane combiner).
  Mutex mu_;
>>>>>>> 0b689cb6
  DroppedRequestsMap dropped_requests_;
};

// Locality stats for an xds cluster.
class XdsClusterLocalityStats : public RefCounted<XdsClusterLocalityStats> {
 public:
  struct BackendMetric {
    uint64_t num_requests_finished_with_metric;
    double total_metric_value;

    BackendMetric& operator+=(const BackendMetric& other) {
      num_requests_finished_with_metric +=
          other.num_requests_finished_with_metric;
      total_metric_value += other.total_metric_value;
      return *this;
    }

    bool IsZero() const {
      return num_requests_finished_with_metric == 0 && total_metric_value == 0;
    }
  };

  struct Snapshot {
    uint64_t total_successful_requests;
    uint64_t total_requests_in_progress;
    uint64_t total_error_requests;
    uint64_t total_issued_requests;
    std::map<std::string, BackendMetric> backend_metrics;

    Snapshot& operator+=(const Snapshot& other) {
      total_successful_requests += other.total_successful_requests;
      total_requests_in_progress += other.total_requests_in_progress;
      total_error_requests += other.total_error_requests;
      total_issued_requests += other.total_issued_requests;
      for (const auto& p : other.backend_metrics) {
        backend_metrics[p.first] += p.second;
      }
      return *this;
    }

    bool IsZero() const {
      if (total_successful_requests != 0 || total_requests_in_progress != 0 ||
          total_error_requests != 0 || total_issued_requests != 0) {
        return false;
      }
      for (const auto& p : backend_metrics) {
        if (!p.second.IsZero()) return false;
      }
      return true;
    }
  };

  XdsClusterLocalityStats(RefCountedPtr<XdsClient> xds_client,
                          StringView lrs_server_name, StringView cluster_name,
                          StringView eds_service_name,
                          RefCountedPtr<XdsLocalityName> name);
  ~XdsClusterLocalityStats();

  // Returns a snapshot of this instance and resets all the counters.
  Snapshot GetSnapshotAndReset();

  void AddCallStarted();
  void AddCallFinished(bool fail = false);

 private:
  RefCountedPtr<XdsClient> xds_client_;
  StringView lrs_server_name_;
  StringView cluster_name_;
  StringView eds_service_name_;
  RefCountedPtr<XdsLocalityName> name_;

  Atomic<uint64_t> total_successful_requests_{0};
  Atomic<uint64_t> total_requests_in_progress_{0};
  Atomic<uint64_t> total_error_requests_{0};
  Atomic<uint64_t> total_issued_requests_{0};

  // Protects backend_metrics_. A mutex is necessary because the length of
  // backend_metrics_ can be accessed by both the callback intercepting the
  // call's recv_trailing_metadata (not from the control plane combiner) and
  // the load reporting thread (from the control plane combiner).
  Mutex backend_metrics_mu_;
  std::map<std::string, BackendMetric> backend_metrics_;
};

}  // namespace grpc_core

#endif /* GRPC_CORE_EXT_FILTERS_CLIENT_CHANNEL_XDS_XDS_CLIENT_STATS_H */
