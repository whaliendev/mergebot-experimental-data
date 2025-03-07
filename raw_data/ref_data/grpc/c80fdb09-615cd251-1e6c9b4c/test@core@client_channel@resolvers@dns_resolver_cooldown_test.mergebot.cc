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

#include <cstring>
#include <grpc/grpc.h>
#include <grpc/support/log.h>
#include "src/core/ext/filters/client_channel/resolver/dns/c_ares/grpc_ares_wrapper.h"
#include "src/core/ext/filters/client_channel/resolver_registry.h"
#include "src/core/ext/filters/client_channel/server_address.h"
#include "src/core/lib/channel/channel_args.h"
#include "src/core/lib/gprpp/memory.h"
#include "src/core/lib/iomgr/combiner.h"
#include "src/core/lib/iomgr/sockaddr_utils.h"
#include "src/core/lib/iomgr/work_serializer.h"
#include "test/core/util/test_config.h"

constexpr int kMinResolutionPeriodMs = 1000;
// Provide some slack when checking intervals, to allow for test timing issues.
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

// Counter incremented by test_resolve_address_impl indicating the number of
// times a system-level resolution has happened.
static int g_resolution_count;

static struct iomgr_args {
  gpr_event ev;
  gpr_atm done_atm;
  gpr_mu* mu;
  grpc_pollset* pollset;
  grpc_pollset_set* pollset_set;
} g_iomgr_args;

// Wrapper around default resolve_address in order to count the number of
// times we incur in a system-level name resolution.
static void test_resolve_address_impl(const char* name,
                                      const char* default_port,
                                      grpc_pollset_set* /*interested_parties*/,
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
    grpc_pollset_set* /*interested_parties*/, grpc_closure* on_done,
    std::unique_ptr<grpc_core::ServerAddressList>* addresses,
    std::unique_ptr<grpc_core::ServerAddressList>* balancer_addresses,
    bool check_grpclb, char** service_config_json, int query_timeout_ms,
    std::shared_ptr<grpc_core::WorkSerializer> work_serializer,
    grpc_core::Combiner* combiner) {
  grpc_ares_request* result = g_default_dns_lookup_ares_locked(
      dns_server, name, default_port, g_iomgr_args.pollset_set, on_done,
<<<<<<< HEAD
      addresses, check_grpclb, service_config_json, query_timeout_ms,
      std::move(work_serializer));
|||||||
      addresses, check_grpclb, service_config_json, query_timeout_ms, combiner);
=======
      addresses, balancer_addresses, service_config_json, query_timeout_ms,
      combiner);
>>>>>>> 615cd251b8de723a9ff75ac46db54f5de7bd2ec8
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

static void do_nothing(void* /*arg*/, grpc_error* /*error*/) {}

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
  // exec_ctx needs to be flushed before calling grpc_pollset_destroy()
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
  gpr_atm output_ = 0;  // ResolverOutput*
};

struct OnResolutionCallbackArg {
  const char* uri_str = nullptr;
  grpc_core::OrphanablePtr<grpc_core::Resolver> resolver;
  ResultHandler* result_handler;
};

// Set to true by the last callback in the resolution chain.
static bool g_all_callbacks_invoked;

// It's interesting to run a few rounds of this test because as
// we run more rounds, the base starting time
// (i.e. ExecCtx g_start_time) gets further and further away
// from "Now()". Thus the more rounds ran, the more highlighted the
// difference is between absolute and relative times values.
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
  // The resolution callback was not invoked until new data was
  // available, which was delayed until after the cooldown period.
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
  // There's one initial system-level resolution and one invocation of a
  // notification callback (the current function).
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
  // First resolution, would incur in system-level resolution.
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