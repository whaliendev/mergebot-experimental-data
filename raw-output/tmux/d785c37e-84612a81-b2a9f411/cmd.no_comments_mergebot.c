#include <sys/types.h>
#include <sys/time.h>
#include <fnmatch.h>
#include <paths.h>
#include <pwd.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "tmux.h"
const struct cmd_entry *cmd_table[] = {
 &cmd_attach_session_entry,
 &cmd_bind_key_entry,
 &cmd_break_pane_entry,
 &cmd_capture_pane_entry,
 &cmd_choose_buffer_entry,
 &cmd_choose_client_entry,
 &cmd_choose_list_entry,
 &cmd_choose_session_entry,
 &cmd_choose_tree_entry,
 &cmd_choose_window_entry,
 &cmd_clear_history_entry,
 &cmd_clock_mode_entry,
 &cmd_command_prompt_entry,
 &cmd_confirm_before_entry,
 &cmd_copy_mode_entry,
 &cmd_delete_buffer_entry,
 &cmd_detach_client_entry,
 &cmd_display_message_entry,
 &cmd_display_panes_entry,
 &cmd_find_window_entry,
 &cmd_has_session_entry,
 &cmd_if_shell_entry,
 &cmd_join_pane_entry,
 &cmd_kill_pane_entry,
 &cmd_kill_server_entry,
 &cmd_kill_session_entry,
 &cmd_kill_window_entry,
 &cmd_last_pane_entry,
 &cmd_last_window_entry,
 &cmd_link_window_entry,
 &cmd_list_buffers_entry,
 &cmd_list_clients_entry,
 &cmd_list_commands_entry,
 &cmd_list_keys_entry,
 &cmd_list_panes_entry,
 &cmd_list_sessions_entry,
 &cmd_list_windows_entry,
 &cmd_load_buffer_entry,
 &cmd_lock_client_entry,
 &cmd_lock_server_entry,
 &cmd_lock_session_entry,
 &cmd_move_pane_entry,
 &cmd_move_window_entry,
 &cmd_new_session_entry,
 &cmd_new_window_entry,
 &cmd_next_layout_entry,
 &cmd_next_window_entry,
 &cmd_paste_buffer_entry,
 &cmd_pipe_pane_entry,
 &cmd_previous_layout_entry,
 &cmd_previous_window_entry,
 &cmd_refresh_client_entry,
 &cmd_rename_session_entry,
 &cmd_rename_window_entry,
 &cmd_resize_pane_entry,
 &cmd_respawn_pane_entry,
 &cmd_respawn_window_entry,
 &cmd_rotate_window_entry,
 &cmd_run_shell_entry,
 &cmd_save_buffer_entry,
 &cmd_select_layout_entry,
 &cmd_select_pane_entry,
 &cmd_select_window_entry,
 &cmd_send_keys_entry,
 &cmd_send_prefix_entry,
 &cmd_server_info_entry,
 &cmd_set_buffer_entry,
 &cmd_set_environment_entry,
 &cmd_set_option_entry,
 &cmd_set_window_option_entry,
 &cmd_show_buffer_entry,
 &cmd_show_environment_entry,
 &cmd_show_messages_entry,
 &cmd_show_options_entry,
 &cmd_show_window_options_entry,
 &cmd_source_file_entry,
 &cmd_split_window_entry,
 &cmd_start_server_entry,
 &cmd_suspend_client_entry,
 &cmd_swap_pane_entry,
 &cmd_swap_window_entry,
 &cmd_switch_client_entry,
 &cmd_unbind_key_entry,
 &cmd_unlink_window_entry,
 NULL
};
struct session *cmd_choose_session_list(struct sessionslist *);
struct session *cmd_choose_session(int);
struct client *cmd_choose_client(struct clients *);
struct client *cmd_lookup_client(const char *);
struct session *cmd_lookup_session(const char *, int *);
struct winlink *cmd_lookup_window(struct session *, const char *, int *);
int cmd_lookup_index(struct session *, const char *, int *);
struct window_pane *cmd_lookup_paneid(const char *);
struct winlink *cmd_lookup_winlink_windowid(struct session *, const char *);
struct window *cmd_lookup_windowid(const char *);
struct session *cmd_window_session(struct cmd_ctx *,
      struct window *, struct winlink **);
struct winlink *cmd_find_window_offset(const char *, struct session *, int *);
int cmd_find_index_offset(const char *, struct session *, int *);
struct window_pane *cmd_find_pane_offset(const char *, struct winlink *);
int
cmd_pack_argv(int argc, char **argv, char *buf, size_t len)
{
 size_t arglen;
 int i;
 *buf = '\0';
 for (i = 0; i < argc; i++) {
  if (strlcpy(buf, argv[i], len) >= len)
   return (-1);
  arglen = strlen(argv[i]) + 1;
  buf += arglen;
  len -= arglen;
 }
 return (0);
}
int
cmd_unpack_argv(char *buf, size_t len, int argc, char ***argv)
{
 int i;
 size_t arglen;
 if (argc == 0)
  return (0);
 *argv = xcalloc(argc, sizeof **argv);
 buf[len - 1] = '\0';
 for (i = 0; i < argc; i++) {
  if (len == 0) {
   cmd_free_argv(argc, *argv);
   return (-1);
  }
  arglen = strlen(buf) + 1;
  (*argv)[i] = xstrdup(buf);
  buf += arglen;
  len -= arglen;
 }
 return (0);
}
char **
cmd_copy_argv(int argc, char *const *argv)
{
 char **new_argv;
 int i;
 if (argc == 0)
  return (NULL);
 new_argv = xcalloc(argc, sizeof *new_argv);
 for (i = 0; i < argc; i++) {
  if (argv[i] != NULL)
   new_argv[i] = xstrdup(argv[i]);
 }
 return (new_argv);
}
void
cmd_free_argv(int argc, char **argv)
{
 int i;
 if (argc == 0)
  return;
 for (i = 0; i < argc; i++)
  free(argv[i]);
 free(argv);
}
struct cmd *
cmd_parse(int argc, char **argv, char **cause)
{
 const struct cmd_entry **entryp, *entry;
 struct cmd *cmd;
 struct args *args;
 char s[BUFSIZ];
 int ambiguous = 0;
 *cause = NULL;
 if (argc == 0) {
  xasprintf(cause, "no command");
  return (NULL);
 }
 entry = NULL;
 for (entryp = cmd_table; *entryp != NULL; entryp++) {
  if ((*entryp)->alias != NULL &&
      strcmp((*entryp)->alias, argv[0]) == 0) {
   ambiguous = 0;
   entry = *entryp;
   break;
  }
  if (strncmp((*entryp)->name, argv[0], strlen(argv[0])) != 0)
   continue;
  if (entry != NULL)
   ambiguous = 1;
  entry = *entryp;
  if (strcmp(entry->name, argv[0]) == 0)
   break;
 }
 if (ambiguous)
  goto ambiguous;
 if (entry == NULL) {
  xasprintf(cause, "unknown command: %s", argv[0]);
  return (NULL);
 }
 args = args_parse(entry->args_template, argc, argv);
 if (args == NULL)
  goto usage;
 if (entry->args_lower != -1 && args->argc < entry->args_lower)
  goto usage;
 if (entry->args_upper != -1 && args->argc > entry->args_upper)
  goto usage;
 if (entry->check != NULL && entry->check(args) != 0)
  goto usage;
 cmd = xmalloc(sizeof *cmd);
 cmd->entry = entry;
 cmd->args = args;
 return (cmd);
ambiguous:
 *s = '\0';
 for (entryp = cmd_table; *entryp != NULL; entryp++) {
  if (strncmp((*entryp)->name, argv[0], strlen(argv[0])) != 0)
   continue;
  if (strlcat(s, (*entryp)->name, sizeof s) >= sizeof s)
   break;
  if (strlcat(s, ", ", sizeof s) >= sizeof s)
   break;
 }
 s[strlen(s) - 2] = '\0';
 xasprintf(cause, "ambiguous command: %s, could be: %s", argv[0], s);
 return (NULL);
usage:
 if (args != NULL)
  args_free(args);
 xasprintf(cause, "usage: %s %s", entry->name, entry->usage);
 return (NULL);
}
enum cmd_retval
cmd_exec(struct cmd *cmd, struct cmd_ctx *ctx)
{
 return (cmd->entry->exec(cmd, ctx));
}
void
cmd_free(struct cmd *cmd)
{
 args_free(cmd->args);
 free(cmd);
}
size_t
cmd_print(struct cmd *cmd, char *buf, size_t len)
{
 size_t off, used;
 off = xsnprintf(buf, len, "%s ", cmd->entry->name);
 if (off < len) {
  used = args_print(cmd->args, buf + off, len - off);
  if (used == 0)
   off--;
  else
   off += used;
  buf[off] = '\0';
 }
 return (off);
}
struct session *
cmd_current_session(struct cmd_ctx *ctx, int prefer_unattached)
{
 struct msg_command_data *data = ctx->msgdata;
 struct client *c = ctx->cmdclient;
 struct session *s;
 struct sessionslist ss;
 struct winlink *wl;
 struct window_pane *wp;
 int found;
 if (ctx->curclient != NULL && ctx->curclient->session != NULL)
  return (ctx->curclient->session);
 if (c != NULL && c->tty.path != NULL) {
  ARRAY_INIT(&ss);
  RB_FOREACH(s, sessions, &sessions) {
   found = 0;
   RB_FOREACH(wl, winlinks, &s->windows) {
    TAILQ_FOREACH(wp, &wl->window->panes, entry) {
     if (strcmp(wp->tty, c->tty.path) == 0) {
      found = 1;
      break;
     }
    }
    if (found)
     break;
   }
   if (found)
    ARRAY_ADD(&ss, s);
  }
  s = cmd_choose_session_list(&ss);
  ARRAY_FREE(&ss);
  if (s != NULL)
   return (s);
 }
 if (data != NULL && data->pid == getpid() && data->idx != -1) {
  s = session_find_by_index(data->idx);
  if (s != NULL)
   return (s);
 }
 return (cmd_choose_session(prefer_unattached));
}
struct session *
cmd_choose_session(int prefer_unattached)
{
 struct session *s, *sbest;
 struct timeval *tv = NULL;
 sbest = NULL;
 RB_FOREACH(s, sessions, &sessions) {
  if (tv == NULL || timercmp(&s->activity_time, tv, >) ||
      (prefer_unattached &&
      !(sbest->flags & SESSION_UNATTACHED) &&
      (s->flags & SESSION_UNATTACHED))) {
   sbest = s;
   tv = &s->activity_time;
  }
 }
 return (sbest);
}
struct session *
cmd_choose_session_list(struct sessionslist *ss)
{
 struct session *s, *sbest;
 struct timeval *tv = NULL;
 u_int i;
 sbest = NULL;
 for (i = 0; i < ARRAY_LENGTH(ss); i++) {
  if ((s = ARRAY_ITEM(ss, i)) == NULL)
   continue;
  if (tv == NULL || timercmp(&s->activity_time, tv, >)) {
   sbest = s;
   tv = &s->activity_time;
  }
 }
 return (sbest);
}
struct client *
cmd_current_client(struct cmd_ctx *ctx)
{
 struct session *s;
 struct client *c;
 struct clients cc;
 u_int i;
 if (ctx->curclient != NULL)
  return (ctx->curclient);
 s = cmd_current_session(ctx, 0);
 if (s != NULL && !(s->flags & SESSION_UNATTACHED)) {
  ARRAY_INIT(&cc);
  for (i = 0; i < ARRAY_LENGTH(&clients); i++) {
   if ((c = ARRAY_ITEM(&clients, i)) == NULL)
    continue;
   if (s == c->session)
    ARRAY_ADD(&cc, c);
  }
  c = cmd_choose_client(&cc);
  ARRAY_FREE(&cc);
  if (c != NULL)
   return (c);
 }
 return (cmd_choose_client(&clients));
}
struct client *
cmd_choose_client(struct clients *cc)
{
 struct client *c, *cbest;
 struct timeval *tv = NULL;
 u_int i;
 cbest = NULL;
 for (i = 0; i < ARRAY_LENGTH(cc); i++) {
  if ((c = ARRAY_ITEM(cc, i)) == NULL)
   continue;
  if (c->session == NULL)
   continue;
  if (tv == NULL || timercmp(&c->activity_time, tv, >)) {
   cbest = c;
   tv = &c->activity_time;
  }
 }
 return (cbest);
}
struct client *
cmd_find_client(struct cmd_ctx *ctx, const char *arg)
{
 struct client *c;
 char *tmparg;
 size_t arglen;
 if (arg == NULL)
  return (cmd_current_client(ctx));
 tmparg = xstrdup(arg);
 arglen = strlen(tmparg);
 if (arglen != 0 && tmparg[arglen - 1] == ':')
  tmparg[arglen - 1] = '\0';
 c = cmd_lookup_client(tmparg);
 if (c == NULL)
  ctx->error(ctx, "client not found: %s", tmparg);
 free(tmparg);
 return (c);
}
struct client *
cmd_lookup_client(const char *name)
{
 struct client *c;
 const char *path;
 u_int i;
 for (i = 0; i < ARRAY_LENGTH(&clients); i++) {
  c = ARRAY_ITEM(&clients, i);
  if (c == NULL || c->session == NULL)
   continue;
  path = c->tty.path;
  if (strcmp(name, path) == 0)
   return (c);
  if (strncmp(path, _PATH_DEV, (sizeof _PATH_DEV) - 1) != 0)
   continue;
  if (strcmp(name, path + (sizeof _PATH_DEV) - 1) == 0)
   return (c);
 }
 return (NULL);
}
struct session *
cmd_lookup_session(const char *name, int *ambiguous)
{
 struct session *s, *sfound;
 *ambiguous = 0;
 if ((s = session_find(name)) != NULL)
  return (s);
 sfound = NULL;
 RB_FOREACH(s, sessions, &sessions) {
  if (strncmp(name, s->name, strlen(name)) == 0 ||
      fnmatch(name, s->name, 0) == 0) {
   if (sfound != NULL) {
    *ambiguous = 1;
    return (NULL);
   }
   sfound = s;
  }
 }
 return (sfound);
}
struct winlink *
cmd_lookup_window(struct session *s, const char *name, int *ambiguous)
{
 struct winlink *wl, *wlfound;
 const char *errstr;
 u_int idx;
 *ambiguous = 0;
 if ((wl = cmd_lookup_winlink_windowid(s, name)) != NULL)
     return (wl);
 idx = strtonum(name, 0, INT_MAX, &errstr);
 if (errstr == NULL) {
  if ((wl = winlink_find_by_index(&s->windows, idx)) != NULL)
   return (wl);
 }
 wlfound = NULL;
 RB_FOREACH(wl, winlinks, &s->windows) {
  if (strcmp(name, wl->window->name) == 0) {
   if (wlfound != NULL) {
    *ambiguous = 1;
    return (NULL);
   }
   wlfound = wl;
  }
 }
 if (wlfound != NULL)
  return (wlfound);
 wlfound = NULL;
 RB_FOREACH(wl, winlinks, &s->windows) {
  if (strncmp(name, wl->window->name, strlen(name)) == 0 ||
      fnmatch(name, wl->window->name, 0) == 0) {
   if (wlfound != NULL) {
    *ambiguous = 1;
    return (NULL);
   }
   wlfound = wl;
  }
 }
 if (wlfound != NULL)
  return (wlfound);
 return (NULL);
}
int
cmd_lookup_index(struct session *s, const char *name, int *ambiguous)
{
 struct winlink *wl;
 const char *errstr;
 u_int idx;
 if ((wl = cmd_lookup_window(s, name, ambiguous)) != NULL)
  return (wl->idx);
 if (*ambiguous)
  return (-1);
 idx = strtonum(name, 0, INT_MAX, &errstr);
 if (errstr == NULL)
  return (idx);
 return (-1);
}
struct window_pane *
cmd_lookup_paneid(const char *arg)
{
 const char *errstr;
 u_int paneid;
 if (*arg != '%')
  return (NULL);
 paneid = strtonum(arg + 1, 0, UINT_MAX, &errstr);
 if (errstr != NULL)
  return (NULL);
 return (window_pane_find_by_id(paneid));
}
struct winlink *
cmd_lookup_winlink_windowid(struct session *s, const char *arg)
{
 const char *errstr;
 u_int windowid;
 if (*arg != '@')
  return (NULL);
 windowid = strtonum(arg + 1, 0, UINT_MAX, &errstr);
 if (errstr != NULL)
  return (NULL);
 return (winlink_find_by_window_id(&s->windows, windowid));
}
struct window *
cmd_lookup_windowid(const char *arg)
{
 const char *errstr;
 u_int windowid;
 if (*arg != '@')
  return (NULL);
 windowid = strtonum(arg + 1, 0, UINT_MAX, &errstr);
 if (errstr != NULL)
  return (NULL);
 return (window_find_by_id(windowid));
}
struct session *
cmd_window_session(struct cmd_ctx *ctx, struct window *w, struct winlink **wlp)
{
 struct session *s;
 struct sessionslist ss;
 struct winlink *wl;
 s = cmd_current_session(ctx, 0);
 if (s != NULL) {
  wl = winlink_find_by_window(&s->windows, w);
  if (wl != NULL) {
   if (wlp != NULL)
    *wlp = wl;
   return (s);
  }
 }
 ARRAY_INIT(&ss);
 RB_FOREACH(s, sessions, &sessions) {
  if (winlink_find_by_window(&s->windows, w) != NULL)
   ARRAY_ADD(&ss, s);
 }
 s = cmd_choose_session_list(&ss);
 ARRAY_FREE(&ss);
 if (wlp != NULL)
  *wlp = winlink_find_by_window(&s->windows, w);
 return (s);
}
struct session *
cmd_find_session(struct cmd_ctx *ctx, const char *arg, int prefer_unattached)
{
 struct session *s;
 struct window_pane *wp;
 struct window *w;
 struct client *c;
 char *tmparg;
 size_t arglen;
 int ambiguous;
 if (arg == NULL)
  return (cmd_current_session(ctx, prefer_unattached));
 if ((wp = cmd_lookup_paneid(arg)) != NULL)
  return (cmd_window_session(ctx, wp->window, NULL));
 if ((w = cmd_lookup_windowid(arg)) != NULL)
  return (cmd_window_session(ctx, w, NULL));
 tmparg = xstrdup(arg);
 arglen = strlen(tmparg);
 if (arglen != 0 && tmparg[arglen - 1] == ':')
  tmparg[arglen - 1] = '\0';
 if (*tmparg == '\0') {
  free(tmparg);
  return (cmd_current_session(ctx, prefer_unattached));
 }
 s = cmd_lookup_session(tmparg, &ambiguous);
 if (s == NULL && (c = cmd_lookup_client(tmparg)) != NULL)
  s = c->session;
 if (s == NULL) {
  if (ambiguous)
   ctx->error(ctx, "more than one session: %s", tmparg);
  else
   ctx->error(ctx, "session not found: %s", tmparg);
 }
 free(tmparg);
 return (s);
}
struct winlink *
cmd_find_window(struct cmd_ctx *ctx, const char *arg, struct session **sp)
{
 struct session *s;
 struct winlink *wl;
 struct window_pane *wp;
 const char *winptr;
 char *sessptr = NULL;
 int ambiguous = 0;
 if ((s = cmd_current_session(ctx, 0)) == NULL) {
  ctx->error(ctx, "can't establish current session");
  return (NULL);
 }
 if (arg == NULL) {
  if (sp != NULL)
   *sp = s;
  return (s->curw);
 }
 if ((wp = cmd_lookup_paneid(arg)) != NULL) {
  s = cmd_window_session(ctx, wp->window, &wl);
  if (sp != NULL)
   *sp = s;
  return (wl);
 }
 if (*arg == '\0')
  goto not_found;
 winptr = strchr(arg, ':');
 if (winptr == NULL)
  goto no_colon;
 winptr++;
 sessptr = xstrdup(arg);
 *strchr(sessptr, ':') = '\0';
 if (*sessptr != '\0') {
  if ((s = cmd_lookup_session(sessptr, &ambiguous)) == NULL)
   goto no_session;
 }
 if (sp != NULL)
  *sp = s;
 if (*winptr == '\0')
  wl = s->curw;
 else if (winptr[0] == '!' && winptr[1] == '\0')
  wl = TAILQ_FIRST(&s->lastw);
 else if (winptr[0] == '+' || winptr[0] == '-')
  wl = cmd_find_window_offset(winptr, s, &ambiguous);
 else
  wl = cmd_lookup_window(s, winptr, &ambiguous);
 if (wl == NULL)
  goto not_found;
 if (sessptr != NULL)
  free(sessptr);
 return (wl);
no_colon:
 if (arg[0] == '!' && arg[1] == '\0') {
  if ((wl = TAILQ_FIRST(&s->lastw)) == NULL)
   goto not_found;
 } else if (arg[0] == '+' || arg[0] == '-') {
  if ((wl = cmd_find_window_offset(arg, s, &ambiguous)) == NULL)
   goto lookup_session;
 } else if ((wl = cmd_lookup_window(s, arg, &ambiguous)) == NULL)
  goto lookup_session;
 if (sp != NULL)
  *sp = s;
 return (wl);
lookup_session:
 if (ambiguous)
  goto not_found;
 if (*arg != '\0' && (s = cmd_lookup_session(arg, &ambiguous)) == NULL)
  goto no_session;
 if (sp != NULL)
  *sp = s;
 return (s->curw);
no_session:
 if (ambiguous)
  ctx->error(ctx, "multiple sessions: %s", arg);
 else
  ctx->error(ctx, "session not found: %s", arg);
 free(sessptr);
 return (NULL);
not_found:
 if (ambiguous)
  ctx->error(ctx, "multiple windows: %s", arg);
 else
  ctx->error(ctx, "window not found: %s", arg);
 free(sessptr);
 return (NULL);
}
struct winlink *
cmd_find_window_offset(const char *winptr, struct session *s, int *ambiguous)
{
 struct winlink *wl;
 int offset = 1;
 if (winptr[1] != '\0')
  offset = strtonum(winptr + 1, 1, INT_MAX, NULL);
 if (offset == 0)
  wl = cmd_lookup_window(s, winptr, ambiguous);
 else {
  if (winptr[0] == '+')
   wl = winlink_next_by_number(s->curw, s, offset);
  else
   wl = winlink_previous_by_number(s->curw, s, offset);
 }
 return (wl);
}
int
cmd_find_index(struct cmd_ctx *ctx, const char *arg, struct session **sp)
{
 struct session *s;
 struct winlink *wl;
 const char *winptr;
 char *sessptr = NULL;
 int idx, ambiguous = 0;
 if ((s = cmd_current_session(ctx, 0)) == NULL) {
  ctx->error(ctx, "can't establish current session");
  return (-2);
 }
 if (arg == NULL) {
  if (sp != NULL)
   *sp = s;
  return (-1);
 }
 if (*arg == '\0')
  goto not_found;
 winptr = strchr(arg, ':');
 if (winptr == NULL)
  goto no_colon;
 winptr++;
 sessptr = xstrdup(arg);
 *strchr(sessptr, ':') = '\0';
 if (sessptr != NULL && *sessptr != '\0') {
  if ((s = cmd_lookup_session(sessptr, &ambiguous)) == NULL)
   goto no_session;
 }
 if (sp != NULL)
  *sp = s;
 if (*winptr == '\0')
  idx = -1;
 else if (winptr[0] == '!' && winptr[1] == '\0') {
  if ((wl = TAILQ_FIRST(&s->lastw)) == NULL)
   goto not_found;
  idx = wl->idx;
 } else if (winptr[0] == '+' || winptr[0] == '-') {
  if ((idx = cmd_find_index_offset(winptr, s, &ambiguous)) < 0)
   goto invalid_index;
 } else if ((idx = cmd_lookup_index(s, winptr, &ambiguous)) == -1)
  goto invalid_index;
 free(sessptr);
 return (idx);
no_colon:
 if (arg[0] == '!' && arg[1] == '\0') {
  if ((wl = TAILQ_FIRST(&s->lastw)) == NULL)
   goto not_found;
  idx = wl->idx;
 } else if (arg[0] == '+' || arg[0] == '-') {
  if ((idx = cmd_find_index_offset(arg, s, &ambiguous)) < 0)
   goto lookup_session;
 } else if ((idx = cmd_lookup_index(s, arg, &ambiguous)) == -1)
  goto lookup_session;
 if (sp != NULL)
  *sp = s;
 return (idx);
lookup_session:
 if (ambiguous)
  goto not_found;
 if (*arg != '\0' && (s = cmd_lookup_session(arg, &ambiguous)) == NULL)
  goto no_session;
 if (sp != NULL)
  *sp = s;
 return (-1);
no_session:
 if (ambiguous)
  ctx->error(ctx, "multiple sessions: %s", arg);
 else
  ctx->error(ctx, "session not found: %s", arg);
 free(sessptr);
 return (-2);
invalid_index:
 if (ambiguous)
  goto not_found;
 ctx->error(ctx, "invalid index: %s", arg);
 free(sessptr);
 return (-2);
not_found:
 if (ambiguous)
  ctx->error(ctx, "multiple windows: %s", arg);
 else
  ctx->error(ctx, "window not found: %s", arg);
 free(sessptr);
 return (-2);
}
int
cmd_find_index_offset(const char *winptr, struct session *s, int *ambiguous)
{
 int idx, offset = 1;
 if (winptr[1] != '\0')
  offset = strtonum(winptr + 1, 1, INT_MAX, NULL);
 if (offset == 0)
  idx = cmd_lookup_index(s, winptr, ambiguous);
 else {
  if (winptr[0] == '+') {
   if (s->curw->idx == INT_MAX)
    idx = cmd_lookup_index(s, winptr, ambiguous);
   else
    idx = s->curw->idx + offset;
  } else {
   if (s->curw->idx == 0)
    idx = cmd_lookup_index(s, winptr, ambiguous);
   else
    idx = s->curw->idx - offset;
  }
 }
 return (idx);
}
struct winlink *
cmd_find_pane(struct cmd_ctx *ctx,
    const char *arg, struct session **sp, struct window_pane **wpp)
{
 struct session *s;
 struct winlink *wl;
 const char *period, *errstr;
 char *winptr, *paneptr;
 u_int idx;
 if ((s = cmd_current_session(ctx, 0)) == NULL) {
  ctx->error(ctx, "can't establish current session");
  return (NULL);
 }
 if (sp != NULL)
  *sp = s;
 if (arg == NULL) {
  *wpp = s->curw->window->active;
  return (s->curw);
 }
 if ((*wpp = cmd_lookup_paneid(arg)) != NULL) {
  s = cmd_window_session(ctx, (*wpp)->window, &wl);
  if (sp != NULL)
   *sp = s;
  return (wl);
 }
 if ((period = strrchr(arg, '.')) == NULL)
  goto no_period;
 winptr = xstrdup(arg);
 winptr[period - arg] = '\0';
 if (*winptr == '\0')
  wl = s->curw;
 else if ((wl = cmd_find_window(ctx, winptr, sp)) == NULL)
  goto error;
 paneptr = winptr + (period - arg) + 1;
 if (*paneptr == '\0')
  *wpp = wl->window->active;
 else if (paneptr[0] == '+' || paneptr[0] == '-')
  *wpp = cmd_find_pane_offset(paneptr, wl);
 else {
  idx = strtonum(paneptr, 0, INT_MAX, &errstr);
  if (errstr != NULL)
   goto lookup_string;
  *wpp = window_pane_at_index(wl->window, idx);
  if (*wpp == NULL)
   goto lookup_string;
 }
 free(winptr);
 return (wl);
lookup_string:
 if ((*wpp = window_find_string(wl->window, paneptr)) == NULL) {
  ctx->error(ctx, "can't find pane: %s", paneptr);
  goto error;
 }
 free(winptr);
 return (wl);
no_period:
 idx = strtonum(arg, 0, INT_MAX, &errstr);
 if (errstr != NULL)
  goto lookup_window;
 if ((*wpp = window_pane_at_index(s->curw->window, idx)) == NULL)
  goto lookup_window;
 return (s->curw);
lookup_window:
 if ((*wpp = window_find_string(s->curw->window, arg)) != NULL)
  return (s->curw);
 if ((wl = cmd_find_window(ctx, arg, sp)) != NULL)
  *wpp = wl->window->active;
 return (wl);
error:
 free(winptr);
 return (NULL);
}
struct window_pane *
cmd_find_pane_offset(const char *paneptr, struct winlink *wl)
{
 struct window *w = wl->window;
 struct window_pane *wp = w->active;
 u_int offset = 1;
 if (paneptr[1] != '\0')
  offset = strtonum(paneptr + 1, 1, INT_MAX, NULL);
 if (offset > 0) {
  if (paneptr[0] == '+')
   wp = window_pane_next_by_number(w, wp, offset);
  else
   wp = window_pane_previous_by_number(w, wp, offset);
 }
 return (wp);
}
char *
cmd_template_replace(char *template, const char *s, int idx)
{
 char ch;
 char *buf, *ptr;
 int replaced;
 size_t len;
 if (strstr(template, "%") == NULL)
  return (xstrdup(template));
 buf = xmalloc(1);
 *buf = '\0';
 len = 0;
 replaced = 0;
 ptr = template;
 while (*ptr != '\0') {
  switch (ch = *ptr++) {
  case '%':
   if (*ptr < '1' || *ptr > '9' || *ptr - '0' != idx) {
    if (*ptr != '%' || replaced)
     break;
    replaced = 1;
   }
   ptr++;
   len += strlen(s);
   buf = xrealloc(buf, 1, len + 1);
   strlcat(buf, s, len + 1);
   continue;
  }
  buf = xrealloc(buf, 1, len + 2);
  buf[len++] = ch;
  buf[len] = '\0';
 }
 return (buf);
}
const char *
cmd_get_default_path(struct cmd_ctx *ctx, const char *cwd)
{
 struct session *s;
 struct environ_entry *envent;
 const char *root;
 char tmp[MAXPATHLEN];
 struct passwd *pw;
 int n;
 size_t skip;
 static char path[MAXPATHLEN];
 if ((s = cmd_current_session(ctx, 0)) == NULL)
  return (NULL);
 if (cwd == NULL)
  cwd = options_get_string(&s->options, "default-path");
 skip = 1;
 if (strcmp(cwd, "$HOME") == 0 || strncmp(cwd, "$HOME/", 6) == 0) {
  skip = 5;
  goto find_home;
 } else if (cwd[0] == '~' && (cwd[1] == '\0' || cwd[1] == '/')) {
  goto find_home;
 } else if (cwd[0] == '-' && (cwd[1] == '\0' || cwd[1] == '/')) {
  root = s->cwd;
  goto complete_path;
 } else if (cwd[0] == '.' && (cwd[1] == '\0' || cwd[1] == '/')) {
  if (getcwd(tmp, sizeof tmp) != NULL) {
   root = tmp;
   goto complete_path;
  }
  return (s->cwd);
 } else if (*cwd == '/') {
  return (cwd);
 } else {
  if (ctx->cmdclient != NULL && ctx->cmdclient->cwd != NULL)
   root = ctx->cmdclient->cwd;
  else if (ctx->curclient != NULL && s->curw != NULL)
<<<<<<< HEAD
   root = osdep_get_cwd(s->curw->window->active->pid);
||||||| b2a9f411
   root = get_proc_cwd(s->curw->window->active->pid);
=======
   root = get_proc_cwd(s->curw->window->active->fd);
>>>>>>> 84612a81
  else
   return (s->cwd);
  skip = 0;
  if (root != NULL)
   goto complete_path;
 }
 return (s->cwd);
find_home:
 envent = environ_find(&global_environ, "HOME");
 if (envent != NULL && *envent->value != '\0')
  root = envent->value;
 else if ((pw = getpwuid(getuid())) != NULL)
  root = pw->pw_dir;
 else
  return (s->cwd);
complete_path:
 if (root[skip] == '\0') {
  strlcpy(path, root, sizeof path);
  return (path);
 }
 n = snprintf(path, sizeof path, "%s/%s", root, cwd + skip);
 if (n > 0 && (size_t)n < sizeof path)
  return (path);
 return (s->cwd);
}
