#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <grpc/support/alloc.h>
#include <grpc/support/host_port.h>
#include <grpc/support/port_platform.h>
#include <grpc/support/string_util.h>
#include "src/core/ext/client_config/parse_address.h"
#include "src/core/ext/client_config/resolver_registry.h"
#include "src/core/lib/channel/channel_args.h"
#include "src/core/lib/iomgr/resolve_address.h"
#include "src/core/lib/iomgr/unix_sockets_posix.h"
#include "src/core/lib/support/string.h"
typedef struct {
  grpc_resolver base;
  char *target_name;
  grpc_lb_addresses *addresses;
  gpr_mu mu;
  bool published;
  grpc_closure *next_completion;
  grpc_resolver_result **target_result;
} sockaddr_resolver;
static void sockaddr_destroy(grpc_exec_ctx *exec_ctx, grpc_resolver *r);
static void sockaddr_maybe_finish_next_locked(grpc_exec_ctx *exec_ctx,
                                              sockaddr_resolver *r);
static void sockaddr_shutdown(grpc_exec_ctx *exec_ctx, grpc_resolver *r);
static void sockaddr_channel_saw_error(grpc_exec_ctx *exec_ctx,
                                       grpc_resolver *r);
static void sockaddr_next(grpc_exec_ctx *exec_ctx, grpc_resolver *r,
                          grpc_resolver_result **target_result,
                          grpc_closure *on_complete);
static const grpc_resolver_vtable sockaddr_resolver_vtable = {
    sockaddr_destroy, sockaddr_shutdown, sockaddr_channel_saw_error,
    sockaddr_next};
static void sockaddr_shutdown(grpc_exec_ctx *exec_ctx,
                              grpc_resolver *resolver) {
  sockaddr_resolver *r = (sockaddr_resolver *)resolver;
  gpr_mu_lock(&r->mu);
  if (r->next_completion != NULL) {
    *r->target_result = NULL;
    grpc_exec_ctx_sched(exec_ctx, r->next_completion, GRPC_ERROR_NONE, NULL);
    r->next_completion = NULL;
  }
  gpr_mu_unlock(&r->mu);
}
static void sockaddr_channel_saw_error(grpc_exec_ctx *exec_ctx,
                                       grpc_resolver *resolver) {
  sockaddr_resolver *r = (sockaddr_resolver *)resolver;
  gpr_mu_lock(&r->mu);
  r->published = false;
  sockaddr_maybe_finish_next_locked(exec_ctx, r);
  gpr_mu_unlock(&r->mu);
}
static void sockaddr_next(grpc_exec_ctx *exec_ctx, grpc_resolver *resolver,
                          grpc_resolver_result **target_result,
                          grpc_closure *on_complete) {
  sockaddr_resolver *r = (sockaddr_resolver *)resolver;
  gpr_mu_lock(&r->mu);
  GPR_ASSERT(!r->next_completion);
  r->next_completion = on_complete;
  r->target_result = target_result;
  sockaddr_maybe_finish_next_locked(exec_ctx, r);
  gpr_mu_unlock(&r->mu);
}
static void sockaddr_maybe_finish_next_locked(grpc_exec_ctx *exec_ctx,
                                              sockaddr_resolver *r) {
  if (r->next_completion != NULL && !r->published) {
    r->published = true;
    *r->target_result = grpc_resolver_result_create(
        r->target_name,
        grpc_lb_addresses_copy(r->addresses, NULL ),
        NULL , NULL );
    grpc_exec_ctx_sched(exec_ctx, r->next_completion, GRPC_ERROR_NONE, NULL);
    r->next_completion = NULL;
  }
}
static void sockaddr_destroy(grpc_exec_ctx *exec_ctx, grpc_resolver *gr) {
  sockaddr_resolver *r = (sockaddr_resolver *)gr;
  gpr_mu_destroy(&r->mu);
  gpr_free(r->target_name);
  grpc_lb_addresses_destroy(r->addresses, NULL );
  gpr_free(r);
}
static char *ip_get_default_authority(grpc_uri *uri) {
  const char *path = uri->path;
  if (path[0] == '/') ++path;
  return gpr_strdup(path);
}
static char *ipv4_get_default_authority(grpc_resolver_factory *factory,
                                        grpc_uri *uri) {
  return ip_get_default_authority(uri);
}
static char *ipv6_get_default_authority(grpc_resolver_factory *factory,
                                        grpc_uri *uri) {
  return ip_get_default_authority(uri);
}
#ifdef GPR_HAVE_UNIX_SOCKET
char *unix_get_default_authority(grpc_resolver_factory *factory,
                                 grpc_uri *uri) {
  return gpr_strdup("localhost");
}
#endif
static void do_nothing(void *ignored) {}
static grpc_resolver *sockaddr_create(
    grpc_resolver_args *args,
    int parse(grpc_uri *uri, struct sockaddr_storage *dst, size_t *len)) {
  if (0 != strcmp(args->uri->authority, "")) {
    gpr_log(GPR_ERROR, "authority based uri's not supported by the %s scheme",
            args->uri->scheme);
    return NULL;
  }
  gpr_slice path_slice =
      gpr_slice_new(args->uri->path, strlen(args->uri->path), do_nothing);
  gpr_slice_buffer path_parts;
  gpr_slice_buffer_init(&path_parts);
  gpr_slice_split(path_slice, ",", &path_parts);
  grpc_lb_addresses *addresses = grpc_lb_addresses_create(path_parts.count);
  bool errors_found = false;
  for (size_t i = 0; i < addresses->num_addresses; i++) {
    grpc_uri ith_uri = *args->uri;
    char *part_str = gpr_dump_slice(path_parts.slices[i], GPR_DUMP_ASCII);
    ith_uri.path = part_str;
    if (!parse(&ith_uri, (struct sockaddr_storage *)(&addresses->addresses[i]
                                                          .address.addr),
               &addresses->addresses[i].address.len)) {
      errors_found = true;
    }
    gpr_free(part_str);
    if (errors_found) break;
  }
  gpr_slice_buffer_destroy(&path_parts);
  gpr_slice_unref(path_slice);
  if (errors_found) {
    grpc_lb_addresses_destroy(addresses, NULL );
    return NULL;
  }
  sockaddr_resolver *r = gpr_malloc(sizeof(sockaddr_resolver));
  memset(r, 0, sizeof(*r));
  r->target_name = gpr_strdup(args->uri->path);
  r->addresses = addresses;
  gpr_mu_init(&r->mu);
  grpc_resolver_init(&r->base, &sockaddr_resolver_vtable);
  return &r->base;
}
static void sockaddr_factory_ref(grpc_resolver_factory *factory) {}
static void sockaddr_factory_unref(grpc_resolver_factory *factory) {}
#define DECL_FACTORY(name) \
  static grpc_resolver *name##_factory_create_resolver( \
      grpc_resolver_factory *factory, grpc_resolver_args *args) { \
    return sockaddr_create(args, parse_##name); \
  } \
  static const grpc_resolver_factory_vtable name##_factory_vtable = { \
      sockaddr_factory_ref, sockaddr_factory_unref, \
      name##_factory_create_resolver, name##_get_default_authority, #name}; \
  static grpc_resolver_factory name##_resolver_factory = { \
      &name##_factory_vtable}
#ifdef GPR_HAVE_UNIX_SOCKET
DECL_FACTORY(unix);
#endif
DECL_FACTORY(ipv4);
DECL_FACTORY(ipv6);
void grpc_resolver_sockaddr_init(void) {
  grpc_register_resolver_type(&ipv4_resolver_factory);
  grpc_register_resolver_type(&ipv6_resolver_factory);
#ifdef GPR_HAVE_UNIX_SOCKET
  grpc_register_resolver_type(&unix_resolver_factory);
#endif
}
void grpc_resolver_sockaddr_shutdown(void) {}
