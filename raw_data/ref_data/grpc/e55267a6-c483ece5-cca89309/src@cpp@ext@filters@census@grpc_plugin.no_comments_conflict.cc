#include <grpc/support/port_platform.h>
#include "src/cpp/ext/filters/census/grpc_plugin.h"
#include <grpcpp/server_context.h>
#include "opencensus/tags/tag_key.h"
#include "opencensus/trace/span.h"
#include "src/cpp/ext/filters/census/channel_filter.h"
#include "src/cpp/ext/filters/census/client_filter.h"
#include "src/cpp/ext/filters/census/measures.h"
#include "src/cpp/ext/filters/census/server_filter.h"
namespace grpc {
<<<<<<< HEAD
void RegisterOpenCensusPlugin() {
  RegisterChannelFilter<grpc::CensusChannelData,
                              grpc::CensusClientCallData>(
      "opencensus_client", GRPC_CLIENT_CHANNEL, INT_MAX ,
      nullptr );
  RegisterChannelFilter<grpc::CensusChannelData,
                              grpc::CensusServerCallData>(
      "opencensus_server", GRPC_SERVER_CHANNEL, INT_MAX ,
      nullptr );
  RpcClientSentBytesPerRpc();
  RpcClientReceivedBytesPerRpc();
  RpcClientRoundtripLatency();
  RpcClientServerLatency();
  RpcClientSentMessagesPerRpc();
  RpcClientReceivedMessagesPerRpc();
  RpcServerSentBytesPerRpc();
  RpcServerReceivedBytesPerRpc();
  RpcServerServerLatency();
  RpcServerSentMessagesPerRpc();
  RpcServerReceivedMessagesPerRpc();
}
::opencensus::trace::Span GetSpanFromServerContext(
    grpc::ServerContext* context) {
  return reinterpret_cast<const grpc::CensusContext*>(context->census_context())
      ->Span();
}
||||||| cca89309a2
=======
void RegisterOpenCensusPlugin() {
  RegisterChannelFilter<CensusChannelData, CensusClientCallData>(
      "opencensus_client", GRPC_CLIENT_CHANNEL, INT_MAX ,
      nullptr );
  RegisterChannelFilter<CensusChannelData, CensusServerCallData>(
      "opencensus_server", GRPC_SERVER_CHANNEL, INT_MAX ,
      nullptr );
  RpcClientSentBytesPerRpc();
  RpcClientReceivedBytesPerRpc();
  RpcClientRoundtripLatency();
  RpcClientServerLatency();
  RpcClientSentMessagesPerRpc();
  RpcClientReceivedMessagesPerRpc();
  RpcServerSentBytesPerRpc();
  RpcServerReceivedBytesPerRpc();
  RpcServerServerLatency();
  RpcServerSentMessagesPerRpc();
  RpcServerReceivedMessagesPerRpc();
}
::opencensus::trace::Span GetSpanFromServerContext(ServerContext* context) {
  return reinterpret_cast<const CensusContext*>(context->census_context())
      ->Span();
}
>>>>>>> c483ece5
::opencensus::tags::TagKey ClientMethodTagKey() {
  static const auto method_tag_key =
      ::opencensus::tags::TagKey::Register("grpc_client_method");
  return method_tag_key;
}
::opencensus::tags::TagKey ClientStatusTagKey() {
  static const auto status_tag_key =
      ::opencensus::tags::TagKey::Register("grpc_client_status");
  return status_tag_key;
}
::opencensus::tags::TagKey ServerMethodTagKey() {
  static const auto method_tag_key =
      ::opencensus::tags::TagKey::Register("grpc_server_method");
  return method_tag_key;
}
::opencensus::tags::TagKey ServerStatusTagKey() {
  static const auto status_tag_key =
      ::opencensus::tags::TagKey::Register("grpc_server_status");
  return status_tag_key;
}
ABSL_CONST_INIT const absl::string_view
    kRpcClientSentMessagesPerRpcMeasureName =
        "grpc.io/client/sent_messages_per_rpc";
ABSL_CONST_INIT const absl::string_view kRpcClientSentBytesPerRpcMeasureName =
    "grpc.io/client/sent_bytes_per_rpc";
ABSL_CONST_INIT const absl::string_view
    kRpcClientReceivedMessagesPerRpcMeasureName =
        "grpc.io/client/received_messages_per_rpc";
ABSL_CONST_INIT const absl::string_view
    kRpcClientReceivedBytesPerRpcMeasureName =
        "grpc.io/client/received_bytes_per_rpc";
ABSL_CONST_INIT const absl::string_view kRpcClientRoundtripLatencyMeasureName =
    "grpc.io/client/roundtrip_latency";
ABSL_CONST_INIT const absl::string_view kRpcClientServerLatencyMeasureName =
    "grpc.io/client/server_latency";
ABSL_CONST_INIT const absl::string_view
    kRpcServerSentMessagesPerRpcMeasureName =
        "grpc.io/server/sent_messages_per_rpc";
ABSL_CONST_INIT const absl::string_view kRpcServerSentBytesPerRpcMeasureName =
    "grpc.io/server/sent_bytes_per_rpc";
ABSL_CONST_INIT const absl::string_view
    kRpcServerReceivedMessagesPerRpcMeasureName =
        "grpc.io/server/received_messages_per_rpc";
ABSL_CONST_INIT const absl::string_view
    kRpcServerReceivedBytesPerRpcMeasureName =
        "grpc.io/server/received_bytes_per_rpc";
ABSL_CONST_INIT const absl::string_view kRpcServerServerLatencyMeasureName =
    "grpc.io/server/server_latency";
}
