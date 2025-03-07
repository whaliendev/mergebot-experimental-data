#include <cstring>
#include <grpc/grpc.h>
#include <grpc/support/log.h>
#include "src/core/ext/filters/client_channel/resolver/dns/c_ares/grpc_ares_wrapper.h"
#include "src/core/ext/filters/client_channel/resolver_registry.h"
#include "src/core/ext/filters/client_channel/server_address.h"
#include "src/core/lib/channel/channel_args.h"
#include "src/core/lib/gprpp/memory.h"
#include "src/core/lib/iomgr/sockaddr_utils.h"
#include "src/core/lib/iomgr/work_serializer.h"
#include "test/core/util/test_config.h"
constexpr int kMinResolutionPeriodMs = 1000;
constexpr int kMinResolutionPeriodForCheckMs = 900;
extern grpc_address_resolver_vtable* grpc_resolve_address_impl;
static grpc_address_resolver_vtable* default_resolve_address;
static std::shared_ptr<grpc_core::WorkSerializer>* g_work_serializer;
static grpc_ares_request* (*g_default_dns_lookup_ares_locked)(
    const char* dns_server, const char* name, const char* default_port,
    grpc_pollset_set* interested_parties, grpc_closure* on_done,
    std::unique_ptr<grpc_core::ServerAddressList>* addresses,
    std::unique_ptr<grpc_core::ServerAddressList>* balancer_addresses,
    char** service_config_json, int query_timeout_ms,
    std::shared_ptr<grpc_core::WorkSerializer> work_serializer);
static int g_resolution_count;
static struct iomgr_args {
  gpr_event ev;
  gpr_atm done_atm;
  gpr_mu* mu;
  grpc_pollset* pollset;
  grpc_pollset_set* pollset_set;
} g_iomgr_args;
static void test_resolve_address_impl(const char* name,
                                      const char* default_port,
                                      grpc_pollset_set* ,
                                      grpc_closure* on_done,
                                      grpc_resolved_addresses** addrs) {
  default_resolve_address->resolve_address(
      name, default_port, g_iomgr_args.pollset_set, on_done, addrs);
  ++g_resolution_count;
  static grpc_millis last_resolution_time = 0;
  if (last_resolution_time == 0) {
    last_resolution_time =
        grpc_timespec_to_millis_round_up(gpr_now(GPR_CLOCK_MONOTONIC));
  } else {
    grpc_millis now =
        grpc_timespec_to_millis_round_up(gpr_now(GPR_CLOCK_MONOTONIC));
    GPR_ASSERT(now - last_resolution_time >= kMinResolutionPeriodForCheckMs);
    last_resolution_time = now;
  }
}
static grpc_error* test_blocking_resolve_address_impl(
    const char* name, const char* default_port,
    grpc_resolved_addresses** addresses) {
  return default_resolve_address->blocking_resolve_address(name, default_port,
                                                           addresses);
}
static grpc_address_resolver_vtable test_resolver = {
    test_resolve_address_impl, test_blocking_resolve_address_impl};
static grpc_ares_request* test_dns_lookup_ares_locked(
    const char* dns_server, const char* name, const char* default_port,
    grpc_pollset_set* , grpc_closure* on_done,
    std::unique_ptr<grpc_core::ServerAddressList>* addresses,
    std::unique_ptr<grpc_core::ServerAddressList>* balancer_addresses,
    char** service_config_json, int query_timeout_ms,
    std::shared_ptr<grpc_core::WorkSerializer> work_serializer) {
  grpc_ares_request* result = g_default_dns_lookup_ares_locked(
      dns_server, name, default_port, g_iomgr_args.pollset_set, on_done,
grpc_ares_request* result = g_default_dns_lookup_ares_locked( dns_server, name, default_port, g_iomgr_args.pollset_set, on_done, addresses, balancer_addresses, service_config_json, query_timeout_ms, std::move(work_serializer)); ++g_resolution_count;
  ++g_resolution_count;
  static grpc_millis last_resolution_time = 0;
  grpc_millis now =
      grpc_timespec_to_millis_round_up(gpr_now(GPR_CLOCK_MONOTONIC));
  gpr_log(GPR_DEBUG,
          "last_resolution_time:%" PRId64 " now:%" PRId64
          " min_time_between:%d",
          last_resolution_time, now, kMinResolutionPeriodForCheckMs);
  if (last_resolution_time == 0) {
    last_resolution_time =
        grpc_timespec_to_millis_round_up(gpr_now(GPR_CLOCK_MONOTONIC));
  } else {
    GPR_ASSERT(now - last_resolution_time >= kMinResolutionPeriodForCheckMs);
    last_resolution_time = now;
  }
  return result;
}
static gpr_timespec test_deadline(void) {
  return grpc_timeout_seconds_to_deadline(100);
}
static void do_nothing(void* , grpc_error* ) {}
static void iomgr_args_init(iomgr_args* args) {
  gpr_event_init(&args->ev);
  args->pollset = static_cast<grpc_pollset*>(gpr_zalloc(grpc_pollset_size()));
  grpc_pollset_init(args->pollset, &args->mu);
  args->pollset_set = grpc_pollset_set_create();
  grpc_pollset_set_add_pollset(args->pollset_set, args->pollset);
  gpr_atm_rel_store(&args->done_atm, 0);
}
static void iomgr_args_finish(iomgr_args* args) {
  GPR_ASSERT(gpr_event_wait(&args->ev, test_deadline()));
  grpc_pollset_set_del_pollset(args->pollset_set, args->pollset);
  grpc_pollset_set_destroy(args->pollset_set);
  grpc_closure do_nothing_cb;
  GRPC_CLOSURE_INIT(&do_nothing_cb, do_nothing, nullptr,
                    grpc_schedule_on_exec_ctx);
  gpr_mu_lock(args->mu);
  grpc_pollset_shutdown(args->pollset, &do_nothing_cb);
  gpr_mu_unlock(args->mu);
  grpc_core::ExecCtx::Get()->Flush();
  grpc_pollset_destroy(args->pollset);
  gpr_free(args->pollset);
}
static grpc_millis n_sec_deadline(int seconds) {
  return grpc_timespec_to_millis_round_up(
      grpc_timeout_seconds_to_deadline(seconds));
}
static void poll_pollset_until_request_done(iomgr_args* args) {
  grpc_core::ExecCtx exec_ctx;
  grpc_millis deadline = n_sec_deadline(10);
  while (true) {
    bool done = gpr_atm_acq_load(&args->done_atm) != 0;
    if (done) {
      break;
    }
    grpc_millis time_left = deadline - grpc_core::ExecCtx::Get()->Now();
    gpr_log(GPR_DEBUG, "done=%d, time_left=%" PRId64, done, time_left);
    GPR_ASSERT(time_left >= 0);
    grpc_pollset_worker* worker = nullptr;
    gpr_mu_lock(args->mu);
    GRPC_LOG_IF_ERROR("pollset_work", grpc_pollset_work(args->pollset, &worker,
                                                        n_sec_deadline(1)));
    gpr_mu_unlock(args->mu);
    grpc_core::ExecCtx::Get()->Flush();
  }
  gpr_event_set(&args->ev, (void*)1);
}
struct OnResolutionCallbackArg;
class ResultHandler : public grpc_core::Resolver::ResultHandler {
 public:
  using ResultCallback = void (*)(OnResolutionCallbackArg* state);
  void SetCallback(ResultCallback result_cb, OnResolutionCallbackArg* state) {
    GPR_ASSERT(result_cb_ == nullptr);
    result_cb_ = result_cb;
    GPR_ASSERT(state_ == nullptr);
    state_ = state;
  }
  void ReturnResult(grpc_core::Resolver::Result ) override {
    GPR_ASSERT(result_cb_ != nullptr);
    GPR_ASSERT(state_ != nullptr);
    ResultCallback cb = result_cb_;
    OnResolutionCallbackArg* state = state_;
    result_cb_ = nullptr;
    state_ = nullptr;
    cb(state);
  }
  void ReturnError(grpc_error* error) override {
    gpr_log(GPR_ERROR, "resolver returned error: %s", grpc_error_string(error));
    GPR_ASSERT(false);
  }
 private:
  ResultCallback result_cb_ = nullptr;
  OnResolutionCallbackArg* state_ = nullptr;
};
struct OnResolutionCallbackArg {
  const char* uri_str = nullptr;
  grpc_core::OrphanablePtr<grpc_core::Resolver> resolver;
  ResultHandler* result_handler;
};
static bool g_all_callbacks_invoked;
static void on_fourth_resolution(OnResolutionCallbackArg* cb_arg) {
  gpr_log(GPR_INFO, "4th: g_resolution_count: %d", g_resolution_count);
  GPR_ASSERT(g_resolution_count == 4);
  cb_arg->resolver.reset();
  gpr_atm_rel_store(&g_iomgr_args.done_atm, 1);
  gpr_mu_lock(g_iomgr_args.mu);
  GRPC_LOG_IF_ERROR("pollset_kick",
                    grpc_pollset_kick(g_iomgr_args.pollset, nullptr));
  gpr_mu_unlock(g_iomgr_args.mu);
  delete cb_arg;
  g_all_callbacks_invoked = true;
}
static void on_third_resolution(OnResolutionCallbackArg* cb_arg) {
  gpr_log(GPR_INFO, "3rd: g_resolution_count: %d", g_resolution_count);
  GPR_ASSERT(g_resolution_count == 3);
  cb_arg->result_handler->SetCallback(on_fourth_resolution, cb_arg);
  cb_arg->resolver->RequestReresolutionLocked();
  gpr_mu_lock(g_iomgr_args.mu);
  GRPC_LOG_IF_ERROR("pollset_kick",
                    grpc_pollset_kick(g_iomgr_args.pollset, nullptr));
  gpr_mu_unlock(g_iomgr_args.mu);
}
static void on_second_resolution(OnResolutionCallbackArg* cb_arg) {
  gpr_log(GPR_INFO, "2nd: g_resolution_count: %d", g_resolution_count);
  GPR_ASSERT(g_resolution_count == 2);
  cb_arg->result_handler->SetCallback(on_third_resolution, cb_arg);
  cb_arg->resolver->RequestReresolutionLocked();
  gpr_mu_lock(g_iomgr_args.mu);
  GRPC_LOG_IF_ERROR("pollset_kick",
                    grpc_pollset_kick(g_iomgr_args.pollset, nullptr));
  gpr_mu_unlock(g_iomgr_args.mu);
}
static void on_first_resolution(OnResolutionCallbackArg* cb_arg) {
  gpr_log(GPR_INFO, "1st: g_resolution_count: %d", g_resolution_count);
  GPR_ASSERT(g_resolution_count == 1);
  cb_arg->result_handler->SetCallback(on_second_resolution, cb_arg);
  cb_arg->resolver->RequestReresolutionLocked();
  gpr_mu_lock(g_iomgr_args.mu);
  GRPC_LOG_IF_ERROR("pollset_kick",
                    grpc_pollset_kick(g_iomgr_args.pollset, nullptr));
  gpr_mu_unlock(g_iomgr_args.mu);
}
static void start_test_under_work_serializer(void* arg) {
  OnResolutionCallbackArg* res_cb_arg =
      static_cast<OnResolutionCallbackArg*>(arg);
  res_cb_arg->result_handler = new ResultHandler();
  grpc_core::ResolverFactory* factory =
      grpc_core::ResolverRegistry::LookupResolverFactory("dns");
  grpc_uri* uri = grpc_uri_parse(res_cb_arg->uri_str, 0);
  gpr_log(GPR_DEBUG, "test: '%s' should be valid for '%s'", res_cb_arg->uri_str,
          factory->scheme());
  GPR_ASSERT(uri != nullptr);
  grpc_core::ResolverArgs args;
  args.uri = uri;
  args.work_serializer = *g_work_serializer;
  args.result_handler = std::unique_ptr<grpc_core::Resolver::ResultHandler>(
      res_cb_arg->result_handler);
  g_resolution_count = 0;
  grpc_arg cooldown_arg = grpc_channel_arg_integer_create(
      const_cast<char*>(GRPC_ARG_DNS_MIN_TIME_BETWEEN_RESOLUTIONS_MS),
      kMinResolutionPeriodMs);
  grpc_channel_args cooldown_args = {1, &cooldown_arg};
  args.args = &cooldown_args;
  res_cb_arg->resolver = factory->CreateResolver(std::move(args));
  GPR_ASSERT(res_cb_arg->resolver != nullptr);
  grpc_uri_destroy(uri);
  res_cb_arg->result_handler->SetCallback(on_first_resolution, res_cb_arg);
  res_cb_arg->resolver->StartLocked();
}
static void test_cooldown() {
  grpc_core::ExecCtx exec_ctx;
  iomgr_args_init(&g_iomgr_args);
  OnResolutionCallbackArg* res_cb_arg = new OnResolutionCallbackArg();
  res_cb_arg->uri_str = "dns:127.0.0.1";
  (*g_work_serializer)
      ->Run([res_cb_arg]() { start_test_under_work_serializer(res_cb_arg); },
            DEBUG_LOCATION);
  grpc_core::ExecCtx::Get()->Flush();
  poll_pollset_until_request_done(&g_iomgr_args);
  iomgr_args_finish(&g_iomgr_args);
}
int main(int argc, char** argv) {
  grpc::testing::TestEnvironment env(argc, argv);
  grpc_init();
  auto work_serializer = std::make_shared<grpc_core::WorkSerializer>();
  g_work_serializer = &work_serializer;
  g_default_dns_lookup_ares_locked = grpc_dns_lookup_ares_locked;
  grpc_dns_lookup_ares_locked = test_dns_lookup_ares_locked;
  default_resolve_address = grpc_resolve_address_impl;
  grpc_set_resolver_impl(&test_resolver);
  test_cooldown();
  grpc_shutdown_blocking();
  GPR_ASSERT(g_all_callbacks_invoked);
  return 0;
}
