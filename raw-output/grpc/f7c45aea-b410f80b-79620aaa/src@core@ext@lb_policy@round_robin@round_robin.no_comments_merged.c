#include <string.h>
#include <grpc/support/alloc.h>
#include "src/core/ext/client_config/lb_policy_registry.h"
#include "src/core/lib/debug/trace.h"
#include "src/core/lib/transport/connectivity_state.h"
#include "src/core/lib/transport/static_metadata.h"
typedef struct round_robin_lb_policy round_robin_lb_policy;
int grpc_lb_round_robin_trace = 0;
typedef struct pending_pick {
  struct pending_pick *next;
<<<<<<< HEAD
||||||| 79620aaa10
  grpc_polling_entity *pollent;
=======
  grpc_polling_entity *pollent;
  void **user_data;
>>>>>>> b410f80b
  uint32_t initial_metadata_flags;
  grpc_connected_subchannel **target;
  grpc_closure *on_complete;
} pending_pick;
typedef struct ready_list {
  grpc_subchannel *subchannel;
  void *user_data;
  struct ready_list *next;
  struct ready_list *prev;
} ready_list;
typedef struct {
  size_t index;
  round_robin_lb_policy *policy;
  grpc_subchannel *subchannel;
  grpc_closure connectivity_changed_closure;
  ready_list *ready_list_node;
  grpc_connectivity_state connectivity_state;
  void *user_data;
} subchannel_data;
struct round_robin_lb_policy {
  grpc_lb_policy base;
  size_t num_addresses;
  void **user_data_pointers;
  size_t num_subchannels;
  subchannel_data **subchannels;
  gpr_mu mu;
  int started_picking;
  int shutdown;
  pending_pick *pending_picks;
  grpc_connectivity_state_tracker state_tracker;
  ready_list ready_list;
  ready_list *ready_list_last_pick;
};
static ready_list *peek_next_connected_locked(const round_robin_lb_policy *p) {
  ready_list *selected;
  selected = p->ready_list_last_pick->next;
  while (selected != NULL) {
    if (selected == &p->ready_list) {
      GPR_ASSERT(selected->subchannel == NULL);
      selected = selected->next;
    } else {
      GPR_ASSERT(selected->subchannel != NULL);
      return selected;
    }
  }
  return NULL;
}
static void advance_last_picked_locked(round_robin_lb_policy *p) {
  if (p->ready_list_last_pick->next != NULL) {
    p->ready_list_last_pick = p->ready_list_last_pick->next;
    if (p->ready_list_last_pick == &p->ready_list) {
      p->ready_list_last_pick = p->ready_list_last_pick->next;
    }
  } else {
    GPR_ASSERT(p->ready_list_last_pick == &p->ready_list);
  }
  if (grpc_lb_round_robin_trace) {
    gpr_log(GPR_DEBUG, "[READYLIST] ADVANCED LAST PICK. NOW AT NODE %p (SC %p)",
            (void *)p->ready_list_last_pick,
            (void *)p->ready_list_last_pick->subchannel);
  }
}
static ready_list *add_connected_sc_locked(round_robin_lb_policy *p,
                                           subchannel_data *sd) {
  ready_list *new_elem = gpr_malloc(sizeof(ready_list));
  memset(new_elem, 0, sizeof(ready_list));
  new_elem->subchannel = sd->subchannel;
  new_elem->user_data = sd->user_data;
  if (p->ready_list.prev == NULL) {
    new_elem->next = &p->ready_list;
    new_elem->prev = &p->ready_list;
    p->ready_list.next = new_elem;
    p->ready_list.prev = new_elem;
  } else {
    new_elem->next = &p->ready_list;
    new_elem->prev = p->ready_list.prev;
    p->ready_list.prev->next = new_elem;
    p->ready_list.prev = new_elem;
  }
  if (grpc_lb_round_robin_trace) {
    gpr_log(GPR_DEBUG, "[READYLIST] ADDING NODE %p (Conn. SC %p)",
            (void *)new_elem, (void *)sd->subchannel);
  }
  return new_elem;
}
static void remove_disconnected_sc_locked(round_robin_lb_policy *p,
                                          ready_list *node) {
  if (node == NULL) {
    return;
  }
  if (node == p->ready_list_last_pick) {
    p->ready_list_last_pick = p->ready_list_last_pick->prev;
  }
  if (node->next == &p->ready_list && node->prev == &p->ready_list) {
    GPR_ASSERT(p->ready_list.next == node);
    GPR_ASSERT(p->ready_list.prev == node);
    p->ready_list.next = NULL;
    p->ready_list.prev = NULL;
  } else {
    node->prev->next = node->next;
    node->next->prev = node->prev;
  }
  if (grpc_lb_round_robin_trace) {
    gpr_log(GPR_DEBUG, "[READYLIST] REMOVED NODE %p (SC %p)", (void *)node,
            (void *)node->subchannel);
  }
  node->next = NULL;
  node->prev = NULL;
  node->subchannel = NULL;
  gpr_free(node);
}
static void rr_destroy(grpc_exec_ctx *exec_ctx, grpc_lb_policy *pol) {
  round_robin_lb_policy *p = (round_robin_lb_policy *)pol;
  ready_list *elem;
  for (size_t i = 0; i < p->num_subchannels; i++) {
    subchannel_data *sd = p->subchannels[i];
    GRPC_SUBCHANNEL_UNREF(exec_ctx, sd->subchannel, "round_robin");
    gpr_free(sd);
  }
  grpc_connectivity_state_destroy(exec_ctx, &p->state_tracker);
  gpr_free(p->subchannels);
  gpr_mu_destroy(&p->mu);
  elem = p->ready_list.next;
  while (elem != NULL && elem != &p->ready_list) {
    ready_list *tmp;
    tmp = elem->next;
    elem->next = NULL;
    elem->prev = NULL;
    elem->subchannel = NULL;
    gpr_free(elem);
    elem = tmp;
  }
  gpr_free(p->user_data_pointers);
  gpr_free(p);
}
static void rr_shutdown(grpc_exec_ctx *exec_ctx, grpc_lb_policy *pol) {
  round_robin_lb_policy *p = (round_robin_lb_policy *)pol;
  pending_pick *pp;
  size_t i;
  gpr_mu_lock(&p->mu);
  p->shutdown = 1;
  while ((pp = p->pending_picks)) {
    p->pending_picks = pp->next;
    *pp->target = NULL;
    grpc_exec_ctx_sched(exec_ctx, pp->on_complete,
                        GRPC_ERROR_CREATE("Channel Shutdown"), NULL);
    gpr_free(pp);
  }
  grpc_connectivity_state_set(
      exec_ctx, &p->state_tracker, GRPC_CHANNEL_SHUTDOWN,
      GRPC_ERROR_CREATE("Channel Shutdown"), "shutdown");
  for (i = 0; i < p->num_subchannels; i++) {
    subchannel_data *sd = p->subchannels[i];
    grpc_subchannel_notify_on_state_change(exec_ctx, sd->subchannel, NULL, NULL,
                                           &sd->connectivity_changed_closure);
  }
  gpr_mu_unlock(&p->mu);
}
static void rr_cancel_pick(grpc_exec_ctx *exec_ctx, grpc_lb_policy *pol,
                           grpc_connected_subchannel **target) {
  round_robin_lb_policy *p = (round_robin_lb_policy *)pol;
  pending_pick *pp;
  gpr_mu_lock(&p->mu);
  pp = p->pending_picks;
  p->pending_picks = NULL;
  while (pp != NULL) {
    pending_pick *next = pp->next;
    if (pp->target == target) {
      *target = NULL;
      grpc_exec_ctx_sched(exec_ctx, pp->on_complete, GRPC_ERROR_CANCELLED,
                          NULL);
      gpr_free(pp);
    } else {
      pp->next = p->pending_picks;
      p->pending_picks = pp;
    }
    pp = next;
  }
  gpr_mu_unlock(&p->mu);
}
static void rr_cancel_picks(grpc_exec_ctx *exec_ctx, grpc_lb_policy *pol,
                            uint32_t initial_metadata_flags_mask,
                            uint32_t initial_metadata_flags_eq) {
  round_robin_lb_policy *p = (round_robin_lb_policy *)pol;
  pending_pick *pp;
  gpr_mu_lock(&p->mu);
  pp = p->pending_picks;
  p->pending_picks = NULL;
  while (pp != NULL) {
    pending_pick *next = pp->next;
    if ((pp->initial_metadata_flags & initial_metadata_flags_mask) ==
        initial_metadata_flags_eq) {
      *pp->target = NULL;
      grpc_exec_ctx_sched(exec_ctx, pp->on_complete, GRPC_ERROR_CANCELLED,
                          NULL);
      gpr_free(pp);
    } else {
      pp->next = p->pending_picks;
      p->pending_picks = pp;
    }
    pp = next;
  }
  gpr_mu_unlock(&p->mu);
}
static void start_picking(grpc_exec_ctx *exec_ctx, round_robin_lb_policy *p) {
  size_t i;
  p->started_picking = 1;
  if (grpc_lb_round_robin_trace) {
    gpr_log(GPR_DEBUG, "LB_POLICY: p=%p num_subchannels=%" PRIuPTR, (void *)p,
            p->num_subchannels);
  }
  for (i = 0; i < p->num_subchannels; i++) {
    subchannel_data *sd = p->subchannels[i];
    sd->connectivity_state = GRPC_CHANNEL_IDLE;
    grpc_subchannel_notify_on_state_change(
        exec_ctx, sd->subchannel, p->base.interested_parties,
        &sd->connectivity_state, &sd->connectivity_changed_closure);
    GRPC_LB_POLICY_WEAK_REF(&p->base, "round_robin_connectivity");
  }
}
static void rr_exit_idle(grpc_exec_ctx *exec_ctx, grpc_lb_policy *pol) {
  round_robin_lb_policy *p = (round_robin_lb_policy *)pol;
  gpr_mu_lock(&p->mu);
  if (!p->started_picking) {
    start_picking(exec_ctx, p);
  }
  gpr_mu_unlock(&p->mu);
}
static int rr_pick(grpc_exec_ctx *exec_ctx, grpc_lb_policy *pol,
<<<<<<< HEAD
                   grpc_metadata_batch *initial_metadata,
                   uint32_t initial_metadata_flags,
                   grpc_connected_subchannel **target,
||||||| 79620aaa10
                   grpc_polling_entity *pollent,
                   grpc_metadata_batch *initial_metadata,
                   uint32_t initial_metadata_flags,
                   grpc_connected_subchannel **target,
=======
                   const grpc_lb_policy_pick_args *pick_args,
                   grpc_connected_subchannel **target, void **user_data,
>>>>>>> b410f80b
                   grpc_closure *on_complete) {
  round_robin_lb_policy *p = (round_robin_lb_policy *)pol;
  pending_pick *pp;
  ready_list *selected;
  gpr_mu_lock(&p->mu);
  if ((selected = peek_next_connected_locked(p))) {
    gpr_mu_unlock(&p->mu);
    *target = grpc_subchannel_get_connected_subchannel(selected->subchannel);
    *user_data = selected->user_data;
    if (grpc_lb_round_robin_trace) {
      gpr_log(GPR_DEBUG,
              "[RR PICK] TARGET <-- CONNECTED SUBCHANNEL %p (NODE %p)",
              (void *)*target, (void *)selected);
    }
    advance_last_picked_locked(p);
    return 1;
  } else {
    if (!p->started_picking) {
      start_picking(exec_ctx, p);
    }
<<<<<<< HEAD
||||||| 79620aaa10
    grpc_polling_entity_add_to_pollset_set(exec_ctx, pollent,
                                           p->base.interested_parties);
=======
    grpc_polling_entity_add_to_pollset_set(exec_ctx, pick_args->pollent,
                                           p->base.interested_parties);
>>>>>>> b410f80b
    pp = gpr_malloc(sizeof(*pp));
    pp->next = p->pending_picks;
<<<<<<< HEAD
||||||| 79620aaa10
    pp->pollent = pollent;
=======
    pp->pollent = pick_args->pollent;
>>>>>>> b410f80b
    pp->target = target;
    pp->on_complete = on_complete;
    pp->initial_metadata_flags = pick_args->initial_metadata_flags;
    pp->user_data = user_data;
    p->pending_picks = pp;
    gpr_mu_unlock(&p->mu);
    return 0;
  }
}
static void rr_connectivity_changed(grpc_exec_ctx *exec_ctx, void *arg,
                                    grpc_error *error) {
  subchannel_data *sd = arg;
  round_robin_lb_policy *p = sd->policy;
  pending_pick *pp;
  ready_list *selected;
  int unref = 0;
  GRPC_ERROR_REF(error);
  gpr_mu_lock(&p->mu);
  if (p->shutdown) {
    unref = 1;
  } else {
    switch (sd->connectivity_state) {
      case GRPC_CHANNEL_READY:
        grpc_connectivity_state_set(exec_ctx, &p->state_tracker,
                                    GRPC_CHANNEL_READY, GRPC_ERROR_REF(error),
                                    "connecting_ready");
        sd->ready_list_node = add_connected_sc_locked(p, sd);
        selected = peek_next_connected_locked(p);
        if (p->pending_picks != NULL) {
          advance_last_picked_locked(p);
        }
        while ((pp = p->pending_picks)) {
          p->pending_picks = pp->next;
          *pp->target =
              grpc_subchannel_get_connected_subchannel(selected->subchannel);
          *pp->user_data = selected->user_data;
          if (grpc_lb_round_robin_trace) {
            gpr_log(GPR_DEBUG,
                    "[RR CONN CHANGED] TARGET <-- SUBCHANNEL %p (NODE %p)",
                    (void *)selected->subchannel, (void *)selected);
          }
          grpc_exec_ctx_sched(exec_ctx, pp->on_complete, GRPC_ERROR_NONE, NULL);
          gpr_free(pp);
        }
        grpc_subchannel_notify_on_state_change(
            exec_ctx, sd->subchannel, p->base.interested_parties,
            &sd->connectivity_state, &sd->connectivity_changed_closure);
        break;
      case GRPC_CHANNEL_CONNECTING:
      case GRPC_CHANNEL_IDLE:
        grpc_connectivity_state_set(
            exec_ctx, &p->state_tracker, sd->connectivity_state,
            GRPC_ERROR_REF(error), "connecting_changed");
        grpc_subchannel_notify_on_state_change(
            exec_ctx, sd->subchannel, p->base.interested_parties,
            &sd->connectivity_state, &sd->connectivity_changed_closure);
        break;
      case GRPC_CHANNEL_TRANSIENT_FAILURE:
        grpc_subchannel_notify_on_state_change(
            exec_ctx, sd->subchannel, p->base.interested_parties,
            &sd->connectivity_state, &sd->connectivity_changed_closure);
        if (sd->ready_list_node != NULL) {
          remove_disconnected_sc_locked(p, sd->ready_list_node);
          sd->ready_list_node = NULL;
        }
        grpc_connectivity_state_set(
            exec_ctx, &p->state_tracker, GRPC_CHANNEL_TRANSIENT_FAILURE,
            GRPC_ERROR_REF(error), "connecting_transient_failure");
        break;
      case GRPC_CHANNEL_SHUTDOWN:
        if (sd->ready_list_node != NULL) {
          remove_disconnected_sc_locked(p, sd->ready_list_node);
          sd->ready_list_node = NULL;
        }
        p->num_subchannels--;
        GPR_SWAP(subchannel_data *, p->subchannels[sd->index],
                 p->subchannels[p->num_subchannels]);
        GRPC_SUBCHANNEL_UNREF(exec_ctx, sd->subchannel, "round_robin");
        p->subchannels[sd->index]->index = sd->index;
        gpr_free(sd);
        unref = 1;
        if (p->num_subchannels == 0) {
          grpc_connectivity_state_set(
              exec_ctx, &p->state_tracker, GRPC_CHANNEL_SHUTDOWN,
              GRPC_ERROR_CREATE_REFERENCING("Round Robin Channels Exhausted",
                                            &error, 1),
              "no_more_channels");
          while ((pp = p->pending_picks)) {
            p->pending_picks = pp->next;
            *pp->target = NULL;
            grpc_exec_ctx_sched(exec_ctx, pp->on_complete, GRPC_ERROR_NONE,
                                NULL);
            gpr_free(pp);
          }
        } else {
          grpc_connectivity_state_set(
              exec_ctx, &p->state_tracker, GRPC_CHANNEL_TRANSIENT_FAILURE,
              GRPC_ERROR_REF(error), "subchannel_failed");
        }
    }
  }
  gpr_mu_unlock(&p->mu);
  if (unref) {
    GRPC_LB_POLICY_WEAK_UNREF(exec_ctx, &p->base, "round_robin_connectivity");
  }
  GRPC_ERROR_UNREF(error);
}
static grpc_connectivity_state rr_check_connectivity(grpc_exec_ctx *exec_ctx,
                                                     grpc_lb_policy *pol,
                                                     grpc_error **error) {
  round_robin_lb_policy *p = (round_robin_lb_policy *)pol;
  grpc_connectivity_state st;
  gpr_mu_lock(&p->mu);
  st = grpc_connectivity_state_check(&p->state_tracker, error);
  gpr_mu_unlock(&p->mu);
  return st;
}
static void rr_notify_on_state_change(grpc_exec_ctx *exec_ctx,
                                      grpc_lb_policy *pol,
                                      grpc_connectivity_state *current,
                                      grpc_closure *notify) {
  round_robin_lb_policy *p = (round_robin_lb_policy *)pol;
  gpr_mu_lock(&p->mu);
  grpc_connectivity_state_notify_on_state_change(exec_ctx, &p->state_tracker,
                                                 current, notify);
  gpr_mu_unlock(&p->mu);
}
static void rr_ping_one(grpc_exec_ctx *exec_ctx, grpc_lb_policy *pol,
                        grpc_closure *closure) {
  round_robin_lb_policy *p = (round_robin_lb_policy *)pol;
  ready_list *selected;
  grpc_connected_subchannel *target;
  gpr_mu_lock(&p->mu);
  if ((selected = peek_next_connected_locked(p))) {
    gpr_mu_unlock(&p->mu);
    target = grpc_subchannel_get_connected_subchannel(selected->subchannel);
    grpc_connected_subchannel_ping(exec_ctx, target, closure);
  } else {
    gpr_mu_unlock(&p->mu);
    grpc_exec_ctx_sched(exec_ctx, closure,
                        GRPC_ERROR_CREATE("Round Robin not connected"), NULL);
  }
}
static const grpc_lb_policy_vtable round_robin_lb_policy_vtable = {
    rr_destroy, rr_shutdown, rr_pick,
    rr_cancel_pick, rr_cancel_picks, rr_ping_one,
    rr_exit_idle, rr_check_connectivity, rr_notify_on_state_change};
static void round_robin_factory_ref(grpc_lb_policy_factory *factory) {}
static void round_robin_factory_unref(grpc_lb_policy_factory *factory) {}
static grpc_lb_policy *round_robin_create(grpc_exec_ctx *exec_ctx,
                                          grpc_lb_policy_factory *factory,
                                          grpc_lb_policy_args *args) {
  GPR_ASSERT(args->addresses != NULL);
  GPR_ASSERT(args->client_channel_factory != NULL);
  if (args->num_addresses == 0) return NULL;
  round_robin_lb_policy *p = gpr_malloc(sizeof(*p));
  memset(p, 0, sizeof(*p));
  p->num_addresses = args->num_addresses;
  p->subchannels = gpr_malloc(sizeof(subchannel_data) * p->num_addresses);
  memset(p->subchannels, 0, sizeof(*p->subchannels) * p->num_addresses);
  p->user_data_pointers = gpr_malloc(sizeof(void *) * p->num_addresses);
  memset(p->user_data_pointers, 0, sizeof(void *) * p->num_addresses);
  grpc_subchannel_args sc_args;
  size_t subchannel_idx = 0;
  for (size_t i = 0; i < p->num_addresses; i++) {
    memset(&sc_args, 0, sizeof(grpc_subchannel_args));
    sc_args.addr = (struct sockaddr *)args->addresses[i].resolved_address->addr;
    sc_args.addr_len = args->addresses[i].resolved_address->len;
    p->user_data_pointers[i] = args->addresses[i].user_data;
    grpc_subchannel *subchannel = grpc_client_channel_factory_create_subchannel(
        exec_ctx, args->client_channel_factory, &sc_args);
    if (subchannel != NULL) {
      subchannel_data *sd = gpr_malloc(sizeof(*sd));
      memset(sd, 0, sizeof(*sd));
      p->subchannels[subchannel_idx] = sd;
      sd->policy = p;
      sd->index = subchannel_idx;
      sd->subchannel = subchannel;
      sd->user_data = p->user_data_pointers[i];
      ++subchannel_idx;
      grpc_closure_init(&sd->connectivity_changed_closure,
                        rr_connectivity_changed, sd);
    }
  }
  if (subchannel_idx == 0) {
    gpr_free(p->subchannels);
    gpr_free(p);
    return NULL;
  }
  p->num_subchannels = subchannel_idx;
  p->ready_list.subchannel = NULL;
  p->ready_list.prev = NULL;
  p->ready_list.next = NULL;
  p->ready_list_last_pick = &p->ready_list;
  grpc_lb_policy_init(&p->base, &round_robin_lb_policy_vtable);
  grpc_connectivity_state_init(&p->state_tracker, GRPC_CHANNEL_IDLE,
                               "round_robin");
  gpr_mu_init(&p->mu);
  return &p->base;
}
static const grpc_lb_policy_factory_vtable round_robin_factory_vtable = {
    round_robin_factory_ref, round_robin_factory_unref, round_robin_create,
    "round_robin"};
static grpc_lb_policy_factory round_robin_lb_policy_factory = {
    &round_robin_factory_vtable};
static grpc_lb_policy_factory *round_robin_lb_factory_create() {
  return &round_robin_lb_policy_factory;
}
void grpc_lb_policy_round_robin_init() {
  grpc_register_lb_policy(round_robin_lb_factory_create());
  grpc_register_tracer("round_robin", &grpc_lb_round_robin_trace);
}
void grpc_lb_policy_round_robin_shutdown() {}
