#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/uio.h>
#include <errno.h>
#include <event.h>
#include <imsg.h>
#include <paths.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "tmux.h"
void server_client_key_table(struct client *, const char *);
void server_client_free(int, short, void *);
void server_client_check_focus(struct window_pane *);
void server_client_check_resize(struct window_pane *);
int server_client_check_mouse(struct client *);
void server_client_repeat_timer(int, short, void *);
void server_client_check_exit(struct client *);
void server_client_check_redraw(struct client *);
void server_client_set_title(struct client *);
void server_client_reset_state(struct client *);
int server_client_assume_paste(struct session *);
void server_client_dispatch(struct imsg *, void *);
void server_client_dispatch_command(struct client *, struct imsg *);
void server_client_dispatch_identify(struct client *, struct imsg *);
void server_client_dispatch_shell(struct client *);
int server_client_check_nested(struct client *c) {
  struct environ_entry *envent;
  struct window_pane *wp;
  if (c->tty.path == NULL) return (0);
  envent = environ_find(&c->environ, "TMUX");
  if (envent == NULL || *envent->value == '\0') return (0);
  RB_FOREACH(wp, window_pane_tree, &all_window_panes) {
    if (strcmp(wp->tty, c->tty.path) == 0) return (1);
  }
  return (0);
}
void server_client_key_table(struct client *c, const char *name) {
  key_bindings_unref_table(c->keytable);
  c->keytable = key_bindings_get_table(name, 1);
  c->keytable->references++;
}
void server_client_create(int fd) {
  struct client *c;
  setblocking(fd, 0);
  c = xcalloc(1, sizeof *c);
  c->references = 1;
  c->peer = proc_add_peer(server_proc, fd, server_client_dispatch, c);
  if (gettimeofday(&c->creation_time, NULL) != 0) fatal("gettimeofday failed");
  memcpy(&c->activity_time, &c->creation_time, sizeof c->activity_time);
  environ_init(&c->environ);
  c->fd = -1;
  c->cwd = -1;
  c->cmdq = cmdq_new(c);
  c->cmdq->client_exit = 1;
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
  TAILQ_INSERT_TAIL(&clients, c, entry);
  log_debug("new client %p", c);
}
int server_client_open(struct client *c, char **cause) {
  if (c->flags & CLIENT_CONTROL) return (0);
  if (strcmp(c->ttyname, "/dev/tty") == 0) {
    *cause = xstrdup("can't use /dev/tty");
    return (-1);
  }
  if (!(c->flags & CLIENT_TERMINAL)) {
    *cause = xstrdup("not a terminal");
    return (-1);
  }
  if (tty_open(&c->tty, cause) != 0) return (-1);
  return (0);
}
void server_client_lost(struct client *c) {
  struct message_entry *msg, *msg1;
  c->flags |= CLIENT_DEAD;
  status_prompt_clear(c);
  status_message_clear(c);
  if (c->stdin_callback != NULL)
    c->stdin_callback(c, 1, c->stdin_callback_data);
  TAILQ_REMOVE(&clients, c, entry);
  log_debug("lost client %p", c);
  if (c->flags & CLIENT_TERMINAL) tty_free(&c->tty);
  free(c->ttyname);
  free(c->term);
  evbuffer_free(c->stdin_data);
  evbuffer_free(c->stdout_data);
  if (c->stderr_data != c->stdout_data) evbuffer_free(c->stderr_data);
  if (event_initialized(&c->status_timer)) evtimer_del(&c->status_timer);
  screen_free(&c->status);
  free(c->title);
  close(c->cwd);
  evtimer_del(&c->repeat_timer);
  key_bindings_unref_table(c->keytable);
  if (event_initialized(&c->identify_timer)) evtimer_del(&c->identify_timer);
  free(c->message_string);
  if (event_initialized(&c->message_timer)) evtimer_del(&c->message_timer);
  TAILQ_FOREACH_SAFE(msg, &c->message_log, entry, msg1) {
    free(msg->msg);
    TAILQ_REMOVE(&c->message_log, msg, entry);
    free(msg);
  }
  free(c->prompt_string);
  free(c->prompt_buffer);
  c->cmdq->flags |= CMD_Q_DEAD;
  cmdq_free(c->cmdq);
  c->cmdq = NULL;
  environ_free(&c->environ);
  proc_remove_peer(c->peer);
  c->peer = NULL;
  server_client_unref(c);
  server_add_accept(0);
  recalculate_sizes();
  server_check_unattached();
  server_update_socket();
}
void server_client_unref(struct client *c) {
  log_debug("unref client %p (%d references)", c, c->references);
  c->references--;
  if (c->references == 0)
    event_once(-1, EV_TIMEOUT, server_client_free, c, NULL);
}
void server_client_free(unused int fd, unused short events, void *arg) {
  struct client *c = arg;
  log_debug("free client %p (%d references)", c, c->references);
  if (c->references == 0) free(c);
}
int server_client_check_mouse(struct client *c) {
  struct session *s = c->session;
  struct mouse_event *m = &c->tty.mouse;
  struct window *w;
  struct window_pane *wp;
  enum { NOTYPE, DOWN, UP, DRAG, WHEEL } type = NOTYPE;
  enum { NOWHERE, PANE, STATUS, BORDER } where = NOWHERE;
  u_int x, y, b;
  int key;
  log_debug("mouse %02x at %u,%u (last %u,%u) (%d)", m->b, m->x, m->y, m->lx,
            m->ly, c->tty.mouse_drag_flag);
  if (MOUSE_DRAG(m->b)) {
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
  } else if (MOUSE_BUTTONS(m->b) == 3) {
    type = UP;
    x = m->x, y = m->y, b = m->lb;
    log_debug("up at %u,%u", x, y);
  } else {
    type = DOWN;
    x = m->x, y = m->y, b = m->b;
    log_debug("down at %u,%u", x, y);
  }
  if (type == NOTYPE) return (KEYC_NONE);
  m->s = s->id;
  m->statusat = status_at_line(c);
  if (m->statusat != -1 && y == (u_int)m->statusat) {
    w = status_get_window_at(c, x);
    if (w == NULL) return (KEYC_NONE);
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
      if ((wp->xoff + wp->sx == x && wp->yoff <= 1 + y &&
           wp->yoff + wp->sy >= y) ||
          (wp->yoff + wp->sy == y && wp->xoff <= 1 + x &&
           wp->xoff + wp->sx >= x))
        break;
    }
    if (wp != NULL)
      where = BORDER;
    else {
      wp = window_get_active_at(s->curw->window, x, y);
      if (wp != NULL) where = PANE;
      log_debug("mouse at %u,%u is on pane %%%u", x, y, wp->id);
    }
    if (where == NOWHERE) return (KEYC_NONE);
    m->wp = wp->id;
    m->w = wp->window->id;
  } else
    m->wp = -1;
  if (type != DRAG && c->tty.mouse_drag_flag) {
    if (c->tty.mouse_drag_release != NULL) c->tty.mouse_drag_release(c, m);
    c->tty.mouse_drag_update = NULL;
    c->tty.mouse_drag_release = NULL;
    c->tty.mouse_drag_flag = 0;
    return (KEYC_MOUSE);
  }
  key = KEYC_NONE;
  switch (type) {
    case NOTYPE:
      break;
    case DRAG:
      if (c->tty.mouse_drag_update != NULL)
        c->tty.mouse_drag_update(c, m);
      else {
        switch (MOUSE_BUTTONS(b)) {
          case 0:
            if (where == PANE) key = KEYC_MOUSEDRAG1_PANE;
            if (where == STATUS) key = KEYC_MOUSEDRAG1_STATUS;
            if (where == BORDER) key = KEYC_MOUSEDRAG1_BORDER;
            break;
          case 1:
            if (where == PANE) key = KEYC_MOUSEDRAG2_PANE;
            if (where == STATUS) key = KEYC_MOUSEDRAG2_STATUS;
            if (where == BORDER) key = KEYC_MOUSEDRAG2_BORDER;
            break;
          case 2:
            if (where == PANE) key = KEYC_MOUSEDRAG3_PANE;
            if (where == STATUS) key = KEYC_MOUSEDRAG3_STATUS;
            if (where == BORDER) key = KEYC_MOUSEDRAG3_BORDER;
            break;
        }
      }
      c->tty.mouse_drag_flag = 1;
      break;
    case WHEEL:
      if (MOUSE_BUTTONS(b) == MOUSE_WHEEL_UP) {
        if (where == PANE) key = KEYC_WHEELUP_PANE;
        if (where == STATUS) key = KEYC_WHEELUP_STATUS;
        if (where == BORDER) key = KEYC_WHEELUP_BORDER;
      } else {
        if (where == PANE) key = KEYC_WHEELDOWN_PANE;
        if (where == STATUS) key = KEYC_WHEELDOWN_STATUS;
        if (where == BORDER) key = KEYC_WHEELDOWN_BORDER;
      }
      break;
    case UP:
      switch (MOUSE_BUTTONS(b)) {
        case 0:
          if (where == PANE) key = KEYC_MOUSEUP1_PANE;
          if (where == STATUS) key = KEYC_MOUSEUP1_STATUS;
          if (where == BORDER) key = KEYC_MOUSEUP1_BORDER;
          break;
        case 1:
          if (where == PANE) key = KEYC_MOUSEUP2_PANE;
          if (where == STATUS) key = KEYC_MOUSEUP2_STATUS;
          if (where == BORDER) key = KEYC_MOUSEUP2_BORDER;
          break;
        case 2:
          if (where == PANE) key = KEYC_MOUSEUP3_PANE;
          if (where == STATUS) key = KEYC_MOUSEUP3_STATUS;
          if (where == BORDER) key = KEYC_MOUSEUP3_BORDER;
          break;
      }
      break;
    case DOWN:
      switch (MOUSE_BUTTONS(b)) {
        case 0:
          if (where == PANE) key = KEYC_MOUSEDOWN1_PANE;
          if (where == STATUS) key = KEYC_MOUSEDOWN1_STATUS;
          if (where == BORDER) key = KEYC_MOUSEDOWN1_BORDER;
          break;
        case 1:
          if (where == PANE) key = KEYC_MOUSEDOWN2_PANE;
          if (where == STATUS) key = KEYC_MOUSEDOWN2_STATUS;
          if (where == BORDER) key = KEYC_MOUSEDOWN2_BORDER;
          break;
        case 2:
          if (where == PANE) key = KEYC_MOUSEDOWN3_PANE;
          if (where == STATUS) key = KEYC_MOUSEDOWN3_STATUS;
          if (where == BORDER) key = KEYC_MOUSEDOWN3_BORDER;
          break;
      }
      break;
  }
  if (key == KEYC_NONE) return (KEYC_NONE);
  if (b & MOUSE_MASK_META) key |= KEYC_ESCAPE;
  if (b & MOUSE_MASK_CTRL) key |= KEYC_CTRL;
  if (b & MOUSE_MASK_SHIFT) key |= KEYC_SHIFT;
  return (key);
}
int server_client_assume_paste(struct session *s) {
  struct timeval tv;
  int t;
  if ((t = options_get_number(s->options, "assume-paste-time")) == 0)
    return (0);
  timersub(&s->activity_time, &s->last_activity_time, &tv);
  if (tv.tv_sec == 0 && tv.tv_usec < t * 1000) return (1);
  return (0);
}
void server_client_handle_key(struct client *c, int key) {
  struct mouse_event *m = &c->tty.mouse;
  struct session *s = c->session;
  struct window *w;
  struct window_pane *wp;
  struct timeval tv;
  struct key_table *table;
  struct key_binding bd_find, *bd;
  int xtimeout;
  if (s == NULL || (c->flags & (CLIENT_DEAD | CLIENT_SUSPENDED)) != 0) return;
  w = s->curw->window;
  if (gettimeofday(&c->activity_time, NULL) != 0) fatal("gettimeofday failed");
  session_update_activity(s, &c->activity_time);
  if (c->flags & CLIENT_IDENTIFY && key >= '0' && key <= '9') {
    if (c->flags & CLIENT_READONLY) return;
    window_unzoom(w);
    wp = window_pane_at_index(w, key - '0');
    if (wp != NULL && window_pane_visible(wp)) window_set_active_pane(w, wp);
    server_clear_identify(c);
    return;
  }
  if (!(c->flags & CLIENT_READONLY)) {
    status_message_clear(c);
    server_clear_identify(c);
  }
  if (c->prompt_string != NULL) {
    if (!(c->flags & CLIENT_READONLY)) status_prompt_key(c, key);
    return;
  }
  if (key == KEYC_MOUSE) {
    if (c->flags & CLIENT_READONLY) return;
    key = server_client_check_mouse(c);
    if (key == KEYC_NONE) return;
    m->valid = 1;
    m->key = key;
    if (!options_get_number(s->options, "mouse")) goto forward;
  } else
    m->valid = 0;
  if (!KEYC_IS_MOUSE(key) && server_client_assume_paste(s)) goto forward;
retry:
  bd_find.key = key;
  bd = RB_FIND(key_bindings, &c->keytable->key_bindings, &bd_find);
  if (bd != NULL) {
    if ((c->flags & CLIENT_REPEAT) && !bd->can_repeat) {
      server_client_key_table(c, "root");
      c->flags &= ~CLIENT_REPEAT;
      server_status_client(c);
      goto retry;
    }
    table = c->keytable;
    table->references++;
    xtimeout = options_get_number(s->options, "repeat-time");
    if (xtimeout != 0 && bd->can_repeat) {
      c->flags |= CLIENT_REPEAT;
      tv.tv_sec = xtimeout / 1000;
      tv.tv_usec = (xtimeout % 1000) * 1000L;
      evtimer_del(&c->repeat_timer);
      evtimer_add(&c->repeat_timer, &tv);
    } else {
      c->flags &= ~CLIENT_REPEAT;
      server_client_key_table(c, "root");
    }
    server_status_client(c);
    key_bindings_dispatch(bd, c, m);
    key_bindings_unref_table(table);
    return;
  }
  if (c->flags & CLIENT_REPEAT) {
    server_client_key_table(c, "root");
    c->flags &= ~CLIENT_REPEAT;
    server_status_client(c);
    goto retry;
  }
  if (strcmp(c->keytable->name, "root") != 0) {
    server_client_key_table(c, "root");
    server_status_client(c);
    return;
  }
  if (key == options_get_number(s->options, "prefix") ||
      key == options_get_number(s->options, "prefix2")) {
    server_client_key_table(c, "prefix");
    server_status_client(c);
    return;
  }
forward:
  if (c->flags & CLIENT_READONLY) return;
  if (KEYC_IS_MOUSE(key))
    wp = cmd_mouse_pane(m, NULL, NULL);
  else
    wp = w->active;
  if (wp != NULL) window_pane_key(wp, c, s, key, m);
}
void server_client_loop(void) {
  struct client *c;
  struct window *w;
  struct window_pane *wp;
  TAILQ_FOREACH(c, &clients, entry) {
    server_client_check_exit(c);
    if (c->session != NULL) {
      server_client_check_redraw(c);
      server_client_reset_state(c);
    }
  }
  RB_FOREACH(w, windows, &windows) {
    w->flags &= ~WINDOW_REDRAW;
    TAILQ_FOREACH(wp, &w->panes, entry) {
      if (wp->fd != -1) {
        server_client_check_focus(wp);
        server_client_check_resize(wp);
      }
      wp->flags &= ~PANE_REDRAW;
    }
    check_window_name(w);
  }
}
void server_client_check_resize(struct window_pane *wp) {
  struct winsize ws;
  if (!(wp->flags & PANE_RESIZE)) return;
  memset(&ws, 0, sizeof ws);
  ws.ws_col = wp->sx;
  ws.ws_row = wp->sy;
  if (ioctl(wp->fd, TIOCSWINSZ, &ws) == -1) {
#ifdef __sun
    if (errno != EINVAL && errno != ENXIO)
#endif
      fatal("ioctl failed");
  }
  wp->flags &= ~PANE_RESIZE;
}
void server_client_check_focus(struct window_pane *wp) {
  struct client *c;
  int push;
  if (!options_get_number(global_options, "focus-events")) return;
  push = wp->flags & PANE_FOCUSPUSH;
  wp->flags &= ~PANE_FOCUSPUSH;
  if (!(wp->base.mode & MODE_FOCUSON)) return;
  if (wp->window->active != wp) goto not_focused;
  if (wp->screen != &wp->base) goto not_focused;
  TAILQ_FOREACH(c, &clients, entry) {
    if (c->session == NULL || !(c->flags & CLIENT_FOCUSED)) continue;
    if (c->session->flags & SESSION_UNATTACHED) continue;
    if (c->session->curw->window == wp->window) goto focused;
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
void server_client_reset_state(struct client *c) {
  struct window *w = c->session->curw->window;
  struct window_pane *wp = w->active;
  struct screen *s = wp->screen;
  struct options *oo = c->session->options;
  int status, mode, o;
  if (c->flags & CLIENT_SUSPENDED) return;
  if (c->flags & CLIENT_CONTROL) return;
  tty_region(&c->tty, 0, c->tty.sy - 1);
  status = options_get_number(oo, "status");
  if (!window_pane_visible(wp) || wp->yoff + s->cy >= c->tty.sy - status)
    tty_cursor(&c->tty, 0, 0);
  else {
    o = status && options_get_number(oo, "status-position") == 0;
    tty_cursor(&c->tty, wp->xoff + s->cx, o + wp->yoff + s->cy);
  }
  mode = s->mode;
  if (options_get_number(oo, "mouse"))
    mode = (mode & ~ALL_MOUSE_MODES) | MODE_MOUSE_BUTTON;
  if ((c->tty.flags & TTY_UTF8) && (mode & ALL_MOUSE_MODES) &&
      options_get_number(oo, "mouse-utf8"))
    mode |= MODE_MOUSE_UTF8;
  else
    mode &= ~MODE_MOUSE_UTF8;
  tty_update_mode(&c->tty, mode, s);
  tty_reset(&c->tty);
}
void server_client_repeat_timer(unused int fd, unused short events,
                                void *data) {
  struct client *c = data;
  if (c->flags & CLIENT_REPEAT) {
    server_client_key_table(c, "root");
    c->flags &= ~CLIENT_REPEAT;
    server_status_client(c);
  }
}
void server_client_check_exit(struct client *c) {
  if (!(c->flags & CLIENT_EXIT)) return;
  if (EVBUFFER_LENGTH(c->stdin_data) != 0) return;
  if (EVBUFFER_LENGTH(c->stdout_data) != 0) return;
  if (EVBUFFER_LENGTH(c->stderr_data) != 0) return;
  proc_send(c->peer, MSG_EXIT, -1, &c->retval, sizeof c->retval);
  c->flags &= ~CLIENT_EXIT;
}
void server_client_check_redraw(struct client *c) {
  struct session *s = c->session;
  struct tty *tty = &c->tty;
  struct window_pane *wp;
  int flags, redraw;
  if (c->flags & (CLIENT_CONTROL | CLIENT_SUSPENDED)) return;
  if (c->flags & (CLIENT_REDRAW | CLIENT_STATUS)) {
    if (options_get_number(s->options, "set-titles"))
      server_client_set_title(c);
    if (c->message_string != NULL)
      redraw = status_message_redraw(c);
    else if (c->prompt_string != NULL)
      redraw = status_prompt_redraw(c);
    else
      redraw = status_redraw(c);
    if (!redraw) c->flags &= ~CLIENT_STATUS;
  }
  flags = tty->flags & (TTY_FREEZE | TTY_NOCURSOR);
  tty->flags = (tty->flags & ~TTY_FREEZE) | TTY_NOCURSOR;
  if (c->flags & CLIENT_REDRAW) {
    tty_update_mode(tty, tty->mode, NULL);
    screen_redraw_screen(c, 1, 1, 1);
    c->flags &= ~(CLIENT_STATUS | CLIENT_BORDERS);
  } else if (c->flags & CLIENT_REDRAWWINDOW) {
    tty_update_mode(tty, tty->mode, NULL);
    TAILQ_FOREACH(wp, &c->session->curw->window->panes, entry)
    screen_redraw_pane(c, wp);
    c->flags &= ~CLIENT_REDRAWWINDOW;
  } else {
    TAILQ_FOREACH(wp, &c->session->curw->window->panes, entry) {
      if (wp->flags & PANE_REDRAW) {
        tty_update_mode(tty, tty->mode, NULL);
        screen_redraw_pane(c, wp);
      }
    }
  }
  if (c->flags & CLIENT_BORDERS) {
    tty_update_mode(tty, tty->mode, NULL);
    screen_redraw_screen(c, 0, 0, 1);
  }
  if (c->flags & CLIENT_STATUS) {
    tty_update_mode(tty, tty->mode, NULL);
    screen_redraw_screen(c, 0, 1, 0);
  }
  tty->flags = (tty->flags & ~(TTY_FREEZE | TTY_NOCURSOR)) | flags;
  tty_update_mode(tty, tty->mode, NULL);
  c->flags &=
      ~(CLIENT_REDRAW | CLIENT_BORDERS | CLIENT_STATUS | CLIENT_STATUSFORCE);
}
void server_client_set_title(struct client *c) {
  struct session *s = c->session;
  const char *template;
  char *title;
  struct format_tree *ft;
  template = options_get_string(s->options, "set-titles-string");
  ft = format_create();
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
voidserver_client_dispatch(struct imsg *imsg, void *arg) {
  struct client *c = arg;
  struct msg_stdin_data stdindata;
  const char *data;
  ssize_t datalen;
  struct session *s;
  if (c->flags & CLIENT_DEAD) return;
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
      if (datalen != sizeof stdindata) fatalx("bad MSG_STDIN size");
      memcpy(&stdindata, data, sizeof stdindata);
      if (c->stdin_callback == NULL) break;
      if (stdindata.size <= 0)
        c->stdin_closed = 1;
      else {
        evbuffer_add(c->stdin_data, stdindata.data, stdindata.size);
      }
      c->stdin_callback(c, c->stdin_closed, c->stdin_callback_data);
      break;
    case MSG_RESIZE:
      if (datalen != 0) fatalx("bad MSG_RESIZE size");
      if (c->flags & CLIENT_CONTROL) break;
      if (tty_resize(&c->tty)) {
        recalculate_sizes();
        server_redraw_client(c);
      }
      break;
    case MSG_EXITING:
      if (datalen != 0) fatalx("bad MSG_EXITING size");
      c->session = NULL;
      tty_close(&c->tty);
      proc_send(c->peer, MSG_EXITED, -1, NULL, 0);
      break;
    case MSG_WAKEUP:
    case MSG_UNLOCK:
      if (datalen != 0) fatalx("bad MSG_WAKEUP size");
      if (!(c->flags & CLIENT_SUSPENDED)) break;
      c->flags &= ~CLIENT_SUSPENDED;
      if (c->tty.fd == -1)
        break;
      s = c->session;
      if (gettimeofday(&c->activity_time, NULL) != 0)
        fatal("gettimeofday failed");
      if (s != NULL) session_update_activity(s, &c->activity_time);
      tty_start_tty(&c->tty);
      server_redraw_client(c);
      recalculate_sizes();
      break;
    case MSG_SHELL:
      if (datalen != 0) fatalx("bad MSG_SHELL size");
      server_client_dispatch_shell(c);
      break;
  }
  server_push_stdout(c);
  server_push_stderr(c);
}
void server_client_dispatch_command(struct client *c, struct imsg *imsg) {
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
  if (len > 0 && buf[len - 1] != '\0') fatalx("bad MSG_COMMAND string");
  argc = data.argc;
  if (cmd_unpack_argv(buf, len, argc, &argv) != 0) {
    cmdq_error(c->cmdq, "command too long");
    goto error;
  }
  if (argc == 0) {
    argc = 1;
    argv = xcalloc(1, sizeof *argv);
    *argv = xstrdup("new-session");
  }
  if ((cmdlist = cmd_list_parse(argc, argv, NULL, 0, &cause)) == NULL) {
    cmdq_error(c->cmdq, "%s", cause);
    cmd_free_argv(argc, argv);
    goto error;
  }
  cmd_free_argv(argc, argv);
  if (c != cfg_client || cfg_finished)
    cmdq_run(c->cmdq, cmdlist, NULL);
  else
    cmdq_append(c->cmdq, cmdlist, NULL);
  cmd_list_free(cmdlist);
  return;
error:
  if (cmdlist != NULL) cmd_list_free(cmdlist);
  c->flags |= CLIENT_EXIT;
}
voidserver_client_dispatch_identify(struct client *c, struct imsg *imsg) {
  const char *data;
  size_t datalen;
  int flags;
  if (c->flags & CLIENT_IDENTIFIED) fatalx("out-of-order identify message");
  data = imsg->data;
  datalen = imsg->hdr.len - IMSG_HEADER_SIZE;
  switch (imsg->hdr.type) {
    case MSG_IDENTIFY_FLAGS:
      if (datalen != sizeof flags) fatalx("bad MSG_IDENTIFY_FLAGS size");
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
      if ((c->cwd = open(data, O_RDONLY)) == -1) c->cwd = open("/", O_RDONLY);
      log_debug("client %p IDENTIFY_CWD %s", c, data);
      break;
    case MSG_IDENTIFY_STDIN:
      if (datalen != 0) fatalx("bad MSG_IDENTIFY_STDIN size");
      c->fd = imsg->fd;
      log_debug("client %p IDENTIFY_STDIN %d", c, imsg->fd);
      break;
    case MSG_IDENTIFY_ENVIRON:
      if (datalen == 0 || data[datalen - 1] != '\0')
        fatalx("bad MSG_IDENTIFY_ENVIRON string");
      if (strchr(data, '=') != NULL) environ_put(&c->environ, data);
      log_debug("client %p IDENTIFY_ENVIRON %s", c, data);
      break;
    case MSG_IDENTIFY_CLIENTPID:
      if (datalen != sizeof c->pid) fatalx("bad MSG_IDENTIFY_CLIENTPID size");
      memcpy(&c->pid, data, sizeof c->pid);
      log_debug("client %p IDENTIFY_CLIENTPID %ld", c, (long)c->pid);
      break;
    default:
      break;
  }
  if (imsg->hdr.type != MSG_IDENTIFY_DONE) return;
  c->flags |= CLIENT_IDENTIFIED;
  if (c->flags & CLIENT_CONTROL) {
    c->stdin_callback = control_callback;
    evbuffer_free(c->stderr_data);
    c->stderr_data = c->stdout_data;
    if (c->flags & CLIENT_CONTROLCONTROL)
      evbuffer_add_printf(c->stdout_data, "\033P1000p");
    proc_send(c->peer, MSG_STDIN, -1, NULL, 0);
    c->tty.fd = -1;
    c->tty.log_fd = -1;
    close(c->fd);
    c->fd = -1;
    return;
  }
  if (c->fd == -1) return;
  if (tty_init(&c->tty, c, c->fd, c->term) != 0) {
    close(c->fd);
    c->fd = -1;
    return;
  }
  if (c->flags & CLIENT_UTF8) c->tty.flags |= TTY_UTF8;
  if (c->flags & CLIENT_256COLOURS) c->tty.term_flags |= TERM_256COLOURS;
  tty_resize(&c->tty);
  if (!(c->flags & CLIENT_CONTROL)) c->flags |= CLIENT_TERMINAL;
}
void server_client_dispatch_shell(struct client *c) {
  const char *shell;
  shell = options_get_string(global_s_options, "default-shell");
  if (*shell == '\0' || areshell(shell)) shell = _PATH_BSHELL;
  proc_send_s(c->peer, MSG_SHELL, shell);
  proc_kill_peer(c->peer);
}
