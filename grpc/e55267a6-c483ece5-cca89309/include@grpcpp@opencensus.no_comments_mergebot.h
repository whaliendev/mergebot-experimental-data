#ifndef GRPCPP_OPENCENSUS_H
#define GRPCPP_OPENCENSUS_H 
#include "opencensus/trace/span.h"
#include "grpcpp/opencensus_impl.h"
namespace grpc_impl {
class ServerContext;
}
namespace grpc {
class ServerContext;
void RegisterOpenCensusPlugin();
void RegisterOpenCensusViewsForExport();
::opencensus::trace::Span GetSpanFromServerContext(ServerContext* context);
::opencensus::trace::Span GetSpanFromServerContext(
    ::grpc_impl::ServerContext* context);
}
#endif
