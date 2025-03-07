#include <string.h>
#include <grpc/grpc.h>
#include <grpc/support/alloc.h>
#include "src/core/ext/filters/client_channel/resolver.h"
#include "src/core/ext/filters/client_channel/resolver/dns/c_ares/grpc_ares_wrapper.h"
#include "src/core/ext/filters/client_channel/resolver_registry.h"
#include "src/core/ext/filters/client_channel/server_address.h"
#include "src/core/lib/channel/channel_args.h"
#include "src/core/lib/iomgr/combiner.h"
#include "src/core/lib/iomgr/resolve_address.h"
#include "src/core/lib/iomgr/timer.h"
#include "src/core/lib/iomgr/work_serializer.h"
#include "test/core/util/test_config.h"
static gpr_mu g_mu;
static bool g_fail_resolution = true;
static std::shared_ptr<grpc_core::WorkSerializer>* g_work_serializer;
static void my_resolve_address(const char* addr, const char* ,
                               grpc_pollset_set* ,
                               grpc_closure* on_done,
                               grpc_resolved_addresses** addrs) {
  gpr_mu_lock(&g_mu);
  GPR_ASSERT(0 == strcmp("test", addr));
  grpc_error* error = GRPC_ERROR_NONE;
  if (g_fail_resolution) {
    g_fail_resolution = false;
    gpr_mu_unlock(&g_mu);
    error = GRPC_ERROR_CREATE_FROM_STATIC_STRING("Forced Failure");
  } else {
    gpr_mu_unlock(&g_mu);
    *addrs = static_cast<grpc_resolved_addresses*>(gpr_malloc(sizeof(**addrs)));
    (*addrs)->naddrs = 1;
    (*addrs)->addrs = static_cast<grpc_resolved_address*>(
        gpr_malloc(sizeof(*(*addrs)->addrs)));
    (*addrs)->addrs[0].len = 123;
  }
  grpc_core::ExecCtx::Run(DEBUG_LOCATION, on_done, error);
}
static grpc_address_resolver_vtable test_resolver = {my_resolve_address,
                                                     nullptr};
static grpc_ares_request* my_dns_lookup_ares_locked(
    const char* , const char* addr, const char* ,
    grpc_pollset_set* , grpc_closure* on_done,
    std::unique_ptr<grpc_core::ServerAddressList>* addresses,
    bool , char** ,
    int ,
    std::shared_ptr<
        grpc_core::WorkSerializer> ) {
  gpr_mu_lock(&g_mu);
  GPR_ASSERT(0 == strcmp("test", addr));
  grpc_error* error = GRPC_ERROR_NONE;
  if (g_fail_resolution) {
    g_fail_resolution = false;
    gpr_mu_unlock(&g_mu);
    error = GRPC_ERROR_CREATE_FROM_STATIC_STRING("Forced Failure");
  } else {
    gpr_mu_unlock(&g_mu);
    *addresses = absl::make_unique<grpc_core::ServerAddressList>();
    grpc_resolved_address dummy_resolved_address;
    memset(&dummy_resolved_address, 0, sizeof(dummy_resolved_address));
    dummy_resolved_address.len = 123;
    (*addresses)->emplace_back(dummy_resolved_address, nullptr);
  }
  grpc_core::ExecCtx::Run(DEBUG_LOCATION, on_done, error);
  return nullptr;
}
static void my_cancel_ares_request_locked(grpc_ares_request* request) {
  GPR_ASSERT(request == nullptr);
}
static grpc_core::OrphanablePtr<grpc_core::Resolver> create_resolver(
    const char* name,
    std::unique_ptr<grpc_core::Resolver::ResultHandler> result_handler) {
  grpc_core::ResolverFactory* factory =
      grpc_core::ResolverRegistry::LookupResolverFactory("dns");
  grpc_uri* uri = grpc_uri_parse(name, 0);
  GPR_ASSERT(uri);
  grpc_core::ResolverArgs args;
  args.uri = uri;
  args.work_serializer = *g_work_serializer;
  args.result_handler = std::move(result_handler);
  grpc_core::OrphanablePtr<grpc_core::Resolver> resolver =
      factory->CreateResolver(std::move(args));
  grpc_uri_destroy(uri);
  return resolver;
}
class ResultHandler : public grpc_core::Resolver::ResultHandler {
 public:
  struct ResolverOutput {
    grpc_core::Resolver::Result result;
    grpc_error* error = nullptr;
    gpr_event ev;
    ResolverOutput() { gpr_event_init(&ev); }
    ~ResolverOutput() { GRPC_ERROR_UNREF(error); }
  };
  void SetOutput(ResolverOutput* output) {
    gpr_atm_rel_store(&output_, reinterpret_cast<gpr_atm>(output));
  }
  void ReturnResult(grpc_core::Resolver::Result result) override {
    ResolverOutput* output =
        reinterpret_cast<ResolverOutput*>(gpr_atm_acq_load(&output_));
    GPR_ASSERT(output != nullptr);
    output->result = std::move(result);
    output->error = GRPC_ERROR_NONE;
    gpr_event_set(&output->ev, (void*)1);
  }
  void ReturnError(grpc_error* error) override {
    ResolverOutput* output =
        reinterpret_cast<ResolverOutput*>(gpr_atm_acq_load(&output_));
    GPR_ASSERT(output != nullptr);
    output->error = error;
    gpr_event_set(&output->ev, (void*)1);
  }
 private:
  gpr_atm output_ = 0;
};
static bool wait_loop(int deadline_seconds, gpr_event* ev) {
  while (deadline_seconds) {
    gpr_log(GPR_DEBUG, "Test: waiting for %d more seconds", deadline_seconds);
    if (gpr_event_wait(ev, grpc_timeout_seconds_to_deadline(1))) return true;
    deadline_seconds--;
    grpc_core::ExecCtx exec_ctx;
    grpc_timer_check(nullptr);
  }
  return false;
}
int main(int argc, char** argv) {
  grpc::testing::TestEnvironment env(argc, argv);
  grpc_init();
  gpr_mu_init(&g_mu);
  auto work_serializer = std::make_shared<grpc_core::WorkSerializer>();
  g_work_serializer = &work_serializer;
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
  }
  grpc_shutdown();
  gpr_mu_destroy(&g_mu);
}
