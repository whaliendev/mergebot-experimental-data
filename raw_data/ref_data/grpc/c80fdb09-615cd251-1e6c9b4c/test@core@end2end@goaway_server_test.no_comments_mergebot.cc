#include "src/core/lib/iomgr/sockaddr.h"
#include "src/core/lib/iomgr/socket_utils.h"
#include <grpc/grpc.h>
#include <grpc/support/alloc.h>
#include <grpc/support/log.h>
#include <grpc/support/string_util.h>
#include <string.h>
#include "src/core/ext/filters/client_channel/resolver/dns/c_ares/grpc_ares_wrapper.h"
#include "src/core/ext/filters/client_channel/server_address.h"
#include "src/core/lib/iomgr/resolve_address.h"
#include "test/core/end2end/cq_verifier.h"
#include "test/core/util/port.h"
#include "test/core/util/test_config.h"
extern grpc_address_resolver_vtable* grpc_resolve_address_impl;
static grpc_address_resolver_vtable* default_resolver;
static void* tag(intptr_t i) { return (void*)i; }
static gpr_mu g_mu;
static int g_resolve_port = -1;
static grpc_ares_request* (*iomgr_dns_lookup_ares_locked)(
    const char* dns_server, const char* addr, const char* default_port,
    grpc_pollset_set* interested_parties, grpc_closure* on_done,
    std::unique_ptr<grpc_core::ServerAddressList>* addresses,
    std::unique_ptr<grpc_core::ServerAddressList>* balancer_addresses,
    char** service_config_json, int query_timeout_ms,
    std::shared_ptr<grpc_core::WorkSerializer> combiner);
static void (*iomgr_cancel_ares_request_locked)(grpc_ares_request* request);
static void set_resolve_port(int port) {
  gpr_mu_lock(&g_mu);
  g_resolve_port = port;
  gpr_mu_unlock(&g_mu);
}
static void my_resolve_address(const char* addr, const char* default_port,
                               grpc_pollset_set* interested_parties,
                               grpc_closure* on_done,
                               grpc_resolved_addresses** addrs) {
  if (0 != strcmp(addr, "test")) {
    default_resolver->resolve_address(addr, default_port, interested_parties,
                                      on_done, addrs);
    return;
  }
  grpc_error* error = GRPC_ERROR_NONE;
  gpr_mu_lock(&g_mu);
  if (g_resolve_port < 0) {
    gpr_mu_unlock(&g_mu);
    error = GRPC_ERROR_CREATE_FROM_STATIC_STRING("Forced Failure");
  } else {
    *addrs = static_cast<grpc_resolved_addresses*>(gpr_malloc(sizeof(**addrs)));
    (*addrs)->naddrs = 1;
    (*addrs)->addrs = static_cast<grpc_resolved_address*>(
        gpr_malloc(sizeof(*(*addrs)->addrs)));
    memset((*addrs)->addrs, 0, sizeof(*(*addrs)->addrs));
    grpc_sockaddr_in* sa =
        reinterpret_cast<grpc_sockaddr_in*>((*addrs)->addrs[0].addr);
    sa->sin_family = GRPC_AF_INET;
    sa->sin_addr.s_addr = 0x100007f;
    sa->sin_port = grpc_htons(static_cast<uint16_t>(g_resolve_port));
    (*addrs)->addrs[0].len = static_cast<socklen_t>(sizeof(*sa));
    gpr_mu_unlock(&g_mu);
  }
  grpc_core::ExecCtx::Run(DEBUG_LOCATION, on_done, error);
}
static grpc_error* my_blocking_resolve_address(
    const char* name, const char* default_port,
    grpc_resolved_addresses** addresses) {
  return default_resolver->blocking_resolve_address(name, default_port,
                                                    addresses);
}
static grpc_address_resolver_vtable test_resolver = {
    my_resolve_address, my_blocking_resolve_address};
static grpc_ares_request* my_dns_lookup_ares_locked(
    const char* dns_server, const char* addr, const char* default_port,
    grpc_pollset_set* interested_parties, grpc_closure* on_done,
    std::unique_ptr<grpc_core::ServerAddressList>* addresses,
    std::unique_ptr<grpc_core::ServerAddressList>* balancer_addresses,
    bool check_grpclb, char** service_config_json, int query_timeout_ms,
    std::shared_ptr<grpc_core::WorkSerializer> combiner,
    grpc_core::Combiner* combiner) {
  if (0 != strcmp(addr, "test")) {
<<<<<<< HEAD
    return iomgr_dns_lookup_ares_locked(dns_server, addr, default_port,
                                        interested_parties, on_done, addresses,
                                        check_grpclb, service_config_json,
                                        query_timeout_ms, std::move(combiner));
|||||||
    return iomgr_dns_lookup_ares_locked(
        dns_server, addr, default_port, interested_parties, on_done, addresses,
        check_grpclb, service_config_json, query_timeout_ms, combiner);
=======
    return iomgr_dns_lookup_ares_locked(
        dns_server, addr, default_port, interested_parties, on_done, addresses,
        balancer_addresses, service_config_json, query_timeout_ms, combiner);
>>>>>>> 615cd251b8de723a9ff75ac46db54f5de7bd2ec8
  }
  grpc_error* error = GRPC_ERROR_NONE;
  gpr_mu_lock(&g_mu);
  if (g_resolve_port < 0) {
    gpr_mu_unlock(&g_mu);
    error = GRPC_ERROR_CREATE_FROM_STATIC_STRING("Forced Failure");
  } else {
    *addresses = absl::make_unique<grpc_core::ServerAddressList>();
    grpc_sockaddr_in sa;
    sa.sin_family = GRPC_AF_INET;
    sa.sin_addr.s_addr = 0x100007f;
    sa.sin_port = grpc_htons(static_cast<uint16_t>(g_resolve_port));
    (*addresses)->emplace_back(&sa, sizeof(sa), nullptr);
    gpr_mu_unlock(&g_mu);
  }
  grpc_core::ExecCtx::Run(DEBUG_LOCATION, on_done, error);
  return nullptr;
}
static void my_cancel_ares_request_locked(grpc_ares_request* request) {
  if (request != nullptr) {
    iomgr_cancel_ares_request_locked(request);
  }
}
int main(int argc, char** argv) {
  grpc::testing::TestEnvironment env(argc, argv);
  grpc_init();
  gpr_mu_init(&g_mu);
  g_combiner = grpc_combiner_create();
  grpc_set_resolver_impl(&test_resolver);
  grpc_dns_lookup_ares_locked = my_dns_lookup_ares_locked;
  grpc_cancel_ares_request_locked = my_cancel_ares_request_locked;
  {
    grpc_core::ExecCtx exec_ctx;
    ResultHandler* result_handler = new ResultHandler();
    grpc_core::OrphanablePtr<grpc_core::Resolver> resolver = create_resolver(
        "dns:test",
        std::unique_ptr<grpc_core::Resolver::ResultHandler>(result_handler));
    ResultHandler::ResolverOutput output1;
    result_handler->SetOutput(&output1);
    resolver->StartLocked();
    grpc_core::ExecCtx::Get()->Flush();
    GPR_ASSERT(wait_loop(5, &output1.ev));
    GPR_ASSERT(output1.result.addresses.empty());
    GPR_ASSERT(output1.error != GRPC_ERROR_NONE);
    ResultHandler::ResolverOutput output2;
    result_handler->SetOutput(&output2);
    grpc_core::ExecCtx::Get()->Flush();
    GPR_ASSERT(wait_loop(30, &output2.ev));
    GPR_ASSERT(!output2.result.addresses.empty());
    GPR_ASSERT(output2.error == GRPC_ERROR_NONE);
    GRPC_COMBINER_UNREF(g_combiner, "test");
  }
  grpc_shutdown();
  gpr_mu_destroy(&g_mu);
}
