#include "src/core/surface/channel.h"
#include <stdlib.h>
#include <string.h>
#include "src/core/surface/call.h"
#include "src/core/surface/client.h"
#include <grpc/support/alloc.h>
#include <grpc/support/log.h>
struct grpc_channel {
  int is_client;
  gpr_refcount refs;
  grpc_mdctx *metadata_context;
  grpc_mdstr *grpc_status_string;
  grpc_mdstr *grpc_message_string;
  grpc_mdstr *path_string;
  grpc_mdstr *authority_string;
};
grpc_channel *grpc_channel_create_from_filters(
    const grpc_channel_filter **filters, size_t num_filters,
    const grpc_channel_args *args, grpc_mdctx *mdctx, int is_client) {
  size_t size =
      sizeof(grpc_channel) + grpc_channel_stack_size(filters, num_filters);
  grpc_channel *channel = gpr_malloc(size);
  channel->is_client = is_client;
  gpr_ref_init(&channel->refs, 1);
  channel->metadata_context = mdctx;
  channel->grpc_status_string = grpc_mdstr_from_string(mdctx, "grpc-status");
  channel->grpc_message_string = grpc_mdstr_from_string(mdctx, "grpc-message");
  channel->path_string = grpc_mdstr_from_string(mdctx, ":path");
  channel->authority_string = grpc_mdstr_from_string(mdctx, ":authority");
  grpc_channel_stack_init(filters, num_filters, args, channel->metadata_context,
                          CHANNEL_STACK_FROM_CHANNEL(channel));
  return channel;
}
static void do_nothing(void *ignored, grpc_op_error error) {}
grpc_call *grpc_channel_create_call_old(grpc_channel *channel, const char *method, const char *host, gpr_timespec absolute_deadline) {
  grpc_call *call;
  grpc_mdelem *path_mdelem;
  grpc_mdelem *authority_mdelem;
  if (!channel->is_client) {
    gpr_log(GPR_ERROR, "Cannot create a call on the server.");
    return NULL;
  }
  call = grpc_call_create(channel, NULL);
  grpc_mdstr_ref(channel->path_string);
  path_mdelem = grpc_mdelem_from_metadata_strings(
      channel->metadata_context, channel->path_string,
      grpc_mdstr_from_string(channel->metadata_context, method));
  grpc_call_add_mdelem(call, path_mdelem, 0);
  grpc_mdstr_ref(channel->authority_string);
  authority_mdelem = grpc_mdelem_from_metadata_strings(
      channel->metadata_context, channel->authority_string,
      grpc_mdstr_from_string(channel->metadata_context, host));
  grpc_call_add_mdelem(call, authority_mdelem, 0);
  if (0 != gpr_time_cmp(absolute_deadline, gpr_inf_future)) {
    grpc_call_op op;
    op.type = GRPC_SEND_DEADLINE;
    op.dir = GRPC_CALL_DOWN;
    op.flags = 0;
    op.data.deadline = absolute_deadline;
    op.done_cb = do_nothing;
    op.user_data = NULL;
    grpc_call_execute_op(call, &op);
  }
  return call;
}
void grpc_channel_internal_ref(grpc_channel *channel) {
  gpr_ref(&channel->refs);
}
void grpc_channel_internal_unref(grpc_channel *channel) {
  if (gpr_unref(&channel->refs)) {
    grpc_channel_stack_destroy(CHANNEL_STACK_FROM_CHANNEL(channel));
    grpc_mdstr_unref(channel->grpc_status_string);
    grpc_mdstr_unref(channel->grpc_message_string);
    grpc_mdstr_unref(channel->path_string);
    grpc_mdstr_unref(channel->authority_string);
    grpc_mdctx_orphan(channel->metadata_context);
    gpr_free(channel);
  }
}
void grpc_channel_destroy(grpc_channel *channel) {
  grpc_channel_op op;
  grpc_channel_element *elem;
  elem = grpc_channel_stack_element(CHANNEL_STACK_FROM_CHANNEL(channel), 0);
  op.type = GRPC_CHANNEL_GOAWAY;
  op.dir = GRPC_CALL_DOWN;
  op.data.goaway.status = GRPC_STATUS_OK;
  op.data.goaway.message = gpr_slice_from_copied_string("Client disconnect");
  elem->filter->channel_op(elem, NULL, &op);
  op.type = GRPC_CHANNEL_DISCONNECT;
  op.dir = GRPC_CALL_DOWN;
  elem->filter->channel_op(elem, NULL, &op);
  grpc_channel_internal_unref(channel);
}
grpc_channel_stack *grpc_channel_get_channel_stack(grpc_channel *channel) {
  return CHANNEL_STACK_FROM_CHANNEL(channel);
}
grpc_mdctx *grpc_channel_get_metadata_context(grpc_channel *channel) {
  return channel->metadata_context;
}
grpc_mdstr *grpc_channel_get_status_string(grpc_channel *channel) {
  return channel->grpc_status_string;
}
grpc_mdstr *grpc_channel_get_message_string(grpc_channel *channel) {
  return channel->grpc_message_string;
}
