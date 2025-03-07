#ifndef GRPCPP_OPENCENSUS_H
#define GRPCPP_OPENCENSUS_H 
<<<<<<< HEAD
#include "opencensus/trace/span.h"
||||||| cca89309a2
#include "grpcpp/opencensus_impl.h"
=======
#include "opencensus/trace/span.h"
namespace grpc_impl {
class ServerContext;
}
>>>>>>> c483ece5
namespace grpc {
<<<<<<< HEAD
class ServerContext;
||||||| cca89309a2
=======
>>>>>>> c483ece5
<<<<<<< HEAD
void RegisterOpenCensusPlugin();
void RegisterOpenCensusViewsForExport();
::opencensus::trace::Span GetSpanFromServerContext(ServerContext* context);
||||||| cca89309a2
static inline void RegisterOpenCensusPlugin() {
  ::grpc_impl::RegisterOpenCensusPlugin();
}
static inline void RegisterOpenCensusViewsForExport() {
  ::grpc_impl::RegisterOpenCensusViewsForExport();
}
static inline ::opencensus::trace::Span GetSpanFromServerContext(
    ::grpc_impl::ServerContext* context) {
  return ::grpc_impl::GetSpanFromServerContext(context);
}
=======
void RegisterOpenCensusPlugin();
void RegisterOpenCensusViewsForExport();
::opencensus::trace::Span GetSpanFromServerContext(
    ::grpc_impl::ServerContext* context);
>>>>>>> c483ece5
}
#endif
