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

#ifndef GRPCPP_SECURITY_CREDENTIALS_H
#define GRPCPP_SECURITY_CREDENTIALS_H

#include <map>
#include <memory>
#include <vector>

#include <grpc/grpc_security_constants.h>
#include <grpcpp/channel.h>
#include <grpcpp/impl/codegen/client_interceptor.h>
#include <grpcpp/impl/codegen/grpc_library.h>
#include <grpcpp/security/auth_context.h>
#include <grpcpp/security/tls_credentials_options.h>
#include <grpcpp/support/channel_arguments.h>
#include <grpcpp/support/status.h>
#include <grpcpp/support/string_ref.h>

struct grpc_call;

namespace grpc {
class CallCredentials;
class SecureCallCredentials;
class SecureChannelCredentials;
class ChannelCredentials;

std::shared_ptr<Channel> CreateCustomChannel(
    const grpc::string& target,
    const std::shared_ptr<grpc::ChannelCredentials>& creds,
    const grpc::ChannelArguments& args);

namespace experimental {
std::shared_ptr<grpc::Channel> CreateCustomChannelWithInterceptors(
    const grpc::string& target,
    const std::shared_ptr<grpc::ChannelCredentials>& creds,
    const grpc::ChannelArguments& args,
    std::vector<
        std::unique_ptr<grpc::experimental::ClientInterceptorFactoryInterface>>
        interceptor_creators);
}

/// A channel credentials object encapsulates all the state needed by a client
/// to authenticate with a server for a given channel.
/// It can make various assertions, e.g., about the client’s identity, role
/// for all the calls on that channel.
///
/// \see https://grpc.io/docs/guides/auth.html
class ChannelCredentials : private grpc::GrpcLibraryCodegen {
 public:
  ChannelCredentials();
  ~ChannelCredentials();

 protected:
  friend std::shared_ptr<ChannelCredentials> CompositeChannelCredentials(
      const std::shared_ptr<ChannelCredentials>& channel_creds,
      const std::shared_ptr<CallCredentials>& call_creds);

  virtual SecureChannelCredentials* AsSecureCredentials() = 0;


friend std::shared_ptr<grpc::Channel> CreateCustomChannel( const grpc::string& target, const std::shared_ptr<grpc::ChannelCredentials>& creds, const grpc::ChannelArguments& args); friend std::shared_ptr<grpc::Channel> grpc::experimental::CreateCustomChannelWithInterceptors( const grpc::string& target, const std::shared_ptr<grpc::ChannelCredentials>& creds, const grpc::ChannelArguments& args, std::vector<std::unique_ptr< grpc::experimental::ClientInterceptorFactoryInterface>> interceptor_creators); virtual std::shared_ptr<Channel> CreateChannelImpl( const std::string& target, const ChannelArguments& /*args*/, std::vector<std::unique_ptr< grpc::experimental::ClientInterceptorFactoryInterface>> /*interceptor_creators*/) { return nullptr; } }; /// A call credentials object encapsulates the state needed by a client to /// authenticate with a server for a given call on a channel. /// /// \see https://grpc.io/docs/guides/auth.html class CallCredentials : private grpc::GrpcLibraryCodegen { public: CallCredentials(); ~CallCredentials(); /// Apply this instance's credentials to \a call. virtual bool ApplyToCall(grpc_call* call) = 0; virtual grpc::string DebugString() { return "CallCredentials did not provide a debug string"; } protected: friend std::shared_ptr<ChannelCredentials> CompositeChannelCredentials( const std::shared_ptr<ChannelCredentials>& channel_creds, const std::shared_ptr<CallCredentials>& call_creds); friend std::shared_ptr<CallCredentials>& call_creds); friend std::shared_ptr<CallCredentials>& channel_creds, const std::shared_ptr<CallCredentials>& channel_creds, const std::shared_ptr<CallCredentials>& call_


/// Builds refresh token credentials. /// json_refresh_token is the JSON string containing the refresh token along /// with a client_id and client_secret. /// /// \warning Only use these credentials when connecting to a Google endpoint. /// Using these credentials to connect to any other service may result in this /// service being able to impersonate your client for requests to Google /// services. std::shared_ptr<CallCredentials> GoogleRefreshTokenCredentials( const std::string& json_refresh_token);


/// Builds access token credentials. /// access_token is an oauth2 access token that was fetched using an out of band /// mechanism. /// /// \warning Only use these credentials when connecting to a Google endpoint. /// Using these credentials to connect to any other service may result in this /// service being able to impersonate your client for requests to Google /// services. std::shared_ptr<CallCredentials> AccessTokenCredentials( const std::string& access_token);


std::shared_ptr<CallCredentials> GoogleIAMCredentials( const std::string& authorization_token, const std::string& authority_selector);

/// Combines a channel credentials and a call credentials into a composite
/// channel credentials.
std::shared_ptr<ChannelCredentials> CompositeChannelCredentials(
    const std::shared_ptr<ChannelCredentials>& channel_creds,
    const std::shared_ptr<CallCredentials>& call_creds);

/// Combines two call credentials objects into a composite call credentials.
std::shared_ptr<CallCredentials> CompositeCallCredentials(
    const std::shared_ptr<CallCredentials>& creds1,
    const std::shared_ptr<CallCredentials>& creds2);

/// Credentials for an unencrypted, unauthenticated channel
std::shared_ptr<ChannelCredentials> InsecureChannelCredentials();

/// User defined metadata credentials.
class MetadataCredentialsPlugin {
 public:
  virtual ~MetadataCredentialsPlugin() {}

  /// If this method returns true, the Process function will be scheduled in
  /// a different thread from the one processing the call.
  virtual bool IsBlocking() const { return true; }

  /// Type of credentials this plugin is implementing.
  virtual const char* GetType() const { return ""; }

  /// Gets the auth metatada produced by this plugin.
  /// The fully qualified method name is:
  /// service_url + "/" + method_name.
  /// The channel_auth_context contains (among other things), the identity of
  /// the server.
  virtual grpc::Status GetMetadata(
      grpc::string_ref service_url, grpc::string_ref method_name,
      const grpc::AuthContext& channel_auth_context,
      std::multimap<grpc::string, grpc::string>* metadata) = 0;

  virtual grpc::string DebugString() {
    return "MetadataCredentialsPlugin did not provide a debug string";
  }
};

std::shared_ptr<CallCredentials> MetadataCredentialsFromPlugin(
    std::unique_ptr<MetadataCredentialsPlugin> plugin);

namespace experimental {

/// Options for creating STS Oauth Token Exchange credentials following the IETF
/// draft https://tools.ietf.org/html/draft-ietf-oauth-token-exchange-16.
/// Optional fields may be set to empty string. It is the responsibility of the
/// caller to ensure that the subject and actor tokens are refreshed on disk at
/// the specified paths.
struct StsCredentialsOptions {
  grpc::string token_exchange_service_uri;  // Required.
  grpc::string resource;                    // Optional.
  grpc::string audience;                    // Optional.
  grpc::string scope;                       // Optional.
  grpc::string requested_token_type;        // Optional.
  grpc::string subject_token_path;          // Required.
  grpc::string subject_token_type;          // Required.
  grpc::string actor_token_path;            // Optional.
  grpc::string actor_token_type;            // Optional.
};


/// Creates STS Options from a JSON string. The JSON schema is as follows: /// { /// "title": "STS Credentials Config", /// "type": "object", /// "required": ["token_exchange_service_uri", "subject_token_path", /// "subject_token_type"], /// "properties": { /// "token_exchange_service_uri": { /// "type": "string" /// }, /// "resource": { /// "type": "string" /// }, /// "audience": { /// "type": "string" /// }, /// "scope": { /// "type": "string" /// }, /// "requested_token_type": { /// "type": "string" /// }, /// "subject_token_path": { /// "type": "string" /// }, /// "subject_token_type": { /// "type": "string" /// }, /// "actor_token_path" : { /// "type": "string" /// }, /// "actor_token_type": { /// "type": "string" /// } /// } /// } grpc::Status StsCredentialsOptionsFromJson(const std::string& json_string, StsCredentialsOptions* options);

/// Creates STS credentials options from the $STS_CREDENTIALS environment
/// variable. This environment variable points to the path of a JSON file
/// comforming to the schema described above.
grpc::Status StsCredentialsOptionsFromEnv(StsCredentialsOptions* options);

std::shared_ptr<CallCredentials> StsCredentials(
    const StsCredentialsOptions& options);

std::shared_ptr<CallCredentials> MetadataCredentialsFromPlugin(
    std::unique_ptr<MetadataCredentialsPlugin> plugin,
    grpc_security_level min_security_level);

/// Options used to build AltsCredentials.
struct AltsCredentialsOptions {
  /// service accounts of target endpoint that will be acceptable
  /// by the client. If service accounts are provided and none of them matches
  /// that of the server, authentication will fail.
  std::vector<grpc::string> target_service_accounts;
};

/// Builds ALTS Credentials given ALTS specific options
std::shared_ptr<ChannelCredentials> AltsCredentials(
    const AltsCredentialsOptions& options);

/// Builds Local Credentials.
std::shared_ptr<ChannelCredentials> LocalCredentials(
    grpc_local_connect_type type);

/// Builds TLS Credentials given TLS options.
std::shared_ptr<ChannelCredentials> TlsCredentials(
    const TlsCredentialsOptions& options);

}  // namespace experimental
}  // namespace grpc

#endif  // GRPCPP_SECURITY_CREDENTIALS_H
