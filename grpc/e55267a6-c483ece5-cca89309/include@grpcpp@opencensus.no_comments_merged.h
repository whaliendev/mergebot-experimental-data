#ifndef GRPCPP_OPENCENSUS_H
#define GRPCPP_OPENCENSUS_H 
#include "opencensus/trace/span.h"
namespace grpc_impl {
class ServerContext;
}
namespace grpc {
void RegisterOpenCensusPlugin();
void RegisterOpenCensusViewsForExport();
::opencensus::trace::Span GetSpanFromServerContext(
    ::grpc_impl::ServerContext* context);
}
#endif
