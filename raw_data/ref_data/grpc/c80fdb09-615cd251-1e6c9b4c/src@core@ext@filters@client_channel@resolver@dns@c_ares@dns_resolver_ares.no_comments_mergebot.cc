#include <grpc/support/port_platform.h>
#if GRPC_ARES == 1
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <grpc/support/alloc.h>
#include <grpc/support/string_util.h>
#include <address_sorting/address_sorting.h>
#include "src/core/ext/filters/client_channel/http_connect_handshaker.h"
#include "src/core/ext/filters/client_channel/lb_policy/grpclb/grpclb_balancer_addresses.h"
#include "src/core/ext/filters/client_channel/lb_policy_registry.h"
#include "src/core/ext/filters/client_channel/resolver/dns/c_ares/grpc_ares_wrapper.h"
#include "src/core/ext/filters/client_channel/resolver/dns/dns_resolver_selection.h"
#include "src/core/ext/filters/client_channel/resolver_registry.h"
#include "src/core/ext/filters/client_channel/server_address.h"
#include "src/core/ext/filters/client_channel/service_config.h"
#include "src/core/lib/backoff/backoff.h"
#include "src/core/lib/channel/channel_args.h"
#include "src/core/lib/gpr/string.h"
#include "src/core/lib/gprpp/manual_constructor.h"
#include "src/core/lib/iomgr/combiner.h"
#include "src/core/lib/iomgr/gethostname.h"
#include "src/core/lib/iomgr/iomgr_custom.h"
#include "src/core/lib/iomgr/resolve_address.h"
#include "src/core/lib/iomgr/timer.h"
#include "src/core/lib/json/json.h"
#define GRPC_DNS_INITIAL_CONNECT_BACKOFF_SECONDS 1
#define GRPC_DNS_RECONNECT_BACKOFF_MULTIPLIER 1.6
#define GRPC_DNS_RECONNECT_MAX_BACKOFF_SECONDS 120
#define GRPC_DNS_RECONNECT_JITTER 0.2
namespace grpc_core {
namespace {
const char kDefaultPort[] = "https";
class AresDnsResolver : public Resolver {
 public:
  explicit AresDnsResolver(ResolverArgs args);
  void StartLocked() override;
  void RequestReresolutionLocked() override;
  void ResetBackoffLocked() override;
  void ShutdownLocked() override;
 private:
  virtual ~AresDnsResolver();
  void MaybeStartResolvingLocked();
  void StartResolvingLocked();
  static void OnNextResolution(void* arg, grpc_error* error);
  static void OnResolved(void* arg, grpc_error* error);
  static void OnNextResolutionLocked(void* arg, grpc_error* error);
  static void OnResolvedLocked(void* arg, grpc_error* error);
  char* dns_server_;
  char* name_to_resolve_;
  grpc_channel_args* channel_args_;
  bool request_service_config_;
  grpc_pollset_set* interested_parties_;
  grpc_closure on_next_resolution_;
  grpc_closure on_resolved_;
  bool resolving_ = false;
  grpc_ares_request* pending_request_ = nullptr;
  bool have_next_resolution_timer_ = false;
  grpc_timer next_resolution_timer_;
  grpc_millis min_time_between_resolutions_;
  grpc_millis last_resolution_timestamp_ = -1;
  BackOff backoff_;
  std::unique_ptr<ServerAddressList> addresses_;
  std::unique_ptr<ServerAddressList> balancer_addresses_;
  char* service_config_json_ = nullptr;
  bool shutdown_initiated_ = false;
  int query_timeout_ms_;
  bool enable_srv_queries_;
};
AresDnsResolver::AresDnsResolver(ResolverArgs args)
    : Resolver(args.combiner, std::move(args.result_handler)),
      backoff_(
          BackOff::Options()
              .set_initial_backoff(GRPC_DNS_INITIAL_CONNECT_BACKOFF_SECONDS *
                                   1000)
              .set_multiplier(GRPC_DNS_RECONNECT_BACKOFF_MULTIPLIER)
              .set_jitter(GRPC_DNS_RECONNECT_JITTER)
              .set_max_backoff(GRPC_DNS_RECONNECT_MAX_BACKOFF_SECONDS * 1000)) {
  const char* path = args.uri->path;
  if (path[0] == '/') ++path;
  name_to_resolve_ = gpr_strdup(path);
  dns_server_ = nullptr;
  if (0 != strcmp(args.uri->authority, "")) {
    dns_server_ = gpr_strdup(args.uri->authority);
  }
  channel_args_ = grpc_channel_args_copy(args.args);
  const grpc_arg* arg = grpc_channel_args_find(
      channel_args_, GRPC_ARG_SERVICE_CONFIG_DISABLE_RESOLUTION);
  request_service_config_ = !grpc_channel_arg_get_bool(arg, true);
  arg = grpc_channel_args_find(channel_args_,
                               GRPC_ARG_DNS_MIN_TIME_BETWEEN_RESOLUTIONS_MS);
  min_time_between_resolutions_ =
      grpc_channel_arg_get_integer(arg, {1000 * 30, 0, INT_MAX});
  arg = grpc_channel_args_find(channel_args_, GRPC_ARG_DNS_ENABLE_SRV_QUERIES);
  enable_srv_queries_ = grpc_channel_arg_get_bool(arg, false);
  interested_parties_ = grpc_pollset_set_create();
  if (args.pollset_set != nullptr) {
    grpc_pollset_set_add_pollset_set(interested_parties_, args.pollset_set);
  }
  const grpc_arg* query_timeout_ms_arg =
      grpc_channel_args_find(channel_args_, GRPC_ARG_DNS_ARES_QUERY_TIMEOUT_MS);
  query_timeout_ms_ = grpc_channel_arg_get_integer(
      query_timeout_ms_arg,
      {GRPC_DNS_ARES_DEFAULT_QUERY_TIMEOUT_MS, 0, INT_MAX});
}
AresDnsResolver::~AresDnsResolver() {
  GRPC_CARES_TRACE_LOG("resolver:%p destroying AresDnsResolver", this);
  grpc_pollset_set_destroy(interested_parties_);
  gpr_free(dns_server_);
  gpr_free(name_to_resolve_);
  grpc_channel_args_destroy(channel_args_);
}
void AresDnsResolver::StartLocked() {
  GRPC_CARES_TRACE_LOG("resolver:%p AresDnsResolver::StartLocked() is called.",
                       this);
  MaybeStartResolvingLocked();
}
void AresDnsResolver::RequestReresolutionLocked() {
  if (!resolving_) {
    MaybeStartResolvingLocked();
  }
}
void AresDnsResolver::ResetBackoffLocked() {
  if (have_next_resolution_timer_) {
    grpc_timer_cancel(&next_resolution_timer_);
  }
  backoff_.Reset();
}
void AresDnsResolver::ShutdownLocked() {
  shutdown_initiated_ = true;
  if (have_next_resolution_timer_) {
    grpc_timer_cancel(&next_resolution_timer_);
  }
  if (pending_request_ != nullptr) {
    grpc_cancel_ares_request_locked(pending_request_);
  }
}
void AresDnsResolver::OnNextResolution(void* arg, grpc_error* error) {
  AresDnsResolver* r = static_cast<AresDnsResolver*>(arg);
  r->combiner()->Run(GRPC_CLOSURE_INIT(&r->on_next_resolution_,
                                       OnNextResolutionLocked, r, nullptr),
                     GRPC_ERROR_REF(error));
}
void AresDnsResolver::OnNextResolutionLocked(void* arg, grpc_error* error) {
  AresDnsResolver* r = static_cast<AresDnsResolver*>(arg);
  GRPC_CARES_TRACE_LOG(
      "resolver:%p re-resolution timer fired. error: %s. shutdown_initiated_: "
      "%d",
      r, grpc_error_string(error), r->shutdown_initiated_);
  r->have_next_resolution_timer_ = false;
  if (error == GRPC_ERROR_NONE && !r->shutdown_initiated_) {
    if (!r->resolving_) {
      GRPC_CARES_TRACE_LOG(
          "resolver:%p start resolving due to re-resolution timer", r);
      r->StartResolvingLocked();
    }
  }
  r->Unref(DEBUG_LOCATION, "next_resolution_timer");
}
bool ValueInJsonArray(const Json::Array& array, const char* value) {
  for (const Json& entry : array) {
    if (entry.type() == Json::Type::STRING && entry.string_value() == value) {
      return true;
    }
  }
  return false;
}
std::string ChooseServiceConfig(char* service_config_choice_json,
                                grpc_error** error) {
  Json json = Json::Parse(service_config_choice_json, error);
  if (*error != GRPC_ERROR_NONE) return "";
  if (json.type() != Json::Type::ARRAY) {
    *error = GRPC_ERROR_CREATE_FROM_STATIC_STRING(
        "Service Config Choices, error: should be of type array");
    return "";
  }
  const Json* service_config = nullptr;
  InlinedVector<grpc_error*, 4> error_list;
  for (const Json& choice : json.array_value()) {
    if (choice.type() != Json::Type::OBJECT) {
      error_list.push_back(GRPC_ERROR_CREATE_FROM_STATIC_STRING(
          "Service Config Choice, error: should be of type object"));
      continue;
    }
    auto it = choice.object_value().find("clientLanguage");
    if (it != choice.object_value().end()) {
      if (it->second.type() != Json::Type::ARRAY) {
        error_list.push_back(GRPC_ERROR_CREATE_FROM_STATIC_STRING(
            "field:clientLanguage error:should be of type array"));
      } else if (!ValueInJsonArray(it->second.array_value(), "c++")) {
        continue;
      }
    }
    it = choice.object_value().find("clientHostname");
    if (it != choice.object_value().end()) {
      if (it->second.type() != Json::Type::ARRAY) {
        error_list.push_back(GRPC_ERROR_CREATE_FROM_STATIC_STRING(
            "field:clientHostname error:should be of type array"));
      } else {
        char* hostname = grpc_gethostname();
        if (hostname == nullptr ||
            !ValueInJsonArray(it->second.array_value(), hostname)) {
          continue;
        }
      }
    }
    it = choice.object_value().find("percentage");
    if (it != choice.object_value().end()) {
      if (it->second.type() != Json::Type::NUMBER) {
        error_list.push_back(GRPC_ERROR_CREATE_FROM_STATIC_STRING(
            "field:percentage error:should be of type number"));
      } else {
        int random_pct = rand() % 100;
        int percentage;
        if (sscanf(it->second.string_value().c_str(), "%d", &percentage) != 1) {
          error_list.push_back(GRPC_ERROR_CREATE_FROM_STATIC_STRING(
              "field:percentage error:should be of type integer"));
        } else if (random_pct > percentage || percentage == 0) {
          continue;
        }
      }
    }
    it = choice.object_value().find("serviceConfig");
    if (it == choice.object_value().end()) {
      error_list.push_back(GRPC_ERROR_CREATE_FROM_STATIC_STRING(
          "field:serviceConfig error:required field missing"));
    } else if (it->second.type() != Json::Type::OBJECT) {
      error_list.push_back(GRPC_ERROR_CREATE_FROM_STATIC_STRING(
          "field:serviceConfig error:should be of type object"));
    } else if (service_config == nullptr) {
      service_config = &it->second;
    }
  }
  if (!error_list.empty()) {
    service_config = nullptr;
    *error = GRPC_ERROR_CREATE_FROM_VECTOR("Service Config Choices Parser",
                                           &error_list);
  }
  if (service_config == nullptr) return "";
  return service_config->Dump();
}
void AresDnsResolver::OnResolved(void* arg, grpc_error* error) {
  AresDnsResolver* r = static_cast<AresDnsResolver*>(arg);
  r->combiner()->Run(
      GRPC_CLOSURE_INIT(&r->on_resolved_, OnResolvedLocked, r, nullptr),
      GRPC_ERROR_REF(error));
}
void AresDnsResolver::OnResolvedLocked(void* arg, grpc_error* error) {
  AresDnsResolver* r = static_cast<AresDnsResolver*>(arg);
  GPR_ASSERT(r->resolving_);
  r->resolving_ = false;
  gpr_free(r->pending_request_);
  r->pending_request_ = nullptr;
  if (r->shutdown_initiated_) {
    r->Unref(DEBUG_LOCATION, "OnResolvedLocked() shutdown");
    return;
  }
  if (r->addresses_ != nullptr || r->balancer_addresses_ != nullptr) {
    Result result;
    if (r->addresses_ != nullptr) {
      result.addresses = std::move(*r->addresses_);
    }
    if (r->service_config_json_ != nullptr) {
      std::string service_config_string = ChooseServiceConfig(
          r->service_config_json_, &result.service_config_error);
      gpr_free(r->service_config_json_);
      if (result.service_config_error == GRPC_ERROR_NONE &&
          !service_config_string.empty()) {
        GRPC_CARES_TRACE_LOG("resolver:%p selected service config choice: %s",
                             r, service_config_string.c_str());
        result.service_config = ServiceConfig::Create(
            service_config_string, &result.service_config_error);
      }
    }
    InlinedVector<grpc_arg, 1> new_args;
    if (r->balancer_addresses_ != nullptr) {
      new_args.push_back(
          CreateGrpclbBalancerAddressesArg(r->balancer_addresses_.get()));
    }
    result.args = grpc_channel_args_copy_and_add(
        r->channel_args_, new_args.data(), new_args.size());
    r->result_handler()->ReturnResult(std::move(result));
    r->addresses_.reset();
    r->balancer_addresses_.reset();
    r->backoff_.Reset();
  } else {
    GRPC_CARES_TRACE_LOG("resolver:%p dns resolution failed: %s", r,
                         grpc_error_string(error));
    r->result_handler()->ReturnError(grpc_error_set_int(
        GRPC_ERROR_CREATE_REFERENCING_FROM_STATIC_STRING(
            "DNS resolution failed", &error, 1),
        GRPC_ERROR_INT_GRPC_STATUS, GRPC_STATUS_UNAVAILABLE));
    grpc_millis next_try = r->backoff_.NextAttemptTime();
    grpc_millis timeout = next_try - ExecCtx::Get()->Now();
    GRPC_CARES_TRACE_LOG("resolver:%p dns resolution failed (will retry): %s",
                         r, grpc_error_string(error));
    GPR_ASSERT(!r->have_next_resolution_timer_);
    r->have_next_resolution_timer_ = true;
    r->Ref(DEBUG_LOCATION, "retry-timer").release();
    if (timeout > 0) {
      GRPC_CARES_TRACE_LOG("resolver:%p retrying in %" PRId64 " milliseconds",
                           r, timeout);
    } else {
      GRPC_CARES_TRACE_LOG("resolver:%p retrying immediately", r);
    }
    GRPC_CLOSURE_INIT(&r->on_next_resolution_, OnNextResolution, r,
                      grpc_schedule_on_exec_ctx);
    grpc_timer_init(&r->next_resolution_timer_, next_try,
                    &r->on_next_resolution_);
  }
  r->Unref(DEBUG_LOCATION, "dns-resolving");
}
void AresDnsResolver::MaybeStartResolvingLocked() {
  if (have_next_resolution_timer_) return;
  if (last_resolution_timestamp_ >= 0) {
    const grpc_millis earliest_next_resolution =
        last_resolution_timestamp_ + min_time_between_resolutions_;
    const grpc_millis ms_until_next_resolution =
        earliest_next_resolution - grpc_core::ExecCtx::Get()->Now();
    if (ms_until_next_resolution > 0) {
      const grpc_millis last_resolution_ago =
          grpc_core::ExecCtx::Get()->Now() - last_resolution_timestamp_;
      GRPC_CARES_TRACE_LOG(
          "resolver:%p In cooldown from last resolution (from %" PRId64
          " ms ago). Will resolve again in %" PRId64 " ms",
          this, last_resolution_ago, ms_until_next_resolution);
      have_next_resolution_timer_ = true;
      Ref(DEBUG_LOCATION, "next_resolution_timer_cooldown").release();
      GRPC_CLOSURE_INIT(&on_next_resolution_, OnNextResolution, this,
                        grpc_schedule_on_exec_ctx);
      grpc_timer_init(&next_resolution_timer_,
                      ExecCtx::Get()->Now() + ms_until_next_resolution,
                      &on_next_resolution_);
      return;
    }
  }
  StartResolvingLocked();
}
void AresDnsResolver::StartResolvingLocked() {
  Ref(DEBUG_LOCATION, "dns-resolving").release();
  GPR_ASSERT(!resolving_);
  resolving_ = true;
  service_config_json_ = nullptr;
  GRPC_CLOSURE_INIT(&on_resolved_, OnResolved, this, grpc_schedule_on_exec_ctx);
  pending_request_ = grpc_dns_lookup_ares_locked(
      dns_server_, name_to_resolve_, kDefaultPort, interested_parties_,
      &on_resolved_, &addresses_,
      enable_srv_queries_ ? &balancer_addresses_ : nullptr,
      request_service_config_ ? &service_config_json_ : nullptr,
      query_timeout_ms_, combiner());
  last_resolution_timestamp_ = grpc_core::ExecCtx::Get()->Now();
  GRPC_CARES_TRACE_LOG("resolver:%p Started resolving. pending_request_:%p",
                       this, pending_request_);
}
class AresDnsResolverFactory : public ResolverFactory {
 public:
  bool IsValidUri(const grpc_uri* ) const override { return true; }
  OrphanablePtr<Resolver> CreateResolver(ResolverArgs args) const override {
    return MakeOrphanable<AresDnsResolver>(std::move(args));
  }
  const char* scheme() const override { return "dns"; }
};
}
}
extern grpc_address_resolver_vtable* grpc_resolve_address_impl;
static grpc_address_resolver_vtable* default_resolver;
static grpc_error* blocking_resolve_address_ares(
    const char* name, const char* default_port,
    grpc_resolved_addresses** addresses) {
  return default_resolver->blocking_resolve_address(name, default_port,
                                                    addresses);
}
static grpc_address_resolver_vtable ares_resolver = {
    grpc_resolve_address_ares, blocking_resolve_address_ares};
#ifdef GRPC_UV
static bool should_use_ares(const char* resolver_env) {
  return resolver_env != nullptr && gpr_stricmp(resolver_env, "ares") == 0;
}
#else
static bool should_use_ares(const char* resolver_env) {
  return !g_custom_iomgr_enabled &&
         (resolver_env == nullptr || strlen(resolver_env) == 0 ||
          gpr_stricmp(resolver_env, "ares") == 0);
}
#endif
static bool g_use_ares_dns_resolver;
void grpc_resolver_dns_ares_init() {
  grpc_core::UniquePtr<char> resolver =
      GPR_GLOBAL_CONFIG_GET(grpc_dns_resolver);
  if (should_use_ares(resolver.get())) {
    g_use_ares_dns_resolver = true;
    gpr_log(GPR_DEBUG, "Using ares dns resolver");
    address_sorting_init();
    grpc_error* error = grpc_ares_init();
    if (error != GRPC_ERROR_NONE) {
      GRPC_LOG_IF_ERROR("grpc_ares_init() failed", error);
      return;
    }
    if (default_resolver == nullptr) {
      default_resolver = grpc_resolve_address_impl;
    }
    grpc_set_resolver_impl(&ares_resolver);
    grpc_core::ResolverRegistry::Builder::RegisterResolverFactory(
        absl::make_unique<grpc_core::AresDnsResolverFactory>());
  } else {
    g_use_ares_dns_resolver = false;
  }
}
void grpc_resolver_dns_ares_shutdown() {
  if (g_use_ares_dns_resolver) {
    address_sorting_shutdown();
    grpc_ares_cleanup();
  }
}
#else
void grpc_resolver_dns_ares_init(void) {}
void grpc_resolver_dns_ares_shutdown(void) {}
#endif
#if GRPC_ARES == 1
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <grpc/support/alloc.h>
#include <grpc/support/string_util.h>
#include <address_sorting/address_sorting.h>
#include "src/core/ext/filters/client_channel/http_connect_handshaker.h"
#include "src/core/ext/filters/client_channel/lb_policy_registry.h"
#include "src/core/ext/filters/client_channel/resolver/dns/c_ares/grpc_ares_wrapper.h"
#include "src/core/ext/filters/client_channel/resolver/dns/dns_resolver_selection.h"
#include "src/core/ext/filters/client_channel/resolver_registry.h"
#include "src/core/ext/filters/client_channel/server_address.h"
#include "src/core/ext/filters/client_channel/service_config.h"
#include "src/core/lib/backoff/backoff.h"
#include "src/core/lib/channel/channel_args.h"
#include "src/core/lib/gpr/string.h"
#include "src/core/lib/gprpp/manual_constructor.h"
#include "src/core/lib/iomgr/combiner.h"
#include "src/core/lib/iomgr/gethostname.h"
#include "src/core/lib/iomgr/iomgr_custom.h"
#include "src/core/lib/iomgr/resolve_address.h"
#include "src/core/lib/iomgr/timer.h"
#include "src/core/lib/json/json.h"
#define GRPC_DNS_INITIAL_CONNECT_BACKOFF_SECONDS 1
#define GRPC_DNS_RECONNECT_BACKOFF_MULTIPLIER 1.6
#define GRPC_DNS_RECONNECT_MAX_BACKOFF_SECONDS 120
#define GRPC_DNS_RECONNECT_JITTER 0.2
namespace grpc_core {
namespace {
const char kDefaultPort[] = "https";
class AresDnsResolver : public Resolver {
 public:
  explicit AresDnsResolver(ResolverArgs args);
  void StartLocked() override;
  void RequestReresolutionLocked() override;
  void ResetBackoffLocked() override;
  void ShutdownLocked() override;
 private:
  virtual ~AresDnsResolver();
  void MaybeStartResolvingLocked();
  void StartResolvingLocked();
  static void OnNextResolution(void* arg, grpc_error* error);
  static void OnResolved(void* arg, grpc_error* error);
  static void OnNextResolutionLocked(void* arg, grpc_error* error);
  static void OnResolvedLocked(void* arg, grpc_error* error);
  char* dns_server_;
  char* name_to_resolve_;
  grpc_channel_args* channel_args_;
  bool request_service_config_;
  grpc_pollset_set* interested_parties_;
  grpc_closure on_next_resolution_;
  grpc_closure on_resolved_;
  bool resolving_ = false;
  grpc_ares_request* pending_request_ = nullptr;
  bool have_next_resolution_timer_ = false;
  grpc_timer next_resolution_timer_;
  grpc_millis min_time_between_resolutions_;
  grpc_millis last_resolution_timestamp_ = -1;
  BackOff backoff_;
  std::unique_ptr<ServerAddressList> addresses_;
  char* service_config_json_ = nullptr;
  bool shutdown_initiated_ = false;
  int query_timeout_ms_;
  bool enable_srv_queries_;
};
AresDnsResolver::AresDnsResolver(ResolverArgs args)
    : Resolver(args.combiner, std::move(args.result_handler)),
      backoff_(
          BackOff::Options()
              .set_initial_backoff(GRPC_DNS_INITIAL_CONNECT_BACKOFF_SECONDS *
                                   1000)
              .set_multiplier(GRPC_DNS_RECONNECT_BACKOFF_MULTIPLIER)
              .set_jitter(GRPC_DNS_RECONNECT_JITTER)
              .set_max_backoff(GRPC_DNS_RECONNECT_MAX_BACKOFF_SECONDS * 1000)) {
  const char* path = args.uri->path;
  if (path[0] == '/') ++path;
  name_to_resolve_ = gpr_strdup(path);
  dns_server_ = nullptr;
  if (0 != strcmp(args.uri->authority, "")) {
    dns_server_ = gpr_strdup(args.uri->authority);
  }
  channel_args_ = grpc_channel_args_copy(args.args);
  const grpc_arg* arg = grpc_channel_args_find(
      channel_args_, GRPC_ARG_SERVICE_CONFIG_DISABLE_RESOLUTION);
  request_service_config_ = !grpc_channel_arg_get_bool(arg, true);
  arg = grpc_channel_args_find(channel_args_,
                               GRPC_ARG_DNS_MIN_TIME_BETWEEN_RESOLUTIONS_MS);
  min_time_between_resolutions_ =
      grpc_channel_arg_get_integer(arg, {1000 * 30, 0, INT_MAX});
  arg = grpc_channel_args_find(channel_args_, GRPC_ARG_DNS_ENABLE_SRV_QUERIES);
  enable_srv_queries_ = grpc_channel_arg_get_bool(arg, false);
  interested_parties_ = grpc_pollset_set_create();
  if (args.pollset_set != nullptr) {
    grpc_pollset_set_add_pollset_set(interested_parties_, args.pollset_set);
  }
  const grpc_arg* query_timeout_ms_arg =
      grpc_channel_args_find(channel_args_, GRPC_ARG_DNS_ARES_QUERY_TIMEOUT_MS);
  query_timeout_ms_ = grpc_channel_arg_get_integer(
      query_timeout_ms_arg,
      {GRPC_DNS_ARES_DEFAULT_QUERY_TIMEOUT_MS, 0, INT_MAX});
}
AresDnsResolver::~AresDnsResolver() {
  GRPC_CARES_TRACE_LOG("resolver:%p destroying AresDnsResolver", this);
  grpc_pollset_set_destroy(interested_parties_);
  gpr_free(dns_server_);
  gpr_free(name_to_resolve_);
  grpc_channel_args_destroy(channel_args_);
}
void AresDnsResolver::StartLocked() {
  GRPC_CARES_TRACE_LOG("resolver:%p AresDnsResolver::StartLocked() is called.",
                       this);
  MaybeStartResolvingLocked();
}
void AresDnsResolver::RequestReresolutionLocked() {
  if (!resolving_) {
    MaybeStartResolvingLocked();
  }
}
void AresDnsResolver::ResetBackoffLocked() {
  if (have_next_resolution_timer_) {
    grpc_timer_cancel(&next_resolution_timer_);
  }
  backoff_.Reset();
}
void AresDnsResolver::ShutdownLocked() {
  shutdown_initiated_ = true;
  if (have_next_resolution_timer_) {
    grpc_timer_cancel(&next_resolution_timer_);
  }
  if (pending_request_ != nullptr) {
    grpc_cancel_ares_request_locked(pending_request_);
  }
}
void AresDnsResolver::OnNextResolution(void* arg, grpc_error* error) {
  AresDnsResolver* r = static_cast<AresDnsResolver*>(arg);
  r->combiner()->Run(GRPC_CLOSURE_INIT(&r->on_next_resolution_,
                                       OnNextResolutionLocked, r, nullptr),
                     GRPC_ERROR_REF(error));
}
void AresDnsResolver::OnNextResolutionLocked(void* arg, grpc_error* error) {
  AresDnsResolver* r = static_cast<AresDnsResolver*>(arg);
  GRPC_CARES_TRACE_LOG(
      "resolver:%p re-resolution timer fired. error: %s. shutdown_initiated_: "
      "%d",
      r, grpc_error_string(error), r->shutdown_initiated_);
  r->have_next_resolution_timer_ = false;
  if (error == GRPC_ERROR_NONE && !r->shutdown_initiated_) {
    if (!r->resolving_) {
      GRPC_CARES_TRACE_LOG(
          "resolver:%p start resolving due to re-resolution timer", r);
      r->StartResolvingLocked();
    }
  }
  r->Unref(DEBUG_LOCATION, "next_resolution_timer");
}
bool ValueInJsonArray(const Json::Array& array, const char* value) {
  for (const Json& entry : array) {
    if (entry.type() == Json::Type::STRING && entry.string_value() == value) {
      return true;
    }
  }
  return false;
}
std::string ChooseServiceConfig(char* service_config_choice_json,
                                grpc_error** error) {
  Json json = Json::Parse(service_config_choice_json, error);
  if (*error != GRPC_ERROR_NONE) return "";
  if (json.type() != Json::Type::ARRAY) {
    *error = GRPC_ERROR_CREATE_FROM_STATIC_STRING(
        "Service Config Choices, error: should be of type array");
    return "";
  }
  const Json* service_config = nullptr;
  InlinedVector<grpc_error*, 4> error_list;
  for (const Json& choice : json.array_value()) {
    if (choice.type() != Json::Type::OBJECT) {
      error_list.push_back(GRPC_ERROR_CREATE_FROM_STATIC_STRING(
          "Service Config Choice, error: should be of type object"));
      continue;
    }
    auto it = choice.object_value().find("clientLanguage");
    if (it != choice.object_value().end()) {
      if (it->second.type() != Json::Type::ARRAY) {
        error_list.push_back(GRPC_ERROR_CREATE_FROM_STATIC_STRING(
            "field:clientLanguage error:should be of type array"));
      } else if (!ValueInJsonArray(it->second.array_value(), "c++")) {
        continue;
      }
    }
    it = choice.object_value().find("clientHostname");
    if (it != choice.object_value().end()) {
      if (it->second.type() != Json::Type::ARRAY) {
        error_list.push_back(GRPC_ERROR_CREATE_FROM_STATIC_STRING(
            "field:clientHostname error:should be of type array"));
      } else {
        char* hostname = grpc_gethostname();
        if (hostname == nullptr ||
            !ValueInJsonArray(it->second.array_value(), hostname)) {
          continue;
        }
      }
    }
    it = choice.object_value().find("percentage");
    if (it != choice.object_value().end()) {
      if (it->second.type() != Json::Type::NUMBER) {
        error_list.push_back(GRPC_ERROR_CREATE_FROM_STATIC_STRING(
            "field:percentage error:should be of type number"));
      } else {
        int random_pct = rand() % 100;
        int percentage;
        if (sscanf(it->second.string_value().c_str(), "%d", &percentage) != 1) {
          error_list.push_back(GRPC_ERROR_CREATE_FROM_STATIC_STRING(
              "field:percentage error:should be of type integer"));
        } else if (random_pct > percentage || percentage == 0) {
          continue;
        }
      }
    }
    it = choice.object_value().find("serviceConfig");
    if (it == choice.object_value().end()) {
      error_list.push_back(GRPC_ERROR_CREATE_FROM_STATIC_STRING(
          "field:serviceConfig error:required field missing"));
    } else if (it->second.type() != Json::Type::OBJECT) {
      error_list.push_back(GRPC_ERROR_CREATE_FROM_STATIC_STRING(
          "field:serviceConfig error:should be of type object"));
    } else if (service_config == nullptr) {
      service_config = &it->second;
    }
  }
  if (!error_list.empty()) {
    service_config = nullptr;
    *error = GRPC_ERROR_CREATE_FROM_VECTOR("Service Config Choices Parser",
                                           &error_list);
  }
  if (service_config == nullptr) return "";
  return service_config->Dump();
}
void AresDnsResolver::OnResolved(void* arg, grpc_error* error) {
  AresDnsResolver* r = static_cast<AresDnsResolver*>(arg);
  r->combiner()->Run(
      GRPC_CLOSURE_INIT(&r->on_resolved_, OnResolvedLocked, r, nullptr),
      GRPC_ERROR_REF(error));
}
void AresDnsResolver::OnResolvedLocked(void* arg, grpc_error* error) {
  AresDnsResolver* r = static_cast<AresDnsResolver*>(arg);
  GPR_ASSERT(r->resolving_);
  r->resolving_ = false;
  gpr_free(r->pending_request_);
  r->pending_request_ = nullptr;
  if (r->shutdown_initiated_) {
    r->Unref(DEBUG_LOCATION, "OnResolvedLocked() shutdown");
    return;
  }
  if (r->addresses_ != nullptr) {
    Result result;
    result.addresses = std::move(*r->addresses_);
    if (r->service_config_json_ != nullptr) {
      std::string service_config_string = ChooseServiceConfig(
          r->service_config_json_, &result.service_config_error);
      gpr_free(r->service_config_json_);
      if (result.service_config_error == GRPC_ERROR_NONE &&
          !service_config_string.empty()) {
        GRPC_CARES_TRACE_LOG("resolver:%p selected service config choice: %s",
                             r, service_config_string.c_str());
        result.service_config = ServiceConfig::Create(
            service_config_string, &result.service_config_error);
      }
    }
    result.args = grpc_channel_args_copy(r->channel_args_);
    r->result_handler()->ReturnResult(std::move(result));
    r->addresses_.reset();
    r->backoff_.Reset();
  } else {
    GRPC_CARES_TRACE_LOG("resolver:%p dns resolution failed: %s", r,
                         grpc_error_string(error));
    r->result_handler()->ReturnError(grpc_error_set_int(
        GRPC_ERROR_CREATE_REFERENCING_FROM_STATIC_STRING(
            "DNS resolution failed", &error, 1),
        GRPC_ERROR_INT_GRPC_STATUS, GRPC_STATUS_UNAVAILABLE));
    grpc_millis next_try = r->backoff_.NextAttemptTime();
    grpc_millis timeout = next_try - ExecCtx::Get()->Now();
    GRPC_CARES_TRACE_LOG("resolver:%p dns resolution failed (will retry): %s",
                         r, grpc_error_string(error));
    GPR_ASSERT(!r->have_next_resolution_timer_);
    r->have_next_resolution_timer_ = true;
    r->Ref(DEBUG_LOCATION, "retry-timer").release();
    if (timeout > 0) {
      GRPC_CARES_TRACE_LOG("resolver:%p retrying in %" PRId64 " milliseconds",
                           r, timeout);
    } else {
      GRPC_CARES_TRACE_LOG("resolver:%p retrying immediately", r);
    }
    GRPC_CLOSURE_INIT(&r->on_next_resolution_, OnNextResolution, r,
                      grpc_schedule_on_exec_ctx);
    grpc_timer_init(&r->next_resolution_timer_, next_try,
                    &r->on_next_resolution_);
  }
  r->Unref(DEBUG_LOCATION, "dns-resolving");
}
void AresDnsResolver::MaybeStartResolvingLocked() {
  if (have_next_resolution_timer_) return;
  if (last_resolution_timestamp_ >= 0) {
    const grpc_millis earliest_next_resolution =
        last_resolution_timestamp_ + min_time_between_resolutions_;
    const grpc_millis ms_until_next_resolution =
        earliest_next_resolution - grpc_core::ExecCtx::Get()->Now();
    if (ms_until_next_resolution > 0) {
      const grpc_millis last_resolution_ago =
          grpc_core::ExecCtx::Get()->Now() - last_resolution_timestamp_;
      GRPC_CARES_TRACE_LOG(
          "resolver:%p In cooldown from last resolution (from %" PRId64
          " ms ago). Will resolve again in %" PRId64 " ms",
          this, last_resolution_ago, ms_until_next_resolution);
      have_next_resolution_timer_ = true;
      Ref(DEBUG_LOCATION, "next_resolution_timer_cooldown").release();
      GRPC_CLOSURE_INIT(&on_next_resolution_, OnNextResolution, this,
                        grpc_schedule_on_exec_ctx);
      grpc_timer_init(&next_resolution_timer_,
                      ExecCtx::Get()->Now() + ms_until_next_resolution,
                      &on_next_resolution_);
      return;
    }
  }
  StartResolvingLocked();
}
void AresDnsResolver::StartResolvingLocked() {
  Ref(DEBUG_LOCATION, "dns-resolving").release();
  GPR_ASSERT(!resolving_);
  resolving_ = true;
  service_config_json_ = nullptr;
  GRPC_CLOSURE_INIT(&on_resolved_, OnResolved, this, grpc_schedule_on_exec_ctx);
  pending_request_ = grpc_dns_lookup_ares_locked(
      dns_server_, name_to_resolve_, kDefaultPort, interested_parties_,
      &on_resolved_, &addresses_, enable_srv_queries_ ,
      request_service_config_ ? &service_config_json_ : nullptr,
      query_timeout_ms_, combiner());
  last_resolution_timestamp_ = grpc_core::ExecCtx::Get()->Now();
  GRPC_CARES_TRACE_LOG("resolver:%p Started resolving. pending_request_:%p",
                       this, pending_request_);
}
class AresDnsResolverFactory : public ResolverFactory {
 public:
  bool IsValidUri(const grpc_uri* ) const override { return true; }
  OrphanablePtr<Resolver> CreateResolver(ResolverArgs args) const override {
    return MakeOrphanable<AresDnsResolver>(std::move(args));
  }
  const char* scheme() const override { return "dns"; }
};
}
}
extern grpc_address_resolver_vtable* grpc_resolve_address_impl;
static grpc_address_resolver_vtable* default_resolver;
static grpc_error* blocking_resolve_address_ares(
    const char* name, const char* default_port,
    grpc_resolved_addresses** addresses) {
  return default_resolver->blocking_resolve_address(name, default_port,
                                                    addresses);
}
static grpc_address_resolver_vtable ares_resolver = {
    grpc_resolve_address_ares, blocking_resolve_address_ares};
#ifdef GRPC_UV
static bool should_use_ares(const char* resolver_env) {
  return resolver_env != nullptr && gpr_stricmp(resolver_env, "ares") == 0;
}
#else
static bool should_use_ares(const char* resolver_env) {
  return !g_custom_iomgr_enabled &&
         (resolver_env == nullptr || strlen(resolver_env) == 0 ||
          gpr_stricmp(resolver_env, "ares") == 0);
}
#endif
static bool g_use_ares_dns_resolver;
void grpc_resolver_dns_ares_init() {
  grpc_core::UniquePtr<char> resolver =
      GPR_GLOBAL_CONFIG_GET(grpc_dns_resolver);
  if (should_use_ares(resolver.get())) {
    g_use_ares_dns_resolver = true;
    gpr_log(GPR_DEBUG, "Using ares dns resolver");
    address_sorting_init();
    grpc_error* error = grpc_ares_init();
    if (error != GRPC_ERROR_NONE) {
      GRPC_LOG_IF_ERROR("grpc_ares_init() failed", error);
      return;
    }
    if (default_resolver == nullptr) {
      default_resolver = grpc_resolve_address_impl;
    }
    grpc_set_resolver_impl(&ares_resolver);
    grpc_core::ResolverRegistry::Builder::RegisterResolverFactory(
        absl::make_unique<grpc_core::AresDnsResolverFactory>());
  } else {
    g_use_ares_dns_resolver = false;
  }
}
void grpc_resolver_dns_ares_shutdown() {
  if (g_use_ares_dns_resolver) {
    address_sorting_shutdown();
    grpc_ares_cleanup();
  }
}
#else
void grpc_resolver_dns_ares_init(void) {}
void grpc_resolver_dns_ares_shutdown(void) {}
#endif
#if GRPC_ARES == 1
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <grpc/support/alloc.h>
#include <grpc/support/string_util.h>
#include <address_sorting/address_sorting.h>
#include "src/core/ext/filters/client_channel/http_connect_handshaker.h"
#include "src/core/ext/filters/client_channel/lb_policy_registry.h"
#include "src/core/ext/filters/client_channel/resolver/dns/c_ares/grpc_ares_wrapper.h"
#include "src/core/ext/filters/client_channel/resolver/dns/dns_resolver_selection.h"
#include "src/core/ext/filters/client_channel/resolver_registry.h"
#include "src/core/ext/filters/client_channel/server_address.h"
#include "src/core/ext/filters/client_channel/service_config.h"
#include "src/core/lib/backoff/backoff.h"
#include "src/core/lib/channel/channel_args.h"
#include "src/core/lib/gpr/string.h"
#include "src/core/lib/gprpp/manual_constructor.h"
#include "src/core/lib/iomgr/gethostname.h"
#include "src/core/lib/iomgr/iomgr_custom.h"
#include "src/core/lib/iomgr/resolve_address.h"
#include "src/core/lib/iomgr/timer.h"
#include "src/core/lib/iomgr/work_serializer.h"
#include "src/core/lib/json/json.h"
#define GRPC_DNS_INITIAL_CONNECT_BACKOFF_SECONDS 1
#define GRPC_DNS_RECONNECT_BACKOFF_MULTIPLIER 1.6
#define GRPC_DNS_RECONNECT_MAX_BACKOFF_SECONDS 120
#define GRPC_DNS_RECONNECT_JITTER 0.2
namespace grpc_core {
namespace {
const char kDefaultPort[] = "https";
class AresDnsResolver : public Resolver {
 public:
  explicit AresDnsResolver(ResolverArgs args);
  void StartLocked() override;
  void RequestReresolutionLocked() override;
  void ResetBackoffLocked() override;
  void ShutdownLocked() override;
 private:
  virtual ~AresDnsResolver();
  void MaybeStartResolvingLocked();
  void StartResolvingLocked();
  static void OnNextResolution(void* arg, grpc_error* error);
  static void OnResolved(void* arg, grpc_error* error);
  void OnNextResolutionLocked(grpc_error* error);
  void OnResolvedLocked(grpc_error* error);
  char* dns_server_;
  char* name_to_resolve_;
  grpc_channel_args* channel_args_;
  bool request_service_config_;
  grpc_pollset_set* interested_parties_;
  grpc_closure on_next_resolution_;
  grpc_closure on_resolved_;
  bool resolving_ = false;
  grpc_ares_request* pending_request_ = nullptr;
  bool have_next_resolution_timer_ = false;
  grpc_timer next_resolution_timer_;
  grpc_millis min_time_between_resolutions_;
  grpc_millis last_resolution_timestamp_ = -1;
  BackOff backoff_;
  std::unique_ptr<ServerAddressList> addresses_;
  char* service_config_json_ = nullptr;
  bool shutdown_initiated_ = false;
  int query_timeout_ms_;
  bool enable_srv_queries_;
};
AresDnsResolver::AresDnsResolver(ResolverArgs args)
    : Resolver(std::move(args.work_serializer), std::move(args.result_handler)),
      backoff_(
          BackOff::Options()
              .set_initial_backoff(GRPC_DNS_INITIAL_CONNECT_BACKOFF_SECONDS *
                                   1000)
              .set_multiplier(GRPC_DNS_RECONNECT_BACKOFF_MULTIPLIER)
              .set_jitter(GRPC_DNS_RECONNECT_JITTER)
              .set_max_backoff(GRPC_DNS_RECONNECT_MAX_BACKOFF_SECONDS * 1000)) {
  GRPC_CLOSURE_INIT(&on_next_resolution_, OnNextResolution, this,
                    grpc_schedule_on_exec_ctx);
  GRPC_CLOSURE_INIT(&on_resolved_, OnResolved, this, grpc_schedule_on_exec_ctx);
  const char* path = args.uri->path;
  if (path[0] == '/') ++path;
  name_to_resolve_ = gpr_strdup(path);
  dns_server_ = nullptr;
  if (0 != strcmp(args.uri->authority, "")) {
    dns_server_ = gpr_strdup(args.uri->authority);
  }
  channel_args_ = grpc_channel_args_copy(args.args);
  const grpc_arg* arg = grpc_channel_args_find(
      channel_args_, GRPC_ARG_SERVICE_CONFIG_DISABLE_RESOLUTION);
  request_service_config_ = !grpc_channel_arg_get_bool(arg, true);
  arg = grpc_channel_args_find(channel_args_,
                               GRPC_ARG_DNS_MIN_TIME_BETWEEN_RESOLUTIONS_MS);
  min_time_between_resolutions_ =
      grpc_channel_arg_get_integer(arg, {1000 * 30, 0, INT_MAX});
  arg = grpc_channel_args_find(channel_args_, GRPC_ARG_DNS_ENABLE_SRV_QUERIES);
  enable_srv_queries_ = grpc_channel_arg_get_bool(arg, false);
  interested_parties_ = grpc_pollset_set_create();
  if (args.pollset_set != nullptr) {
    grpc_pollset_set_add_pollset_set(interested_parties_, args.pollset_set);
  }
  const grpc_arg* query_timeout_ms_arg =
      grpc_channel_args_find(channel_args_, GRPC_ARG_DNS_ARES_QUERY_TIMEOUT_MS);
  query_timeout_ms_ = grpc_channel_arg_get_integer(
      query_timeout_ms_arg,
      {GRPC_DNS_ARES_DEFAULT_QUERY_TIMEOUT_MS, 0, INT_MAX});
}
AresDnsResolver::~AresDnsResolver() {
  GRPC_CARES_TRACE_LOG("resolver:%p destroying AresDnsResolver", this);
  grpc_pollset_set_destroy(interested_parties_);
  gpr_free(dns_server_);
  gpr_free(name_to_resolve_);
  grpc_channel_args_destroy(channel_args_);
}
void AresDnsResolver::StartLocked() {
  GRPC_CARES_TRACE_LOG("resolver:%p AresDnsResolver::StartLocked() is called.",
                       this);
  MaybeStartResolvingLocked();
}
void AresDnsResolver::RequestReresolutionLocked() {
  if (!resolving_) {
    MaybeStartResolvingLocked();
  }
}
void AresDnsResolver::ResetBackoffLocked() {
  if (have_next_resolution_timer_) {
    grpc_timer_cancel(&next_resolution_timer_);
  }
  backoff_.Reset();
}
void AresDnsResolver::ShutdownLocked() {
  shutdown_initiated_ = true;
  if (have_next_resolution_timer_) {
    grpc_timer_cancel(&next_resolution_timer_);
  }
  if (pending_request_ != nullptr) {
    grpc_cancel_ares_request_locked(pending_request_);
  }
}
void AresDnsResolver::OnNextResolution(void* arg, grpc_error* error) {
  AresDnsResolver* r = static_cast<AresDnsResolver*>(arg);
  GRPC_ERROR_REF(error);
  r->work_serializer()->Run([r, error]() { r->OnNextResolutionLocked(error); },
                            DEBUG_LOCATION);
}
void AresDnsResolver::OnNextResolutionLocked(grpc_error* error) {
  GRPC_CARES_TRACE_LOG(
      "resolver:%p re-resolution timer fired. error: %s. shutdown_initiated_: "
      "%d",
      this, grpc_error_string(error), shutdown_initiated_);
  have_next_resolution_timer_ = false;
  if (error == GRPC_ERROR_NONE && !shutdown_initiated_) {
    if (!resolving_) {
      GRPC_CARES_TRACE_LOG(
          "resolver:%p start resolving due to re-resolution timer", this);
      StartResolvingLocked();
    }
  }
  Unref(DEBUG_LOCATION, "next_resolution_timer");
  GRPC_ERROR_UNREF(error);
}
bool ValueInJsonArray(const Json::Array& array, const char* value) {
  for (const Json& entry : array) {
    if (entry.type() == Json::Type::STRING && entry.string_value() == value) {
      return true;
    }
  }
  return false;
}
std::string ChooseServiceConfig(char* service_config_choice_json,
                                grpc_error** error) {
  Json json = Json::Parse(service_config_choice_json, error);
  if (*error != GRPC_ERROR_NONE) return "";
  if (json.type() != Json::Type::ARRAY) {
    *error = GRPC_ERROR_CREATE_FROM_STATIC_STRING(
        "Service Config Choices, error: should be of type array");
    return "";
  }
  const Json* service_config = nullptr;
  InlinedVector<grpc_error*, 4> error_list;
  for (const Json& choice : json.array_value()) {
    if (choice.type() != Json::Type::OBJECT) {
      error_list.push_back(GRPC_ERROR_CREATE_FROM_STATIC_STRING(
          "Service Config Choice, error: should be of type object"));
      continue;
    }
    auto it = choice.object_value().find("clientLanguage");
    if (it != choice.object_value().end()) {
      if (it->second.type() != Json::Type::ARRAY) {
        error_list.push_back(GRPC_ERROR_CREATE_FROM_STATIC_STRING(
            "field:clientLanguage error:should be of type array"));
      } else if (!ValueInJsonArray(it->second.array_value(), "c++")) {
        continue;
      }
    }
    it = choice.object_value().find("clientHostname");
    if (it != choice.object_value().end()) {
      if (it->second.type() != Json::Type::ARRAY) {
        error_list.push_back(GRPC_ERROR_CREATE_FROM_STATIC_STRING(
            "field:clientHostname error:should be of type array"));
      } else {
        char* hostname = grpc_gethostname();
        if (hostname == nullptr ||
            !ValueInJsonArray(it->second.array_value(), hostname)) {
          continue;
        }
      }
    }
    it = choice.object_value().find("percentage");
    if (it != choice.object_value().end()) {
      if (it->second.type() != Json::Type::NUMBER) {
        error_list.push_back(GRPC_ERROR_CREATE_FROM_STATIC_STRING(
            "field:percentage error:should be of type number"));
      } else {
        int random_pct = rand() % 100;
        int percentage;
        if (sscanf(it->second.string_value().c_str(), "%d", &percentage) != 1) {
          error_list.push_back(GRPC_ERROR_CREATE_FROM_STATIC_STRING(
              "field:percentage error:should be of type integer"));
        } else if (random_pct > percentage || percentage == 0) {
          continue;
        }
      }
    }
    it = choice.object_value().find("serviceConfig");
    if (it == choice.object_value().end()) {
      error_list.push_back(GRPC_ERROR_CREATE_FROM_STATIC_STRING(
          "field:serviceConfig error:required field missing"));
    } else if (it->second.type() != Json::Type::OBJECT) {
      error_list.push_back(GRPC_ERROR_CREATE_FROM_STATIC_STRING(
          "field:serviceConfig error:should be of type object"));
    } else if (service_config == nullptr) {
      service_config = &it->second;
    }
  }
  if (!error_list.empty()) {
    service_config = nullptr;
    *error = GRPC_ERROR_CREATE_FROM_VECTOR("Service Config Choices Parser",
                                           &error_list);
  }
  if (service_config == nullptr) return "";
  return service_config->Dump();
}
void AresDnsResolver::OnResolved(void* arg, grpc_error* error) {
  AresDnsResolver* r = static_cast<AresDnsResolver*>(arg);
  GRPC_ERROR_REF(error);
  r->work_serializer()->Run([r, error]() { r->OnResolvedLocked(error); },
                            DEBUG_LOCATION);
}
void AresDnsResolver::OnResolvedLocked(grpc_error* error) {
  GPR_ASSERT(resolving_);
  resolving_ = false;
  gpr_free(pending_request_);
  pending_request_ = nullptr;
  if (shutdown_initiated_) {
    Unref(DEBUG_LOCATION, "OnResolvedLocked() shutdown");
    GRPC_ERROR_UNREF(error);
    return;
  }
  if (addresses_ != nullptr) {
    Result result;
    result.addresses = std::move(*addresses_);
    if (service_config_json_ != nullptr) {
      std::string service_config_string = ChooseServiceConfig(
          service_config_json_, &result.service_config_error);
      gpr_free(service_config_json_);
      if (result.service_config_error == GRPC_ERROR_NONE &&
          !service_config_string.empty()) {
        GRPC_CARES_TRACE_LOG("resolver:%p selected service config choice: %s",
                             this, service_config_string.c_str());
        result.service_config = ServiceConfig::Create(
            service_config_string, &result.service_config_error);
      }
    }
    result.args = grpc_channel_args_copy(channel_args_);
    result_handler()->ReturnResult(std::move(result));
    addresses_.reset();
    backoff_.Reset();
  } else {
    GRPC_CARES_TRACE_LOG("resolver:%p dns resolution failed: %s", this,
                         grpc_error_string(error));
    result_handler()->ReturnError(grpc_error_set_int(
        GRPC_ERROR_CREATE_REFERENCING_FROM_STATIC_STRING(
            "DNS resolution failed", &error, 1),
        GRPC_ERROR_INT_GRPC_STATUS, GRPC_STATUS_UNAVAILABLE));
    grpc_millis next_try = backoff_.NextAttemptTime();
    grpc_millis timeout = next_try - ExecCtx::Get()->Now();
    GRPC_CARES_TRACE_LOG("resolver:%p dns resolution failed (will retry): %s",
                         this, grpc_error_string(error));
    GPR_ASSERT(!have_next_resolution_timer_);
    have_next_resolution_timer_ = true;
    Ref(DEBUG_LOCATION, "retry-timer").release();
    if (timeout > 0) {
      GRPC_CARES_TRACE_LOG("resolver:%p retrying in %" PRId64 " milliseconds",
                           this, timeout);
    } else {
      GRPC_CARES_TRACE_LOG("resolver:%p retrying immediately", this);
    }
    grpc_timer_init(&next_resolution_timer_, next_try, &on_next_resolution_);
  }
  Unref(DEBUG_LOCATION, "dns-resolving");
  GRPC_ERROR_UNREF(error);
}
void AresDnsResolver::MaybeStartResolvingLocked() {
  if (have_next_resolution_timer_) return;
  if (last_resolution_timestamp_ >= 0) {
    const grpc_millis earliest_next_resolution =
        last_resolution_timestamp_ + min_time_between_resolutions_;
    const grpc_millis ms_until_next_resolution =
        earliest_next_resolution - grpc_core::ExecCtx::Get()->Now();
    if (ms_until_next_resolution > 0) {
      const grpc_millis last_resolution_ago =
          grpc_core::ExecCtx::Get()->Now() - last_resolution_timestamp_;
      GRPC_CARES_TRACE_LOG(
          "resolver:%p In cooldown from last resolution (from %" PRId64
          " ms ago). Will resolve again in %" PRId64 " ms",
          this, last_resolution_ago, ms_until_next_resolution);
      have_next_resolution_timer_ = true;
      Ref(DEBUG_LOCATION, "next_resolution_timer_cooldown").release();
      grpc_timer_init(&next_resolution_timer_,
                      ExecCtx::Get()->Now() + ms_until_next_resolution,
                      &on_next_resolution_);
      return;
    }
  }
  StartResolvingLocked();
}
void AresDnsResolver::StartResolvingLocked() {
  Ref(DEBUG_LOCATION, "dns-resolving").release();
  GPR_ASSERT(!resolving_);
  resolving_ = true;
  service_config_json_ = nullptr;
  pending_request_ = grpc_dns_lookup_ares_locked(
      dns_server_, name_to_resolve_, kDefaultPort, interested_parties_,
      &on_resolved_, &addresses_, enable_srv_queries_ ,
      request_service_config_ ? &service_config_json_ : nullptr,
      query_timeout_ms_, work_serializer());
  last_resolution_timestamp_ = grpc_core::ExecCtx::Get()->Now();
  GRPC_CARES_TRACE_LOG("resolver:%p Started resolving. pending_request_:%p",
                       this, pending_request_);
}
class AresDnsResolverFactory : public ResolverFactory {
 public:
  bool IsValidUri(const grpc_uri* ) const override { return true; }
  OrphanablePtr<Resolver> CreateResolver(ResolverArgs args) const override {
    return MakeOrphanable<AresDnsResolver>(std::move(args));
  }
  const char* scheme() const override { return "dns"; }
};
}
}
extern grpc_address_resolver_vtable* grpc_resolve_address_impl;
static grpc_address_resolver_vtable* default_resolver;
static grpc_error* blocking_resolve_address_ares(
    const char* name, const char* default_port,
    grpc_resolved_addresses** addresses) {
  return default_resolver->blocking_resolve_address(name, default_port,
                                                    addresses);
}
static grpc_address_resolver_vtable ares_resolver = {
    grpc_resolve_address_ares, blocking_resolve_address_ares};
#ifdef GRPC_UV
static bool should_use_ares(const char* resolver_env) {
  return resolver_env != nullptr && gpr_stricmp(resolver_env, "ares") == 0;
}
#else
static bool should_use_ares(const char* resolver_env) {
  return !g_custom_iomgr_enabled &&
         (resolver_env == nullptr || strlen(resolver_env) == 0 ||
          gpr_stricmp(resolver_env, "ares") == 0);
}
#endif
static bool g_use_ares_dns_resolver;
void grpc_resolver_dns_ares_init() {
  grpc_core::UniquePtr<char> resolver =
      GPR_GLOBAL_CONFIG_GET(grpc_dns_resolver);
  if (should_use_ares(resolver.get())) {
    g_use_ares_dns_resolver = true;
    gpr_log(GPR_DEBUG, "Using ares dns resolver");
    address_sorting_init();
    grpc_error* error = grpc_ares_init();
    if (error != GRPC_ERROR_NONE) {
      GRPC_LOG_IF_ERROR("grpc_ares_init() failed", error);
      return;
    }
    if (default_resolver == nullptr) {
      default_resolver = grpc_resolve_address_impl;
    }
    grpc_set_resolver_impl(&ares_resolver);
    grpc_core::ResolverRegistry::Builder::RegisterResolverFactory(
        absl::make_unique<grpc_core::AresDnsResolverFactory>());
  } else {
    g_use_ares_dns_resolver = false;
  }
}
void grpc_resolver_dns_ares_shutdown() {
  if (g_use_ares_dns_resolver) {
    address_sorting_shutdown();
    grpc_ares_cleanup();
  }
}
#else
void grpc_resolver_dns_ares_init(void) {}
void grpc_resolver_dns_ares_shutdown(void) {}
#endif
