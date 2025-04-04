#include <string.h>
#include <grpc/byte_buffer_reader.h>
#include <grpc/grpc.h>
#include <grpc/support/alloc.h>
#include <grpc/support/host_port.h>
#include <grpc/support/string_util.h>
#include "src/core/ext/client_config/client_channel_factory.h"
#include "src/core/ext/client_config/lb_policy_registry.h"
#include "src/core/ext/client_config/parse_address.h"
#include "src/core/ext/lb_policy/grpclb/grpclb.h"
#include "src/core/ext/lb_policy/grpclb/load_balancer_api.h"
#include "src/core/lib/iomgr/sockaddr.h"
#include "src/core/lib/iomgr/sockaddr_utils.h"
#include "src/core/lib/support/string.h"
#include "src/core/lib/surface/call.h"
#include "src/core/lib/surface/channel.h"
#include <errno.h>
#include "src/core/lib/transport/static_metadata.h"
int grpc_lb_glb_trace = 0;
static void lb_addrs_destroy(grpc_lb_address *lb_addresses, size_t num_addresses) {
  gpr_free(lb_addresses->resolved_address);
  for (size_t i = 0; i < num_addresses; ++i) {
    if (lb_addresses[i].user_data != NULL) {
      GRPC_MDELEM_UNREF(lb_addresses[i].user_data);
    }
  }
  gpr_free(lb_addresses);
}
static void initial_metadata_add_lb_token(grpc_metadata_batch *initial_metadata, grpc_linked_mdelem *lb_token_mdelem_storage, grpc_mdelem *lb_token) {
  GPR_ASSERT(lb_token_mdelem_storage != NULL);
  GPR_ASSERT(lb_token != NULL);
  grpc_metadata_batch_add_tail(initial_metadata, lb_token_mdelem_storage,
                               lb_token);
}
typedef struct wrapped_rr_closure_arg {
  grpc_closure *wrapped_closure;
  grpc_metadata_batch *initial_metadata;
  grpc_connected_subchannel **target;
  grpc_mdelem *lb_token;
  grpc_linked_mdelem *lb_token_mdelem_storage;
  grpc_lb_policy *rr_policy;
  void *owning_pending_node;
} wrapped_rr_closure_arg;
static void wrapped_rr_closure(grpc_exec_ctx *exec_ctx, void *arg,
                               grpc_error *error) {
  wrapped_rr_closure_arg *wc_arg = arg;
  if (wc_arg->rr_policy != NULL) {
    if (grpc_lb_glb_trace) {
      gpr_log(GPR_INFO, "Unreffing RR (0x%" PRIxPTR ")",
              (intptr_t)wc_arg->rr_policy);
    }
    GRPC_LB_POLICY_UNREF(exec_ctx, wc_arg->rr_policy, "wrapped_rr_closure");
  }
  GPR_ASSERT(wc_arg->wrapped_closure != NULL);
  initial_metadata_add_lb_token(wc_arg->initial_metadata,
                                wc_arg->lb_token_mdelem_storage,
                                GRPC_MDELEM_REF(wc_arg->lb_token));
  grpc_exec_ctx_sched(exec_ctx, wc_arg->wrapped_closure, error, NULL);
  gpr_free(wc_arg->owning_pending_node);
}
typedef struct pending_pick {
  struct pending_pick *next;
  grpc_metadata_batch *initial_metadata;
  uint32_t initial_metadata_flags;
  grpc_connected_subchannel **target;
  grpc_closure wrapped_on_complete;
  wrapped_rr_closure_arg wrapped_on_complete_arg;
} pending_pick;
typedef struct pending_pick {
  struct pending_pick *next;
  grpc_polling_entity *pollent;
  grpc_metadata_batch *initial_metadata;
  grpc_linked_mdelem *lb_token_mdelem_storage;
  uint32_t initial_metadata_flags;
  grpc_connected_subchannel **target;
  grpc_closure wrapped_on_complete;
  wrapped_rr_closure_arg wrapped_on_complete_arg;
} pending_pick;
static void add_pending_pick(pending_pick **root, const grpc_lb_policy_pick_args *pick_args, grpc_polling_entity *pollent, grpc_metadata_batch *initial_metadata, uint32_t initial_metadata_flags, grpc_connected_subchannel **target, grpc_closure *on_complete) {
  pending_pick *pp = gpr_malloc(sizeof(*pp));
  memset(pp, 0, sizeof(pending_pick));
  memset(&pp->wrapped_on_complete_arg, 0, sizeof(wrapped_rr_closure_arg));
  pp->next = *root;
<<<<<<< HEAD
||||||| 79620aaa10
  pp->pollent = pollent;
=======
  pp->pollent = pick_args->pollent;
>>>>>>> b410f80b
  pp->target = target;
  pp->initial_metadata = pick_args->initial_metadata;
  pp->initial_metadata_flags = pick_args->initial_metadata_flags;
  pp->lb_token_mdelem_storage = pick_args->lb_token_mdelem_storage;
  pp->wrapped_on_complete_arg.wrapped_closure = on_complete;
  pp->wrapped_on_complete_arg.initial_metadata = pick_args->initial_metadata;
  pp->wrapped_on_complete_arg.lb_token_mdelem_storage =
      pick_args->lb_token_mdelem_storage;
  grpc_closure_init(&pp->wrapped_on_complete, wrapped_rr_closure,
                    &pp->wrapped_on_complete_arg);
  *root = pp;
}
typedef struct pending_ping {
  struct pending_ping *next;
  grpc_closure wrapped_notify;
  wrapped_rr_closure_arg wrapped_notify_arg;
} pending_ping;
static void add_pending_ping(pending_ping **root, grpc_closure *notify) {
  pending_ping *pping = gpr_malloc(sizeof(*pping));
  memset(pping, 0, sizeof(pending_ping));
  memset(&pping->wrapped_notify_arg, 0, sizeof(wrapped_rr_closure_arg));
  pping->next = *root;
  grpc_closure_init(&pping->wrapped_notify, wrapped_rr_closure,
                    &pping->wrapped_notify_arg);
  pping->wrapped_notify_arg.wrapped_closure = notify;
  *root = pping;
}
typedef struct rr_connectivity_data rr_connectivity_data;
struct lb_client_data;
static const grpc_lb_policy_vtable glb_lb_policy_vtable = {
    glb_destroy, glb_shutdown, glb_pick,
    glb_cancel_pick, glb_cancel_picks, glb_ping_one,
    glb_exit_idle, glb_check_connectivity, glb_notify_on_state_change};
typedef struct glb_lb_policy {
  grpc_lb_policy base;
  gpr_mu mu;
  grpc_client_channel_factory *cc_factory;
  grpc_channel *lb_channel;
  grpc_lb_policy *rr_policy;
  bool started_picking;
  grpc_connectivity_state_tracker state_tracker;
  grpc_grpclb_serverlist *serverlist;
  size_t num_ok_serverlist_addresses;
  grpc_lb_address *lb_addresses;
  pending_pick *pending_picks;
  pending_ping *pending_pings;
  struct lb_client_data *lb_client;
  rr_connectivity_data *rr_connectivity;
  grpc_closure wrapped_on_complete;
  wrapped_rr_closure_arg wc_arg;
} glb_lb_policy;
struct rr_connectivity_data {
  grpc_closure on_change;
  grpc_connectivity_state state;
  glb_lb_policy *glb_policy;
};
static bool is_server_valid(const grpc_grpclb_server *server, size_t idx, bool log) {
  const grpc_grpclb_ip_address *ip = &server->ip_address;
  if (server->port >> 16 != 0) {
    if (log) {
      gpr_log(GPR_ERROR,
              "Invalid port '%d' at index %zu of serverlist. Ignoring.",
              server->port, idx);
    }
    return false;
  }
  if (ip->size != 4 && ip->size != 16) {
    if (log) {
      gpr_log(GPR_ERROR,
              "Expected IP to be 4 or 16 bytes, got %d at index %zu of "
              "serverlist. Ignoring",
              ip->size, idx);
    }
    return false;
  }
  return true;
}
static size_t process_serverlist(const grpc_grpclb_serverlist *serverlist, grpc_lb_address **lb_addresses) {
  size_t num_valid = 0;
  for (size_t i = 0; i < serverlist->num_servers; ++i) {
    if (is_server_valid(serverlist->servers[i], i, true)) ++num_valid;
  }
  if (num_valid == 0) {
    return 0;
  }
  grpc_resolved_address *r_addrs_memblock =
      gpr_malloc(sizeof(grpc_resolved_address) * num_valid);
  memset(r_addrs_memblock, 0, sizeof(grpc_resolved_address) * num_valid);
  grpc_lb_address *lb_addrs = gpr_malloc(sizeof(grpc_lb_address) * num_valid);
  memset(lb_addrs, 0, sizeof(grpc_lb_address) * num_valid);
  size_t addr_idx = 0;
  for (size_t sl_idx = 0; sl_idx < serverlist->num_servers; ++sl_idx) {
    GPR_ASSERT(addr_idx < num_valid);
    const grpc_grpclb_server *server = serverlist->servers[sl_idx];
    if (!is_server_valid(serverlist->servers[sl_idx], sl_idx, false)) continue;
    grpc_lb_address *const lb_addr = &lb_addrs[addr_idx];
    const uint16_t netorder_port = htons((uint16_t)server->port);
    const grpc_grpclb_ip_address *ip = &server->ip_address;
    lb_addr->resolved_address = &r_addrs_memblock[addr_idx];
    struct sockaddr_storage *sa =
        (struct sockaddr_storage *)lb_addr->resolved_address->addr;
    size_t *sa_len = &lb_addr->resolved_address->len;
    *sa_len = 0;
    if (ip->size == 4) {
      struct sockaddr_in *addr4 = (struct sockaddr_in *)sa;
      *sa_len = sizeof(struct sockaddr_in);
      memset(addr4, 0, *sa_len);
      addr4->sin_family = AF_INET;
      memcpy(&addr4->sin_addr, ip->bytes, ip->size);
      addr4->sin_port = netorder_port;
    } else if (ip->size == 16) {
      struct sockaddr_in6 *addr6 = (struct sockaddr_in6 *)sa;
      *sa_len = sizeof(struct sockaddr_in6);
      memset(addr6, 0, *sa_len);
      addr6->sin6_family = AF_INET;
      memcpy(&addr6->sin6_addr, ip->bytes, ip->size);
      addr6->sin6_port = netorder_port;
    }
    GPR_ASSERT(*sa_len > 0);
    if (server->has_load_balance_token) {
      const size_t lb_token_size =
          GPR_ARRAY_SIZE(server->load_balance_token) - 1;
      grpc_mdstr *lb_token_mdstr = grpc_mdstr_from_buffer(
          (uint8_t *)server->load_balance_token, lb_token_size);
      lb_addr->user_data = grpc_mdelem_from_metadata_strings(
          GRPC_MDSTR_LOAD_REPORTING_INITIAL, lb_token_mdstr);
    } else {
      gpr_log(GPR_ERROR,
              "Missing LB token for backend address '%s'. The empty token will "
              "be used instead",
              grpc_sockaddr_to_uri((struct sockaddr *)sa));
      lb_addr->user_data = GRPC_MDELEM_LOAD_REPORTING_INITIAL_EMPTY;
    }
    ++addr_idx;
  }
  GPR_ASSERT(addr_idx == num_valid);
  *lb_addresses = lb_addrs;
  return num_valid;
}
static grpc_lb_policy *create_rr(grpc_exec_ctx *exec_ctx,
                                 const grpc_grpclb_serverlist *serverlist,
                                 glb_lb_policy *glb_policy) {
  GPR_ASSERT(serverlist != NULL && serverlist->num_servers > 0);
  grpc_lb_policy_args args;
  memset(&args, 0, sizeof(args));
  args.client_channel_factory = glb_policy->cc_factory;
  const size_t num_ok_addresses =
      process_serverlist(serverlist, &args.addresses);
  args.num_addresses = num_ok_addresses;
  grpc_lb_policy *rr = grpc_lb_policy_create(exec_ctx, "round_robin", &args);
  if (glb_policy->lb_addresses != NULL) {
    lb_addrs_destroy(glb_policy->lb_addresses,
                     glb_policy->num_ok_serverlist_addresses);
  }
  glb_policy->num_ok_serverlist_addresses = num_ok_addresses;
  glb_policy->lb_addresses = args.addresses;
  return rr;
}
static void rr_handover(grpc_exec_ctx *exec_ctx, glb_lb_policy *glb_policy,
                        grpc_error *error) {
  GPR_ASSERT(glb_policy->serverlist != NULL &&
             glb_policy->serverlist->num_servers > 0);
  glb_policy->rr_policy =
      create_rr(exec_ctx, glb_policy->serverlist, glb_policy);
  if (grpc_lb_glb_trace) {
    gpr_log(GPR_INFO, "Created RR policy (0x%" PRIxPTR ")",
            (intptr_t)glb_policy->rr_policy);
  }
  GPR_ASSERT(glb_policy->rr_policy != NULL);
  grpc_pollset_set_add_pollset_set(exec_ctx,
                                   glb_policy->rr_policy->interested_parties,
                                   glb_policy->base.interested_parties);
  glb_policy->rr_connectivity->state = grpc_lb_policy_check_connectivity(
      exec_ctx, glb_policy->rr_policy, &error);
  grpc_lb_policy_notify_on_state_change(
      exec_ctx, glb_policy->rr_policy, &glb_policy->rr_connectivity->state,
      &glb_policy->rr_connectivity->on_change);
  grpc_connectivity_state_set(exec_ctx, &glb_policy->state_tracker,
                              glb_policy->rr_connectivity->state,
                              GRPC_ERROR_REF(error), "rr_handover");
  grpc_lb_policy_exit_idle(exec_ctx, glb_policy->rr_policy);
  pending_pick *pp;
  while ((pp = glb_policy->pending_picks)) {
    glb_policy->pending_picks = pp->next;
    GRPC_LB_POLICY_REF(glb_policy->rr_policy, "rr_handover_pending_pick");
    pp->wrapped_on_complete_arg.rr_policy = glb_policy->rr_policy;
    if (grpc_lb_glb_trace) {
      gpr_log(GPR_INFO, "Pending pick about to PICK from 0x%" PRIxPTR "",
              (intptr_t)glb_policy->rr_policy);
    }
<<<<<<< HEAD
    grpc_lb_policy_pick(exec_ctx, glb_policy->rr_policy, pp->initial_metadata,
                        pp->initial_metadata_flags, pp->target,
                        &pp->wrapped_on_complete);
||||||| 79620aaa10
    grpc_lb_policy_pick(exec_ctx, glb_policy->rr_policy, pp->pollent,
                        pp->initial_metadata, pp->initial_metadata_flags,
                        pp->target, &pp->wrapped_on_complete);
=======
    const grpc_lb_policy_pick_args pick_args = {
        pp->pollent, pp->initial_metadata, pp->initial_metadata_flags,
        pp->lb_token_mdelem_storage};
    grpc_lb_policy_pick(exec_ctx, glb_policy->rr_policy, &pick_args, pp->target,
                        (void **)&pp->wrapped_on_complete_arg.lb_token,
                        &pp->wrapped_on_complete);
>>>>>>> b410f80b
    pp->wrapped_on_complete_arg.owning_pending_node = pp;
  }
  pending_ping *pping;
  while ((pping = glb_policy->pending_pings)) {
    glb_policy->pending_pings = pping->next;
    GRPC_LB_POLICY_REF(glb_policy->rr_policy, "rr_handover_pending_ping");
    pping->wrapped_notify_arg.rr_policy = glb_policy->rr_policy;
    if (grpc_lb_glb_trace) {
      gpr_log(GPR_INFO, "Pending ping about to PING from 0x%" PRIxPTR "",
              (intptr_t)glb_policy->rr_policy);
    }
    grpc_lb_policy_ping_one(exec_ctx, glb_policy->rr_policy,
                            &pping->wrapped_notify);
    pping->wrapped_notify_arg.owning_pending_node = pping;
  }
}
static void glb_rr_connectivity_changed(grpc_exec_ctx *exec_ctx, void *arg,
                                        grpc_error *error) {
  rr_connectivity_data *rr_conn_data = arg;
  glb_lb_policy *glb_policy = rr_conn_data->glb_policy;
  if (rr_conn_data->state == GRPC_CHANNEL_SHUTDOWN) {
    if (glb_policy->serverlist != NULL) {
      rr_handover(exec_ctx, glb_policy, error);
    } else {
      gpr_free(rr_conn_data);
    }
  } else {
    if (error == GRPC_ERROR_NONE) {
      grpc_connectivity_state_set(exec_ctx, &glb_policy->state_tracker,
                                  rr_conn_data->state, GRPC_ERROR_REF(error),
                                  "glb_rr_connectivity_changed");
      grpc_lb_policy_notify_on_state_change(exec_ctx, glb_policy->rr_policy,
                                            &rr_conn_data->state,
                                            &rr_conn_data->on_change);
    } else {
      gpr_free(rr_conn_data);
    }
  }
}
static grpc_lb_policy *glb_create(grpc_exec_ctx *exec_ctx,
                                  grpc_lb_policy_factory *factory,
                                  grpc_lb_policy_args *args) {
  glb_lb_policy *glb_policy = gpr_malloc(sizeof(*glb_policy));
  memset(glb_policy, 0, sizeof(*glb_policy));
  glb_policy->cc_factory = args->client_channel_factory;
  GPR_ASSERT(glb_policy->cc_factory != NULL);
  if (args->num_addresses == 0) {
    return NULL;
  }
  if (args->addresses[0].user_data != NULL) {
    gpr_log(GPR_ERROR,
            "This LB policy doesn't support user data. It will be ignored");
  }
  char **addr_strs = gpr_malloc(sizeof(char *) * args->num_addresses);
  addr_strs[0] = grpc_sockaddr_to_uri(
      (const struct sockaddr *)&args->addresses[0].resolved_address->addr);
  for (size_t i = 1; i < args->num_addresses; i++) {
    if (args->addresses[i].user_data != NULL) {
      gpr_log(GPR_ERROR,
              "This LB policy doesn't support user data. It will be ignored");
    }
    GPR_ASSERT(
        grpc_sockaddr_to_string(
            &addr_strs[i],
            (const struct sockaddr *)&args->addresses[i].resolved_address->addr,
            true) == 0);
  }
  size_t uri_path_len;
  char *target_uri_str = gpr_strjoin_sep(
      (const char **)addr_strs, args->num_addresses, ",", &uri_path_len);
  glb_policy->lb_channel = grpc_client_channel_factory_create_channel(
      exec_ctx, glb_policy->cc_factory, target_uri_str,
      GRPC_CLIENT_CHANNEL_TYPE_LOAD_BALANCING, NULL);
  gpr_free(target_uri_str);
  for (size_t i = 0; i < args->num_addresses; i++) {
    gpr_free(addr_strs[i]);
  }
  gpr_free(addr_strs);
  if (glb_policy->lb_channel == NULL) {
    gpr_free(glb_policy);
    return NULL;
  }
  rr_connectivity_data *rr_connectivity =
      gpr_malloc(sizeof(rr_connectivity_data));
  memset(rr_connectivity, 0, sizeof(rr_connectivity_data));
  grpc_closure_init(&rr_connectivity->on_change, glb_rr_connectivity_changed,
                    rr_connectivity);
  rr_connectivity->glb_policy = glb_policy;
  glb_policy->rr_connectivity = rr_connectivity;
  grpc_lb_policy_init(&glb_policy->base, &glb_lb_policy_vtable);
  gpr_mu_init(&glb_policy->mu);
  grpc_connectivity_state_init(&glb_policy->state_tracker, GRPC_CHANNEL_IDLE,
                               "grpclb");
  return &glb_policy->base;
}
static void glb_destroy(grpc_exec_ctx *exec_ctx, grpc_lb_policy *pol) {
  glb_lb_policy *glb_policy = (glb_lb_policy *)pol;
  GPR_ASSERT(glb_policy->pending_picks == NULL);
  GPR_ASSERT(glb_policy->pending_pings == NULL);
  grpc_channel_destroy(glb_policy->lb_channel);
  glb_policy->lb_channel = NULL;
  grpc_connectivity_state_destroy(exec_ctx, &glb_policy->state_tracker);
  if (glb_policy->serverlist != NULL) {
    grpc_grpclb_destroy_serverlist(glb_policy->serverlist);
  }
  gpr_mu_destroy(&glb_policy->mu);
  lb_addrs_destroy(glb_policy->lb_addresses,
                   glb_policy->num_ok_serverlist_addresses);
  gpr_free(glb_policy);
}
static void lb_client_data_destroy(struct lb_client_data *lb_client);
static void glb_shutdown(grpc_exec_ctx *exec_ctx, grpc_lb_policy *pol) {
  glb_lb_policy *glb_policy = (glb_lb_policy *)pol;
  gpr_mu_lock(&glb_policy->mu);
  pending_pick *pp = glb_policy->pending_picks;
  glb_policy->pending_picks = NULL;
  pending_ping *pping = glb_policy->pending_pings;
  glb_policy->pending_pings = NULL;
  gpr_mu_unlock(&glb_policy->mu);
  while (pp != NULL) {
    pending_pick *next = pp->next;
    *pp->target = NULL;
    grpc_exec_ctx_sched(exec_ctx, &pp->wrapped_on_complete, GRPC_ERROR_NONE,
                        NULL);
    gpr_free(pp);
    pp = next;
  }
  while (pping != NULL) {
    pending_ping *next = pping->next;
    grpc_exec_ctx_sched(exec_ctx, &pping->wrapped_notify, GRPC_ERROR_NONE,
                        NULL);
    pping = next;
  }
  if (glb_policy->rr_policy) {
    grpc_lb_policy_notify_on_state_change(
        exec_ctx, glb_policy->rr_policy, NULL,
        &glb_policy->rr_connectivity->on_change);
    GRPC_LB_POLICY_UNREF(exec_ctx, glb_policy->rr_policy, "glb_shutdown");
  }
  lb_client_data_destroy(glb_policy->lb_client);
  glb_policy->lb_client = NULL;
  grpc_connectivity_state_set(
      exec_ctx, &glb_policy->state_tracker, GRPC_CHANNEL_SHUTDOWN,
      GRPC_ERROR_CREATE("Channel Shutdown"), "glb_shutdown");
}
static void glb_cancel_pick(grpc_exec_ctx *exec_ctx, grpc_lb_policy *pol,
                            grpc_connected_subchannel **target) {
  glb_lb_policy *glb_policy = (glb_lb_policy *)pol;
  gpr_mu_lock(&glb_policy->mu);
  pending_pick *pp = glb_policy->pending_picks;
  glb_policy->pending_picks = NULL;
  while (pp != NULL) {
    pending_pick *next = pp->next;
    if (pp->target == target) {
      *target = NULL;
      grpc_exec_ctx_sched(exec_ctx, &pp->wrapped_on_complete,
                          GRPC_ERROR_CANCELLED, NULL);
    } else {
      pp->next = glb_policy->pending_picks;
      glb_policy->pending_picks = pp;
    }
    pp = next;
  }
  gpr_mu_unlock(&glb_policy->mu);
}
static grpc_call *lb_client_data_get_call(struct lb_client_data *lb_client);
static void glb_cancel_picks(grpc_exec_ctx *exec_ctx, grpc_lb_policy *pol,
                             uint32_t initial_metadata_flags_mask,
                             uint32_t initial_metadata_flags_eq) {
  glb_lb_policy *glb_policy = (glb_lb_policy *)pol;
  gpr_mu_lock(&glb_policy->mu);
  if (glb_policy->lb_client != NULL) {
    grpc_call_cancel(lb_client_data_get_call(glb_policy->lb_client), NULL);
  }
  pending_pick *pp = glb_policy->pending_picks;
  glb_policy->pending_picks = NULL;
  while (pp != NULL) {
    pending_pick *next = pp->next;
    if ((pp->initial_metadata_flags & initial_metadata_flags_mask) ==
        initial_metadata_flags_eq) {
      grpc_exec_ctx_sched(exec_ctx, &pp->wrapped_on_complete,
                          GRPC_ERROR_CANCELLED, NULL);
    } else {
      pp->next = glb_policy->pending_picks;
      glb_policy->pending_picks = pp;
    }
    pp = next;
  }
  gpr_mu_unlock(&glb_policy->mu);
}
static void query_for_backends(grpc_exec_ctx *exec_ctx,
                               glb_lb_policy *glb_policy);
static void start_picking(grpc_exec_ctx *exec_ctx, glb_lb_policy *glb_policy) {
  glb_policy->started_picking = true;
  query_for_backends(exec_ctx, glb_policy);
}
static void glb_exit_idle(grpc_exec_ctx *exec_ctx, grpc_lb_policy *pol) {
  glb_lb_policy *glb_policy = (glb_lb_policy *)pol;
  gpr_mu_lock(&glb_policy->mu);
  if (!glb_policy->started_picking) {
    start_picking(exec_ctx, glb_policy);
  }
  gpr_mu_unlock(&glb_policy->mu);
}
static int glb_pick(grpc_exec_ctx *exec_ctx, grpc_lb_policy *pol, const grpc_lb_policy_pick_args *pick_args, grpc_polling_entity *pollent, grpc_metadata_batch *initial_metadata, void **user_data, uint32_t initial_metadata_flags, grpc_connected_subchannel **target, grpc_closure *on_complete) {
  glb_lb_policy *glb_policy = (glb_lb_policy *)pol;
  if (pick_args->lb_token_mdelem_storage == NULL) {
    *target = NULL;
    grpc_exec_ctx_sched(
        exec_ctx, on_complete,
        GRPC_ERROR_CREATE("No mdelem storage for the LB token. Load reporting "
                          "won't work without it. Failing"),
        NULL);
    return 1;
  }
  gpr_mu_lock(&glb_policy->mu);
  int r;
  if (glb_policy->rr_policy != NULL) {
    if (grpc_lb_glb_trace) {
      gpr_log(GPR_INFO, "about to PICK from 0x%" PRIxPTR "",
              (intptr_t)glb_policy->rr_policy);
    }
    GRPC_LB_POLICY_REF(glb_policy->rr_policy, "glb_pick");
    memset(&glb_policy->wc_arg, 0, sizeof(wrapped_rr_closure_arg));
    glb_policy->wc_arg.rr_policy = glb_policy->rr_policy;
    glb_policy->wc_arg.wrapped_closure = on_complete;
    glb_policy->wc_arg.lb_token_mdelem_storage =
        pick_args->lb_token_mdelem_storage;
    glb_policy->wc_arg.initial_metadata = pick_args->initial_metadata;
    glb_policy->wc_arg.owning_pending_node = NULL;
    grpc_closure_init(&glb_policy->wrapped_on_complete, wrapped_rr_closure,
                      &glb_policy->wc_arg);
<<<<<<< HEAD
    r = grpc_lb_policy_pick(exec_ctx, glb_policy->rr_policy, initial_metadata,
                            initial_metadata_flags, target,
||||||| 79620aaa10
    r = grpc_lb_policy_pick(exec_ctx, glb_policy->rr_policy, pollent,
                            initial_metadata, initial_metadata_flags, target,
=======
    r = grpc_lb_policy_pick(exec_ctx, glb_policy->rr_policy, pick_args, target,
                            (void **)&glb_policy->wc_arg.lb_token,
>>>>>>> b410f80b
                            &glb_policy->wrapped_on_complete);
    if (r != 0) {
      if (grpc_lb_glb_trace) {
        gpr_log(GPR_INFO, "Unreffing RR (0x%" PRIxPTR ")",
                (intptr_t)glb_policy->wc_arg.rr_policy);
      }
      GRPC_LB_POLICY_UNREF(exec_ctx, glb_policy->wc_arg.rr_policy, "glb_pick");
      initial_metadata_add_lb_token(
          pick_args->initial_metadata, pick_args->lb_token_mdelem_storage,
          GRPC_MDELEM_REF(glb_policy->wc_arg.lb_token));
    }
  } else {
<<<<<<< HEAD
    add_pending_pick(&glb_policy->pending_picks, initial_metadata,
                     initial_metadata_flags, target, on_complete);
||||||| 79620aaa10
    grpc_polling_entity_add_to_pollset_set(exec_ctx, pollent,
                                           glb_policy->base.interested_parties);
    add_pending_pick(&glb_policy->pending_picks, pollent, initial_metadata,
                     initial_metadata_flags, target, on_complete);
=======
    grpc_polling_entity_add_to_pollset_set(exec_ctx, pick_args->pollent,
                                           glb_policy->base.interested_parties);
    add_pending_pick(&glb_policy->pending_picks, pick_args, target,
                     on_complete);
>>>>>>> b410f80b
    if (!glb_policy->started_picking) {
      start_picking(exec_ctx, glb_policy);
    }
    r = 0;
  }
  gpr_mu_unlock(&glb_policy->mu);
  return r;
}
static grpc_connectivity_state glb_check_connectivity(
    grpc_exec_ctx *exec_ctx, grpc_lb_policy *pol,
    grpc_error **connectivity_error) {
  glb_lb_policy *glb_policy = (glb_lb_policy *)pol;
  grpc_connectivity_state st;
  gpr_mu_lock(&glb_policy->mu);
  st = grpc_connectivity_state_check(&glb_policy->state_tracker,
                                     connectivity_error);
  gpr_mu_unlock(&glb_policy->mu);
  return st;
}
static void glb_ping_one(grpc_exec_ctx *exec_ctx, grpc_lb_policy *pol,
                         grpc_closure *closure) {
  glb_lb_policy *glb_policy = (glb_lb_policy *)pol;
  gpr_mu_lock(&glb_policy->mu);
  if (glb_policy->rr_policy) {
    grpc_lb_policy_ping_one(exec_ctx, glb_policy->rr_policy, closure);
  } else {
    add_pending_ping(&glb_policy->pending_pings, closure);
    if (!glb_policy->started_picking) {
      start_picking(exec_ctx, glb_policy);
    }
  }
  gpr_mu_unlock(&glb_policy->mu);
}
static void glb_notify_on_state_change(grpc_exec_ctx *exec_ctx,
                                       grpc_lb_policy *pol,
                                       grpc_connectivity_state *current,
                                       grpc_closure *notify) {
  glb_lb_policy *glb_policy = (glb_lb_policy *)pol;
  gpr_mu_lock(&glb_policy->mu);
  grpc_connectivity_state_notify_on_state_change(
      exec_ctx, &glb_policy->state_tracker, current, notify);
  gpr_mu_unlock(&glb_policy->mu);
}
typedef struct lb_client_data {
  gpr_mu mu;
  grpc_closure md_sent;
  grpc_closure req_sent;
  grpc_closure res_rcvd;
  grpc_closure close_sent;
  grpc_closure srv_status_rcvd;
  grpc_call *lb_call;
  gpr_timespec deadline;
  grpc_metadata_array initial_metadata_recv;
  grpc_metadata_array trailing_metadata_recv;
  grpc_byte_buffer *request_payload;
  grpc_byte_buffer *response_payload;
  grpc_status_code status;
  char *status_details;
  size_t status_details_capacity;
  glb_lb_policy *glb_policy;
} lb_client_data;
static void md_sent_cb(grpc_exec_ctx *exec_ctx, void *arg, grpc_error *error);
static void req_sent_cb(grpc_exec_ctx *exec_ctx, void *arg, grpc_error *error);
static void res_recv_cb(grpc_exec_ctx *exec_ctx, void *arg, grpc_error *error);
static void close_sent_cb(grpc_exec_ctx *exec_ctx, void *arg,
                          grpc_error *error);
static void srv_status_rcvd_cb(grpc_exec_ctx *exec_ctx, void *arg,
                               grpc_error *error);
static lb_client_data *lb_client_data_create(glb_lb_policy *glb_policy) {
  lb_client_data *lb_client = gpr_malloc(sizeof(lb_client_data));
  memset(lb_client, 0, sizeof(lb_client_data));
  gpr_mu_init(&lb_client->mu);
  grpc_closure_init(&lb_client->md_sent, md_sent_cb, lb_client);
  grpc_closure_init(&lb_client->req_sent, req_sent_cb, lb_client);
  grpc_closure_init(&lb_client->res_rcvd, res_recv_cb, lb_client);
  grpc_closure_init(&lb_client->close_sent, close_sent_cb, lb_client);
  grpc_closure_init(&lb_client->srv_status_rcvd, srv_status_rcvd_cb, lb_client);
  lb_client->deadline = gpr_time_add(gpr_now(GPR_CLOCK_MONOTONIC),
                                     gpr_time_from_seconds(3, GPR_TIMESPAN));
  lb_client->lb_call = grpc_channel_create_pollset_set_call(
      glb_policy->lb_channel, NULL, GRPC_PROPAGATE_DEFAULTS,
      glb_policy->base.interested_parties, "/BalanceLoad",
      NULL,
      lb_client->deadline, NULL);
  grpc_metadata_array_init(&lb_client->initial_metadata_recv);
  grpc_metadata_array_init(&lb_client->trailing_metadata_recv);
  grpc_grpclb_request *request = grpc_grpclb_request_create(
      "load.balanced.service.name");
  gpr_slice request_payload_slice = grpc_grpclb_request_encode(request);
  lb_client->request_payload =
      grpc_raw_byte_buffer_create(&request_payload_slice, 1);
  gpr_slice_unref(request_payload_slice);
  grpc_grpclb_request_destroy(request);
  lb_client->status_details = NULL;
  lb_client->status_details_capacity = 0;
  lb_client->glb_policy = glb_policy;
  return lb_client;
}
static void lb_client_data_destroy(lb_client_data *lb_client) {
  grpc_call_destroy(lb_client->lb_call);
  grpc_metadata_array_destroy(&lb_client->initial_metadata_recv);
  grpc_metadata_array_destroy(&lb_client->trailing_metadata_recv);
  grpc_byte_buffer_destroy(lb_client->request_payload);
  gpr_free(lb_client->status_details);
  gpr_mu_destroy(&lb_client->mu);
  gpr_free(lb_client);
}
static grpc_call *lb_client_data_get_call(lb_client_data *lb_client) {
  return lb_client->lb_call;
}
static void query_for_backends(grpc_exec_ctx *exec_ctx,
                               glb_lb_policy *glb_policy) {
  GPR_ASSERT(glb_policy->lb_channel != NULL);
  glb_policy->lb_client = lb_client_data_create(glb_policy);
  grpc_call_error call_error;
  grpc_op ops[1];
  memset(ops, 0, sizeof(ops));
  grpc_op *op = ops;
  op->op = GRPC_OP_SEND_INITIAL_METADATA;
  op->data.send_initial_metadata.count = 0;
  op->flags = 0;
  op->reserved = NULL;
  op++;
  call_error = grpc_call_start_batch_and_execute(
      exec_ctx, glb_policy->lb_client->lb_call, ops, (size_t)(op - ops),
      &glb_policy->lb_client->md_sent);
  GPR_ASSERT(GRPC_CALL_OK == call_error);
  op = ops;
  op->op = GRPC_OP_RECV_STATUS_ON_CLIENT;
  op->data.recv_status_on_client.trailing_metadata =
      &glb_policy->lb_client->trailing_metadata_recv;
  op->data.recv_status_on_client.status = &glb_policy->lb_client->status;
  op->data.recv_status_on_client.status_details =
      &glb_policy->lb_client->status_details;
  op->data.recv_status_on_client.status_details_capacity =
      &glb_policy->lb_client->status_details_capacity;
  op->flags = 0;
  op->reserved = NULL;
  op++;
  call_error = grpc_call_start_batch_and_execute(
      exec_ctx, glb_policy->lb_client->lb_call, ops, (size_t)(op - ops),
      &glb_policy->lb_client->srv_status_rcvd);
  GPR_ASSERT(GRPC_CALL_OK == call_error);
}
static void md_sent_cb(grpc_exec_ctx *exec_ctx, void *arg, grpc_error *error) {
  lb_client_data *lb_client = arg;
  GPR_ASSERT(lb_client->lb_call);
  grpc_op ops[1];
  memset(ops, 0, sizeof(ops));
  grpc_op *op = ops;
  op->op = GRPC_OP_SEND_MESSAGE;
  op->data.send_message = lb_client->request_payload;
  op->flags = 0;
  op->reserved = NULL;
  op++;
  grpc_call_error call_error = grpc_call_start_batch_and_execute(
      exec_ctx, lb_client->lb_call, ops, (size_t)(op - ops),
      &lb_client->req_sent);
  GPR_ASSERT(GRPC_CALL_OK == call_error);
}
static void req_sent_cb(grpc_exec_ctx *exec_ctx, void *arg, grpc_error *error) {
  lb_client_data *lb_client = arg;
  GPR_ASSERT(lb_client->lb_call);
  grpc_op ops[2];
  memset(ops, 0, sizeof(ops));
  grpc_op *op = ops;
  op->op = GRPC_OP_RECV_INITIAL_METADATA;
  op->data.recv_initial_metadata = &lb_client->initial_metadata_recv;
  op->flags = 0;
  op->reserved = NULL;
  op++;
  op->op = GRPC_OP_RECV_MESSAGE;
  op->data.recv_message = &lb_client->response_payload;
  op->flags = 0;
  op->reserved = NULL;
  op++;
  grpc_call_error call_error = grpc_call_start_batch_and_execute(
      exec_ctx, lb_client->lb_call, ops, (size_t)(op - ops),
      &lb_client->res_rcvd);
  GPR_ASSERT(GRPC_CALL_OK == call_error);
}
static void res_recv_cb(grpc_exec_ctx *exec_ctx, void *arg, grpc_error *error) {
  lb_client_data *lb_client = arg;
  grpc_op ops[2];
  memset(ops, 0, sizeof(ops));
  grpc_op *op = ops;
  if (lb_client->response_payload != NULL) {
    grpc_byte_buffer_reader bbr;
    grpc_byte_buffer_reader_init(&bbr, lb_client->response_payload);
    gpr_slice response_slice = grpc_byte_buffer_reader_readall(&bbr);
    grpc_byte_buffer_destroy(lb_client->response_payload);
    grpc_grpclb_serverlist *serverlist =
        grpc_grpclb_response_parse_serverlist(response_slice);
    if (serverlist != NULL) {
      gpr_slice_unref(response_slice);
      if (grpc_lb_glb_trace) {
        gpr_log(GPR_INFO, "Serverlist with %zu servers received",
                serverlist->num_servers);
      }
      if (serverlist->num_servers > 0) {
        if (grpc_grpclb_serverlist_equals(lb_client->glb_policy->serverlist,
                                          serverlist)) {
          if (grpc_lb_glb_trace) {
            gpr_log(GPR_INFO,
                    "Incoming server list identical to current, ignoring.");
          }
        } else {
          if (lb_client->glb_policy->serverlist != NULL) {
            grpc_grpclb_destroy_serverlist(lb_client->glb_policy->serverlist);
          }
          lb_client->glb_policy->serverlist = serverlist;
        }
        if (lb_client->glb_policy->rr_policy == NULL) {
          rr_handover(exec_ctx, lb_client->glb_policy, error);
        } else {
          GRPC_LB_POLICY_UNREF(exec_ctx, lb_client->glb_policy->rr_policy,
                               "serverlist_received");
        }
      } else {
        if (grpc_lb_glb_trace) {
          gpr_log(GPR_INFO,
                  "Received empty server list. Picks will stay pending until a "
                  "response with > 0 servers is received");
        }
      }
      op->op = GRPC_OP_RECV_MESSAGE;
      op->data.recv_message = &lb_client->response_payload;
      op->flags = 0;
      op->reserved = NULL;
      op++;
      const grpc_call_error call_error = grpc_call_start_batch_and_execute(
          exec_ctx, lb_client->lb_call, ops, (size_t)(op - ops),
          &lb_client->res_rcvd);
      GPR_ASSERT(GRPC_CALL_OK == call_error);
      return;
    }
    GPR_ASSERT(serverlist == NULL);
    gpr_log(GPR_ERROR, "Invalid LB response received: '%s'",
            gpr_dump_slice(response_slice, GPR_DUMP_ASCII));
    gpr_slice_unref(response_slice);
    op->op = GRPC_OP_SEND_CLOSE_FROM_CLIENT;
    op->flags = 0;
    op->reserved = NULL;
    op++;
    grpc_call_error call_error = grpc_call_start_batch_and_execute(
        exec_ctx, lb_client->lb_call, ops, (size_t)(op - ops),
        &lb_client->close_sent);
    GPR_ASSERT(GRPC_CALL_OK == call_error);
  }
}
static void close_sent_cb(grpc_exec_ctx *exec_ctx, void *arg,
                          grpc_error *error) {
  if (grpc_lb_glb_trace) {
    gpr_log(GPR_INFO,
            "Close from LB client sent. Waiting from server status now");
  }
}
static void srv_status_rcvd_cb(grpc_exec_ctx *exec_ctx, void *arg,
                               grpc_error *error) {
  lb_client_data *lb_client = arg;
  if (grpc_lb_glb_trace) {
    gpr_log(GPR_INFO,
            "status from lb server received. Status = %d, Details = '%s', "
            "Capaticy "
            "= %zu",
            lb_client->status, lb_client->status_details,
            lb_client->status_details_capacity);
  }
}
static const grpc_lb_policy_vtable glb_lb_policy_vtable = {
    glb_destroy, glb_shutdown, glb_pick,
    glb_cancel_pick, glb_cancel_picks, glb_ping_one,
    glb_exit_idle, glb_check_connectivity, glb_notify_on_state_change};
static void glb_factory_ref(grpc_lb_policy_factory *factory) {}
static void glb_factory_unref(grpc_lb_policy_factory *factory) {}
static const grpc_lb_policy_factory_vtable glb_factory_vtable = {
    glb_factory_ref, glb_factory_unref, glb_create, "grpclb"};
static grpc_lb_policy_factory glb_lb_policy_factory = {&glb_factory_vtable};
grpc_lb_policy_factory *grpc_glb_lb_factory_create() {
  return &glb_lb_policy_factory;
}
void grpc_lb_policy_grpclb_init() {
  grpc_register_lb_policy(grpc_glb_lb_factory_create());
  grpc_register_tracer("glb", &grpc_lb_glb_trace);
}
void grpc_lb_policy_grpclb_shutdown() {}
