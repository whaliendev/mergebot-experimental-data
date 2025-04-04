#ifndef GRPC_CORE_EXT_CLIENT_CONFIG_LB_POLICY_H
#define GRPC_CORE_EXT_CLIENT_CONFIG_LB_POLICY_H 
#include "src/core/ext/client_config/subchannel.h"
#include "src/core/lib/iomgr/polling_entity.h"
#include "src/core/lib/transport/connectivity_state.h"
typedef struct grpc_lb_policy grpc_lb_policy;
typedef struct grpc_lb_policy_vtable grpc_lb_policy_vtable;
typedef void (*grpc_lb_completion)(void *cb_arg, grpc_subchannel *subchannel,
                                   grpc_status_code status, const char *errmsg);
struct grpc_lb_policy {
  const grpc_lb_policy_vtable *vtable;
  gpr_atm ref_pair;
  grpc_pollset_set *interested_parties;
};
typedef struct grpc_lb_policy_pick_args {
  grpc_polling_entity *pollent;
  grpc_metadata_batch *initial_metadata;
  uint32_t initial_metadata_flags;
  grpc_linked_mdelem *lb_token_mdelem_storage;
} grpc_lb_policy_pick_args;
struct grpc_lb_policy_vtable {
  void (*destroy)(grpc_exec_ctx *exec_ctx, grpc_lb_policy *policy);
  void (*shutdown)(grpc_exec_ctx *exec_ctx, grpc_lb_policy *policy);
  int (*pick)(grpc_exec_ctx *exec_ctx, grpc_lb_policy *policy,
<<<<<<< HEAD
              grpc_metadata_batch *initial_metadata,
              uint32_t initial_metadata_flags,
              grpc_connected_subchannel **target, grpc_closure *on_complete);
||||||| 79620aaa10
              grpc_polling_entity *pollent,
              grpc_metadata_batch *initial_metadata,
              uint32_t initial_metadata_flags,
              grpc_connected_subchannel **target, grpc_closure *on_complete);
=======
              const grpc_lb_policy_pick_args *pick_args,
              grpc_connected_subchannel **target, void **user_data,
              grpc_closure *on_complete);
>>>>>>> b410f80b
  void (*cancel_pick)(grpc_exec_ctx *exec_ctx, grpc_lb_policy *policy,
                      grpc_connected_subchannel **target);
  void (*cancel_picks)(grpc_exec_ctx *exec_ctx, grpc_lb_policy *policy,
                       uint32_t initial_metadata_flags_mask,
                       uint32_t initial_metadata_flags_eq);
  void (*ping_one)(grpc_exec_ctx *exec_ctx, grpc_lb_policy *policy,
                   grpc_closure *closure);
  void (*exit_idle)(grpc_exec_ctx *exec_ctx, grpc_lb_policy *policy);
  grpc_connectivity_state (*check_connectivity)(
      grpc_exec_ctx *exec_ctx, grpc_lb_policy *policy,
      grpc_error **connectivity_error);
  void (*notify_on_state_change)(grpc_exec_ctx *exec_ctx,
                                 grpc_lb_policy *policy,
                                 grpc_connectivity_state *state,
                                 grpc_closure *closure);
};
#ifdef GRPC_LB_POLICY_REFCOUNT_DEBUG
#define GRPC_LB_POLICY_REF(p,r) \
  grpc_lb_policy_ref((p), __FILE__, __LINE__, (r))
#define GRPC_LB_POLICY_UNREF(exec_ctx,p,r) \
  grpc_lb_policy_unref((exec_ctx), (p), __FILE__, __LINE__, (r))
#define GRPC_LB_POLICY_WEAK_REF(p,r) \
  grpc_lb_policy_weak_ref((p), __FILE__, __LINE__, (r))
#define GRPC_LB_POLICY_WEAK_UNREF(exec_ctx,p,r) \
  grpc_lb_policy_weak_unref((exec_ctx), (p), __FILE__, __LINE__, (r))
void grpc_lb_policy_ref(grpc_lb_policy *policy, const char *file, int line,
                        const char *reason);
void grpc_lb_policy_unref(grpc_exec_ctx *exec_ctx, grpc_lb_policy *policy,
                          const char *file, int line, const char *reason);
void grpc_lb_policy_weak_ref(grpc_lb_policy *policy, const char *file, int line,
                             const char *reason);
void grpc_lb_policy_weak_unref(grpc_exec_ctx *exec_ctx, grpc_lb_policy *policy,
                               const char *file, int line, const char *reason);
#else
#define GRPC_LB_POLICY_REF(p,r) grpc_lb_policy_ref((p))
#define GRPC_LB_POLICY_UNREF(cl,p,r) grpc_lb_policy_unref((cl), (p))
#define GRPC_LB_POLICY_WEAK_REF(p,r) grpc_lb_policy_weak_ref((p))
#define GRPC_LB_POLICY_WEAK_UNREF(cl,p,r) grpc_lb_policy_weak_unref((cl), (p))
void grpc_lb_policy_ref(grpc_lb_policy *policy);
void grpc_lb_policy_unref(grpc_exec_ctx *exec_ctx, grpc_lb_policy *policy);
void grpc_lb_policy_weak_ref(grpc_lb_policy *policy);
void grpc_lb_policy_weak_unref(grpc_exec_ctx *exec_ctx, grpc_lb_policy *policy);
#endif
void grpc_lb_policy_init(grpc_lb_policy *policy,
                         const grpc_lb_policy_vtable *vtable);
int grpc_lb_policy_pick(grpc_exec_ctx *exec_ctx, grpc_lb_policy *policy,
                        const grpc_lb_policy_pick_args *pick_args,
                        grpc_connected_subchannel **target, void **user_data,
                        grpc_closure *on_complete);
void grpc_lb_policy_ping_one(grpc_exec_ctx *exec_ctx, grpc_lb_policy *policy,
                             grpc_closure *closure);
void grpc_lb_policy_cancel_pick(grpc_exec_ctx *exec_ctx, grpc_lb_policy *policy,
                                grpc_connected_subchannel **target);
void grpc_lb_policy_cancel_picks(grpc_exec_ctx *exec_ctx,
                                 grpc_lb_policy *policy,
                                 uint32_t initial_metadata_flags_mask,
                                 uint32_t initial_metadata_flags_eq);
void grpc_lb_policy_exit_idle(grpc_exec_ctx *exec_ctx, grpc_lb_policy *policy);
void grpc_lb_policy_notify_on_state_change(grpc_exec_ctx *exec_ctx,
                                           grpc_lb_policy *policy,
                                           grpc_connectivity_state *state,
                                           grpc_closure *closure);
grpc_connectivity_state grpc_lb_policy_check_connectivity(
    grpc_exec_ctx *exec_ctx, grpc_lb_policy *policy,
    grpc_error **connectivity_error);
#endif
