#ifndef GRPCPP_IMPL_CODEGEN_COMPLETION_QUEUE_H
#define GRPCPP_IMPL_CODEGEN_COMPLETION_QUEUE_H 
#include <list>
#include <grpc/impl/codegen/atm.h>
#include <grpcpp/impl/codegen/completion_queue_tag.h>
#include <grpcpp/impl/codegen/core_codegen_interface.h>
#include <grpcpp/impl/codegen/grpc_library.h>
#include <grpcpp/impl/codegen/status.h>
#include <grpcpp/impl/codegen/sync.h>
#include <grpcpp/impl/codegen/time.h>
struct grpc_completion_queue;
namespace grpc_impl {
class ServerContextBase;
}
namespace grpc {
template <class R>
class ClientReader;
template <class W>
class ClientWriter;
template <class W, class R>
class ClientReaderWriter;
template <class R>
class ServerReader;
template <class W>
class ServerWriter;
namespace internal {
template <class W, class R>
class ServerReaderWriterBody;
template <class ServiceType, class RequestType, class ResponseType>
class RpcMethodHandler;
template <class ServiceType, class RequestType, class ResponseType>
class ClientStreamingHandler;
template <class ServiceType, class RequestType, class ResponseType>
class ServerStreamingHandler;
template <class Streamer, bool WriteNeeded>
class TemplatedBidiStreamingHandler;
template <::grpc::StatusCode code>
class ErrorMethodHandler;
}
class Channel;
class ChannelInterface;
class Server;
class ServerBuilder;
class ServerContextBase;
class ServerInterface;
namespace internal {
class CompletionQueueTag;
class RpcMethod;
template <class InputMessage, class OutputMessage>
class BlockingUnaryCallImpl;
template <class Op1, class Op2, class Op3, class Op4, class Op5, class Op6>
class CallOpSet;
}
extern CoreCodegenInterface* g_core_codegen_interface;
class CompletionQueue : private ::grpc::GrpcLibraryCodegen {
 public:
  CompletionQueue()
      : CompletionQueue(grpc_completion_queue_attributes{
            GRPC_CQ_CURRENT_VERSION, GRPC_CQ_NEXT, GRPC_CQ_DEFAULT_POLLING,
            nullptr}) {}
  explicit CompletionQueue(grpc_completion_queue* take);
  ~CompletionQueue() {
    ::grpc::g_core_codegen_interface->grpc_completion_queue_destroy(cq_);
  }
  enum NextStatus {
    SHUTDOWN,
    GOT_EVENT,
    TIMEOUT
  };
  bool Next(void** tag, bool* ok) {
    return (AsyncNextInternal(tag, ok,
                              ::grpc::g_core_codegen_interface->gpr_inf_future(
                                  GPR_CLOCK_REALTIME)) != SHUTDOWN);
  }
  template <typename T>
  NextStatus AsyncNext(void** tag, bool* ok, const T& deadline) {
    ::grpc::TimePoint<T> deadline_tp(deadline);
    return AsyncNextInternal(tag, ok, deadline_tp.raw_time());
  }
  template <typename T, typename F>
  NextStatus DoThenAsyncNext(F&& f, void** tag, bool* ok, const T& deadline) {
    CompletionQueueTLSCache cache = CompletionQueueTLSCache(this);
    f();
    if (cache.Flush(tag, ok)) {
      return GOT_EVENT;
    } else {
      return AsyncNext(tag, ok, deadline);
    }
  }
  void Shutdown();
  grpc_completion_queue* cq() { return cq_; }
 protected:
  CompletionQueue(const grpc_completion_queue_attributes& attributes) {
    cq_ = ::grpc::g_core_codegen_interface->grpc_completion_queue_create(
        ::grpc::g_core_codegen_interface->grpc_completion_queue_factory_lookup(
            &attributes),
        &attributes, NULL);
    InitialAvalanching();
  }
 private:
  friend class ::grpc::ServerBuilder;
  friend class ::grpc::Server;
  template <class R>
  friend class ::grpc::ClientReader;
  template <class W>
  friend class ::grpc::ClientWriter;
  template <class W, class R>
  friend class ::grpc::ClientReaderWriter;
  template <class R>
  friend class ::grpc::ServerReader;
  template <class W>
  friend class ::grpc::ServerWriter;
  template <class W, class R>
  friend class ::grpc::internal::ServerReaderWriterBody;
  template <class ServiceType, class RequestType, class ResponseType>
  friend class ::grpc::internal::RpcMethodHandler;
  template <class ServiceType, class RequestType, class ResponseType>
  friend class ::grpc::internal::ClientStreamingHandler;
  template <class ServiceType, class RequestType, class ResponseType>
  friend class ::grpc::internal::ServerStreamingHandler;
  template <class Streamer, bool WriteNeeded>
  friend class ::grpc::internal::TemplatedBidiStreamingHandler;
  template <::grpc::StatusCode code>
  friend class ::grpc::internal::ErrorMethodHandler;
  friend class ::grpc::ServerContextBase;
  friend class ::grpc::ServerInterface;
  template <class InputMessage, class OutputMessage>
  friend class ::grpc::internal::BlockingUnaryCallImpl;
  friend class ::grpc::Channel;
  template <class Op1, class Op2, class Op3, class Op4, class Op5, class Op6>
  friend class ::grpc::internal::CallOpSet;
  class CompletionQueueTLSCache {
   public:
    CompletionQueueTLSCache(CompletionQueue* cq);
    ~CompletionQueueTLSCache();
    bool Flush(void** tag, bool* ok);
   private:
    CompletionQueue* cq_;
    bool flushed_;
  };
  NextStatus AsyncNextInternal(void** tag, bool* ok, gpr_timespec deadline);
  bool Pluck(::grpc::internal::CompletionQueueTag* tag) {
    auto deadline =
        ::grpc::g_core_codegen_interface->gpr_inf_future(GPR_CLOCK_REALTIME);
    while (true) {
      auto ev = ::grpc::g_core_codegen_interface->grpc_completion_queue_pluck(
          cq_, tag, deadline, nullptr);
      bool ok = ev.success != 0;
      void* ignored = tag;
      if (tag->FinalizeResult(&ignored, &ok)) {
        GPR_CODEGEN_ASSERT(ignored == tag);
        return ok;
      }
    }
  }
  void TryPluck(::grpc::internal::CompletionQueueTag* tag) {
    auto deadline =
        ::grpc::g_core_codegen_interface->gpr_time_0(GPR_CLOCK_REALTIME);
    auto ev = ::grpc::g_core_codegen_interface->grpc_completion_queue_pluck(
        cq_, tag, deadline, nullptr);
    if (ev.type == GRPC_QUEUE_TIMEOUT) return;
    bool ok = ev.success != 0;
    void* ignored = tag;
    GPR_CODEGEN_ASSERT(!tag->FinalizeResult(&ignored, &ok));
  }
  void TryPluck(::grpc::internal::CompletionQueueTag* tag,
                gpr_timespec deadline) {
    auto ev = ::grpc::g_core_codegen_interface->grpc_completion_queue_pluck(
        cq_, tag, deadline, nullptr);
    if (ev.type == GRPC_QUEUE_TIMEOUT || ev.type == GRPC_QUEUE_SHUTDOWN) {
      return;
    }
    bool ok = ev.success != 0;
    void* ignored = tag;
    GPR_CODEGEN_ASSERT(!tag->FinalizeResult(&ignored, &ok));
  }
  void InitialAvalanching() {
    gpr_atm_rel_store(&avalanches_in_flight_, static_cast<gpr_atm>(1));
  }
  void RegisterAvalanching() {
    gpr_atm_no_barrier_fetch_add(&avalanches_in_flight_,
                                 static_cast<gpr_atm>(1));
  }
  void CompleteAvalanching() {
    if (gpr_atm_no_barrier_fetch_add(&avalanches_in_flight_,
                                     static_cast<gpr_atm>(-1)) == 1) {
      ::grpc::g_core_codegen_interface->grpc_completion_queue_shutdown(cq_);
    }
  }
  void RegisterServer(const ::grpc::Server* server) {
    (void)server;
#ifndef NDEBUG
    grpc::internal::MutexLock l(&server_list_mutex_);
    server_list_.push_back(server);
#endif
  }
  void UnregisterServer(const ::grpc::Server* server) {
    (void)server;
#ifndef NDEBUG
    grpc::internal::MutexLock l(&server_list_mutex_);
    server_list_.remove(server);
#endif
  }
  bool ServerListEmpty() const {
#ifndef NDEBUG
    grpc::internal::MutexLock l(&server_list_mutex_);
    return server_list_.empty();
#endif
    return true;
  }
  grpc_completion_queue* cq_;
  gpr_atm avalanches_in_flight_;
  mutable grpc::internal::Mutex server_list_mutex_;
  std::list<const ::grpc::Server*>
      server_list_ ;
};
class ServerCompletionQueue : public CompletionQueue {
 public:
  bool IsFrequentlyPolled() { return polling_type_ != GRPC_CQ_NON_LISTENING; }
 protected:
  ServerCompletionQueue() : polling_type_(GRPC_CQ_DEFAULT_POLLING) {}
 private:
  ServerCompletionQueue(grpc_cq_completion_type completion_type,
                        grpc_cq_polling_type polling_type,
                        grpc_experimental_completion_queue_functor* shutdown_cb)
      : CompletionQueue(grpc_completion_queue_attributes{
            GRPC_CQ_CURRENT_VERSION, completion_type, polling_type,
            shutdown_cb}),
        polling_type_(polling_type) {}
  grpc_cq_polling_type polling_type_;
  friend class ::grpc::ServerBuilder;
  friend class ::grpc::Server;
};
}
#endif
