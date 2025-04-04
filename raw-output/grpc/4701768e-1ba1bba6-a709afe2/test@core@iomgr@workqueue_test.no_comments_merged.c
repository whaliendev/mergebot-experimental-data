#include "src/core/lib/iomgr/workqueue.h"
#include <grpc/grpc.h>
#include <grpc/support/alloc.h>
#include <grpc/support/log.h>
#include "test/core/util/test_config.h"
static gpr_mu *g_mu;
static grpc_pollset *g_pollset;
static void must_succeed(grpc_exec_ctx *exec_ctx, void *p, grpc_error *error) {
  GPR_ASSERT(error == GRPC_ERROR_NONE);
  gpr_mu_lock(g_mu);
  *(int *)p = 1;
  GPR_ASSERT(
      GRPC_LOG_IF_ERROR("pollset_kick", grpc_pollset_kick(g_pollset, NULL)));
  gpr_mu_unlock(g_mu);
}
static void test_ref_unref(void) {
  grpc_exec_ctx exec_ctx = GRPC_EXEC_CTX_INIT;
  grpc_workqueue *wq;
  GPR_ASSERT(GRPC_LOG_IF_ERROR("grpc_workqueue_create",
                               grpc_workqueue_create(&exec_ctx, &wq)));
  GRPC_WORKQUEUE_REF(wq, "test");
  GRPC_WORKQUEUE_UNREF(&exec_ctx, wq, "test");
  GRPC_WORKQUEUE_UNREF(&exec_ctx, wq, "destroy");
  grpc_exec_ctx_finish(&exec_ctx);
}
static void test_add_closure(void) {
  grpc_closure c;
  int done = 0;
  grpc_exec_ctx exec_ctx = GRPC_EXEC_CTX_INIT;
  grpc_workqueue *wq;
  GPR_ASSERT(GRPC_LOG_IF_ERROR("grpc_workqueue_create",
                               grpc_workqueue_create(&exec_ctx, &wq)));
  gpr_timespec deadline = GRPC_TIMEOUT_SECONDS_TO_DEADLINE(5);
  grpc_pollset_worker *worker = NULL;
  grpc_closure_init(&c, must_succeed, &done);
  grpc_workqueue_push(&exec_ctx, wq, &c, GRPC_ERROR_NONE);
  grpc_workqueue_add_to_pollset(&exec_ctx, wq, g_pollset);
  gpr_mu_lock(g_mu);
  GPR_ASSERT(!done);
<<<<<<< HEAD
  GPR_ASSERT(GRPC_LOG_IF_ERROR(
      "pollset_work",
      grpc_pollset_work(&exec_ctx, g_pollset, &worker,
                        gpr_now(deadline.clock_type), deadline)));
||||||| a709afe241
  grpc_pollset_work(&exec_ctx, g_pollset, &worker, gpr_now(deadline.clock_type),
                    deadline);
=======
  while (!done) {
    grpc_pollset_work(&exec_ctx, g_pollset, &worker,
                      gpr_now(deadline.clock_type), deadline);
  }
>>>>>>> 1ba1bba6
  gpr_mu_unlock(g_mu);
  grpc_exec_ctx_finish(&exec_ctx);
  GPR_ASSERT(done);
  GRPC_WORKQUEUE_UNREF(&exec_ctx, wq, "destroy");
  grpc_exec_ctx_finish(&exec_ctx);
}
static void test_flush(void) {
  grpc_closure c;
  int done = 0;
  grpc_exec_ctx exec_ctx = GRPC_EXEC_CTX_INIT;
  grpc_workqueue *wq;
  GPR_ASSERT(GRPC_LOG_IF_ERROR("grpc_workqueue_create",
                               grpc_workqueue_create(&exec_ctx, &wq)));
  gpr_timespec deadline = GRPC_TIMEOUT_SECONDS_TO_DEADLINE(5);
  grpc_pollset_worker *worker = NULL;
  grpc_closure_init(&c, must_succeed, &done);
  grpc_exec_ctx_push(&exec_ctx, &c, GRPC_ERROR_NONE, NULL);
  grpc_workqueue_flush(&exec_ctx, wq);
  grpc_workqueue_add_to_pollset(&exec_ctx, wq, g_pollset);
  gpr_mu_lock(g_mu);
<<<<<<< HEAD
  GPR_ASSERT(!done);
  GPR_ASSERT(GRPC_LOG_IF_ERROR(
      "pollset_work",
      grpc_pollset_work(&exec_ctx, g_pollset, &worker,
                        gpr_now(deadline.clock_type), deadline)));
||||||| a709afe241
  GPR_ASSERT(!done);
  grpc_pollset_work(&exec_ctx, g_pollset, &worker, gpr_now(deadline.clock_type),
                    deadline);
=======
  while (!done) {
    grpc_pollset_work(&exec_ctx, g_pollset, &worker,
                      gpr_now(deadline.clock_type), deadline);
  }
>>>>>>> 1ba1bba6
  gpr_mu_unlock(g_mu);
  grpc_exec_ctx_finish(&exec_ctx);
  GPR_ASSERT(done);
  GRPC_WORKQUEUE_UNREF(&exec_ctx, wq, "destroy");
  grpc_exec_ctx_finish(&exec_ctx);
}
static void destroy_pollset(grpc_exec_ctx *exec_ctx, void *p,
                            grpc_error *error) {
  grpc_pollset_destroy(p);
}
int main(int argc, char **argv) {
  grpc_closure destroyed;
  grpc_exec_ctx exec_ctx = GRPC_EXEC_CTX_INIT;
  grpc_test_init(argc, argv);
  grpc_init();
  g_pollset = gpr_malloc(grpc_pollset_size());
  grpc_pollset_init(g_pollset, &g_mu);
  test_ref_unref();
  test_add_closure();
  test_flush();
  grpc_closure_init(&destroyed, destroy_pollset, g_pollset);
  grpc_pollset_shutdown(&exec_ctx, g_pollset, &destroyed);
  grpc_exec_ctx_finish(&exec_ctx);
  grpc_shutdown();
  gpr_free(g_pollset);
  return 0;
}
