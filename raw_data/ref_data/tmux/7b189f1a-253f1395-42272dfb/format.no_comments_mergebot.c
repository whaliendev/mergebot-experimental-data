#include <sys/types.h>
#include <netdb.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "tmux.h"
int format_replace(struct format_tree *, const char *, size_t, char **,
                   size_t *, size_t *);
int format_cmp(struct format_entry *fe1, struct format_entry *fe2) {
  return (strcmp(fe1->key, fe2->key));
}
const char *format_aliases[26] = {
    NULL,
    NULL,
    NULL,
    "pane_id",
    NULL,
    "window_flags",
    NULL,
    "host",
    "window_index",
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    "pane_index",
    NULL,
    NULL,
    "session_name",
    "pane_title",
    NULL,
    NULL,
    "window_name",
    NULL,
    NULL,
    NULL
};
struct format_tree *format_create(void) {
  struct format_tree *ft;
  char host[MAXHOSTNAMELEN];
  ft = xmalloc(sizeof *ft);
  RB_INIT(ft);
  if (gethostname(host, sizeof host) == 0) format_add(ft, "host", "%s", host);
  return (ft);
}
void format_free(struct format_tree *ft) {
  struct format_entry *fe, *fe_next;
  fe_next = RB_MIN(format_tree, ft);
  while (fe_next != NULL) {
    fe = fe_next;
    fe_next = RB_NEXT(format_tree, ft, fe);
    RB_REMOVE(format_tree, ft, fe);
    free(fe->value);
    free(fe->key);
    free(fe);
  }
  free(ft);
}
void format_add(struct format_tree *ft, const char *key, const char *fmt, ...) {
  struct format_entry *fe;
  va_list ap;
  fe = xmalloc(sizeof *fe);
  fe->key = xstrdup(key);
  va_start(ap, fmt);
  xvasprintf(&fe->value, fmt, ap);
  va_end(ap);
  RB_INSERT(format_tree, ft, fe);
}
const char *format_find(struct format_tree *ft, const char *key) {
  struct format_entry *fe, fe_find;
  fe_find.key = (char *)key;
  fe = RB_FIND(format_tree, ft, &fe_find);
  if (fe == NULL) return (NULL);
  return (fe->value);
}
int format_replace(struct format_tree *ft, const char *key, size_t keylen,
                   char **buf, size_t *len, size_t *off) {
  char *copy, *ptr;
  const char *value;
  size_t valuelen;
  copy = xmalloc(keylen + 1);
  memcpy(copy, key, keylen);
  copy[keylen] = '\0';
  if (*copy == '?') {
    ptr = strchr(copy, ',');
    if (ptr == NULL) goto fail;
    *ptr = '\0';
    value = format_find(ft, copy + 1);
    if (value != NULL && (value[0] != '0' || value[1] != '\0')) {
      value = ptr + 1;
      ptr = strchr(value, ',');
      if (ptr == NULL) goto fail;
      *ptr = '\0';
    } else {
      ptr = strchr(ptr + 1, ',');
      if (ptr == NULL) goto fail;
      value = ptr + 1;
    }
  } else {
    value = format_find(ft, copy);
    if (value == NULL) value = "";
  }
  valuelen = strlen(value);
  while (*len - *off < valuelen + 1) {
    *buf = xrealloc(*buf, 2, *len);
    *len *= 2;
  }
  memcpy(*buf + *off, value, valuelen);
  *off += valuelen;
  free(copy);
  return (0);
fail:
  free(copy);
  return (-1);
}
char *format_expand(struct format_tree *ft, const char *fmt) {
  char *buf, *ptr;
  const char *s;
  size_t off, len, n;
  int ch;
  len = 64;
  buf = xmalloc(len);
  off = 0;
  while (*fmt != '\0') {
    if (*fmt != '#') {
      while (len - off < 2) {
        buf = xrealloc(buf, 2, len);
        len *= 2;
      }
      buf[off++] = *fmt++;
      continue;
    }
    fmt++;
    ch = (u_char)*fmt++;
    switch (ch) {
      case '{':
        ptr = strchr(fmt, '}');
        if (ptr == NULL) break;
        n = ptr - fmt;
        if (format_replace(ft, fmt, n, &buf, &len, &off) != 0) break;
        fmt += n + 1;
        continue;
      default:
        if (ch >= 'A' && ch <= 'Z') {
          s = format_aliases[ch - 'A'];
          if (s != NULL) {
            n = strlen(s);
            if (format_replace(ft, s, n, &buf, &len, &off) != 0) break;
            continue;
          }
        }
        while (len - off < 2) {
          buf = xrealloc(buf, 2, len);
          len *= 2;
        }
        buf[off++] = ch;
        continue;
    }
    break;
  }
  buf[off] = '\0';
  return (buf);
}
void format_session(struct format_tree *ft, struct session *s) {
  struct session_group *sg;
  char *tim;
  time_t t;
  format_add(ft, "session_name", "%s", s->name);
  format_add(ft, "session_windows", "%u", winlink_count(&s->windows));
  format_add(ft, "session_width", "%u", s->sx);
  format_add(ft, "session_height", "%u", s->sy);
  sg = session_group_find(s);
  format_add(ft, "session_grouped", "%d", sg != NULL);
  if (sg != NULL)
    format_add(ft, "session_group", "%u", session_group_index(sg));
  t = s->creation_time.tv_sec;
  format_add(ft, "session_created", "%ld", (long)t);
  tim = ctime(&t);
  *strchr(tim, '\n') = '\0';
  format_add(ft, "session_created_string", "%s", tim);
  if (s->flags & SESSION_UNATTACHED)
    format_add(ft, "session_attached", "%d", 0);
  else
    format_add(ft, "session_attached", "%d", 1);
}
void format_client(struct format_tree *ft, struct client *c) {
  char *tim;
  time_t t;
  format_add(ft, "client_cwd", "%s", c->cwd);
  format_add(ft, "client_height", "%u", c->tty.sy);
  format_add(ft, "client_width", "%u", c->tty.sx);
  format_add(ft, "client_tty", "%s", c->tty.path);
  format_add(ft, "client_termname", "%s", c->tty.termname);
  t = c->creation_time.tv_sec;
  format_add(ft, "client_created", "%ld", (long)t);
  tim = ctime(&t);
  *strchr(tim, '\n') = '\0';
  format_add(ft, "client_created_string", "%s", tim);
  t = c->activity_time.tv_sec;
  format_add(ft, "client_activity", "%ld", (long)t);
  tim = ctime(&t);
  *strchr(tim, '\n') = '\0';
  format_add(ft, "client_activity_string", "%s", tim);
  if (c->tty.flags & TTY_UTF8)
    format_add(ft, "client_utf8", "%d", 1);
  else
    format_add(ft, "client_utf8", "%d", 0);
  if (c->flags & CLIENT_READONLY)
    format_add(ft, "client_readonly", "%d", 1);
  else
    format_add(ft, "client_readonly", "%d", 0);
}
void format_winlink(struct format_tree *ft, struct session *s,
                    struct winlink *wl) {
  struct window *w = wl->window;
  char *layout, *flags;
  layout = layout_dump(w);
  flags = window_printable_flags(s, wl);
  format_add(ft, "window_id", "@%u", w->id);
  format_add(ft, "window_index", "%d", wl->idx);
  format_add(ft, "window_name", "%s", w->name);
  format_add(ft, "window_width", "%u", w->sx);
  format_add(ft, "window_height", "%u", w->sy);
  format_add(ft, "window_flags", "%s", flags);
  format_add(ft, "window_layout", "%s", layout);
  format_add(ft, "window_active", "%d", wl == s->curw);
  format_add(ft, "window_panes", "%u", window_count_panes(w));
  free(flags);
  free(layout);
}
void format_window_pane(struct format_tree *ft, struct window_pane *wp) {
  struct grid *gd = wp->base.grid;
  struct grid_line *gl;
  unsigned long long size;
  u_int i;
  u_int idx;
  size = 0;
  for (i = 0; i < gd->hsize; i++) {
    gl = &gd->linedata[i];
    size += gl->cellsize * sizeof *gl->celldata;
    size += gl->utf8size * sizeof *gl->utf8data;
  }
  size += gd->hsize * sizeof *gd->linedata;
  if (window_pane_index(wp, &idx) != 0) fatalx("index not found");
  format_add(ft, "pane_width", "%u", wp->sx);
  format_add(ft, "pane_height", "%u", wp->sy);
  format_add(ft, "pane_title", "%s", wp->base.title);
  format_add(ft, "pane_index", "%u", idx);
  format_add(ft, "history_size", "%u", gd->hsize);
  format_add(ft, "history_limit", "%u", gd->hlimit);
  format_add(ft, "history_bytes", "%llu", size);
  format_add(ft, "pane_id", "%%%u", wp->id);
  format_add(ft, "pane_active", "%d", wp == wp->window->active);
  format_add(ft, "pane_dead", "%d", wp->fd == -1);
  if (wp->cmd != NULL) format_add(ft, "pane_start_command", "%s", wp->cmd);
  if (wp->cwd != NULL) format_add(ft, "pane_start_path", "%s", wp->cwd);
<<<<<<< HEAD
  format_add(ft, "pane_current_path", "%s", osdep_get_cwd(wp->fd));
|||||||
  format_add(ft, "pane_current_path", "%s", get_proc_cwd(wp->fd));
=======
  format_add(ft, "pane_current_path", "%s", osdep_get_cwd(wp->pid));
>>>>>>> 253f1395a03b7b3371799055a7e3a442a8fa7ba6
  format_add(ft, "pane_pid", "%ld", (long)wp->pid);
  format_add(ft, "pane_tty", "%s", wp->tty);
}
void format_paste_buffer(struct format_tree *ft, struct paste_buffer *pb) {
  char *pb_print = paste_print(pb, 50);
  format_add(ft, "buffer_size", "%zu", pb->size);
  format_add(ft, "buffer_sample", "%s", pb_print);
  free(pb_print);
}
