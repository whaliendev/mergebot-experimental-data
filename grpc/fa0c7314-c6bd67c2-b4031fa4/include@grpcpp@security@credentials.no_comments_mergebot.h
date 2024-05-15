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
struct grpc_call
#include <grpcpp/security/credentials_impl.h>
    namespace grpc {
  constexpr long kMaxAuthTokenLifetimeSecs = 3600;
  std::shared_ptr<CallCredentials> ServiceAccountJWTAccessCredentials(
      const grpc::string& json_key,
      long token_lifetime_seconds = kMaxAuthTokenLifetimeSecs);
  std::shared_ptr<CallCredentials> GoogleRefreshTokenCredentials(
      const grpc::string& json_refresh_token);
  std::shared_ptr<CallCredentials> AccessTokenCredentials(
      const grpc::string& access_token);
  std::shared_ptr<CallCredentials> GoogleIAMCredentials(
      const grpc::string& authorization_token,
      const grpc::string& authority_selector);
  std::shared_ptr<ChannelCredentials> CompositeChannelCredentials(
      const std::shared_ptr<ChannelCredentials>& channel_creds,
      const std::shared_ptr<CallCredentials>& call_creds);
  std::shared_ptr<CallCredentials> CompositeCallCredentials(
      const std::shared_ptr<CallCredentials>& creds1,
      const std::shared_ptr<CallCredentials>& creds2);
  std::shared_ptr<ChannelCredentials> InsecureChannelCredentials();
  class MetadataCredentialsPlugin {
   public:
    ~MetadataCredentialsPlugin() {}
    virtual bool IsBlocking() const { return true; }
    virtual const char* GetType() const { return ""; }
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
  std::shared_ptr<Channel> CreateCustomChannel(
      const grpc::string& target,
      const std::shared_ptr<grpc::ChannelCredentials>& creds,
      const grpc::ChannelArguments& args);
  namespace experimental {
  std::shared_ptr<grpc::Channel> CreateCustomChannelWithInterceptors(
      const grpc::string& target,
      const std::shared_ptr<grpc::ChannelCredentials>& creds,
      const grpc::ChannelArguments& args,
      std::vector<std::unique_ptr<
          grpc::experimental::ClientInterceptorFactoryInterface>>
          interceptor_creators);
  grpc::Status StsCredentialsOptionsFromJson(const grpc::string& json_string,
                                             StsCredentialsOptions* options);
  grpc::Status StsCredentialsOptionsFromEnv(StsCredentialsOptions* options);
  std::shared_ptr<CallCredentials> StsCredentials(
      const StsCredentialsOptions& options);
  std::shared_ptr<CallCredentials> MetadataCredentialsFromPlugin(
      std::unique_ptr<MetadataCredentialsPlugin> plugin,
      grpc_security_level min_security_level);
  struct AltsCredentialsOptions {
    std::vector<grpc::string> target_service_accounts;
  };
  std::shared_ptr<ChannelCredentials> AltsCredentials(
      const AltsCredentialsOptions& options);
  std::shared_ptr<ChannelCredentials> LocalCredentials(
      grpc_local_connect_type type);
  std::shared_ptr<ChannelCredentials> TlsCredentials(
      const TlsCredentialsOptions& options);
  }
  class ChannelCredentials : private grpc::GrpcLibraryCodegen {
   public:
    ChannelCredentials();
    ~ChannelCredentials();
   protected:
    friend std::shared_ptr<ChannelCredentials> CompositeChannelCredentials(
        const std::shared_ptr<ChannelCredentials>& channel_creds,
        const std::shared_ptr<CallCredentials>& call_creds);
    virtual SecureChannelCredentials* AsSecureCredentials() = 0;
   private:
    friend std::shared_ptr<grpc::Channel> CreateCustomChannel(
        const grpc::string& target,
        const std::shared_ptr<grpc::ChannelCredentials>& creds,
        const grpc::ChannelArguments& args);
    friend std::shared_ptr<grpc::Channel>
    grpc::experimental::CreateCustomChannelWithInterceptors(
        const grpc::string& target,
        const std::shared_ptr<grpc::ChannelCredentials>& creds,
        const grpc::ChannelArguments& args,
        std::vector<std::unique_ptr<
            grpc::experimental::ClientInterceptorFactoryInterface>>
            interceptor_creators);
    virtual std::shared_ptr<Channel> CreateChannelImpl(
        const grpc::string& target, const ChannelArguments& args) = 0;
    virtual std::shared_ptr<Channel> CreateChannelWithInterceptors(
        const grpc::string& , const ChannelArguments& ,
        std::vector<std::unique_ptr<
            grpc::experimental::ClientInterceptorFactoryInterface>>
                                ) {
      return nullptr;
    }
  };
  class CallCredentials : private grpc::GrpcLibraryCodegen {
   public:
    CallCredentials();
    ~CallCredentials();
    virtual bool ApplyToCall(grpc_call* call) = 0;
    virtual grpc::string DebugString() {
      return "CallCredentials did not provide a debug string";
    }
   protected:
    friend std::shared_ptr<ChannelCredentials> CompositeChannelCredentials(
        const std::shared_ptr<ChannelCredentials>& channel_creds,
        const std::shared_ptr<CallCredentials>& call_creds);
    friend std::shared_ptr<CallCredentials> CompositeCallCredentials(
        const std::shared_ptr<CallCredentials>& creds1,
        const std::shared_ptr<CallCredentials>& creds2);
    virtual SecureCallCredentials* AsSecureCredentials() = 0;
  };
  struct SslCredentialsOptions {
    grpc::string pem_root_certs;
    grpc::string pem_private_key;
    grpc::string pem_cert_chain;
  };
  std::shared_ptr<ChannelCredentials> GoogleDefaultCredentials();
  std::shared_ptr<ChannelCredentials> SslCredentials(
      const SslCredentialsOptions& options);
  std::shared_ptr<CallCredentials> GoogleComputeEngineCredentials();
}
#endif
