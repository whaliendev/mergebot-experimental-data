#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/uio.h>
#include <errno.h>
#include <event.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "tmux.h"
static void server_client_free(int, short, void *);
static void server_client_check_focus(struct window_pane *);
static void server_client_check_resize(struct window_pane *);
static key_code server_client_check_mouse(struct client *);
static void server_client_repeat_timer(int, short, void *);
static void server_client_click_timer(int, short, void *);
static void server_client_check_exit(struct client *);
static void server_client_check_redraw(struct client *);
static void server_client_set_title(struct client *);
static void server_client_reset_state(struct client *);
static int server_client_assume_paste(struct session *);
static void server_client_dispatch(struct imsg *, void *);
static void server_client_dispatch_command(struct client *, struct imsg *);
static void server_client_dispatch_identify(struct client *, struct imsg *);
static void server_client_dispatch_shell(struct client *);
u_int
server_client_how_many(void)
{
 struct client *c;
 u_int n;
 n = 0;
 TAILQ_FOREACH(c, &clients, entry) {
  if (c->session != NULL && (~c->flags & CLIENT_DETACHING))
   n++;
 }
 return (n);
}
static void
server_client_callback_identify(__unused int fd, __unused short events,
    void *data)
{
 server_client_clear_identify(data, NULL);
}
void
server_client_set_identify(struct client *c)
{
 struct timeval tv;
 int delay;
 delay = options_get_number(c->session->options, "display-panes-time");
 tv.tv_sec = delay / 1000;
 tv.tv_usec = (delay % 1000) * 1000L;
 if (event_initialized(&c->identify_timer))
  evtimer_del(&c->identify_timer);
 evtimer_set(&c->identify_timer, server_client_callback_identify, c);
 evtimer_add(&c->identify_timer, &tv);
 c->flags |= CLIENT_IDENTIFY;
 c->tty.flags |= (TTY_FREEZE|TTY_NOCURSOR);
 server_redraw_client(c);
}
void
server_client_clear_identify(struct client *c, struct window_pane *wp)
{
 if (~c->flags & CLIENT_IDENTIFY)
  return;
 c->flags &= ~CLIENT_IDENTIFY;
 if (c->identify_callback != NULL)
  c->identify_callback(c, wp);
 c->tty.flags &= ~(TTY_FREEZE|TTY_NOCURSOR);
 server_redraw_client(c);
}
int
server_client_check_nested(struct client *c)
{
 struct environ_entry *envent;
 struct window_pane *wp;
 envent = environ_find(c->environ, "TMUX");
 if (envent == NULL || *envent->value == '\0')
  return (0);
 RB_FOREACH(wp, window_pane_tree, &all_window_panes) {
  if (strcmp(wp->tty, c->ttyname) == 0)
   return (1);
 }
 return (0);
}
void
server_client_set_key_table(struct client *c, const char *name)
{
 if (name == NULL)
  name = server_client_get_key_table(c);
 key_bindings_unref_table(c->keytable);
 c->keytable = key_bindings_get_table(name, 1);
 c->keytable->references++;
}
const char *
server_client_get_key_table(struct client *c)
{
 struct session *s = c->session;
 const char *name;
 if (s == NULL)
  return ("root");
 name = options_get_string(s->options, "key-table");
 if (*name == '\0')
  return ("root");
 return (name);
}
static int
server_client_is_default_key_table(struct client *c, struct key_table *table)
{
 return (strcmp(table->name, server_client_get_key_table(c)) == 0);
}
void
server_client_create(int fd)
{
 struct client *c;
 setblocking(fd, 0);
 c = xcalloc(1, sizeof *c);
 c->references = 1;
 c->peer = proc_add_peer(server_proc, fd, server_client_dispatch, c);
 if (gettimeofday(&c->creation_time, NULL) != 0)
  fatal("gettimeofday failed");
 memcpy(&c->activity_time, &c->creation_time, sizeof c->activity_time);
 c->environ = environ_create();
 c->fd = -1;
 c->cwd = NULL;
 TAILQ_INIT(&c->queue);
 c->stdin_data = evbuffer_new();
 c->stdout_data = evbuffer_new();
 c->stderr_data = evbuffer_new();
 c->tty.fd = -1;
 c->title = NULL;
 c->session = NULL;
 c->last_session = NULL;
 c->tty.sx = 80;
 c->tty.sy = 24;
 screen_init(&c->status, c->tty.sx, 1, 0);
 c->message_string = NULL;
 TAILQ_INIT(&c->message_log);
 c->prompt_string = NULL;
 c->prompt_buffer = NULL;
 c->prompt_index = 0;
 c->flags |= CLIENT_FOCUSED;
 c->keytable = key_bindings_get_table("root", 1);
 c->keytable->references++;
 evtimer_set(&c->repeat_timer, server_client_repeat_timer, c);
 evtimer_set(&c->click_timer, server_client_click_timer, c);
 TAILQ_INSERT_TAIL(&clients, c, entry);
 log_debug("new client %p", c);
}
int
server_client_open(struct client *c, char **cause)
{
 if (c->flags & CLIENT_CONTROL)
  return (0);
 if (strcmp(c->ttyname, "/dev/tty") == 0) {
  *cause = xstrdup("can't use /dev/tty");
  return (-1);
 }
 if (!(c->flags & CLIENT_TERMINAL)) {
  *cause = xstrdup("not a terminal");
  return (-1);
 }
 if (tty_open(&c->tty, cause) != 0)
  return (-1);
 return (0);
}
void
server_client_lost(struct client *c)
{
 struct message_entry *msg, *msg1;
 c->flags |= CLIENT_DEAD;
 server_client_clear_identify(c, NULL);
 status_prompt_clear(c);
 status_message_clear(c);
 if (c->stdin_callback != NULL)
  c->stdin_callback(c, 1, c->stdin_callback_data);
 TAILQ_REMOVE(&clients, c, entry);
 log_debug("lost client %p", c);
 if (c->flags & CLIENT_TERMINAL)
  tty_free(&c->tty);
 free(c->ttyname);
 free(c->term);
 evbuffer_free(c->stdin_data);
 evbuffer_free(c->stdout_data);
 if (c->stderr_data != c->stdout_data)
  evbuffer_free(c->stderr_data);
 if (event_initialized(&c->status_timer))
  evtimer_del(&c->status_timer);
 screen_free(&c->status);
 if (c->old_status != NULL) {
  screen_free(c->old_status);
  free(c->old_status);
 }
 free(c->title);
 free((void *)c->cwd);
 evtimer_del(&c->repeat_timer);
 evtimer_del(&c->click_timer);
 key_bindings_unref_table(c->keytable);
 if (event_initialized(&c->identify_timer))
  evtimer_del(&c->identify_timer);
 free(c->message_string);
 if (event_initialized(&c->message_timer))
  evtimer_del(&c->message_timer);
 TAILQ_FOREACH_SAFE(msg, &c->message_log, entry, msg1) {
  free(msg->msg);
  TAILQ_REMOVE(&c->message_log, msg, entry);
  free(msg);
 }
 free(c->prompt_string);
 free(c->prompt_buffer);
 format_lost_client(c);
 environ_free(c->environ);
 proc_remove_peer(c->peer);
 c->peer = NULL;
 server_client_unref(c);
 server_add_accept(0);
 recalculate_sizes();
 server_check_unattached();
 server_update_socket();
}
void
server_client_unref(struct client *c)
{
 log_debug("unref client %p (%d references)", c, c->references);
 c->references--;
 if (c->references == 0)
  event_once(-1, EV_TIMEOUT, server_client_free, c, NULL);
}
static void
server_client_free(__unused int fd, __unused short events, void *arg)
{
 struct client *c = arg;
 log_debug("free client %p (%d references)", c, c->references);
 if (!TAILQ_EMPTY(&c->queue))
  fatalx("queue not empty");
 if (c->references == 0) {
  free((void *)c->name);
  free(c);
 }
}
void
server_client_suspend(struct client *c)
{
 struct session *s = c->session;
 if (s == NULL || (c->flags & CLIENT_DETACHING))
  return;
 tty_stop_tty(&c->tty);
 c->flags |= CLIENT_SUSPENDED;
 proc_send(c->peer, MSG_SUSPEND, -1, NULL, 0);
}
void
server_client_detach(struct client *c, enum msgtype msgtype)
{
 struct session *s = c->session;
 if (s == NULL || (c->flags & CLIENT_DETACHING))
  return;
 c->flags |= CLIENT_DETACHING;
 notify_client("client-detached", c);
 proc_send_s(c->peer, msgtype, s->name);
}
void
server_client_exec(struct client *c, const char *cmd)
{
 struct session *s = c->session;
 char *msg;
 const char *shell;
 size_t cmdsize, shellsize;
 if (*cmd == '\0')
  return;
 cmdsize = strlen(cmd) + 1;
 if (s != NULL)
  shell = options_get_string(s->options, "default-shell");
 else
  shell = options_get_string(global_s_options, "default-shell");
 shellsize = strlen(shell) + 1;
 msg = xmalloc(cmdsize + shellsize);
 memcpy(msg, cmd, cmdsize);
 memcpy(msg + cmdsize, shell, shellsize);
 proc_send(c->peer, MSG_EXEC, -1, msg, cmdsize + shellsize);
 free(msg);
}
static key_code
server_client_check_mouse(struct client *c)
{
 struct session *s = c->session;
 struct mouse_event *m = &c->tty.mouse;
 struct window *w;
 struct window_pane *wp;
 u_int x, y, b;
 int flag;
 key_code key;
 struct timeval tv;
 enum { NOTYPE, MOVE, DOWN, UP, DRAG, WHEEL, DOUBLE, TRIPLE } type;
 enum { NOWHERE, PANE, STATUS, BORDER } where;
 type = NOTYPE;
 where = NOWHERE;
 log_debug("mouse %02x at %u,%u (last %u,%u) (%d)", m->b, m->x, m->y,
     m->lx, m->ly, c->tty.mouse_drag_flag);
 if ((m->sgr_type != ' ' &&
     MOUSE_DRAG(m->sgr_b) &&
     MOUSE_BUTTONS(m->sgr_b) == 3) ||
     (m->sgr_type == ' ' &&
     MOUSE_DRAG(m->b) &&
     MOUSE_BUTTONS(m->b) == 3 &&
     MOUSE_BUTTONS(m->lb) == 3)) {
  type = MOVE;
  x = m->x, y = m->y, b = 0;
  log_debug("move at %u,%u", x, y);
 } else if (MOUSE_DRAG(m->b)) {
  type = DRAG;
  if (c->tty.mouse_drag_flag) {
   x = m->x, y = m->y, b = m->b;
   log_debug("drag update at %u,%u", x, y);
  } else {
   x = m->lx, y = m->ly, b = m->lb;
   log_debug("drag start at %u,%u", x, y);
  }
 } else if (MOUSE_WHEEL(m->b)) {
  type = WHEEL;
  x = m->x, y = m->y, b = m->b;
  log_debug("wheel at %u,%u", x, y);
 } else if (MOUSE_RELEASE(m->b)) {
  type = UP;
  x = m->x, y = m->y, b = m->lb;
  log_debug("up at %u,%u", x, y);
 } else {
  if (c->flags & CLIENT_DOUBLECLICK) {
   evtimer_del(&c->click_timer);
   c->flags &= ~CLIENT_DOUBLECLICK;
   if (m->b == c->click_button) {
    type = DOUBLE;
    x = m->x, y = m->y, b = m->b;
    log_debug("double-click at %u,%u", x, y);
    flag = CLIENT_TRIPLECLICK;
    goto add_timer;
   }
  } else if (c->flags & CLIENT_TRIPLECLICK) {
   evtimer_del(&c->click_timer);
   c->flags &= ~CLIENT_TRIPLECLICK;
   if (m->b == c->click_button) {
    type = TRIPLE;
    x = m->x, y = m->y, b = m->b;
    log_debug("triple-click at %u,%u", x, y);
    goto have_event;
   }
  }
  type = DOWN;
  x = m->x, y = m->y, b = m->b;
  log_debug("down at %u,%u", x, y);
  flag = CLIENT_DOUBLECLICK;
 add_timer:
  if (KEYC_CLICK_TIMEOUT != 0) {
   c->flags |= flag;
   c->click_button = m->b;
   tv.tv_sec = KEYC_CLICK_TIMEOUT / 1000;
   tv.tv_usec = (KEYC_CLICK_TIMEOUT % 1000) * 1000L;
   evtimer_del(&c->click_timer);
   evtimer_add(&c->click_timer, &tv);
  }
 }
have_event:
 if (type == NOTYPE)
  return (KEYC_UNKNOWN);
 m->s = s->id;
 m->statusat = status_at_line(c);
 if (m->statusat != -1 && y == (u_int)m->statusat) {
  w = status_get_window_at(c, x);
  if (w == NULL)
   return (KEYC_UNKNOWN);
  m->w = w->id;
  where = STATUS;
 } else
  m->w = -1;
 if (where == NOWHERE) {
  if (m->statusat == 0 && y > 0)
   y--;
  else if (m->statusat > 0 && y >= (u_int)m->statusat)
   y = m->statusat - 1;
  TAILQ_FOREACH(wp, &s->curw->window->panes, entry) {
   if ((wp->xoff + wp->sx == x &&
       wp->yoff <= 1 + y &&
       wp->yoff + wp->sy >= y) ||
       (wp->yoff + wp->sy == y &&
       wp->xoff <= 1 + x &&
       wp->xoff + wp->sx >= x))
    break;
  }
  if (wp != NULL)
   where = BORDER;
  else {
   wp = window_get_active_at(s->curw->window, x, y);
   if (wp != NULL) {
    where = PANE;
    log_debug("mouse at %u,%u is on pane %%%u",
        x, y, wp->id);
   }
  }
  if (where == NOWHERE)
   return (KEYC_UNKNOWN);
  m->wp = wp->id;
  m->w = wp->window->id;
 } else
  m->wp = -1;
 if (type != DRAG && type != WHEEL && c->tty.mouse_drag_flag) {
  if (c->tty.mouse_drag_release != NULL)
   c->tty.mouse_drag_release(c, m);
  c->tty.mouse_drag_update = NULL;
  c->tty.mouse_drag_release = NULL;
  switch (c->tty.mouse_drag_flag) {
  case 1:
   if (where == PANE)
    key = KEYC_MOUSEDRAGEND1_PANE;
   if (where == STATUS)
    key = KEYC_MOUSEDRAGEND1_STATUS;
   if (where == BORDER)
    key = KEYC_MOUSEDRAGEND1_BORDER;
   break;
  case 2:
   if (where == PANE)
    key = KEYC_MOUSEDRAGEND2_PANE;
   if (where == STATUS)
    key = KEYC_MOUSEDRAGEND2_STATUS;
   if (where == BORDER)
    key = KEYC_MOUSEDRAGEND2_BORDER;
   break;
  case 3:
   if (where == PANE)
    key = KEYC_MOUSEDRAGEND3_PANE;
   if (where == STATUS)
    key = KEYC_MOUSEDRAGEND3_STATUS;
   if (where == BORDER)
    key = KEYC_MOUSEDRAGEND3_BORDER;
   break;
  default:
   key = KEYC_MOUSE;
   break;
  }
  c->tty.mouse_drag_flag = 0;
  return (key);
 }
 key = KEYC_UNKNOWN;
 switch (type) {
 case NOTYPE:
  break;
 case MOVE:
  if (where == PANE)
   key = KEYC_MOUSEMOVE_PANE;
  if (where == STATUS)
   key = KEYC_MOUSEMOVE_STATUS;
  if (where == BORDER)
   key = KEYC_MOUSEMOVE_BORDER;
  break;
 case DRAG:
  if (c->tty.mouse_drag_update != NULL)
   key = KEYC_DRAGGING;
  else {
   switch (MOUSE_BUTTONS(b)) {
   case 0:
    if (where == PANE)
     key = KEYC_MOUSEDRAG1_PANE;
    if (where == STATUS)
     key = KEYC_MOUSEDRAG1_STATUS;
    if (where == BORDER)
     key = KEYC_MOUSEDRAG1_BORDER;
    break;
   case 1:
    if (where == PANE)
     key = KEYC_MOUSEDRAG2_PANE;
    if (where == STATUS)
     key = KEYC_MOUSEDRAG2_STATUS;
    if (where == BORDER)
     key = KEYC_MOUSEDRAG2_BORDER;
    break;
   case 2:
    if (where == PANE)
     key = KEYC_MOUSEDRAG3_PANE;
    if (where == STATUS)
     key = KEYC_MOUSEDRAG3_STATUS;
    if (where == BORDER)
     key = KEYC_MOUSEDRAG3_BORDER;
    break;
   }
  }
  c->tty.mouse_drag_flag = MOUSE_BUTTONS(b) + 1;
  break;
 case WHEEL:
  if (MOUSE_BUTTONS(b) == MOUSE_WHEEL_UP) {
   if (where == PANE)
    key = KEYC_WHEELUP_PANE;
   if (where == STATUS)
    key = KEYC_WHEELUP_STATUS;
   if (where == BORDER)
    key = KEYC_WHEELUP_BORDER;
  } else {
   if (where == PANE)
    key = KEYC_WHEELDOWN_PANE;
   if (where == STATUS)
    key = KEYC_WHEELDOWN_STATUS;
   if (where == BORDER)
    key = KEYC_WHEELDOWN_BORDER;
  }
  break;
 case UP:
  switch (MOUSE_BUTTONS(b)) {
  case 0:
   if (where == PANE)
    key = KEYC_MOUSEUP1_PANE;
   if (where == STATUS)
    key = KEYC_MOUSEUP1_STATUS;
   if (where == BORDER)
    key = KEYC_MOUSEUP1_BORDER;
   break;
  case 1:
   if (where == PANE)
    key = KEYC_MOUSEUP2_PANE;
   if (where == STATUS)
    key = KEYC_MOUSEUP2_STATUS;
   if (where == BORDER)
    key = KEYC_MOUSEUP2_BORDER;
   break;
  case 2:
   if (where == PANE)
    key = KEYC_MOUSEUP3_PANE;
   if (where == STATUS)
    key = KEYC_MOUSEUP3_STATUS;
   if (where == BORDER)
    key = KEYC_MOUSEUP3_BORDER;
   break;
  }
  break;
 case DOWN:
  switch (MOUSE_BUTTONS(b)) {
  case 0:
   if (where == PANE)
    key = KEYC_MOUSEDOWN1_PANE;
   if (where == STATUS)
    key = KEYC_MOUSEDOWN1_STATUS;
   if (where == BORDER)
    key = KEYC_MOUSEDOWN1_BORDER;
   break;
  case 1:
   if (where == PANE)
    key = KEYC_MOUSEDOWN2_PANE;
   if (where == STATUS)
    key = KEYC_MOUSEDOWN2_STATUS;
   if (where == BORDER)
    key = KEYC_MOUSEDOWN2_BORDER;
   break;
  case 2:
   if (where == PANE)
    key = KEYC_MOUSEDOWN3_PANE;
   if (where == STATUS)
    key = KEYC_MOUSEDOWN3_STATUS;
   if (where == BORDER)
    key = KEYC_MOUSEDOWN3_BORDER;
   break;
  }
  break;
 case DOUBLE:
  switch (MOUSE_BUTTONS(b)) {
  case 0:
   if (where == PANE)
    key = KEYC_DOUBLECLICK1_PANE;
   if (where == STATUS)
    key = KEYC_DOUBLECLICK1_STATUS;
   if (where == BORDER)
    key = KEYC_DOUBLECLICK1_BORDER;
   break;
  case 1:
   if (where == PANE)
    key = KEYC_DOUBLECLICK2_PANE;
   if (where == STATUS)
    key = KEYC_DOUBLECLICK2_STATUS;
   if (where == BORDER)
    key = KEYC_DOUBLECLICK2_BORDER;
   break;
  case 2:
   if (where == PANE)
    key = KEYC_DOUBLECLICK3_PANE;
   if (where == STATUS)
    key = KEYC_DOUBLECLICK3_STATUS;
   if (where == BORDER)
    key = KEYC_DOUBLECLICK3_BORDER;
   break;
  }
  break;
 case TRIPLE:
  switch (MOUSE_BUTTONS(b)) {
  case 0:
   if (where == PANE)
    key = KEYC_TRIPLECLICK1_PANE;
   if (where == STATUS)
    key = KEYC_TRIPLECLICK1_STATUS;
   if (where == BORDER)
    key = KEYC_TRIPLECLICK1_BORDER;
   break;
  case 1:
   if (where == PANE)
    key = KEYC_TRIPLECLICK2_PANE;
   if (where == STATUS)
    key = KEYC_TRIPLECLICK2_STATUS;
   if (where == BORDER)
    key = KEYC_TRIPLECLICK2_BORDER;
   break;
  case 2:
   if (where == PANE)
    key = KEYC_TRIPLECLICK3_PANE;
   if (where == STATUS)
    key = KEYC_TRIPLECLICK3_STATUS;
   if (where == BORDER)
    key = KEYC_TRIPLECLICK3_BORDER;
   break;
  }
  break;
 }
 if (key == KEYC_UNKNOWN)
  return (KEYC_UNKNOWN);
 if (b & MOUSE_MASK_META)
  key |= KEYC_ESCAPE;
 if (b & MOUSE_MASK_CTRL)
  key |= KEYC_CTRL;
 if (b & MOUSE_MASK_SHIFT)
  key |= KEYC_SHIFT;
 return (key);
}
static int
server_client_assume_paste(struct session *s)
{
 struct timeval tv;
 int t;
 if ((t = options_get_number(s->options, "assume-paste-time")) == 0)
  return (0);
 timersub(&s->activity_time, &s->last_activity_time, &tv);
 if (tv.tv_sec == 0 && tv.tv_usec < t * 1000) {
  log_debug("session %s pasting (flag %d)", s->name,
      !!(s->flags & SESSION_PASTING));
  if (s->flags & SESSION_PASTING)
   return (1);
  s->flags |= SESSION_PASTING;
  return (0);
 }
 log_debug("session %s not pasting", s->name);
 s->flags &= ~SESSION_PASTING;
 return (0);
}
void
server_client_handle_key(struct client *c, key_code key)
{
 struct mouse_event *m = &c->tty.mouse;
 struct session *s = c->session;
 struct window *w;
 struct window_pane *wp;
 struct timeval tv;
 struct key_table *table, *first;
 struct key_binding bd_find, *bd;
 int xtimeout;
 struct cmd_find_state fs;
 if (s == NULL || (c->flags & (CLIENT_DEAD|CLIENT_SUSPENDED)) != 0)
  return;
 w = s->curw->window;
 if (gettimeofday(&c->activity_time, NULL) != 0)
  fatal("gettimeofday failed");
 session_update_activity(s, &c->activity_time);
 if (c->flags & CLIENT_IDENTIFY && key >= '0' && key <= '9') {
  if (c->flags & CLIENT_READONLY)
   return;
  window_unzoom(w);
  wp = window_pane_at_index(w, key - '0');
  if (wp != NULL && !window_pane_visible(wp))
   wp = NULL;
  server_client_clear_identify(c, wp);
  return;
 }
 if (!(c->flags & CLIENT_READONLY)) {
  status_message_clear(c);
  server_client_clear_identify(c, NULL);
 }
 if (c->prompt_string != NULL) {
  if (c->flags & CLIENT_READONLY)
   return;
  if (status_prompt_key(c, key) == 0)
   return;
 }
 m->valid = 0;
 if (key == KEYC_MOUSE) {
  if (c->flags & CLIENT_READONLY)
   return;
  key = server_client_check_mouse(c);
  if (key == KEYC_UNKNOWN)
   return;
  m->valid = 1;
  m->key = key;
  if (key == KEYC_DRAGGING) {
   c->tty.mouse_drag_update(c, m);
   return;
  }
 } else
  m->valid = 0;
 if (!KEYC_IS_MOUSE(key) || cmd_find_from_mouse(&fs, m) != 0)
  cmd_find_from_session(&fs, s);
 wp = fs.wp;
 if (KEYC_IS_MOUSE(key) && !options_get_number(s->options, "mouse"))
  goto forward;
 if (!KEYC_IS_MOUSE(key) && server_client_assume_paste(s))
  goto forward;
 if (server_client_is_default_key_table(c, c->keytable) &&
     wp != NULL &&
     wp->mode != NULL &&
     wp->mode->key_table != NULL)
  table = key_bindings_get_table(wp->mode->key_table(wp), 1);
 else
  table = c->keytable;
 first = table;
 if ((key == (key_code)options_get_number(s->options, "prefix") ||
     key == (key_code)options_get_number(s->options, "prefix2")) &&
     strcmp(table->name, "prefix") != 0) {
  server_client_set_key_table(c, "prefix");
  server_status_client(c);
  return;
 }
retry:
 if (wp == NULL)
  log_debug("key table %s (no pane)", table->name);
 else
  log_debug("key table %s (pane %%%u)", table->name, wp->id);
 if (c->flags & CLIENT_REPEAT)
  log_debug("currently repeating");
 bd_find.key = (key & ~KEYC_XTERM);
 bd = RB_FIND(key_bindings, &table->key_bindings, &bd_find);
 if (bd != NULL) {
  if ((c->flags & CLIENT_REPEAT) &&
      (~bd->flags & KEY_BINDING_REPEAT)) {
   server_client_set_key_table(c, NULL);
   c->flags &= ~CLIENT_REPEAT;
   server_status_client(c);
   table = c->keytable;
   goto retry;
  }
  log_debug("found in key table %s", table->name);
  table->references++;
  xtimeout = options_get_number(s->options, "repeat-time");
  if (xtimeout != 0 && (bd->flags & KEY_BINDING_REPEAT)) {
   c->flags |= CLIENT_REPEAT;
   tv.tv_sec = xtimeout / 1000;
   tv.tv_usec = (xtimeout % 1000) * 1000L;
   evtimer_del(&c->repeat_timer);
   evtimer_add(&c->repeat_timer, &tv);
  } else {
   c->flags &= ~CLIENT_REPEAT;
   server_client_set_key_table(c, NULL);
  }
  server_status_client(c);
  key_bindings_dispatch(bd, NULL, c, m, &fs);
  key_bindings_unref_table(table);
  return;
 }
 log_debug("not found in key table %s", table->name);
 if (!server_client_is_default_key_table(c, table) ||
     (c->flags & CLIENT_REPEAT)) {
  server_client_set_key_table(c, NULL);
  c->flags &= ~CLIENT_REPEAT;
  server_status_client(c);
  table = c->keytable;
  goto retry;
 }
 if (first != table) {
  server_client_set_key_table(c, NULL);
  server_status_client(c);
  return;
 }
forward:
 if (c->flags & CLIENT_READONLY)
  return;
 if (wp != NULL)
  window_pane_key(wp, c, s, key, m);
}
void
server_client_loop(void)
{
 struct client *c;
 struct window *w;
 struct window_pane *wp;
 int focus;
 TAILQ_FOREACH(c, &clients, entry) {
  server_client_check_exit(c);
  if (c->session != NULL) {
   server_client_check_redraw(c);
   server_client_reset_state(c);
  }
 }
 focus = options_get_number(global_options, "focus-events");
 RB_FOREACH(w, windows, &windows) {
  TAILQ_FOREACH(wp, &w->panes, entry) {
   if (wp->fd != -1) {
    if (focus)
     server_client_check_focus(wp);
    server_client_check_resize(wp);
   }
   wp->flags &= ~PANE_REDRAW;
  }
  check_window_name(w);
 }
}
static int
server_client_resize_force(struct window_pane *wp)
{
 struct timeval tv = { .tv_usec = 100000 };
 struct winsize ws;
 if (wp->flags & PANE_RESIZEFORCE) {
  wp->flags &= ~PANE_RESIZEFORCE;
  return (0);
 }
 if (wp->sx != wp->osx ||
     wp->sy != wp->osy ||
     wp->sx <= 1 ||
     wp->sy <= 1)
  return (0);
 memset(&ws, 0, sizeof ws);
 ws.ws_col = wp->sx;
 ws.ws_row = wp->sy - 1;
 if (ioctl(wp->fd, TIOCSWINSZ, &ws) == -1)
  fatal("ioctl failed");
 log_debug("%s: %%%u forcing resize", __func__, wp->id);
 evtimer_add(&wp->resize_timer, &tv);
 wp->flags |= PANE_RESIZEFORCE;
 return (1);
}
static void
server_client_resize_event(__unused int fd, __unused short events, void *data)
{
 struct window_pane *wp = data;
 struct winsize ws;
 evtimer_del(&wp->resize_timer);
 if (!(wp->flags & PANE_RESIZE))
  return;
 if (server_client_resize_force(wp))
  return;
 memset(&ws, 0, sizeof ws);
 ws.ws_col = wp->sx;
 ws.ws_row = wp->sy;
<<<<<<< HEAD
 if (ioctl(wp->fd, TIOCSWINSZ, &ws) == -1) {
#ifdef __sun
  if (errno != EINVAL && errno != ENXIO)
#endif
||||||| bd39fcbe
 if (ioctl(wp->fd, TIOCSWINSZ, &ws) == -1)
=======
 if (ioctl(wp->fd, TIOCSWINSZ, &ws) == -1)
>>>>>>> d60663ea
  fatal("ioctl failed");
<<<<<<< HEAD
 }
||||||| bd39fcbe
=======
 log_debug("%s: %%%u resize to %u,%u", __func__, wp->id, wp->sx, wp->sy);
>>>>>>> d60663ea
 wp->flags &= ~PANE_RESIZE;
 wp->osx = wp->sx;
 wp->osy = wp->sy;
}
static void
server_client_check_resize(struct window_pane *wp)
{
 struct timeval tv = { .tv_usec = 250000 };
 if (!(wp->flags & PANE_RESIZE))
  return;
 log_debug("%s: %%%u resize to %u,%u", __func__, wp->id, wp->sx, wp->sy);
 if (!event_initialized(&wp->resize_timer))
  evtimer_set(&wp->resize_timer, server_client_resize_event, wp);
 if (!evtimer_pending(&wp->resize_timer, NULL))
  server_client_resize_event(-1, 0, wp);
 if (wp->saved_grid != NULL && evtimer_pending(&wp->resize_timer, NULL))
  return;
 evtimer_del(&wp->resize_timer);
 evtimer_add(&wp->resize_timer, &tv);
}
static void
server_client_check_focus(struct window_pane *wp)
{
 struct client *c;
 int push;
 push = wp->flags & PANE_FOCUSPUSH;
 wp->flags &= ~PANE_FOCUSPUSH;
 if (!(wp->base.mode & MODE_FOCUSON))
  return;
 if (wp->window->active != wp)
  goto not_focused;
 if (wp->screen != &wp->base)
  goto not_focused;
 TAILQ_FOREACH(c, &clients, entry) {
  if (c->session == NULL || !(c->flags & CLIENT_FOCUSED))
   continue;
  if (c->session->flags & SESSION_UNATTACHED)
   continue;
  if (c->session->curw->window == wp->window)
   goto focused;
 }
not_focused:
 if (push || (wp->flags & PANE_FOCUSED))
  bufferevent_write(wp->event, "\033[O", 3);
 wp->flags &= ~PANE_FOCUSED;
 return;
focused:
 if (push || !(wp->flags & PANE_FOCUSED))
  bufferevent_write(wp->event, "\033[I", 3);
 wp->flags |= PANE_FOCUSED;
}
static void
server_client_reset_state(struct client *c)
{
 struct window *w = c->session->curw->window;
 struct window_pane *wp = w->active, *loop;
 struct screen *s = wp->screen;
 struct options *oo = c->session->options;
 int status, mode, o;
 if (c->flags & (CLIENT_CONTROL|CLIENT_SUSPENDED))
  return;
 tty_region_off(&c->tty);
 tty_margin_off(&c->tty);
 status = options_get_number(oo, "status");
 if (!window_pane_visible(wp) || wp->yoff + s->cy >= c->tty.sy - status)
  tty_cursor(&c->tty, 0, 0);
 else {
  o = status && options_get_number(oo, "status-position") == 0;
  tty_cursor(&c->tty, wp->xoff + s->cx, o + wp->yoff + s->cy);
 }
 mode = s->mode;
 if (options_get_number(oo, "mouse")) {
  mode &= ~ALL_MOUSE_MODES;
  TAILQ_FOREACH(loop, &w->panes, entry) {
   if (loop->screen->mode & MODE_MOUSE_ALL)
    mode |= MODE_MOUSE_ALL;
  }
  if (~mode & MODE_MOUSE_ALL)
   mode |= MODE_MOUSE_BUTTON;
 }
 if (c->prompt_string != NULL)
  mode &= ~MODE_BRACKETPASTE;
 tty_update_mode(&c->tty, mode, s);
 tty_reset(&c->tty);
}
static void
server_client_repeat_timer(__unused int fd, __unused short events, void *data)
{
 struct client *c = data;
 if (c->flags & CLIENT_REPEAT) {
  server_client_set_key_table(c, NULL);
  c->flags &= ~CLIENT_REPEAT;
  server_status_client(c);
 }
}
static void
server_client_click_timer(__unused int fd, __unused short events, void *data)
{
 struct client *c = data;
 c->flags &= ~(CLIENT_DOUBLECLICK|CLIENT_TRIPLECLICK);
}
static void
server_client_check_exit(struct client *c)
{
 if (!(c->flags & CLIENT_EXIT))
  return;
 if (EVBUFFER_LENGTH(c->stdin_data) != 0)
  return;
 if (EVBUFFER_LENGTH(c->stdout_data) != 0)
  return;
 if (EVBUFFER_LENGTH(c->stderr_data) != 0)
  return;
 proc_send(c->peer, MSG_EXIT, -1, &c->retval, sizeof c->retval);
 c->flags &= ~CLIENT_EXIT;
}
static void
server_client_redraw_timer(__unused int fd, __unused short events,
    __unused void* data)
{
 log_debug("redraw timer fired");
}
static void
server_client_check_redraw(struct client *c)
{
 struct session *s = c->session;
 struct tty *tty = &c->tty;
 struct window_pane *wp;
 int needed, flags, masked;
 struct timeval tv = { .tv_usec = 1000 };
 static struct event ev;
 size_t left;
 if (c->flags & (CLIENT_CONTROL|CLIENT_SUSPENDED))
  return;
 needed = 0;
 if (c->flags & CLIENT_REDRAW)
  needed = 1;
 else {
  TAILQ_FOREACH(wp, &c->session->curw->window->panes, entry) {
   if (wp->flags & PANE_REDRAW) {
    needed = 1;
    break;
   }
  }
 }
 if (needed) {
  left = EVBUFFER_LENGTH(tty->out);
  if (left != 0) {
   log_debug("%s: redraw deferred (%zu left)", c->name, left);
   if (evtimer_initialized(&ev) && evtimer_pending(&ev, NULL))
    return;
   log_debug("redraw timer started");
   evtimer_set(&ev, server_client_redraw_timer, NULL);
   evtimer_add(&ev, &tv);
   c->flags |= CLIENT_REDRAW;
   return;
  }
  if (evtimer_initialized(&ev))
   evtimer_del(&ev);
  log_debug("%s: redraw needed", c->name);
 }
 if (c->flags & (CLIENT_REDRAW|CLIENT_STATUS)) {
  if (options_get_number(s->options, "set-titles"))
   server_client_set_title(c);
  screen_redraw_update(c);
 }
 flags = tty->flags & (TTY_BLOCK|TTY_FREEZE|TTY_NOCURSOR);
 tty->flags = (tty->flags & ~(TTY_BLOCK|TTY_FREEZE)) | TTY_NOCURSOR;
 if (c->flags & CLIENT_REDRAW) {
  tty_update_mode(tty, tty->mode, NULL);
  screen_redraw_screen(c, 1, 1, 1);
  c->flags &= ~(CLIENT_STATUS|CLIENT_BORDERS);
 } else {
  TAILQ_FOREACH(wp, &c->session->curw->window->panes, entry) {
   if (wp->flags & PANE_REDRAW) {
    tty_update_mode(tty, tty->mode, NULL);
    screen_redraw_pane(c, wp);
   }
  }
 }
 masked = c->flags & (CLIENT_BORDERS|CLIENT_STATUS);
 if (masked != 0)
  tty_update_mode(tty, tty->mode, NULL);
 if (masked == CLIENT_BORDERS)
  screen_redraw_screen(c, 0, 0, 1);
 else if (masked == CLIENT_STATUS)
  screen_redraw_screen(c, 0, 1, 0);
 else if (masked != 0)
  screen_redraw_screen(c, 0, 1, 1);
 tty->flags = (tty->flags & ~(TTY_FREEZE|TTY_NOCURSOR)) | flags;
 tty_update_mode(tty, tty->mode, NULL);
 c->flags &= ~(CLIENT_REDRAW|CLIENT_BORDERS|CLIENT_STATUS|
     CLIENT_STATUSFORCE);
 if (needed) {
  c->redraw = EVBUFFER_LENGTH(tty->out);
  log_debug("%s: redraw added %zu bytes", c->name, c->redraw);
 }
}
static void
server_client_set_title(struct client *c)
{
 struct session *s = c->session;
 const char *template;
 char *title;
 struct format_tree *ft;
 template = options_get_string(s->options, "set-titles-string");
 ft = format_create(c, NULL, FORMAT_NONE, 0);
 format_defaults(ft, c, NULL, NULL, NULL);
 title = format_expand_time(ft, template, time(NULL));
 if (c->title == NULL || strcmp(title, c->title) != 0) {
  free(c->title);
  c->title = xstrdup(title);
  tty_set_title(&c->tty, c->title);
 }
 free(title);
 format_free(ft);
}
static void
server_client_dispatch(struct imsg *imsg, void *arg)
{
 struct client *c = arg;
 struct msg_stdin_data stdindata;
 const char *data;
 ssize_t datalen;
 struct session *s;
 if (c->flags & CLIENT_DEAD)
  return;
 if (imsg == NULL) {
  server_client_lost(c);
  return;
 }
 data = imsg->data;
 datalen = imsg->hdr.len - IMSG_HEADER_SIZE;
 switch (imsg->hdr.type) {
 case MSG_IDENTIFY_FLAGS:
 case MSG_IDENTIFY_TERM:
 case MSG_IDENTIFY_TTYNAME:
 case MSG_IDENTIFY_CWD:
 case MSG_IDENTIFY_STDIN:
 case MSG_IDENTIFY_ENVIRON:
 case MSG_IDENTIFY_CLIENTPID:
 case MSG_IDENTIFY_DONE:
  server_client_dispatch_identify(c, imsg);
  break;
 case MSG_COMMAND:
  server_client_dispatch_command(c, imsg);
  break;
 case MSG_STDIN:
  if (datalen != sizeof stdindata)
   fatalx("bad MSG_STDIN size");
  memcpy(&stdindata, data, sizeof stdindata);
  if (c->stdin_callback == NULL)
   break;
  if (stdindata.size <= 0)
   c->stdin_closed = 1;
  else {
   evbuffer_add(c->stdin_data, stdindata.data,
       stdindata.size);
  }
  c->stdin_callback(c, c->stdin_closed,
      c->stdin_callback_data);
  break;
 case MSG_RESIZE:
  if (datalen != 0)
   fatalx("bad MSG_RESIZE size");
  if (c->flags & CLIENT_CONTROL)
   break;
  tty_resize(&c->tty);
  recalculate_sizes();
  server_redraw_client(c);
  if (c->session != NULL)
   notify_client("client-resized", c);
  break;
 case MSG_EXITING:
  if (datalen != 0)
   fatalx("bad MSG_EXITING size");
  c->session = NULL;
  tty_close(&c->tty);
  proc_send(c->peer, MSG_EXITED, -1, NULL, 0);
  break;
 case MSG_WAKEUP:
 case MSG_UNLOCK:
  if (datalen != 0)
   fatalx("bad MSG_WAKEUP size");
  if (!(c->flags & CLIENT_SUSPENDED))
   break;
  c->flags &= ~CLIENT_SUSPENDED;
  if (c->tty.fd == -1)
   break;
  s = c->session;
  if (gettimeofday(&c->activity_time, NULL) != 0)
   fatal("gettimeofday failed");
  tty_start_tty(&c->tty);
  server_redraw_client(c);
  recalculate_sizes();
  if (s != NULL)
   session_update_activity(s, &c->activity_time);
  break;
 case MSG_SHELL:
  if (datalen != 0)
   fatalx("bad MSG_SHELL size");
  server_client_dispatch_shell(c);
  break;
 }
}
static enum cmd_retval
server_client_command_done(struct cmdq_item *item, __unused void *data)
{
 struct client *c = item->client;
 if (~c->flags & CLIENT_ATTACHED)
  c->flags |= CLIENT_EXIT;
 return (CMD_RETURN_NORMAL);
}
static enum cmd_retval
server_client_command_error(struct cmdq_item *item, void *data)
{
 char *error = data;
 cmdq_error(item, "%s", error);
 free(error);
 return (CMD_RETURN_NORMAL);
}
static void
server_client_dispatch_command(struct client *c, struct imsg *imsg)
{
 struct msg_command_data data;
 char *buf;
 size_t len;
 struct cmd_list *cmdlist = NULL;
 int argc;
 char **argv, *cause;
 if (imsg->hdr.len - IMSG_HEADER_SIZE < sizeof data)
  fatalx("bad MSG_COMMAND size");
 memcpy(&data, imsg->data, sizeof data);
 buf = (char *)imsg->data + sizeof data;
 len = imsg->hdr.len - IMSG_HEADER_SIZE - sizeof data;
 if (len > 0 && buf[len - 1] != '\0')
  fatalx("bad MSG_COMMAND string");
 argc = data.argc;
 if (cmd_unpack_argv(buf, len, argc, &argv) != 0) {
  cause = xstrdup("command too long");
  goto error;
 }
 if (argc == 0) {
  argc = 1;
  argv = xcalloc(1, sizeof *argv);
  *argv = xstrdup("new-session");
 }
 if ((cmdlist = cmd_list_parse(argc, argv, NULL, 0, &cause)) == NULL) {
  cmd_free_argv(argc, argv);
  goto error;
 }
 cmd_free_argv(argc, argv);
 cmdq_append(c, cmdq_get_command(cmdlist, NULL, NULL, 0));
 cmdq_append(c, cmdq_get_callback(server_client_command_done, NULL));
 cmd_list_free(cmdlist);
 return;
error:
 cmdq_append(c, cmdq_get_callback(server_client_command_error, cause));
 if (cmdlist != NULL)
  cmd_list_free(cmdlist);
 c->flags |= CLIENT_EXIT;
}
static void
server_client_dispatch_identify(struct client *c, struct imsg *imsg)
{
 const char *data, *home;
 size_t datalen;
 int flags;
 char *name;
 if (c->flags & CLIENT_IDENTIFIED)
  fatalx("out-of-order identify message");
 data = imsg->data;
 datalen = imsg->hdr.len - IMSG_HEADER_SIZE;
 switch (imsg->hdr.type) {
 case MSG_IDENTIFY_FLAGS:
  if (datalen != sizeof flags)
   fatalx("bad MSG_IDENTIFY_FLAGS size");
  memcpy(&flags, data, sizeof flags);
  c->flags |= flags;
  log_debug("client %p IDENTIFY_FLAGS %#x", c, flags);
  break;
 case MSG_IDENTIFY_TERM:
  if (datalen == 0 || data[datalen - 1] != '\0')
   fatalx("bad MSG_IDENTIFY_TERM string");
  c->term = xstrdup(data);
  log_debug("client %p IDENTIFY_TERM %s", c, data);
  break;
 case MSG_IDENTIFY_TTYNAME:
  if (datalen == 0 || data[datalen - 1] != '\0')
   fatalx("bad MSG_IDENTIFY_TTYNAME string");
  c->ttyname = xstrdup(data);
  log_debug("client %p IDENTIFY_TTYNAME %s", c, data);
  break;
 case MSG_IDENTIFY_CWD:
  if (datalen == 0 || data[datalen - 1] != '\0')
   fatalx("bad MSG_IDENTIFY_CWD string");
  if (access(data, X_OK) == 0)
   c->cwd = xstrdup(data);
  else if ((home = find_home()) != NULL)
   c->cwd = xstrdup(home);
  else
   c->cwd = xstrdup("/");
  log_debug("client %p IDENTIFY_CWD %s", c, data);
  break;
 case MSG_IDENTIFY_STDIN:
  if (datalen != 0)
   fatalx("bad MSG_IDENTIFY_STDIN size");
  c->fd = imsg->fd;
  log_debug("client %p IDENTIFY_STDIN %d", c, imsg->fd);
  break;
 case MSG_IDENTIFY_ENVIRON:
  if (datalen == 0 || data[datalen - 1] != '\0')
   fatalx("bad MSG_IDENTIFY_ENVIRON string");
  if (strchr(data, '=') != NULL)
   environ_put(c->environ, data);
  log_debug("client %p IDENTIFY_ENVIRON %s", c, data);
  break;
 case MSG_IDENTIFY_CLIENTPID:
  if (datalen != sizeof c->pid)
   fatalx("bad MSG_IDENTIFY_CLIENTPID size");
  memcpy(&c->pid, data, sizeof c->pid);
  log_debug("client %p IDENTIFY_CLIENTPID %ld", c, (long)c->pid);
  break;
 default:
  break;
 }
 if (imsg->hdr.type != MSG_IDENTIFY_DONE)
  return;
 c->flags |= CLIENT_IDENTIFIED;
 if (*c->ttyname != '\0')
  name = xstrdup(c->ttyname);
 else
  xasprintf(&name, "client-%ld", (long)c->pid);
 c->name = name;
 log_debug("client %p name is %s", c, c->name);
#ifdef __CYGWIN__
 c->fd = open(c->ttyname, O_RDWR|O_NOCTTY);
#endif
 if (c->flags & CLIENT_CONTROL) {
  c->stdin_callback = control_callback;
  evbuffer_free(c->stderr_data);
  c->stderr_data = c->stdout_data;
  if (c->flags & CLIENT_CONTROLCONTROL)
   evbuffer_add_printf(c->stdout_data, "\033P1000p");
  proc_send(c->peer, MSG_STDIN, -1, NULL, 0);
  c->tty.fd = -1;
  close(c->fd);
  c->fd = -1;
  return;
 }
 if (c->fd == -1)
  return;
 if (tty_init(&c->tty, c, c->fd, c->term) != 0) {
  close(c->fd);
  c->fd = -1;
  return;
 }
 if (c->flags & CLIENT_UTF8)
  c->tty.flags |= TTY_UTF8;
 if (c->flags & CLIENT_256COLOURS)
  c->tty.term_flags |= TERM_256COLOURS;
 tty_resize(&c->tty);
 if (!(c->flags & CLIENT_CONTROL))
  c->flags |= CLIENT_TERMINAL;
}
static void
server_client_dispatch_shell(struct client *c)
{
 const char *shell;
 shell = options_get_string(global_s_options, "default-shell");
 if (*shell == '\0' || areshell(shell))
  shell = _PATH_BSHELL;
 proc_send_s(c->peer, MSG_SHELL, shell);
 proc_kill_peer(c->peer);
}
static void
server_client_stdout_cb(__unused int fd, __unused short events, void *arg)
{
 struct client *c = arg;
 if (~c->flags & CLIENT_DEAD)
  server_client_push_stdout(c);
 server_client_unref(c);
}
void
server_client_push_stdout(struct client *c)
{
 struct msg_stdout_data data;
 size_t sent, left;
 left = EVBUFFER_LENGTH(c->stdout_data);
 while (left != 0) {
  sent = left;
  if (sent > sizeof data.data)
   sent = sizeof data.data;
  memcpy(data.data, EVBUFFER_DATA(c->stdout_data), sent);
  data.size = sent;
  if (proc_send(c->peer, MSG_STDOUT, -1, &data, sizeof data) != 0)
   break;
  evbuffer_drain(c->stdout_data, sent);
  left = EVBUFFER_LENGTH(c->stdout_data);
  log_debug("%s: client %p, sent %zu, left %zu", __func__, c,
      sent, left);
 }
 if (left != 0) {
  c->references++;
  event_once(-1, EV_TIMEOUT, server_client_stdout_cb, c, NULL);
  log_debug("%s: client %p, queued", __func__, c);
 }
}
static void
server_client_stderr_cb(__unused int fd, __unused short events, void *arg)
{
 struct client *c = arg;
 if (~c->flags & CLIENT_DEAD)
  server_client_push_stderr(c);
 server_client_unref(c);
}
void
server_client_push_stderr(struct client *c)
{
 struct msg_stderr_data data;
 size_t sent, left;
 if (c->stderr_data == c->stdout_data) {
  server_client_push_stdout(c);
  return;
 }
 left = EVBUFFER_LENGTH(c->stderr_data);
 while (left != 0) {
  sent = left;
  if (sent > sizeof data.data)
   sent = sizeof data.data;
  memcpy(data.data, EVBUFFER_DATA(c->stderr_data), sent);
  data.size = sent;
  if (proc_send(c->peer, MSG_STDERR, -1, &data, sizeof data) != 0)
   break;
  evbuffer_drain(c->stderr_data, sent);
  left = EVBUFFER_LENGTH(c->stderr_data);
  log_debug("%s: client %p, sent %zu, left %zu", __func__, c,
      sent, left);
 }
 if (left != 0) {
  c->references++;
  event_once(-1, EV_TIMEOUT, server_client_stderr_cb, c, NULL);
  log_debug("%s: client %p, queued", __func__, c);
 }
}
void
server_client_add_message(struct client *c, const char *fmt, ...)
{
 struct message_entry *msg, *msg1;
 char *s;
 va_list ap;
 u_int limit;
 va_start(ap, fmt);
 xvasprintf(&s, fmt, ap);
 va_end(ap);
 log_debug("message %s (client %p)", s, c);
 msg = xcalloc(1, sizeof *msg);
 msg->msg_time = time(NULL);
 msg->msg_num = c->message_next++;
 msg->msg = s;
 TAILQ_INSERT_TAIL(&c->message_log, msg, entry);
 limit = options_get_number(global_options, "message-limit");
 TAILQ_FOREACH_SAFE(msg, &c->message_log, entry, msg1) {
  if (msg->msg_num + limit >= c->message_next)
   break;
  free(msg->msg);
  TAILQ_REMOVE(&c->message_log, msg, entry);
  free(msg);
 }
}
const char *
server_client_get_cwd(struct client *c)
{
 struct session *s;
 if (c != NULL && c->session == NULL && c->cwd != NULL)
  return (c->cwd);
 if (c != NULL && (s = c->session) != NULL && s->cwd != NULL)
  return (s->cwd);
 return (".");
}
char *
server_client_get_path(struct client *c, const char *file)
{
 char *path, resolved[PATH_MAX];
 if (*file == '/')
  path = xstrdup(file);
 else
  xasprintf(&path, "%s/%s", server_client_get_cwd(c), file);
 if (realpath(path, resolved) == NULL)
  return (path);
 free(path);
 return (xstrdup(resolved));
}
