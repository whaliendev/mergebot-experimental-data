#include <sys/types.h>
#include <sys/socket.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "tmux.h"
static enum cmd_retval cmd_pipe_pane_exec(struct cmd *, struct cmdq_item *);
static void cmd_pipe_pane_write_callback(struct bufferevent *, void *);
static void cmd_pipe_pane_error_callback(struct bufferevent *, short, void *);
const struct cmd_entry cmd_pipe_pane_entry = {
 .name = "pipe-pane",
 .alias = "pipep",
 .args = { "ot:", 0, 1 },
 .usage = "[-o] " CMD_TARGET_PANE_USAGE " [command]",
 .target = { 't', CMD_FIND_PANE, 0 },
 .flags = CMD_AFTERHOOK,
 .exec = cmd_pipe_pane_exec
};
static enum cmd_retval
cmd_pipe_pane_exec(struct cmd *self, struct cmdq_item *item)
{
 struct args *args = self->args;
 struct client *c = cmd_find_client(item, NULL, 1);
 struct window_pane *wp = item->target.wp;
 struct session *s = item->target.s;
 struct winlink *wl = item->target.wl;
 char *cmd;
 int old_fd, pipe_fd[2], null_fd;
 struct format_tree *ft;
 sigset_t set, oldset;
 old_fd = wp->pipe_fd;
 if (wp->pipe_fd != -1) {
  bufferevent_free(wp->pipe_event);
  close(wp->pipe_fd);
  wp->pipe_fd = -1;
  if (window_pane_destroy_ready(wp)) {
   server_destroy_pane(wp, 1);
   return (CMD_RETURN_NORMAL);
  }
 }
 if (args->argc == 0 || *args->argv[0] == '\0')
  return (CMD_RETURN_NORMAL);
 if (args_has(self->args, 'o') && old_fd != -1)
  return (CMD_RETURN_NORMAL);
 if (socketpair(AF_UNIX, SOCK_STREAM, PF_UNSPEC, pipe_fd) != 0) {
  cmdq_error(item, "socketpair error: %s", strerror(errno));
  return (CMD_RETURN_ERROR);
 }
 ft = format_create(item->client, item, FORMAT_NONE, 0);
 format_defaults(ft, c, s, wl, wp);
 cmd = format_expand_time(ft, args->argv[0], time(NULL));
 format_free(ft);
 sigfillset(&set);
 sigprocmask(SIG_BLOCK, &set, &oldset);
 switch (fork()) {
 case -1:
  sigprocmask(SIG_SETMASK, &oldset, NULL);
  cmdq_error(item, "fork error: %s", strerror(errno));
  free(cmd);
  return (CMD_RETURN_ERROR);
 case 0:
  proc_clear_signals(server_proc);
  sigprocmask(SIG_SETMASK, &oldset, NULL);
  close(pipe_fd[0]);
  if (dup2(pipe_fd[1], STDIN_FILENO) == -1)
   _exit(1);
  if (pipe_fd[1] != STDIN_FILENO)
   close(pipe_fd[1]);
  null_fd = open(_PATH_DEVNULL, O_WRONLY, 0);
  if (dup2(null_fd, STDOUT_FILENO) == -1)
   _exit(1);
  if (dup2(null_fd, STDERR_FILENO) == -1)
   _exit(1);
  if (null_fd != STDOUT_FILENO && null_fd != STDERR_FILENO)
   close(null_fd);
  closefrom(STDERR_FILENO + 1);
  execl(_PATH_BSHELL, "sh", "-c", cmd, (char *) NULL);
  _exit(1);
 default:
  sigprocmask(SIG_SETMASK, &oldset, NULL);
  close(pipe_fd[1]);
  wp->pipe_fd = pipe_fd[0];
  wp->pipe_off = EVBUFFER_LENGTH(wp->event->input);
  wp->pipe_event = bufferevent_new(wp->pipe_fd, NULL,
      cmd_pipe_pane_write_callback, cmd_pipe_pane_error_callback,
      wp);
  bufferevent_enable(wp->pipe_event, EV_WRITE);
  setblocking(wp->pipe_fd, 0);
  free(cmd);
  return (CMD_RETURN_NORMAL);
 }
}
static void
cmd_pipe_pane_write_callback(__unused struct bufferevent *bufev, void *data)
{
 struct window_pane *wp = data;
 log_debug("%%%u pipe empty", wp->id);
 if (window_pane_destroy_ready(wp))
  server_destroy_pane(wp, 1);
}
static void
cmd_pipe_pane_error_callback(__unused struct bufferevent *bufev,
    __unused short what, void *data)
{
 struct window_pane *wp = data;
 log_debug("%%%u pipe error", wp->id);
 bufferevent_free(wp->pipe_event);
 close(wp->pipe_fd);
 wp->pipe_fd = -1;
 if (window_pane_destroy_ready(wp))
  server_destroy_pane(wp, 1);
}
