#ifndef GRPCPP_SECURITY_SERVER_CREDENTIALS_H
#define GRPCPP_SECURITY_SERVER_CREDENTIALS_H 
#include <memory>
#include <vector>
#include <grpc/grpc_security_constants.h>
#include <grpcpp/security/auth_metadata_processor.h>
#include <grpcpp/security/tls_credentials_options.h>
#include <grpcpp/support/config.h>
struct grpc_server
#include <grpcpp/security/server_credentials_impl.h>
    namespace grpc_impl {
  class Server;
}
namespace grpc {
std::shared_ptr<ServerCredentials> SslServerCredentials(
    const SslServerCredentialsOptions& options);
std::shared_ptr<ServerCredentials> InsecureServerCredentials();
namespace experimental {
std::shared_ptr<ServerCredentials> AltsServerCredentials(
    const AltsServerCredentialsOptions& options);
std::shared_ptr<ServerCredentials> LocalServerCredentials(
    grpc_local_connect_type type);
std::shared_ptr<ServerCredentials> TlsServerCredentials(
    const ::grpc_impl::experimental::TlsCredentialsOptions& options);
std::shared_ptr<ServerCredentials> TlsServerCredentials(
    const ::grpc::experimental::TlsCredentialsOptions& options);
struct AltsServerCredentialsOptions {};
std::shared_ptr<ServerCredentials> AltsServerCredentials(
    const AltsServerCredentialsOptions& options);
}
struct SslServerCredentialsOptions;
class ServerCredentials {
 public:
  virtual ~ServerCredentials();
  virtual void SetAuthMetadataProcessor(
      const std::shared_ptr<grpc::AuthMetadataProcessor>& processor) = 0;
 private:
  friend class ::grpc_impl::Server;
  virtual int AddPortToServer(const std::string& addr, grpc_server* server) = 0;
};
}
#endif
