#ifndef GRPCPP_SERVER_IMPL_H
#define GRPCPP_SERVER_IMPL_H 
#include <list>
#include <memory>
#include <vector>
#include <grpc/impl/codegen/port_platform.h>
#include <grpc/compression.h>
#include <grpc/support/atm.h>
#include <grpcpp/channel_impl.h>
#include <grpcpp/completion_queue_impl.h>
#include <grpcpp/channel.h>
#include <grpcpp/impl/codegen/completion_queue_impl.h>
#include <grpcpp/completion_queue.h>
#include <grpcpp/health_check_service_interface.h>
#include <grpcpp/impl/call.h>
#include <grpcpp/support/channel_arguments.h>
#include <grpcpp/impl/codegen/client_interceptor.h>
#include <grpcpp/impl/codegen/completion_queue.h>
#include <grpcpp/impl/codegen/grpc_library.h>
#include <grpcpp/impl/codegen/server_interface.h>
#include <grpcpp/impl/rpc_service_method.h>
#include <grpcpp/security/server_credentials.h>
#include <grpcpp/support/channel_arguments_impl.h>
#include <grpcpp/support/config.h>
#include <grpcpp/support/status.h>
struct grpc_server
    namespace grpc {
  class AsyncGenericService;
  namespace internal {
  class ExternalConnectionAcceptorImpl;
  }
}
namespace grpc_impl {
class HealthCheckServiceInterface;
class ServerContext;
class ServerInitializer;
class Server : public grpc::ServerInterface, private grpc::GrpcLibraryCodegen {
 public:
  ~Server();
  void Wait() override;
  class GlobalCallbacks {
   public:
    virtual ~GlobalCallbacks() {}
    virtual void UpdateArguments(grpc::ChannelArguments* ) {}
    virtual void PreSynchronousRequest(grpc_impl::ServerContext* context) = 0;
    virtual void PostSynchronousRequest(grpc_impl::ServerContext* context) = 0;
    virtual void PreServerStart(Server* ) {}
    virtual void AddPort(Server* , const std::string& ,
                         grpc::ServerCredentials* , int ) {}
  };
  static void SetGlobalCallbacks(GlobalCallbacks* callbacks);
  grpc_server* c_server();
  grpc::HealthCheckServiceInterface* GetHealthCheckService() const {
    return health_check_service_.get();
  }
  std::shared_ptr<Channel> InProcessChannel(const grpc::ChannelArguments& args);
  class experimental_type {
   public:
    explicit experimental_type(Server* server) : server_(server) {}
<<<<<<< HEAD
    std::shared_ptr<grpc::Channel> InProcessChannelWithInterceptors(
        const ChannelArguments& args,
|||||||
    std::shared_ptr<Channel> InProcessChannelWithInterceptors(
        const ChannelArguments& args,
=======
    std::shared_ptr<Channel> InProcessChannelWithInterceptors(
        const grpc::ChannelArguments& args,
>>>>>>> c8b95f97daf8c2649de8d7bf85edf89566a35b9f
        std::vector<std::unique_ptr<
            grpc::experimental::ClientInterceptorFactoryInterface>>
            interceptor_creators);
   private:
    Server* server_;
  };
  experimental_type experimental() { return experimental_type(this); }
 protected:
  bool RegisterService(const std::string* host,
                       grpc::Service* service) override;
  int AddListeningPort(const std::string& addr,
                       grpc::ServerCredentials* creds) override;
<<<<<<< HEAD
  Server(
      ChannelArguments* args,
      std::shared_ptr<std::vector<std::unique_ptr<grpc::ServerCompletionQueue>>>
|||||||
  Server(ChannelArguments* args,
         std::shared_ptr<std::vector<std::unique_ptr<ServerCompletionQueue>>>
=======
  Server(grpc::ChannelArguments* args,
         std::shared_ptr<std::vector<std::unique_ptr<ServerCompletionQueue>>>
>>>>>>> c8b95f97daf8c2649de8d7bf85edf89566a35b9f
          sync_server_cqs,
      int min_pollers, int max_pollers, int sync_cq_timeout_msec,
      std::vector<
          std::shared_ptr<grpc::internal::ExternalConnectionAcceptorImpl>>
          acceptors,
      grpc_resource_quota* server_rq = nullptr,
      std::vector<std::unique_ptr<
          grpc::experimental::ServerInterceptorFactoryInterface>>
          interceptor_creators = std::vector<std::unique_ptr<
              grpc::experimental::ServerInterceptorFactoryInterface>>());
  void Start(grpc::ServerCompletionQueue** cqs, size_t num_cqs) override;
  grpc_server* server() override { return server_; }
  void set_health_check_service(
      std::unique_ptr<grpc::HealthCheckServiceInterface> service) {
    health_check_service_ = std::move(service);
  }
  bool health_check_service_disabled() const {
    return health_check_service_disabled_;
  }
 private:
  std::unique_ptr < grpc::experimental::ServerInterceptorFactoryInterface >>
      *interceptor_creators() override {
    return &interceptor_creators_;
  }
  friend class grpc::AsyncGenericService;
  friend class grpc::ServerBuilder;
  friend class grpc_impl::ServerInitializer;
  class SyncRequest;
  class CallbackRequestBase;
  template <class ServerContextType>
  class CallbackRequest;
  class UnimplementedAsyncRequest;
  class UnimplementedAsyncResponse;
  class SyncRequestThreadManager;
  void RegisterAsyncGenericService(grpc::AsyncGenericService* service) override;
#ifdef GRPC_CALLBACK_API_NONEXPERIMENTAL
  void RegisterCallbackGenericService(
      grpc::CallbackGenericService* service) override;
#else
  class experimental_registration_type final
      : public experimental_registration_interface {
   public:
    explicit experimental_registration_type(Server* server) : server_(server) {}
    void RegisterCallbackGenericService(
        grpc::experimental::CallbackGenericService* service) override {
      server_->RegisterCallbackGenericService(service);
    }
   private:
    Server* server_;
  };
  void RegisterCallbackGenericService(
      grpc::experimental::CallbackGenericService* service);
  experimental_registration_interface* experimental_registration() override {
    return &experimental_registration_;
  }
#endif
  void PerformOpsOnCall(grpc::internal::CallOpSetInterface* ops,
                        grpc::internal::Call* call) override;
  void ShutdownInternal(gpr_timespec deadline) override;
  int max_receive_message_size() const override {
    return max_receive_message_size_;
  }
  grpc::CompletionQueue* CallbackCQ() override;
  grpc_impl::ServerInitializer* initializer();
  void Ref();
  void UnrefWithPossibleNotify() ;
  void UnrefAndWaitLocked() ;
  std::vector<std::shared_ptr<grpc::internal::ExternalConnectionAcceptorImpl>>
      acceptors_;
  std::vector<
      std::unique_ptr<grpc::experimental::ServerInterceptorFactoryInterface>>
      interceptor_creators_;
  int max_receive_message_size_;
  std::shared_ptr<std::vector<std::unique_ptr<grpc::ServerCompletionQueue>>>
      sync_server_cqs_;
  std::vector<std::unique_ptr<SyncRequestThreadManager>> sync_req_mgrs_;
#ifndef GRPC_CALLBACK_API_NONEXPERIMENTAL
  experimental_registration_type experimental_registration_{this};
#endif
  grpc::internal::Mutex mu_;
  bool started_;
  bool shutdown_;
  bool shutdown_notified_;
  grpc::internal::CondVar shutdown_done_cv_;
  bool shutdown_done_ = false;
  std::atomic_int shutdown_refs_outstanding_{1};
  grpc::internal::CondVar shutdown_cv_;
  std::shared_ptr<GlobalCallbacks> global_callbacks_;
  std::vector<std::string> services_;
  bool has_async_generic_service_ = false;
  bool has_callback_generic_service_ = false;
  bool has_callback_methods_ = false;
  grpc_server* server_;
  std::unique_ptr<grpc_impl::ServerInitializer> server_initializer_;
  std::unique_ptr<grpc::HealthCheckServiceInterface> health_check_service_;
  bool health_check_service_disabled_;
#ifdef GRPC_CALLBACK_API_NONEXPERIMENTAL
  std::unique_ptr<grpc::CallbackGenericService> unimplemented_service_;
#else
  std::unique_ptr<grpc::experimental::CallbackGenericService>
      unimplemented_service_;
#endif
  std::unique_ptr<grpc::internal::MethodHandler> resource_exhausted_handler_;
  std::unique_ptr<grpc::internal::MethodHandler> generic_handler_;
  grpc::CompletionQueue* callback_cq_ = nullptr;
  std::vector<grpc::CompletionQueue*> cq_list_;
};
}
#endif
