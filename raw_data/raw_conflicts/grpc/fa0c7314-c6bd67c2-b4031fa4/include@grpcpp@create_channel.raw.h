/*
 *
 * Copyright 2015 gRPC authors.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

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
/// Create a new \a Channel pointing to \a target.
///
/// \param target The URI of the endpoint to connect to.
/// \param creds Credentials to use for the created channel. If it does not
/// hold an object or is invalid, a lame channel (one on which all operations
/// fail) is returned.
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
/// Create a new \em custom \a Channel pointing to \a target.
///
/// \warning For advanced use and testing ONLY. Override default channel
/// arguments only if necessary.
///
/// \param target The URI of the endpoint to connect to.
/// \param creds Credentials to use for the created channel. If it does not
/// hold an object or is invalid, a lame channel (one on which all operations
/// fail) is returned.
/// \param args Options for channel creation.
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
/// Create a new \em custom \a Channel pointing to \a target with \a
/// interceptors being invoked per call.
///
/// \warning For advanced use and testing ONLY. Override default channel
/// arguments only if necessary.
///
/// \param target The URI of the endpoint to connect to.
/// \param creds Credentials to use for the created channel. If it does not
/// hold an object or is invalid, a lame channel (one on which all operations
/// fail) is returned.
/// \param args Options for channel creation.
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
}  // namespace experimental
}  // namespace grpc

#endif  // GRPCPP_CREATE_CHANNEL_H
