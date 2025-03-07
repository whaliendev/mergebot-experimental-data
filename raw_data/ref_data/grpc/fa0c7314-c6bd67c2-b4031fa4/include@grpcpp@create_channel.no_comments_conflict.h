#ifndef GRPCPP_CREATE_CHANNEL_H
#define GRPCPP_CREATE_CHANNEL_H 
#include <memory>
#include <grpcpp/channel.h>
#include <grpcpp/impl/codegen/client_interceptor.h>
#include <grpcpp/security/credentials.h>
#include <grpcpp/support/channel_arguments.h>
#include <grpcpp/support/config.h>
namespace grpc {
<<<<<<< HEAD
std::shared_ptr<Channel> CreateChannel(
    const grpc::string& target,
    const std::shared_ptr<ChannelCredentials>& creds);
||||||| b4031fa43d
static inline std::shared_ptr<::grpc::Channel> CreateChannel(
    const grpc::string& target,
    const std::shared_ptr<ChannelCredentials>& creds) {
  return ::grpc_impl::CreateChannelImpl(target, creds);
}
=======
static inline std::shared_ptr<::grpc::Channel> CreateChannel(
    const std::string& target,
    const std::shared_ptr<ChannelCredentials>& creds) {
  return ::grpc_impl::CreateChannelImpl(target, creds);
}
>>>>>>> c6bd67c2
<<<<<<< HEAD
std::shared_ptr<Channel> CreateCustomChannel(
    const grpc::string& target,
    const std::shared_ptr<ChannelCredentials>& creds,
    const ChannelArguments& args);
||||||| b4031fa43d
static inline std::shared_ptr<::grpc::Channel> CreateCustomChannel(
    const grpc::string& target,
    const std::shared_ptr<ChannelCredentials>& creds,
    const ChannelArguments& args) {
  return ::grpc_impl::CreateCustomChannelImpl(target, creds, args);
}
=======
static inline std::shared_ptr<::grpc::Channel> CreateCustomChannel(
    const std::string& target, const std::shared_ptr<ChannelCredentials>& creds,
    const ChannelArguments& args) {
  return ::grpc_impl::CreateCustomChannelImpl(target, creds, args);
}
>>>>>>> c6bd67c2
namespace experimental {
<<<<<<< HEAD
std::shared_ptr<Channel> CreateCustomChannelWithInterceptors(
    const grpc::string& target,
    const std::shared_ptr<ChannelCredentials>& creds,
||||||| b4031fa43d
static inline std::shared_ptr<::grpc::Channel>
CreateCustomChannelWithInterceptors(
    const grpc::string& target,
    const std::shared_ptr<ChannelCredentials>& creds,
=======
static inline std::shared_ptr<::grpc::Channel>
CreateCustomChannelWithInterceptors(
    const std::string& target, const std::shared_ptr<ChannelCredentials>& creds,
>>>>>>> c6bd67c2
    const ChannelArguments& args,
    std::vector<
        std::unique_ptr<experimental::ClientInterceptorFactoryInterface>>
        interceptor_creators);
}
}
#endif
