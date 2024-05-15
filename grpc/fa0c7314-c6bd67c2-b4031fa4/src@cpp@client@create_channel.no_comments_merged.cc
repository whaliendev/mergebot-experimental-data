#include <memory>
#include <grpcpp/channel.h>
#include <grpcpp/create_channel.h>
#include <grpcpp/impl/grpc_library.h>
#include <grpcpp/security/credentials.h>
#include <grpcpp/support/channel_arguments.h>
#include "src/cpp/client/create_channel_internal.h"
namespace grpc {
std::shared_ptr<grpc::Channel> CreateChannel(
    const grpc::string& target,
    const std::shared_ptr<grpc::ChannelCredentials>& creds) {
  return CreateCustomChannel(target, creds, grpc::ChannelArguments());
}
std::shared_ptr<grpc::Channel> CreateCustomChannel(
    const grpc::string& target,
    const std::shared_ptr<grpc::ChannelCredentials>& creds,
    const grpc::ChannelArguments& args) {
  grpc::GrpcLibraryCodegen
      init_lib;
  return creds ? creds->CreateChannelImpl(target, args)
               : grpc::CreateChannelInternal(
                     "",
                     grpc_lame_client_channel_create(
                         nullptr, GRPC_STATUS_INVALID_ARGUMENT,
                         "Invalid credentials."),
                     std::vector<std::unique_ptr<
                         grpc::experimental::
                             ClientInterceptorFactoryInterface>>());
}
namespace experimental {
std::shared_ptr<grpc::Channel> CreateCustomChannelWithInterceptors(
    const std::string& target,
    const std::shared_ptr<grpc::ChannelCredentials>& creds,
    const grpc::ChannelArguments& args,
    std::vector<
        std::unique_ptr<grpc::experimental::ClientInterceptorFactoryInterface>>
        interceptor_creators) {
  grpc::GrpcLibraryCodegen
      init_lib;
  return creds ? creds->CreateChannelWithInterceptors(
                     target, args, std::move(interceptor_creators))
               : grpc::CreateChannelInternal(
                     "",
                     grpc_lame_client_channel_create(
                         nullptr, GRPC_STATUS_INVALID_ARGUMENT,
                         "Invalid credentials."),
                     std::move(interceptor_creators));
}
}
}
