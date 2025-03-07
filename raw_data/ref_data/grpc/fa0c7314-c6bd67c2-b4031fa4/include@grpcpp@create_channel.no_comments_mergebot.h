#ifndef GRPCPP_CREATE_CHANNEL_H
#define GRPCPP_CREATE_CHANNEL_H 
#include <grpcpp/create_channel_impl.h>
#include <memory>
#include <grpcpp/channel.h>
#include <grpcpp/impl/codegen/client_interceptor.h>
#include <grpcpp/security/credentials.h>
#include <grpcpp/support/channel_arguments.h>
#include <grpcpp/support/config.h>
namespace grpc {
namespace experimental {
std::shared_ptr<Channel> CreateCustomChannelWithInterceptors(
    const grpc::string& target,
    const std::shared_ptr<ChannelCredentials>& creds,
    const ChannelArguments& args,
    std::vector<
        std::unique_ptr<experimental::ClientInterceptorFactoryInterface>>
        interceptor_creators);
}
std::shared_ptr<Channel> CreateChannel(
    const grpc::string& target,
    const std::shared_ptr<ChannelCredentials>& creds);
std::shared_ptr<Channel> CreateCustomChannel(
    const grpc::string& target,
    const std::shared_ptr<ChannelCredentials>& creds,
    const ChannelArguments& args);
}
#endif
