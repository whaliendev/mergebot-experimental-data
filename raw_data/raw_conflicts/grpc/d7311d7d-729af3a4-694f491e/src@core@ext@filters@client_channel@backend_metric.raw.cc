//
// Copyright 2019 gRPC authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include <grpc/support/port_platform.h>

#include "src/core/ext/filters/client_channel/backend_metric.h"

#include "absl/strings/string_view.h"

#include "udpa/data/orca/v1/orca_load_report.upb.h"

#include "src/core/lib/gprpp/map.h"

namespace grpc_core {

namespace {

template <typename EntryType>
std::map<absl::string_view, double, StringLess> ParseMap(
    udpa_data_orca_v1_OrcaLoadReport* msg,
    const EntryType* (*entry_func)(const udpa_data_orca_v1_OrcaLoadReport*,
                                   size_t*),
    upb_strview (*key_func)(const EntryType*),
    double (*value_func)(const EntryType*), Arena* arena) {
<<<<<<< HEAD
  std::map<StringView, double, StringLess> result;
  size_t i = UPB_MAP_BEGIN;
  while (true) {
    const auto* entry = entry_func(msg, &i);
    if (entry == nullptr) break;
    upb_strview key_view = key_func(entry);
||||||| 694f491e06
  std::map<StringView, double, StringLess> result;
  size_t size;
  const auto* const* entries = entry_func(msg, &size);
  for (size_t i = 0; i < size; ++i) {
    upb_strview key_view = key_func(entries[i]);
=======
  std::map<absl::string_view, double, StringLess> result;
  size_t size;
  const auto* const* entries = entry_func(msg, &size);
  for (size_t i = 0; i < size; ++i) {
    upb_strview key_view = key_func(entries[i]);
>>>>>>> 729af3a4
    char* key = static_cast<char*>(arena->Alloc(key_view.size + 1));
    memcpy(key, key_view.data, key_view.size);
<<<<<<< HEAD
    result[StringView(key, key_view.size)] = value_func(entry);
||||||| 694f491e06
    result[StringView(key, key_view.size)] = value_func(entries[i]);
=======
    result[absl::string_view(key, key_view.size)] = value_func(entries[i]);
>>>>>>> 729af3a4
  }
  return result;
}

}  // namespace

const LoadBalancingPolicy::BackendMetricData* ParseBackendMetricData(
    const grpc_slice& serialized_load_report, Arena* arena) {
  upb::Arena upb_arena;
  udpa_data_orca_v1_OrcaLoadReport* msg =
      udpa_data_orca_v1_OrcaLoadReport_parse(
          reinterpret_cast<const char*>(
              GRPC_SLICE_START_PTR(serialized_load_report)),
          GRPC_SLICE_LENGTH(serialized_load_report), upb_arena.ptr());
  if (msg == nullptr) return nullptr;
  LoadBalancingPolicy::BackendMetricData* backend_metric_data =
      arena->New<LoadBalancingPolicy::BackendMetricData>();
  backend_metric_data->cpu_utilization =
      udpa_data_orca_v1_OrcaLoadReport_cpu_utilization(msg);
  backend_metric_data->mem_utilization =
      udpa_data_orca_v1_OrcaLoadReport_mem_utilization(msg);
  backend_metric_data->requests_per_second =
      udpa_data_orca_v1_OrcaLoadReport_rps(msg);
  backend_metric_data->request_cost =
      ParseMap<udpa_data_orca_v1_OrcaLoadReport_RequestCostEntry>(
          msg, udpa_data_orca_v1_OrcaLoadReport_request_cost_next,
          udpa_data_orca_v1_OrcaLoadReport_RequestCostEntry_key,
          udpa_data_orca_v1_OrcaLoadReport_RequestCostEntry_value, arena);
  backend_metric_data->utilization =
      ParseMap<udpa_data_orca_v1_OrcaLoadReport_UtilizationEntry>(
          msg, udpa_data_orca_v1_OrcaLoadReport_utilization_next,
          udpa_data_orca_v1_OrcaLoadReport_UtilizationEntry_key,
          udpa_data_orca_v1_OrcaLoadReport_UtilizationEntry_value, arena);
  return backend_metric_data;
}

}  // namespace grpc_core
