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

#include <functional>
#include <map>
#include <memory>
#include <grpcpp/impl/codegen/slice.h>
#include <grpcpp/impl/grpc_library.h>
#include <grpcpp/security/auth_metadata_processor.h>
#include "src/cpp/common/secure_auth_context.h"
#include "src/cpp/server/secure_server_credentials.h"

namespace grpc {

int SecureServerCredentials::AddPortToServer(const std::string& addr,
                                             grpc_server* server) {
  return grpc_server_add_secure_http2_port(server, addr.c_str(), creds_);
}

void SecureServerCredentials::SetAuthMetadataProcessor(
    const std::shared_ptr<grpc::AuthMetadataProcessor>& processor) {
  auto* wrapper = new grpc::AuthMetadataProcessorAyncWrapper(processor);
  grpc_server_credentials_set_auth_metadata_processor(
      creds_, {grpc::AuthMetadataProcessorAyncWrapper::Process,
               grpc::AuthMetadataProcessorAyncWrapper::Destroy, wrapper});
}

std::shared_ptr<ServerCredentials> SslServerCredentials(
    const grpc::SslServerCredentialsOptions& options) {
  std::vector<grpc_ssl_pem_key_cert_pair> pem_key_cert_pairs;
  for (const auto& key_cert_pair : options.pem_key_cert_pairs) {
    grpc_ssl_pem_key_cert_pair p = {key_cert_pair.private_key.c_str(),
                                    key_cert_pair.cert_chain.c_str()};
    pem_key_cert_pairs.push_back(p);
  }
  grpc_server_credentials* c_creds = grpc_ssl_server_credentials_create_ex(
      options.pem_root_certs.empty() ? nullptr : options.pem_root_certs.c_str(),
      pem_key_cert_pairs.empty() ? nullptr : &pem_key_cert_pairs[0],
      pem_key_cert_pairs.size(),
      options.force_client_auth
          ? GRPC_SSL_REQUEST_AND_REQUIRE_CLIENT_CERTIFICATE_AND_VERIFY
          : options.client_certificate_request,
      nullptr);
  return std::shared_ptr<ServerCredentials>(
      new SecureServerCredentials(c_creds));
}

}  // namespace grpc

namespace grpc {

int SecureServerCredentials::AddPortToServer(const std::string& addr,
                                             grpc_server* server) {
  return grpc_server_add_secure_http2_port(server, addr.c_str(), creds_);
}

void SecureServerCredentials::SetAuthMetadataProcessor(
    const std::shared_ptr<grpc::AuthMetadataProcessor>& processor) {
  auto* wrapper = new grpc::AuthMetadataProcessorAyncWrapper(processor);
  grpc_server_credentials_set_auth_metadata_processor(
      creds_, {grpc::AuthMetadataProcessorAyncWrapper::Process,
               grpc::AuthMetadataProcessorAyncWrapper::Destroy, wrapper});
}

std::shared_ptr<ServerCredentials> SslServerCredentials(
    const grpc::SslServerCredentialsOptions& options) {
  std::vector<grpc_ssl_pem_key_cert_pair> pem_key_cert_pairs;
  for (const auto& key_cert_pair : options.pem_key_cert_pairs) {
    grpc_ssl_pem_key_cert_pair p = {key_cert_pair.private_key.c_str(),
                                    key_cert_pair.cert_chain.c_str()};
    pem_key_cert_pairs.push_back(p);
  }
  grpc_server_credentials* c_creds = grpc_ssl_server_credentials_create_ex(
      options.pem_root_certs.empty() ? nullptr : options.pem_root_certs.c_str(),
      pem_key_cert_pairs.empty() ? nullptr : &pem_key_cert_pairs[0],
      pem_key_cert_pairs.size(),
      options.force_client_auth
          ? GRPC_SSL_REQUEST_AND_REQUIRE_CLIENT_CERTIFICATE_AND_VERIFY
          : options.client_certificate_request,
      nullptr);
  return std::shared_ptr<ServerCredentials>(
      new SecureServerCredentials(c_creds));
}

}  // namespace grpc