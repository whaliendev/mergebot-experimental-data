#ifndef GRPCPP_SECURITY_SERVER_CREDENTIALS_H
#define GRPCPP_SECURITY_SERVER_CREDENTIALS_H 
#include <memory>
#include <vector>
#include <grpc/grpc_security_constants.h>
#include <grpcpp/security/auth_metadata_processor.h>
#include <grpcpp/security/tls_credentials_options.h>
#include <grpcpp/support/config.h>
struct grpc_server;
namespace grpc_impl {
class Server;
}
namespace grpc {
struct SslServerCredentialsOptions {
  SslServerCredentialsOptions()
      : force_client_auth(false),
        client_certificate_request(GRPC_SSL_DONT_REQUEST_CLIENT_CERTIFICATE) {}
  SslServerCredentialsOptions(
      grpc_ssl_client_certificate_request_type request_type)
      : force_client_auth(false), client_certificate_request(request_type) {}
  struct PemKeyCertPair {
    std::string private_key;
    std::string cert_chain;
  };
  std::string pem_root_certs;
  std::vector<PemKeyCertPair> pem_key_cert_pairs;
  bool force_client_auth;
  grpc_ssl_client_certificate_request_type client_certificate_request;
};
class ServerCredentials {
 public:
  virtual ~ServerCredentials();
  virtual void SetAuthMetadataProcessor(
      const std::shared_ptr<grpc::AuthMetadataProcessor>& processor) = 0;
 private:
  friend class ::grpc_impl::Server;
  virtual int AddPortToServer(const std::string& addr, grpc_server* server) = 0;
};
std::shared_ptr<ServerCredentials> SslServerCredentials(
    const grpc::SslServerCredentialsOptions& options);
std::shared_ptr<ServerCredentials> InsecureServerCredentials();
namespace experimental {
struct AltsServerCredentialsOptions {
};
std::shared_ptr<ServerCredentials> AltsServerCredentials(
    const AltsServerCredentialsOptions& options);
std::shared_ptr<ServerCredentials> AltsServerCredentials(
    const AltsServerCredentialsOptions& options);
std::shared_ptr<ServerCredentials> LocalServerCredentials(
    grpc_local_connect_type type);
std::shared_ptr<ServerCredentials> TlsServerCredentials(
    const experimental::TlsCredentialsOptions& options);
}
}
#endif
