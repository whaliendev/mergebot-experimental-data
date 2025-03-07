#ifndef GRPCPP_IMPL_CODEGEN_CLIENT_CONTEXT_H
#define GRPCPP_IMPL_CODEGEN_CLIENT_CONTEXT_H 
#include <map>
#include <memory>
#include <string>
#include <grpc/impl/codegen/compression_types.h>
#include <grpc/impl/codegen/propagation_bits.h>
#include <grpcpp/impl/codegen/client_interceptor.h>
#include <grpcpp/impl/codegen/config.h>
#include <grpcpp/impl/codegen/core_codegen_interface.h>
#include <grpcpp/impl/codegen/create_auth_context.h>
#include <grpcpp/impl/codegen/metadata_map.h>
#include <grpcpp/impl/codegen/rpc_method.h>
#include <grpcpp/impl/codegen/security/auth_context.h>
#include <grpcpp/impl/codegen/slice.h>
#include <grpcpp/impl/codegen/status.h>
#include <grpcpp/impl/codegen/string_ref.h>
#include <grpcpp/impl/codegen/sync.h>
#include <grpcpp/impl/codegen/time.h>
struct census_context;
struct grpc_call;
namespace grpc {
class ServerContext;
class ServerContextBase;
class CallbackServerContext;
namespace internal {
template <class InputMessage, class OutputMessage>
class CallbackUnaryCallImpl;
template <class Request, class Response>
class ClientCallbackReaderWriterImpl;
template <class Response>
class ClientCallbackReaderImpl;
template <class Request>
class ClientCallbackWriterImpl;
class ClientCallbackUnaryImpl;
class ClientContextAccessor;
}
template <class R>
class ClientReader;
template <class W>
class ClientWriter;
template <class W, class R>
class ClientReaderWriter;
template <class R>
class ClientAsyncReader;
template <class W>
class ClientAsyncWriter;
template <class W, class R>
class ClientAsyncReaderWriter;
template <class R>
class ClientAsyncResponseReader;
namespace testing {
class InteropClientContextInspector;
}
namespace internal {
class RpcMethod;
template <class InputMessage, class OutputMessage>
class BlockingUnaryCallImpl;
class CallOpClientRecvStatus;
class CallOpRecvInitialMetadata;
class ServerContextImpl;
template <class InputMessage, class OutputMessage>
class CallbackUnaryCallImpl;
template <class Request, class Response>
class ClientCallbackReaderWriterImpl;
template <class Response>
class ClientCallbackReaderImpl;
template <class Request>
class ClientCallbackWriterImpl;
class ClientCallbackUnaryImpl;
class ClientContextAccessor;
}
class CallCredentials;
class Channel;
class ChannelInterface;
class CompletionQueue;
class PropagationOptions {
 public:
  PropagationOptions() : propagate_(GRPC_PROPAGATE_DEFAULTS) {}
  PropagationOptions& enable_deadline_propagation() {
    propagate_ |= GRPC_PROPAGATE_DEADLINE;
    return *this;
  }
  PropagationOptions& disable_deadline_propagation() {
    propagate_ &= ~GRPC_PROPAGATE_DEADLINE;
    return *this;
  }
  PropagationOptions& enable_census_stats_propagation() {
    propagate_ |= GRPC_PROPAGATE_CENSUS_STATS_CONTEXT;
    return *this;
  }
  PropagationOptions& disable_census_stats_propagation() {
    propagate_ &= ~GRPC_PROPAGATE_CENSUS_STATS_CONTEXT;
    return *this;
  }
  PropagationOptions& enable_census_tracing_propagation() {
    propagate_ |= GRPC_PROPAGATE_CENSUS_TRACING_CONTEXT;
    return *this;
  }
  PropagationOptions& disable_census_tracing_propagation() {
    propagate_ &= ~GRPC_PROPAGATE_CENSUS_TRACING_CONTEXT;
    return *this;
  }
  PropagationOptions& enable_cancellation_propagation() {
    propagate_ |= GRPC_PROPAGATE_CANCELLATION;
    return *this;
  }
  PropagationOptions& disable_cancellation_propagation() {
    propagate_ &= ~GRPC_PROPAGATE_CANCELLATION;
    return *this;
  }
  uint32_t c_bitmask() const { return propagate_; }
 private:
  uint32_t propagate_;
};
class ClientContext {
 public:
  ClientContext();
  ~ClientContext();
  static std::unique_ptr<ClientContext> FromServerContext(
      const grpc::ServerContext& server_context,
      PropagationOptions options = PropagationOptions());
  static std::unique_ptr<ClientContext> FromCallbackServerContext(
      const grpc::CallbackServerContext& server_context,
      PropagationOptions options = PropagationOptions());
  void AddMetadata(const std::string& meta_key, const std::string& meta_value);
  const std::multimap<grpc::string_ref, grpc::string_ref>&
  GetServerInitialMetadata() const {
    GPR_CODEGEN_ASSERT(initial_metadata_received_);
    return *recv_initial_metadata_.map();
  }
  const std::multimap<grpc::string_ref, grpc::string_ref>&
  GetServerTrailingMetadata() const {
    return *trailing_metadata_.map();
  }
  template <typename T>
  void set_deadline(const T& deadline) {
    grpc::TimePoint<T> deadline_tp(deadline);
    deadline_ = deadline_tp.raw_time();
  }
  void set_idempotent(bool idempotent) { idempotent_ = idempotent; }
  void set_cacheable(bool cacheable) { cacheable_ = cacheable; }
  void set_wait_for_ready(bool wait_for_ready) {
    wait_for_ready_ = wait_for_ready;
    wait_for_ready_explicitly_set_ = true;
  }
  void set_fail_fast(bool fail_fast) { set_wait_for_ready(!fail_fast); }
  std::chrono::system_clock::time_point deadline() const {
    return grpc::Timespec2Timepoint(deadline_);
  }
  gpr_timespec raw_deadline() const { return deadline_; }
  void set_authority(const std::string& authority) { authority_ = authority; }
  std::shared_ptr<const grpc::AuthContext> auth_context() const {
    if (auth_context_.get() == nullptr) {
      auth_context_ = grpc::CreateAuthContext(call_);
    }
    return auth_context_;
  }
  void set_credentials(const std::shared_ptr<grpc::CallCredentials>& creds);
  std::shared_ptr<grpc::CallCredentials> credentials() { return creds_; }
  grpc_compression_algorithm compression_algorithm() const {
    return compression_algorithm_;
  }
  void set_compression_algorithm(grpc_compression_algorithm algorithm);
  void set_initial_metadata_corked(bool corked) {
    initial_metadata_corked_ = corked;
  }
  std::string peer() const;
  void set_census_context(struct census_context* ccp) { census_context_ = ccp; }
  struct census_context* census_context() const {
    return census_context_;
  }
  void TryCancel();
  class GlobalCallbacks {
   public:
    virtual ~GlobalCallbacks() {}
    virtual void DefaultConstructor(ClientContext* context) = 0;
    virtual void Destructor(ClientContext* context) = 0;
  };
  static void SetGlobalCallbacks(GlobalCallbacks* callbacks);
  grpc_call* c_call() { return call_; }
  std::string debug_error_string() const { return debug_error_string_; }
 private:
  ClientContext(const ClientContext&);
  ClientContext& operator=(const ClientContext&);
  friend class ::grpc::testing::InteropClientContextInspector;
  friend class ::grpc::internal::CallOpClientRecvStatus;
  friend class ::grpc::internal::CallOpRecvInitialMetadata;
  friend class ::grpc::Channel;
  template <class R>
  friend class ::grpc::ClientReader;
  template <class W>
  friend class ::grpc::ClientWriter;
  template <class W, class R>
  friend class ::grpc::ClientReaderWriter;
  template <class R>
  friend class ::grpc::ClientAsyncReader;
  template <class W>
  friend class ::grpc::ClientAsyncWriter;
  template <class W, class R>
  friend class ::grpc::ClientAsyncReaderWriter;
  template <class R>
  friend class ::grpc::ClientAsyncResponseReader;
  template <class InputMessage, class OutputMessage>
  friend class ::grpc::internal::BlockingUnaryCallImpl;
  template <class InputMessage, class OutputMessage>
  friend class ::grpc::internal::CallbackUnaryCallImpl;
  template <class Request, class Response>
  friend class ::grpc::internal::ClientCallbackReaderWriterImpl;
  template <class Response>
  friend class ::grpc::internal::ClientCallbackReaderImpl;
  template <class Request>
  friend class ::grpc::internal::ClientCallbackWriterImpl;
  friend class ::grpc::internal::ClientCallbackUnaryImpl;
  friend class ::grpc::internal::ClientContextAccessor;
  void set_debug_error_string(const std::string& debug_error_string) {
    debug_error_string_ = debug_error_string;
  }
  grpc_call* call() const { return call_; }
  void set_call(grpc_call* call,
                const std::shared_ptr<::grpc::Channel>& channel);
  grpc::experimental::ClientRpcInfo* set_client_rpc_info(
      const char* method, grpc::internal::RpcMethod::RpcType type,
      grpc::ChannelInterface* channel,
      const std::vector<std::unique_ptr<
          grpc::experimental::ClientInterceptorFactoryInterface>>& creators,
      size_t interceptor_pos) {
    rpc_info_ = grpc::experimental::ClientRpcInfo(this, type, method, channel);
    rpc_info_.RegisterInterceptors(creators, interceptor_pos);
    return &rpc_info_;
  }
  uint32_t initial_metadata_flags() const {
    return (idempotent_ ? GRPC_INITIAL_METADATA_IDEMPOTENT_REQUEST : 0) |
           (wait_for_ready_ ? GRPC_INITIAL_METADATA_WAIT_FOR_READY : 0) |
           (cacheable_ ? GRPC_INITIAL_METADATA_CACHEABLE_REQUEST : 0) |
           (wait_for_ready_explicitly_set_
                ? GRPC_INITIAL_METADATA_WAIT_FOR_READY_EXPLICITLY_SET
                : 0) |
           (initial_metadata_corked_ ? GRPC_INITIAL_METADATA_CORKED : 0);
  }
  std::string authority() { return authority_; }
  void SendCancelToInterceptors();
  static std::unique_ptr<ClientContext> FromInternalServerContext(
      const grpc::ServerContextBase& server_context,
      PropagationOptions options);
  bool initial_metadata_received_;
  bool wait_for_ready_;
  bool wait_for_ready_explicitly_set_;
  bool idempotent_;
  bool cacheable_;
  std::shared_ptr<::grpc::Channel> channel_;
  grpc::internal::Mutex mu_;
  grpc_call* call_;
  bool call_canceled_;
  gpr_timespec deadline_;
  grpc::string authority_;
  std::shared_ptr<grpc::CallCredentials> creds_;
  mutable std::shared_ptr<const grpc::AuthContext> auth_context_;
  struct census_context* census_context_;
  std::multimap<std::string, std::string> send_initial_metadata_;
  mutable grpc::internal::MetadataMap recv_initial_metadata_;
  mutable grpc::internal::MetadataMap trailing_metadata_;
  grpc_call* propagate_from_call_;
  PropagationOptions propagation_options_;
  grpc_compression_algorithm compression_algorithm_;
  bool initial_metadata_corked_;
  std::string debug_error_string_;
  grpc::experimental::ClientRpcInfo rpc_info_;
};
}
#endif
