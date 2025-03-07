#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/uio.h>
#include <errno.h>
#include <event.h>
#include <fcntl.h>
#include <imsg.h>
#include <paths.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "tmux.h"
static void server_client_free(int, short, void *);
static void server_client_check_pane_focus(struct window_pane *);
static void server_client_check_pane_resize(struct window_pane *);
static void server_client_check_pane_buffer(struct window_pane *);
static void server_client_check_window_resize(struct window *);
static key_code server_client_check_mouse(struct client *, struct key_event *);
static void server_client_repeat_timer(int, short, void *);
static void server_client_click_timer(int, short, void *);
static void server_client_check_exit(struct client *);
static void server_client_check_redraw(struct client *);
static void server_client_set_title(struct client *);
static void server_client_reset_state(struct client *);
static int server_client_assume_paste(struct session *);
static void server_client_resize_event(int, short, void *);
static void server_client_dispatch(struct imsg *, void *);
static void server_client_dispatch_command(struct client *, struct imsg *);
static void server_client_dispatch_identify(struct client *, struct imsg *);
static void server_client_dispatch_shell(struct client *);
static void server_client_dispatch_write_ready(struct client *, struct imsg *);
static void server_client_dispatch_read_data(struct client *, struct imsg *);
static void server_client_dispatch_read_done(struct client *, struct imsg *);
static int server_client_window_cmp(struct client_window *cw1,
                                    struct client_window *cw2) {
  if (cw1->window < cw2->window) return (-1);
  if (cw1->window > cw2->window) return (1);
  return (0);
}
u_int server_client_how_many(void) {
  struct client *c;
  u_int n;
  n = 0;
  TAILQ_FOREACH(c, &clients, entry) {
    if (c->session != NULL && (~c->flags & CLIENT_DETACHING)) n++;
  }
  return (n);
}
static void server_client_overlay_timer(__unused int fd, __unused short events,
                                        void *data) {
  server_client_clear_overlay(data);
}
void server_client_set_overlay(struct client *c, u_int delay,
                               overlay_check_cb checkcb, overlay_mode_cb modecb,
                               overlay_draw_cb drawcb, overlay_key_cb keycb,
                               overlay_free_cb freecb, void *data) {
  struct timeval tv;
  if (c->overlay_draw != NULL) server_client_clear_overlay(c);
  tv.tv_sec = delay / 1000;
  tv.tv_usec = (delay % 1000) * 1000L;
  if (event_initialized(&c->overlay_timer)) evtimer_del(&c->overlay_timer);
  evtimer_set(&c->overlay_timer, server_client_overlay_timer, c);
  if (delay != 0) evtimer_add(&c->overlay_timer, &tv);
  c->overlay_check = checkcb;
  c->overlay_mode = modecb;
  c->overlay_draw = drawcb;
  c->overlay_key = keycb;
  c->overlay_free = freecb;
  c->overlay_data = data;
  c->tty.flags |= TTY_FREEZE;
  if (c->overlay_mode == NULL) c->tty.flags |= TTY_NOCURSOR;
  server_redraw_client(c);
}
void server_client_clear_overlay(struct client *c) {
  if (c->overlay_draw == NULL) return;
  if (event_initialized(&c->overlay_timer)) evtimer_del(&c->overlay_timer);
  if (c->overlay_free != NULL) c->overlay_free(c);
  c->overlay_check = NULL;
  c->overlay_mode = NULL;
  c->overlay_draw = NULL;
  c->overlay_key = NULL;
  c->overlay_free = NULL;
  c->overlay_data = NULL;
  c->tty.flags &= ~(TTY_FREEZE | TTY_NOCURSOR);
  server_redraw_client(c);
}
int server_client_check_nested(struct client *c) {
  struct environ_entry *envent;
  struct window_pane *wp;
  envent = environ_find(c->environ, "TMUX");
  if (envent == NULL || *envent->value == '\0') return (0);
  RB_FOREACH(wp, window_pane_tree, &all_window_panes) {
    if (strcmp(wp->tty, c->ttyname) == 0) return (1);
  }
  return (0);
}
void server_client_set_key_table(struct client *c, const char *name) {
  if (name == NULL) name = server_client_get_key_table(c);
  key_bindings_unref_table(c->keytable);
  c->keytable = key_bindings_get_table(name, 1);
  c->keytable->references++;
}
const char *server_client_get_key_table(struct client *c) {
  struct session *s = c->session;
  const char *name;
  if (s == NULL) return ("root");
  name = options_get_string(s->options, "key-table");
  if (*name == '\0') return ("root");
  return (name);
}
static int server_client_is_default_key_table(struct client *c,
                                              struct key_table *table) {
  return (strcmp(table->name, server_client_get_key_table(c)) == 0);
}
struct client *server_client_create(int fd) {
  struct client *c;
  setblocking(fd, 0);
  c = xcalloc(1, sizeof *c);
  c->references = 1;
  c->peer = proc_add_peer(server_proc, fd, server_client_dispatch, c);
  if (gettimeofday(&c->creation_time, NULL) != 0) fatal("gettimeofday failed");
  memcpy(&c->activity_time, &c->creation_time, sizeof c->activity_time);
  c->environ = environ_create();
  c->fd = -1;
  c->out_fd = -1;
  c->queue = cmdq_new();
  RB_INIT(&c->windows);
  RB_INIT(&c->files);
  c->tty.sx = 80;
  c->tty.sy = 24;
  status_init(c);
  c->flags |= CLIENT_FOCUSED;
  c->keytable = key_bindings_get_table("root", 1);
  c->keytable->references++;
  evtimer_set(&c->repeat_timer, server_client_repeat_timer, c);
  evtimer_set(&c->click_timer, server_client_click_timer, c);
  TAILQ_INSERT_TAIL(&clients, c, entry);
  log_debug("new client %p", c);
  return (c);
}
int server_client_open(struct client *c, char **cause) {
  const char *ttynam = _PATH_TTY;
  if (c->flags & CLIENT_CONTROL) return (0);
  if (strcmp(c->ttyname, ttynam) == 0 ||
      ((isatty(STDIN_FILENO) && (ttynam = ttyname(STDIN_FILENO)) != NULL &&
        strcmp(c->ttyname, ttynam) == 0) ||
       (isatty(STDOUT_FILENO) && (ttynam = ttyname(STDOUT_FILENO)) != NULL &&
        strcmp(c->ttyname, ttynam) == 0) ||
       (isatty(STDERR_FILENO) && (ttynam = ttyname(STDERR_FILENO)) != NULL &&
        strcmp(c->ttyname, ttynam) == 0))) {
    xasprintf(cause, "can't use %s", c->ttyname);
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
  struct client_file *cf, *cf1;
  struct client_window *cw, *cw1;
  c->flags |= CLIENT_DEAD;
  server_client_clear_overlay(c);
  status_prompt_clear(c);
  status_message_clear(c);
  RB_FOREACH_SAFE(cf, client_files, &c->files, cf1) {
    cf->error = EINTR;
    file_fire_done(cf);
  }
  RB_FOREACH_SAFE(cw, client_windows, &c->windows, cw1) {
    RB_REMOVE(client_windows, &c->windows, cw);
    free(cw);
  }
  TAILQ_REMOVE(&clients, c, entry);
  log_debug("lost client %p", c);
  if (c->flags & CLIENT_CONTROL) control_stop(c);
  if (c->flags & CLIENT_TERMINAL) tty_free(&c->tty);
  free(c->ttyname);
  free(c->term_name);
  free(c->term_type);
  status_free(c);
  free(c->title);
  free((void *)c->cwd);
  evtimer_del(&c->repeat_timer);
  evtimer_del(&c->click_timer);
  key_bindings_unref_table(c->keytable);
  free(c->message_string);
  if (event_initialized(&c->message_timer)) evtimer_del(&c->message_timer);
  free(c->prompt_saved);
  free(c->prompt_string);
  free(c->prompt_buffer);
  format_lost_client(c);
  environ_free(c->environ);
  proc_remove_peer(c->peer);
  c->peer = NULL;
  if (c->out_fd != -1) close(c->out_fd);
  if (c->fd != -1) {
    close(c->fd);
    c->fd = -1;
  }
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
static void server_client_free(__unused int fd, __unused short events,
                               void *arg) {
  struct client *c = arg;
  log_debug("free client %p (%d references)", c, c->references);
  cmdq_free(c->queue);
  if (c->references == 0) {
    free((void *)c->name);
    free(c);
  }
}
void server_client_suspend(struct client *c) {
  struct session *s = c->session;
  if (s == NULL || (c->flags & CLIENT_DETACHING)) return;
  tty_stop_tty(&c->tty);
  c->flags |= CLIENT_SUSPENDED;
  proc_send(c->peer, MSG_SUSPEND, -1, NULL, 0);
}
void server_client_detach(struct client *c, enum msgtype msgtype) {
  struct session *s = c->session;
  if (s == NULL || (c->flags & CLIENT_DETACHING)) return;
  c->flags |= CLIENT_DETACHING;
  notify_client("client-detached", c);
  proc_send(c->peer, msgtype, -1, s->name, strlen(s->name) + 1);
}
void server_client_exec(struct client *c, const char *cmd) {
  struct session *s = c->session;
  char *msg;
  const char *shell;
  size_t cmdsize, shellsize;
  if (*cmd == '\0') return;
  cmdsize = strlen(cmd) + 1;
  if (s != NULL)
    shell = options_get_string(s->options, "default-shell");
  else
    shell = options_get_string(global_s_options, "default-shell");
  if (!checkshell(shell)) shell = _PATH_BSHELL;
  shellsize = strlen(shell) + 1;
  msg = xmalloc(cmdsize + shellsize);
  memcpy(msg, cmd, cmdsize);
  memcpy(msg + cmdsize, shell, shellsize);
  proc_send(c->peer, MSG_EXEC, -1, msg, cmdsize + shellsize);
  free(msg);
}
static key_code server_client_check_mouse(struct client *c,
                                          struct key_event *event) {
  struct mouse_event *m = &event->m;
  struct session *s = c->session;
  struct winlink *wl;
  struct window_pane *wp;
  u_int x, y, b, sx, sy, px, py;
  int ignore = 0;
  key_code key;
  struct timeval tv;
  struct style_range *sr;
  enum {
    NOTYPE,
    MOVE,
    DOWN,
    UP,
    DRAG,
    WHEEL,
    SECOND,
    DOUBLE,
    TRIPLE
  } type = NOTYPE;
  enum {
    NOWHERE,
    PANE,
    STATUS,
    STATUS_LEFT,
    STATUS_RIGHT,
    STATUS_DEFAULT,
    BORDER
  } where = NOWHERE;
  log_debug("%s mouse %02x at %u,%u (last %u,%u) (%d)", c->name, m->b, m->x,
            m->y, m->lx, m->ly, c->tty.mouse_drag_flag);
  if (event->key == KEYC_DOUBLECLICK) {
    type = DOUBLE;
    x = m->x, y = m->y, b = m->b;
    ignore = 1;
    log_debug("double-click at %u,%u", x, y);
  } else if ((m->sgr_type != ' ' && MOUSE_DRAG(m->sgr_b) &&
              MOUSE_BUTTONS(m->sgr_b) == 3) ||
             (m->sgr_type == ' ' && MOUSE_DRAG(m->b) &&
              MOUSE_BUTTONS(m->b) == 3 && MOUSE_BUTTONS(m->lb) == 3)) {
    type = MOVE;
    x = m->x, y = m->y, b = 0;
    log_debug("move at %u,%u", x, y);
  } else if (MOUSE_DRAG(m->b)) {
    type = DRAG;
    if (c->tty.mouse_drag_flag) {
      x = m->x, y = m->y, b = m->b;
      if (x == m->lx && y == m->ly) return (KEYC_UNKNOWN);
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
        type = SECOND;
        x = m->x, y = m->y, b = m->b;
        log_debug("second-click at %u,%u", x, y);
        c->flags |= CLIENT_TRIPLECLICK;
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
    } else {
      type = DOWN;
      x = m->x, y = m->y, b = m->b;
      log_debug("down at %u,%u", x, y);
      c->flags |= CLIENT_DOUBLECLICK;
    }
    if (KEYC_CLICK_TIMEOUT != 0) {
      memcpy(&c->click_event, m, sizeof c->click_event);
      c->click_button = m->b;
      log_debug("click timer started");
      tv.tv_sec = KEYC_CLICK_TIMEOUT / 1000;
      tv.tv_usec = (KEYC_CLICK_TIMEOUT % 1000) * 1000L;
      evtimer_del(&c->click_timer);
      evtimer_add(&c->click_timer, &tv);
    }
  }
have_event:
  if (type == NOTYPE) return (KEYC_UNKNOWN);
  m->s = s->id;
  m->w = -1;
  m->ignore = ignore;
  m->statusat = status_at_line(c);
  m->statuslines = status_line_size(c);
  if (m->statusat != -1 && y >= (u_int)m->statusat &&
      y < m->statusat + m->statuslines) {
    sr = status_get_range(c, x, y - m->statusat);
    if (sr == NULL) {
      where = STATUS_DEFAULT;
    } else {
      switch (sr->type) {
        case STYLE_RANGE_NONE:
          return (KEYC_UNKNOWN);
        case STYLE_RANGE_LEFT:
          where = STATUS_LEFT;
          break;
        case STYLE_RANGE_RIGHT:
          where = STATUS_RIGHT;
          break;
        case STYLE_RANGE_WINDOW:
          wl = winlink_find_by_index(&s->windows, sr->argument);
          if (wl == NULL) return (KEYC_UNKNOWN);
          m->w = wl->window->id;
          where = STATUS;
          break;
      }
    }
  }
  if (where == NOWHERE) {
    px = x;
    if (m->statusat == 0 && y >= m->statuslines)
      py = y - m->statuslines;
    else if (m->statusat > 0 && y >= (u_int)m->statusat)
      py = m->statusat - 1;
    else
      py = y;
    tty_window_offset(&c->tty, &m->ox, &m->oy, &sx, &sy);
    log_debug("mouse window @%u at %u,%u (%ux%u)", s->curw->window->id, m->ox,
              m->oy, sx, sy);
    if (px > sx || py > sy) return (KEYC_UNKNOWN);
    px = px + m->ox;
    py = py + m->oy;
    if (~s->curw->window->flags & WINDOW_ZOOMED) {
      TAILQ_FOREACH(wp, &s->curw->window->panes, entry) {
        if ((wp->xoff + wp->sx == px && wp->yoff <= 1 + py &&
             wp->yoff + wp->sy >= py) ||
            (wp->yoff + wp->sy == py && wp->xoff <= 1 + px &&
             wp->xoff + wp->sx >= px))
          break;
      }
      if (wp != NULL) where = BORDER;
    }
    if (where == NOWHERE) {
      wp = window_get_active_at(s->curw->window, px, py);
      if (wp != NULL)
        where = PANE;
      else
        return (KEYC_UNKNOWN);
    }
    if (where == PANE)
      log_debug("mouse %u,%u on pane %%%u", x, y, wp->id);
    else if (where == BORDER)
      log_debug("mouse on pane %%%u border", wp->id);
    m->wp = wp->id;
    m->w = wp->window->id;
  } else
    m->wp = -1;
  if (type != DRAG && type != WHEEL && c->tty.mouse_drag_flag) {
    if (c->tty.mouse_drag_release != NULL) c->tty.mouse_drag_release(c, m);
    c->tty.mouse_drag_update = NULL;
    c->tty.mouse_drag_release = NULL;
    switch (c->tty.mouse_drag_flag) {
      case 1:
        if (where == PANE) key = KEYC_MOUSEDRAGEND1_PANE;
        if (where == STATUS) key = KEYC_MOUSEDRAGEND1_STATUS;
        if (where == STATUS_LEFT) key = KEYC_MOUSEDRAGEND1_STATUS_LEFT;
        if (where == STATUS_RIGHT) key = KEYC_MOUSEDRAGEND1_STATUS_RIGHT;
        if (where == STATUS_DEFAULT) key = KEYC_MOUSEDRAGEND1_STATUS_DEFAULT;
        if (where == BORDER) key = KEYC_MOUSEDRAGEND1_BORDER;
        break;
      case 2:
        if (where == PANE) key = KEYC_MOUSEDRAGEND2_PANE;
        if (where == STATUS) key = KEYC_MOUSEDRAGEND2_STATUS;
        if (where == STATUS_LEFT) key = KEYC_MOUSEDRAGEND2_STATUS_LEFT;
        if (where == STATUS_RIGHT) key = KEYC_MOUSEDRAGEND2_STATUS_RIGHT;
        if (where == STATUS_DEFAULT) key = KEYC_MOUSEDRAGEND2_STATUS_DEFAULT;
        if (where == BORDER) key = KEYC_MOUSEDRAGEND2_BORDER;
        break;
      case 3:
        if (where == PANE) key = KEYC_MOUSEDRAGEND3_PANE;
        if (where == STATUS) key = KEYC_MOUSEDRAGEND3_STATUS;
        if (where == STATUS_LEFT) key = KEYC_MOUSEDRAGEND3_STATUS_LEFT;
        if (where == STATUS_RIGHT) key = KEYC_MOUSEDRAGEND3_STATUS_RIGHT;
        if (where == STATUS_DEFAULT) key = KEYC_MOUSEDRAGEND3_STATUS_DEFAULT;
        if (where == BORDER) key = KEYC_MOUSEDRAGEND3_BORDER;
        break;
      default:
        key = KEYC_MOUSE;
        break;
    }
    c->tty.mouse_drag_flag = 0;
    goto out;
  }
  key = KEYC_UNKNOWN;
  switch (type) {
    case NOTYPE:
      break;
    case MOVE:
      if (where == PANE) key = KEYC_MOUSEMOVE_PANE;
      if (where == STATUS) key = KEYC_MOUSEMOVE_STATUS;
      if (where == STATUS_LEFT) key = KEYC_MOUSEMOVE_STATUS_LEFT;
      if (where == STATUS_RIGHT) key = KEYC_MOUSEMOVE_STATUS_RIGHT;
      if (where == STATUS_DEFAULT) key = KEYC_MOUSEMOVE_STATUS_DEFAULT;
      if (where == BORDER) key = KEYC_MOUSEMOVE_BORDER;
      break;
    case DRAG:
      if (c->tty.mouse_drag_update != NULL)
        key = KEYC_DRAGGING;
      else {
        switch (MOUSE_BUTTONS(b)) {
          case 0:
            if (where == PANE) key = KEYC_MOUSEDRAG1_PANE;
            if (where == STATUS) key = KEYC_MOUSEDRAG1_STATUS;
            if (where == STATUS_LEFT) key = KEYC_MOUSEDRAG1_STATUS_LEFT;
            if (where == STATUS_RIGHT) key = KEYC_MOUSEDRAG1_STATUS_RIGHT;
            if (where == STATUS_DEFAULT) key = KEYC_MOUSEDRAG1_STATUS_DEFAULT;
            if (where == BORDER) key = KEYC_MOUSEDRAG1_BORDER;
            break;
          case 1:
            if (where == PANE) key = KEYC_MOUSEDRAG2_PANE;
            if (where == STATUS) key = KEYC_MOUSEDRAG2_STATUS;
            if (where == STATUS_LEFT) key = KEYC_MOUSEDRAG2_STATUS_LEFT;
            if (where == STATUS_RIGHT) key = KEYC_MOUSEDRAG2_STATUS_RIGHT;
            if (where == STATUS_DEFAULT) key = KEYC_MOUSEDRAG2_STATUS_DEFAULT;
            if (where == BORDER) key = KEYC_MOUSEDRAG2_BORDER;
            break;
          case 2:
            if (where == PANE) key = KEYC_MOUSEDRAG3_PANE;
            if (where == STATUS) key = KEYC_MOUSEDRAG3_STATUS;
            if (where == STATUS_LEFT) key = KEYC_MOUSEDRAG3_STATUS_LEFT;
            if (where == STATUS_RIGHT) key = KEYC_MOUSEDRAG3_STATUS_RIGHT;
            if (where == STATUS_DEFAULT) key = KEYC_MOUSEDRAG3_STATUS_DEFAULT;
            if (where == BORDER) key = KEYC_MOUSEDRAG3_BORDER;
            break;
        }
      }
      c->tty.mouse_drag_flag = MOUSE_BUTTONS(b) + 1;
      break;
    case WHEEL:
      if (MOUSE_BUTTONS(b) == MOUSE_WHEEL_UP) {
        if (where == PANE) key = KEYC_WHEELUP_PANE;
        if (where == STATUS) key = KEYC_WHEELUP_STATUS;
        if (where == STATUS_LEFT) key = KEYC_WHEELUP_STATUS_LEFT;
        if (where == STATUS_RIGHT) key = KEYC_WHEELUP_STATUS_RIGHT;
        if (where == STATUS_DEFAULT) key = KEYC_WHEELUP_STATUS_DEFAULT;
        if (where == BORDER) key = KEYC_WHEELUP_BORDER;
      } else {
        if (where == PANE) key = KEYC_WHEELDOWN_PANE;
        if (where == STATUS) key = KEYC_WHEELDOWN_STATUS;
        if (where == STATUS_LEFT) key = KEYC_WHEELDOWN_STATUS_LEFT;
        if (where == STATUS_RIGHT) key = KEYC_WHEELDOWN_STATUS_RIGHT;
        if (where == STATUS_DEFAULT) key = KEYC_WHEELDOWN_STATUS_DEFAULT;
        if (where == BORDER) key = KEYC_WHEELDOWN_BORDER;
      }
      break;
    case UP:
      switch (MOUSE_BUTTONS(b)) {
        case 0:
          if (where == PANE) key = KEYC_MOUSEUP1_PANE;
          if (where == STATUS) key = KEYC_MOUSEUP1_STATUS;
          if (where == STATUS_LEFT) key = KEYC_MOUSEUP1_STATUS_LEFT;
          if (where == STATUS_RIGHT) key = KEYC_MOUSEUP1_STATUS_RIGHT;
          if (where == STATUS_DEFAULT) key = KEYC_MOUSEUP1_STATUS_DEFAULT;
          if (where == BORDER) key = KEYC_MOUSEUP1_BORDER;
          break;
        case 1:
          if (where == PANE) key = KEYC_MOUSEUP2_PANE;
          if (where == STATUS) key = KEYC_MOUSEUP2_STATUS;
          if (where == STATUS_LEFT) key = KEYC_MOUSEUP2_STATUS_LEFT;
          if (where == STATUS_RIGHT) key = KEYC_MOUSEUP2_STATUS_RIGHT;
          if (where == STATUS_DEFAULT) key = KEYC_MOUSEUP2_STATUS_DEFAULT;
          if (where == BORDER) key = KEYC_MOUSEUP2_BORDER;
          break;
        case 2:
          if (where == PANE) key = KEYC_MOUSEUP3_PANE;
          if (where == STATUS) key = KEYC_MOUSEUP3_STATUS;
          if (where == STATUS_LEFT) key = KEYC_MOUSEUP3_STATUS_LEFT;
          if (where == STATUS_RIGHT) key = KEYC_MOUSEUP3_STATUS_RIGHT;
          if (where == STATUS_DEFAULT) key = KEYC_MOUSEUP3_STATUS_DEFAULT;
          if (where == BORDER) key = KEYC_MOUSEUP3_BORDER;
          break;
      }
      break;
    case DOWN:
      switch (MOUSE_BUTTONS(b)) {
        case 0:
          if (where == PANE) key = KEYC_MOUSEDOWN1_PANE;
          if (where == STATUS) key = KEYC_MOUSEDOWN1_STATUS;
          if (where == STATUS_LEFT) key = KEYC_MOUSEDOWN1_STATUS_LEFT;
          if (where == STATUS_RIGHT) key = KEYC_MOUSEDOWN1_STATUS_RIGHT;
          if (where == STATUS_DEFAULT) key = KEYC_MOUSEDOWN1_STATUS_DEFAULT;
          if (where == BORDER) key = KEYC_MOUSEDOWN1_BORDER;
          break;
        case 1:
          if (where == PANE) key = KEYC_MOUSEDOWN2_PANE;
          if (where == STATUS) key = KEYC_MOUSEDOWN2_STATUS;
          if (where == STATUS_LEFT) key = KEYC_MOUSEDOWN2_STATUS_LEFT;
          if (where == STATUS_RIGHT) key = KEYC_MOUSEDOWN2_STATUS_RIGHT;
          if (where == STATUS_DEFAULT) key = KEYC_MOUSEDOWN2_STATUS_DEFAULT;
          if (where == BORDER) key = KEYC_MOUSEDOWN2_BORDER;
          break;
        case 2:
          if (where == PANE) key = KEYC_MOUSEDOWN3_PANE;
          if (where == STATUS) key = KEYC_MOUSEDOWN3_STATUS;
          if (where == STATUS_LEFT) key = KEYC_MOUSEDOWN3_STATUS_LEFT;
          if (where == STATUS_RIGHT) key = KEYC_MOUSEDOWN3_STATUS_RIGHT;
          if (where == STATUS_DEFAULT) key = KEYC_MOUSEDOWN3_STATUS_DEFAULT;
          if (where == BORDER) key = KEYC_MOUSEDOWN3_BORDER;
          break;
      }
      break;
    case SECOND:
      switch (MOUSE_BUTTONS(b)) {
        case 0:
          if (where == PANE) key = KEYC_SECONDCLICK1_PANE;
          if (where == STATUS) key = KEYC_SECONDCLICK1_STATUS;
          if (where == STATUS_LEFT) key = KEYC_SECONDCLICK1_STATUS_LEFT;
          if (where == STATUS_RIGHT) key = KEYC_SECONDCLICK1_STATUS_RIGHT;
          if (where == STATUS_DEFAULT) key = KEYC_SECONDCLICK1_STATUS_DEFAULT;
          if (where == BORDER) key = KEYC_SECONDCLICK1_BORDER;
          break;
        case 1:
          if (where == PANE) key = KEYC_SECONDCLICK2_PANE;
          if (where == STATUS) key = KEYC_SECONDCLICK2_STATUS;
          if (where == STATUS_LEFT) key = KEYC_SECONDCLICK2_STATUS_LEFT;
          if (where == STATUS_RIGHT) key = KEYC_SECONDCLICK2_STATUS_RIGHT;
          if (where == STATUS_DEFAULT) key = KEYC_SECONDCLICK2_STATUS_DEFAULT;
          if (where == BORDER) key = KEYC_SECONDCLICK2_BORDER;
          break;
        case 2:
          if (where == PANE) key = KEYC_SECONDCLICK3_PANE;
          if (where == STATUS) key = KEYC_SECONDCLICK3_STATUS;
          if (where == STATUS_LEFT) key = KEYC_SECONDCLICK3_STATUS_LEFT;
          if (where == STATUS_RIGHT) key = KEYC_SECONDCLICK3_STATUS_RIGHT;
          if (where == STATUS_DEFAULT) key = KEYC_SECONDCLICK3_STATUS_DEFAULT;
          if (where == BORDER) key = KEYC_SECONDCLICK3_BORDER;
          break;
      }
      break;
    case DOUBLE:
      switch (MOUSE_BUTTONS(b)) {
        case 0:
          if (where == PANE) key = KEYC_DOUBLECLICK1_PANE;
          if (where == STATUS) key = KEYC_DOUBLECLICK1_STATUS;
          if (where == STATUS_LEFT) key = KEYC_DOUBLECLICK1_STATUS_LEFT;
          if (where == STATUS_RIGHT) key = KEYC_DOUBLECLICK1_STATUS_RIGHT;
          if (where == STATUS_DEFAULT) key = KEYC_DOUBLECLICK1_STATUS_DEFAULT;
          if (where == BORDER) key = KEYC_DOUBLECLICK1_BORDER;
          break;
        case 1:
          if (where == PANE) key = KEYC_DOUBLECLICK2_PANE;
          if (where == STATUS) key = KEYC_DOUBLECLICK2_STATUS;
          if (where == STATUS_LEFT) key = KEYC_DOUBLECLICK2_STATUS_LEFT;
          if (where == STATUS_RIGHT) key = KEYC_DOUBLECLICK2_STATUS_RIGHT;
          if (where == STATUS_DEFAULT) key = KEYC_DOUBLECLICK2_STATUS_DEFAULT;
          if (where == BORDER) key = KEYC_DOUBLECLICK2_BORDER;
          break;
        case 2:
          if (where == PANE) key = KEYC_DOUBLECLICK3_PANE;
          if (where == STATUS) key = KEYC_DOUBLECLICK3_STATUS;
          if (where == STATUS_LEFT) key = KEYC_DOUBLECLICK3_STATUS_LEFT;
          if (where == STATUS_RIGHT) key = KEYC_DOUBLECLICK3_STATUS_RIGHT;
          if (where == STATUS_DEFAULT) key = KEYC_DOUBLECLICK3_STATUS_DEFAULT;
          if (where == BORDER) key = KEYC_DOUBLECLICK3_BORDER;
          break;
      }
      break;
    case TRIPLE:
      switch (MOUSE_BUTTONS(b)) {
        case 0:
          if (where == PANE) key = KEYC_TRIPLECLICK1_PANE;
          if (where == STATUS) key = KEYC_TRIPLECLICK1_STATUS;
          if (where == STATUS_LEFT) key = KEYC_TRIPLECLICK1_STATUS_LEFT;
          if (where == STATUS_RIGHT) key = KEYC_TRIPLECLICK1_STATUS_RIGHT;
          if (where == STATUS_DEFAULT) key = KEYC_TRIPLECLICK1_STATUS_DEFAULT;
          if (where == BORDER) key = KEYC_TRIPLECLICK1_BORDER;
          break;
        case 1:
          if (where == PANE) key = KEYC_TRIPLECLICK2_PANE;
          if (where == STATUS) key = KEYC_TRIPLECLICK2_STATUS;
          if (where == STATUS_LEFT) key = KEYC_TRIPLECLICK2_STATUS_LEFT;
          if (where == STATUS_RIGHT) key = KEYC_TRIPLECLICK2_STATUS_RIGHT;
          if (where == STATUS_DEFAULT) key = KEYC_TRIPLECLICK2_STATUS_DEFAULT;
          if (where == BORDER) key = KEYC_TRIPLECLICK2_BORDER;
          break;
        case 2:
          if (where == PANE) key = KEYC_TRIPLECLICK3_PANE;
          if (where == STATUS) key = KEYC_TRIPLECLICK3_STATUS;
          if (where == STATUS_LEFT) key = KEYC_TRIPLECLICK3_STATUS_LEFT;
          if (where == STATUS_RIGHT) key = KEYC_TRIPLECLICK3_STATUS_RIGHT;
          if (where == STATUS_DEFAULT) key = KEYC_TRIPLECLICK3_STATUS_DEFAULT;
          if (where == BORDER) key = KEYC_TRIPLECLICK3_BORDER;
          break;
      }
      break;
  }
  if (key == KEYC_UNKNOWN) return (KEYC_UNKNOWN);
out:
  if (b & MOUSE_MASK_META) key |= KEYC_META;
  if (b & MOUSE_MASK_CTRL) key |= KEYC_CTRL;
  if (b & MOUSE_MASK_SHIFT) key |= KEYC_SHIFT;
  if (log_get_level() != 0)
    log_debug("mouse key is %s", key_string_lookup_key(key, 1));
  return (key);
}
static int server_client_assume_paste(struct session *s) {
  struct timeval tv;
  int t;
  if ((t = options_get_number(s->options, "assume-paste-time")) == 0)
    return (0);
  timersub(&s->activity_time, &s->last_activity_time, &tv);
  if (tv.tv_sec == 0 && tv.tv_usec < t * 1000) {
    log_debug("session %s pasting (flag %d)", s->name,
              !!(s->flags & SESSION_PASTING));
    if (s->flags & SESSION_PASTING) return (1);
    s->flags |= SESSION_PASTING;
    return (0);
  }
  log_debug("session %s not pasting", s->name);
  s->flags &= ~SESSION_PASTING;
  return (0);
}
static void server_client_update_latest(struct client *c) {
  struct window *w;
  if (c->session == NULL) return;
  w = c->session->curw->window;
  if (w->latest == c) return;
  w->latest = c;
  if (options_get_number(w->options, "window-size") == WINDOW_SIZE_LATEST)
    recalculate_size(w);
}
static enum cmd_retval server_client_key_callback(struct cmdq_item *item,
                                                  void *data) {
  struct client *c = cmdq_get_client(item);
  struct key_event *event = data;
  key_code key = event->key;
  struct mouse_event *m = &event->m;
  struct session *s = c->session;
  struct winlink *wl;
  struct window_pane *wp;
  struct window_mode_entry *wme;
  struct timeval tv;
  struct key_table *table, *first;
  struct key_binding *bd;
  int xtimeout, flags;
  struct cmd_find_state fs;
  key_code key0;
  if (s == NULL || (c->flags & CLIENT_UNATTACHEDFLAGS)) goto out;
  wl = s->curw;
  if (gettimeofday(&c->activity_time, NULL) != 0) fatal("gettimeofday failed");
  session_update_activity(s, &c->activity_time);
  m->valid = 0;
  if (key == KEYC_MOUSE || key == KEYC_DOUBLECLICK) {
    if (c->flags & CLIENT_READONLY) goto out;
    key = server_client_check_mouse(c, event);
    if (key == KEYC_UNKNOWN) goto out;
    m->valid = 1;
    m->key = key;
    if ((key & KEYC_MASK_KEY) == KEYC_DRAGGING) {
      c->tty.mouse_drag_update(c, m);
      goto out;
    }
    event->key = key;
  }
  if (!KEYC_IS_MOUSE(key) || cmd_find_from_mouse(&fs, m, 0) != 0)
    cmd_find_from_client(&fs, c, 0);
  wp = fs.wp;
  if (KEYC_IS_MOUSE(key) && !options_get_number(s->options, "mouse"))
    goto forward_key;
  if (!KEYC_IS_MOUSE(key) && server_client_assume_paste(s)) goto forward_key;
  if (server_client_is_default_key_table(c, c->keytable) && wp != NULL &&
      (wme = TAILQ_FIRST(&wp->modes)) != NULL && wme->mode->key_table != NULL)
    table = key_bindings_get_table(wme->mode->key_table(wme), 1);
  else
    table = c->keytable;
  first = table;
table_changed:
  key0 = (key & (KEYC_MASK_KEY | KEYC_MASK_MODIFIERS));
  if ((key0 == (key_code)options_get_number(s->options, "prefix") ||
       key0 == (key_code)options_get_number(s->options, "prefix2")) &&
      strcmp(table->name, "prefix") != 0) {
    server_client_set_key_table(c, "prefix");
    server_status_client(c);
    goto out;
  }
  flags = c->flags;
try_again:
  if (wp == NULL)
    log_debug("key table %s (no pane)", table->name);
  else
    log_debug("key table %s (pane %%%u)", table->name, wp->id);
  if (c->flags & CLIENT_REPEAT) log_debug("currently repeating");
  bd = key_bindings_get(table, key0);
  if (bd != NULL) {
    if ((c->flags & CLIENT_REPEAT) && (~bd->flags & KEY_BINDING_REPEAT)) {
      log_debug("found in key table %s (not repeating)", table->name);
      server_client_set_key_table(c, NULL);
      first = table = c->keytable;
      c->flags &= ~CLIENT_REPEAT;
      server_status_client(c);
      goto table_changed;
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
    key_bindings_dispatch(bd, item, c, event, &fs);
    key_bindings_unref_table(table);
    goto out;
  }
  if (key0 != KEYC_ANY) {
    key0 = KEYC_ANY;
    goto try_again;
  }
  log_debug("not found in key table %s", table->name);
  if (!server_client_is_default_key_table(c, table) ||
      (c->flags & CLIENT_REPEAT)) {
    log_debug("trying in root table");
    server_client_set_key_table(c, NULL);
    table = c->keytable;
    if (c->flags & CLIENT_REPEAT) first = table;
    c->flags &= ~CLIENT_REPEAT;
    server_status_client(c);
    goto table_changed;
  }
  if (first != table && (~flags & CLIENT_REPEAT)) {
    server_client_set_key_table(c, NULL);
    server_status_client(c);
    goto out;
  }
forward_key:
  if (c->flags & CLIENT_READONLY) goto out;
  if (wp != NULL) window_pane_key(wp, c, s, wl, key, m);
out:
  if (s != NULL && key != KEYC_FOCUS_OUT) server_client_update_latest(c);
  free(event);
  return (CMD_RETURN_NORMAL);
}
int server_client_handle_key(struct client *c, struct key_event *event) {
  struct session *s = c->session;
  struct cmdq_item *item;
  if (s == NULL || (c->flags & CLIENT_UNATTACHEDFLAGS)) return (0);
  if (~c->flags & CLIENT_READONLY) {
    status_message_clear(c);
    if (c->overlay_key != NULL) {
      switch (c->overlay_key(c, event)) {
        case 0:
          return (0);
        case 1:
          server_client_clear_overlay(c);
          return (0);
      }
    }
    server_client_clear_overlay(c);
    if (c->prompt_string != NULL) {
      if (status_prompt_key(c, event->key) == 0) return (0);
    }
  }
  item = cmdq_get_callback(server_client_key_callback, event);
  cmdq_append(c, item);
  return (1);
}
void server_client_loop(void) {
  struct client *c;
  struct window *w;
  struct window_pane *wp;
  int focus;
  RB_FOREACH(w, windows, &windows)
  server_client_check_window_resize(w);
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
        if (focus) server_client_check_pane_focus(wp);
        server_client_check_pane_resize(wp);
        server_client_check_pane_buffer(wp);
      }
      wp->flags &= ~PANE_REDRAW;
    }
    check_window_name(w);
  }
}
static void server_client_check_window_resize(struct window *w) {
  struct winlink *wl;
  if (~w->flags & WINDOW_RESIZE) return;
  TAILQ_FOREACH(wl, &w->winlinks, wentry) {
    if (wl->session->attached != 0 && wl->session->curw == wl) break;
  }
  if (wl == NULL) return;
  log_debug("%s: resizing window @%u", __func__, w->id);
  resize_window(w, w->new_sx, w->new_sy, w->new_xpixel, w->new_ypixel);
}
static int server_client_resize_force(struct window_pane *wp) {
  struct timeval tv = {.tv_usec = 100000};
  if (wp->flags & PANE_RESIZEFORCE) {
    wp->flags &= ~PANE_RESIZEFORCE;
    return (0);
  }
  if (wp->sx != wp->osx || wp->sy != wp->osy || wp->sx <= 1 || wp->sy <= 1)
    return (0);
  log_debug("%s: %%%u forcing resize", __func__, wp->id);
  window_pane_send_resize(wp, -1);
  evtimer_add(&wp->resize_timer, &tv);
  wp->flags |= PANE_RESIZEFORCE;
  return (1);
}
static void server_client_resize_pane(struct window_pane *wp) {
  log_debug("%s: %%%u resize to %u,%u", __func__, wp->id, wp->sx, wp->sy);
  window_pane_send_resize(wp, 0);
  wp->flags &= ~PANE_RESIZE;
  wp->osx = wp->sx;
  wp->osy = wp->sy;
}
static void server_client_start_resize_timer(struct window_pane *wp) {
  struct timeval tv = {.tv_usec = 250000};
  if (!evtimer_pending(&wp->resize_timer, NULL))
    evtimer_add(&wp->resize_timer, &tv);
}
static void server_client_resize_event(__unused int fd, __unused short events,
                                       void *data) {
  struct window_pane *wp = data;
  evtimer_del(&wp->resize_timer);
  if (~wp->flags & PANE_RESIZE) return;
  log_debug("%s: %%%u timer fired (was%s resized)", __func__, wp->id,
            (wp->flags & PANE_RESIZED) ? "" : " not");
  if (wp->base.saved_grid == NULL && (wp->flags & PANE_RESIZED)) {
    log_debug("%s: %%%u deferring timer", __func__, wp->id);
    server_client_start_resize_timer(wp);
  } else if (!server_client_resize_force(wp)) {
    log_debug("%s: %%%u resizing pane", __func__, wp->id);
    server_client_resize_pane(wp);
  }
  wp->flags &= ~PANE_RESIZED;
}
static void server_client_check_pane_resize(struct window_pane *wp) {
  if (~wp->flags & PANE_RESIZE) return;
  if (!event_initialized(&wp->resize_timer))
    evtimer_set(&wp->resize_timer, server_client_resize_event, wp);
  if (!evtimer_pending(&wp->resize_timer, NULL)) {
    log_debug("%s: %%%u starting timer", __func__, wp->id);
    server_client_resize_pane(wp);
    server_client_start_resize_timer(wp);
  } else
    log_debug("%s: %%%u timer running", __func__, wp->id);
}
static void server_client_check_pane_buffer(struct window_pane *wp) {
  struct evbuffer *evb = wp->event->input;
  size_t minimum;
  struct client *c;
  struct window_pane_offset *wpo;
  int off = 1, flag;
  u_int attached_clients = 0;
  minimum = wp->offset.acknowledged;
  if (wp->pipe_fd != -1 && wp->pipe_offset.acknowledged < minimum)
    minimum = wp->pipe_offset.acknowledged;
  TAILQ_FOREACH(c, &clients, entry) {
    if (c->session == NULL) continue;
    attached_clients++;
    if (~c->flags & CLIENT_CONTROL) {
      off = 0;
      continue;
    }
    wpo = control_pane_offset(c, wp, &flag);
    if (wpo == NULL) {
      off = 0;
      continue;
    }
    if (!flag) off = 0;
    log_debug(
        "%s: %s has %zu bytes used, %zu bytes acknowledged "
        "for %%%u",
        __func__, c->name, wpo->used, wpo->acknowledged, wp->id);
    if (wpo->acknowledged < minimum) minimum = wpo->acknowledged;
  }
  if (attached_clients == 0) off = 0;
  minimum -= wp->base_offset;
  if (minimum == 0) goto out;
  log_debug("%s: %%%u has %zu minimum (of %zu) bytes acknowledged", __func__,
            wp->id, minimum, EVBUFFER_LENGTH(evb));
  evbuffer_drain(evb, minimum);
  if (wp->base_offset > SIZE_MAX - minimum) {
    log_debug("%s: %%%u base offset has wrapped", __func__, wp->id);
    wp->offset.acknowledged -= wp->base_offset;
    wp->offset.used -= wp->base_offset;
    if (wp->pipe_fd != -1) {
      wp->pipe_offset.acknowledged -= wp->base_offset;
      wp->pipe_offset.used -= wp->base_offset;
    }
    TAILQ_FOREACH(c, &clients, entry) {
      if (c->session == NULL || (~c->flags & CLIENT_CONTROL)) continue;
      wpo = control_pane_offset(c, wp, &flag);
      if (wpo != NULL && !flag) {
        wpo->acknowledged -= wp->base_offset;
        wpo->used -= wp->base_offset;
      }
    }
    wp->base_offset = minimum;
  } else
    wp->base_offset += minimum;
out:
  if (off)
    bufferevent_disable(wp->event, EV_READ);
  else
    bufferevent_enable(wp->event, EV_READ);
}
static void server_client_check_pane_focus(struct window_pane *wp) {
  struct client *c;
  int push;
  push = wp->flags & PANE_FOCUSPUSH;
  wp->flags &= ~PANE_FOCUSPUSH;
  if (wp->window->active != wp) goto not_focused;
  if (wp->screen != &wp->base) goto not_focused;
  TAILQ_FOREACH(c, &clients, entry) {
    if (c->session == NULL || !(c->flags & CLIENT_FOCUSED)) continue;
    if (c->session->attached == 0) continue;
    if (c->session->curw->window == wp->window) goto focused;
  }
not_focused:
  if (push || (wp->flags & PANE_FOCUSED)) {
    if (wp->base.mode & MODE_FOCUSON) bufferevent_write(wp->event, "\033[O", 3);
    notify_pane("pane-focus-out", wp);
  }
  wp->flags &= ~PANE_FOCUSED;
  return;
focused:
  if (push || !(wp->flags & PANE_FOCUSED)) {
    if (wp->base.mode & MODE_FOCUSON) bufferevent_write(wp->event, "\033[I", 3);
    notify_pane("pane-focus-in", wp);
    session_update_activity(c->session, NULL);
  }
  wp->flags |= PANE_FOCUSED;
}
static void server_client_reset_state(struct client *c) {
  struct tty *tty = &c->tty;
  struct window *w = c->session->curw->window;
  struct window_pane *wp = server_client_get_pane(c), *loop;
  struct screen *s = NULL;
  struct options *oo = c->session->options;
  int mode = 0, cursor, flags;
  u_int cx = 0, cy = 0, ox, oy, sx, sy;
  if (c->flags & (CLIENT_CONTROL | CLIENT_SUSPENDED)) return;
  flags = (tty->flags & TTY_BLOCK);
  tty->flags &= ~TTY_BLOCK;
  if (c->overlay_draw != NULL) {
    if (c->overlay_mode != NULL) s = c->overlay_mode(c, &cx, &cy);
  } else
    s = wp->screen;
  if (s != NULL) mode = s->mode;
  if (c->prompt_string != NULL || c->message_string != NULL)
    mode &= ~MODE_CURSOR;
  log_debug("%s: client %s mode %x", __func__, c->name, mode);
  tty_region_off(tty);
  tty_margin_off(tty);
  if (c->overlay_draw == NULL) {
    cursor = 0;
    tty_window_offset(tty, &ox, &oy, &sx, &sy);
    if (wp->xoff + s->cx >= ox && wp->xoff + s->cx <= ox + sx &&
        wp->yoff + s->cy >= oy && wp->yoff + s->cy <= oy + sy) {
      cursor = 1;
      cx = wp->xoff + s->cx - ox;
      cy = wp->yoff + s->cy - oy;
      if (status_at_line(c) == 0) cy += status_line_size(c);
    }
    if (!cursor) mode &= ~MODE_CURSOR;
  }
  log_debug("%s: cursor to %u,%u", __func__, cx, cy);
  tty_cursor(tty, cx, cy);
  if (options_get_number(oo, "mouse")) {
    mode &= ~ALL_MOUSE_MODES;
    if (c->overlay_draw == NULL) {
      TAILQ_FOREACH(loop, &w->panes, entry) {
        if (loop->screen->mode & MODE_MOUSE_ALL) mode |= MODE_MOUSE_ALL;
      }
    }
    if (~mode & MODE_MOUSE_ALL) mode |= MODE_MOUSE_BUTTON;
  }
  if (c->overlay_draw == NULL && c->prompt_string != NULL)
    mode &= ~MODE_BRACKETPASTE;
  tty_update_mode(tty, mode, s);
  tty_reset(tty);
  tty_sync_end(tty);
  tty->flags |= flags;
}
static void server_client_repeat_timer(__unused int fd, __unused short events,
                                       void *data) {
  struct client *c = data;
  if (c->flags & CLIENT_REPEAT) {
    server_client_set_key_table(c, NULL);
    c->flags &= ~CLIENT_REPEAT;
    server_status_client(c);
  }
}
static void server_client_click_timer(__unused int fd, __unused short events,
                                      void *data) {
  struct client *c = data;
  struct key_event *event;
  log_debug("click timer expired");
  if (c->flags & CLIENT_TRIPLECLICK) {
    event = xmalloc(sizeof *event);
    event->key = KEYC_DOUBLECLICK;
    memcpy(&event->m, &c->click_event, sizeof event->m);
    if (!server_client_handle_key(c, event)) free(event);
  }
  c->flags &= ~(CLIENT_DOUBLECLICK | CLIENT_TRIPLECLICK);
}
static void server_client_check_exit(struct client *c) {
  struct client_file *cf;
  if (~c->flags & CLIENT_EXIT) return;
  if (c->flags & CLIENT_EXITED) return;
  RB_FOREACH(cf, client_files, &c->files) {
    if (EVBUFFER_LENGTH(cf->buffer) != 0) return;
  }
  if (c->flags & CLIENT_ATTACHED) notify_client("client-detached", c);
  proc_send(c->peer, MSG_EXIT, -1, &c->retval, sizeof c->retval);
  c->flags |= CLIENT_EXITED;
}
static void server_client_redraw_timer(__unused int fd, __unused short events,
                                       __unused void *data) {
  log_debug("redraw timer fired");
}
static void server_client_check_redraw(struct client *c) {
  struct session *s = c->session;
  struct tty *tty = &c->tty;
  struct window *w = c->session->curw->window;
  struct window_pane *wp;
  int needed, flags, mode = tty->mode, new_flags = 0;
  int redraw;
  u_int bit = 0;
  struct timeval tv = {.tv_usec = 1000};
  static struct event ev;
  size_t left;
  if (c->flags & (CLIENT_CONTROL | CLIENT_SUSPENDED)) return;
  if (c->flags & CLIENT_ALLREDRAWFLAGS) {
    log_debug("%s: redraw%s%s%s%s%s", c->name,
              (c->flags & CLIENT_REDRAWWINDOW) ? " window" : "",
              (c->flags & CLIENT_REDRAWSTATUS) ? " status" : "",
              (c->flags & CLIENT_REDRAWBORDERS) ? " borders" : "",
              (c->flags & CLIENT_REDRAWOVERLAY) ? " overlay" : "",
              (c->flags & CLIENT_REDRAWPANES) ? " panes" : "");
  }
  needed = 0;
  if (c->flags & CLIENT_ALLREDRAWFLAGS)
    needed = 1;
  else {
    TAILQ_FOREACH(wp, &w->panes, entry) {
      if (wp->flags & PANE_REDRAW) {
        needed = 1;
        break;
      }
    }
    if (needed) new_flags |= CLIENT_REDRAWPANES;
  }
  if (needed && (left = EVBUFFER_LENGTH(tty->out)) != 0) {
    log_debug("%s: redraw deferred (%zu left)", c->name, left);
    if (!evtimer_initialized(&ev))
      evtimer_set(&ev, server_client_redraw_timer, NULL);
    if (!evtimer_pending(&ev, NULL)) {
      log_debug("redraw timer started");
      evtimer_add(&ev, &tv);
    }
    if (~c->flags & CLIENT_REDRAWWINDOW) {
      TAILQ_FOREACH(wp, &w->panes, entry) {
        if (wp->flags & PANE_REDRAW) {
          log_debug("%s: pane %%%u needs redraw", c->name, wp->id);
          c->redraw_panes |= (1 << bit);
        }
        if (++bit == 64) {
          new_flags &= CLIENT_REDRAWPANES;
          new_flags |= CLIENT_REDRAWWINDOW;
          break;
        }
      }
      if (c->redraw_panes != 0) c->flags |= CLIENT_REDRAWPANES;
    }
    c->flags |= new_flags;
    return;
  } else if (needed)
    log_debug("%s: redraw needed", c->name);
  flags = tty->flags & (TTY_BLOCK | TTY_FREEZE | TTY_NOCURSOR);
  tty->flags = (tty->flags & ~(TTY_BLOCK | TTY_FREEZE)) | TTY_NOCURSOR;
  if (~c->flags & CLIENT_REDRAWWINDOW) {
    TAILQ_FOREACH(wp, &w->panes, entry) {
      redraw = 0;
      if (wp->flags & PANE_REDRAW)
        redraw = 1;
      else if (c->flags & CLIENT_REDRAWPANES)
        redraw = !!(c->redraw_panes & (1 << bit));
      bit++;
      if (!redraw) continue;
      log_debug("%s: redrawing pane %%%u", __func__, wp->id);
      screen_redraw_pane(c, wp);
    }
    c->redraw_panes = 0;
    c->flags &= ~CLIENT_REDRAWPANES;
  }
  if (c->flags & CLIENT_ALLREDRAWFLAGS) {
    if (options_get_number(s->options, "set-titles"))
      server_client_set_title(c);
    screen_redraw_screen(c);
  }
  tty->flags = (tty->flags & ~TTY_NOCURSOR) | (flags & TTY_NOCURSOR);
  tty_update_mode(tty, mode, NULL);
  tty->flags = (tty->flags & ~(TTY_BLOCK | TTY_FREEZE | TTY_NOCURSOR)) | flags;
  c->flags &= ~(CLIENT_ALLREDRAWFLAGS | CLIENT_STATUSFORCE);
  if (needed) {
    c->redraw = EVBUFFER_LENGTH(tty->out);
    log_debug("%s: redraw added %zu bytes", c->name, c->redraw);
  }
}
static void server_client_set_title(struct client *c) {
  struct session *s = c->session;
  const char *template;
  char *title;
  struct format_tree *ft;
  template = options_get_string(s->options, "set-titles-string");
  ft = format_create(c, NULL, FORMAT_NONE, 0);
  format_defaults(ft, c, NULL, NULL, NULL);
  title = format_expand_time(ft, template);
  if (c->title == NULL || strcmp(title, c->title) != 0) {
    free(c->title);
    c->title = xstrdup(title);
    tty_set_title(&c->tty, c->title);
  }
  free(title);
  format_free(ft);
}
static void server_client_dispatch(struct imsg *imsg, void *arg) {
  struct client *c = arg;
  ssize_t datalen;
  struct session *s;
  if (c->flags & CLIENT_DEAD) return;
  if (imsg == NULL) {
    server_client_lost(c);
    return;
  }
  datalen = imsg->hdr.len - IMSG_HEADER_SIZE;
  switch (imsg->hdr.type) {
    case MSG_IDENTIFY_FEATURES:
    case MSG_IDENTIFY_FLAGS:
    case MSG_IDENTIFY_TERM:
    case MSG_IDENTIFY_TTYNAME:
    case MSG_IDENTIFY_CWD:
    case MSG_IDENTIFY_STDIN:
    case MSG_IDENTIFY_STDOUT:
    case MSG_IDENTIFY_ENVIRON:
    case MSG_IDENTIFY_CLIENTPID:
    case MSG_IDENTIFY_DONE:
      server_client_dispatch_identify(c, imsg);
      break;
    case MSG_COMMAND:
      server_client_dispatch_command(c, imsg);
      break;
    case MSG_RESIZE:
      if (datalen != 0) fatalx("bad MSG_RESIZE size");
      if (c->flags & CLIENT_CONTROL) break;
      server_client_update_latest(c);
      server_client_clear_overlay(c);
      tty_resize(&c->tty);
      recalculate_sizes();
      server_redraw_client(c);
      if (c->session != NULL) notify_client("client-resized", c);
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
      if (c->fd == -1)
        break;
      s = c->session;
      if (gettimeofday(&c->activity_time, NULL) != 0)
        fatal("gettimeofday failed");
      tty_start_tty(&c->tty);
      server_redraw_client(c);
      recalculate_sizes();
      if (s != NULL) session_update_activity(s, &c->activity_time);
      break;
    case MSG_SHELL:
      if (datalen != 0) fatalx("bad MSG_SHELL size");
      server_client_dispatch_shell(c);
      break;
    case MSG_WRITE_READY:
      server_client_dispatch_write_ready(c, imsg);
      break;
    case MSG_READ:
      server_client_dispatch_read_data(c, imsg);
      break;
    case MSG_READ_DONE:
      server_client_dispatch_read_done(c, imsg);
      break;
  }
}
static enum cmd_retval server_client_command_done(struct cmdq_item *item,
                                                  __unused void *data) {
  struct client *c = cmdq_get_client(item);
  if (~c->flags & CLIENT_ATTACHED)
    c->flags |= CLIENT_EXIT;
  else if (~c->flags & CLIENT_DETACHING)
    tty_send_requests(&c->tty);
  return (CMD_RETURN_NORMAL);
}
static void server_client_dispatch_command(struct client *c,
                                           struct imsg *imsg) {
  struct msg_command data;
  char *buf;
  size_t len;
  int argc;
  char **argv, *cause;
  struct cmd_parse_result *pr;
  if (c->flags & CLIENT_EXIT) return;
  if (imsg->hdr.len - IMSG_HEADER_SIZE < sizeof data)
    fatalx("bad MSG_COMMAND size");
  memcpy(&data, imsg->data, sizeof data);
  buf = (char *)imsg->data + sizeof data;
  len = imsg->hdr.len - IMSG_HEADER_SIZE - sizeof data;
  if (len > 0 && buf[len - 1] != '\0') fatalx("bad MSG_COMMAND string");
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
  pr = cmd_parse_from_arguments(argc, argv, NULL);
  switch (pr->status) {
    case CMD_PARSE_EMPTY:
      cause = xstrdup("empty command");
      goto error;
    case CMD_PARSE_ERROR:
      cause = pr->error;
      goto error;
    case CMD_PARSE_SUCCESS:
      break;
  }
  cmd_free_argv(argc, argv);
  cmdq_append(c, cmdq_get_command(pr->cmdlist, NULL));
  cmdq_append(c, cmdq_get_callback(server_client_command_done, NULL));
  cmd_list_free(pr->cmdlist);
  return;
error:
  cmd_free_argv(argc, argv);
  cmdq_append(c, cmdq_get_error(cause));
  free(cause);
  c->flags |= CLIENT_EXIT;
}
static void server_client_dispatch_identify(struct client *c,
                                            struct imsg *imsg) {
  const char *data, *home;
  size_t datalen;
  int flags, feat;
  char *name;
  if (c->flags & CLIENT_IDENTIFIED) fatalx("out-of-order identify message");
  data = imsg->data;
  datalen = imsg->hdr.len - IMSG_HEADER_SIZE;
  switch (imsg->hdr.type) {
    case MSG_IDENTIFY_FEATURES:
      if (datalen != sizeof feat) fatalx("bad MSG_IDENTIFY_FEATURES size");
      memcpy(&feat, data, sizeof feat);
      c->term_features |= feat;
      log_debug("client %p IDENTIFY_FEATURES %s", c, tty_get_features(feat));
      break;
    case MSG_IDENTIFY_FLAGS:
      if (datalen != sizeof flags) fatalx("bad MSG_IDENTIFY_FLAGS size");
      memcpy(&flags, data, sizeof flags);
      c->flags |= flags;
      log_debug("client %p IDENTIFY_FLAGS %#x", c, flags);
      break;
    case MSG_IDENTIFY_TERM:
      if (datalen == 0 || data[datalen - 1] != '\0')
        fatalx("bad MSG_IDENTIFY_TERM string");
      if (*data == '\0')
        c->term_name = xstrdup("unknown");
      else
        c->term_name = xstrdup(data);
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
      if (datalen != 0) fatalx("bad MSG_IDENTIFY_STDIN size");
      c->fd = imsg->fd;
      log_debug("client %p IDENTIFY_STDIN %d", c, imsg->fd);
      break;
    case MSG_IDENTIFY_STDOUT:
      if (datalen != 0) fatalx("bad MSG_IDENTIFY_STDOUT size");
      c->out_fd = imsg->fd;
      log_debug("client %p IDENTIFY_STDOUT %d", c, imsg->fd);
      break;
    case MSG_IDENTIFY_ENVIRON:
      if (datalen == 0 || data[datalen - 1] != '\0')
        fatalx("bad MSG_IDENTIFY_ENVIRON string");
      if (strchr(data, '=') != NULL) environ_put(c->environ, data, 0);
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
  if (*c->ttyname != '\0')
    name = xstrdup(c->ttyname);
  else
    xasprintf(&name, "client-%ld", (long)c->pid);
  c->name = name;
  log_debug("client %p name is %s", c, c->name);
<<<<<<< HEAD
#ifdef __CYGWIN__
  c->fd = open(c->ttyname, O_RDWR | O_NOCTTY);
#endif
  if (c->flags & CLIENT_CONTROL) {
    close(c->fd);
    c->fd = -1;
|||||||
  if (c->flags & CLIENT_CONTROL) {
    close(c->fd);
    c->fd = -1;
=======
  if (c->flags & CLIENT_CONTROL)
>>>>>>> 392b381d1cec6d63c4baaa709243f760ff6c3403
    control_start(c);
    else if (c->fd != -1) {
      if (tty_init(&c->tty, c) != 0) {
        close(c->fd);
        c->fd = -1;
      } else {
        tty_resize(&c->tty);
        c->flags |= CLIENT_TERMINAL;
      }
      close(c->out_fd);
      c->out_fd = -1;
    }
    if ((~c->flags & CLIENT_EXIT) && !cfg_finished &&
        c == TAILQ_FIRST(&clients) && TAILQ_NEXT(c, entry) == NULL)
      start_cfg();
  }
  static void server_client_dispatch_shell(struct client * c) {
    const char *shell;
    shell = options_get_string(global_s_options, "default-shell");
    if (!checkshell(shell)) shell = _PATH_BSHELL;
    proc_send(c->peer, MSG_SHELL, -1, shell, strlen(shell) + 1);
    proc_kill_peer(c->peer);
  }
  static void server_client_dispatch_write_ready(struct client * c,
                                                 struct imsg * imsg) {
    struct msg_write_ready *msg = imsg->data;
    size_t msglen = imsg->hdr.len - IMSG_HEADER_SIZE;
    struct client_file find, *cf;
    if (msglen != sizeof *msg) fatalx("bad MSG_WRITE_READY size");
    find.stream = msg->stream;
    if ((cf = RB_FIND(client_files, &c->files, &find)) == NULL) return;
    if (msg->error != 0) {
      cf->error = msg->error;
      file_fire_done(cf);
    } else
      file_push(cf);
  }
  static void server_client_dispatch_read_data(struct client * c,
                                               struct imsg * imsg) {
    struct msg_read_data *msg = imsg->data;
    size_t msglen = imsg->hdr.len - IMSG_HEADER_SIZE;
    struct client_file find, *cf;
    void *bdata = msg + 1;
    size_t bsize = msglen - sizeof *msg;
    if (msglen < sizeof *msg) fatalx("bad MSG_READ_DATA size");
    find.stream = msg->stream;
    if ((cf = RB_FIND(client_files, &c->files, &find)) == NULL) return;
    log_debug("%s: file %d read %zu bytes", c->name, cf->stream, bsize);
    if (cf->error == 0) {
      if (evbuffer_add(cf->buffer, bdata, bsize) != 0) {
        cf->error = ENOMEM;
        file_fire_done(cf);
      } else
        file_fire_read(cf);
    }
  }
  static void server_client_dispatch_read_done(struct client * c,
                                               struct imsg * imsg) {
    struct msg_read_done *msg = imsg->data;
    size_t msglen = imsg->hdr.len - IMSG_HEADER_SIZE;
    struct client_file find, *cf;
    if (msglen != sizeof *msg) fatalx("bad MSG_READ_DONE size");
    find.stream = msg->stream;
    if ((cf = RB_FIND(client_files, &c->files, &find)) == NULL) return;
    log_debug("%s: file %d read done", c->name, cf->stream);
    cf->error = msg->error;
    file_fire_done(cf);
  }
  const char *server_client_get_cwd(struct client * c, struct session * s) {
    const char *home;
    if (!cfg_finished && cfg_client != NULL) return (cfg_client->cwd);
    if (c != NULL && c->session == NULL && c->cwd != NULL) return (c->cwd);
    if (s != NULL && s->cwd != NULL) return (s->cwd);
    if (c != NULL && (s = c->session) != NULL && s->cwd != NULL)
      return (s->cwd);
    if ((home = find_home()) != NULL) return (home);
    return ("/");
  }
  void server_client_set_flags(struct client * c, const char *flags) {
    char *s, *copy, *next;
    uint64_t flag;
    int not ;
    s = copy = xstrdup(flags);
    while ((next = strsep(&s, ",")) != NULL) {
      not = (*next == '!');
      if (not ) next++;
      flag = 0;
      if (c->flags & CLIENT_CONTROL) {
        if (strcmp(next, "no-output") == 0) flag = CLIENT_CONTROL_NOOUTPUT;
      }
      if (strcmp(next, "read-only") == 0)
        flag = CLIENT_READONLY;
      else if (strcmp(next, "ignore-size") == 0)
        flag = CLIENT_IGNORESIZE;
      else if (strcmp(next, "active-pane") == 0)
        flag = CLIENT_ACTIVEPANE;
      if (flag == 0) continue;
      log_debug("client %s set flag %s", c->name, next);
      if (not )
        c->flags &= ~flag;
      else
        c->flags |= flag;
      if (flag == CLIENT_CONTROL_NOOUTPUT) control_free_offsets(c);
    }
    free(copy);
  }
  const char *server_client_get_flags(struct client * c) {
    static char s[256];
    *s = '\0';
    if (c->flags & CLIENT_ATTACHED) strlcat(s, "attached,", sizeof s);
    if (c->flags & CLIENT_CONTROL) strlcat(s, "control-mode,", sizeof s);
    if (c->flags & CLIENT_IGNORESIZE) strlcat(s, "ignore-size,", sizeof s);
    if (c->flags & CLIENT_CONTROL_NOOUTPUT) strlcat(s, "no-output,", sizeof s);
    if (c->flags & CLIENT_READONLY) strlcat(s, "read-only,", sizeof s);
    if (c->flags & CLIENT_ACTIVEPANE) strlcat(s, "active-pane,", sizeof s);
    if (c->flags & CLIENT_SUSPENDED) strlcat(s, "suspended,", sizeof s);
    if (c->flags & CLIENT_UTF8) strlcat(s, "UTF-8,", sizeof s);
    if (*s != '\0') s[strlen(s) - 1] = '\0';
    return (s);
  }
  static struct client_window *server_client_get_client_window(
      struct client * c, u_int id) {
    struct client_window cw = {.window = id};
    return (RB_FIND(client_windows, &c->windows, &cw));
  }
  struct window_pane *server_client_get_pane(struct client * c) {
    struct session *s = c->session;
    struct client_window *cw;
    if (s == NULL) return (NULL);
    if (~c->flags & CLIENT_ACTIVEPANE) return (s->curw->window->active);
    cw = server_client_get_client_window(c, s->curw->window->id);
    if (cw == NULL) return (s->curw->window->active);
    return (cw->pane);
  }
  void server_client_set_pane(struct client * c, struct window_pane * wp) {
    struct session *s = c->session;
    struct client_window *cw;
    if (s == NULL) return;
    cw = server_client_get_client_window(c, s->curw->window->id);
    if (cw == NULL) {
      cw = xcalloc(1, sizeof *cw);
      cw->window = s->curw->window->id;
      RB_INSERT(client_windows, &c->windows, cw);
    }
    cw->pane = wp;
    log_debug("%s pane now %%%u", c->name, wp->id);
  }
  void server_client_remove_pane(struct window_pane * wp) {
    struct client *c;
    struct window *w = wp->window;
    struct client_window *cw;
    TAILQ_FOREACH(c, &clients, entry) {
      cw = server_client_get_client_window(c, w->id);
      if (cw != NULL && cw->pane == wp) {
        RB_REMOVE(client_windows, &c->windows, cw);
        free(cw);
      }
    }
  }
