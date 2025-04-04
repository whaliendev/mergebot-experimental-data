#include <sys/types.h>
#include <ctype.h>
#include <libgen.h>
#include <stdlib.h>
#include <string.h>
#include "tmux.h"
void window_name_callback(unused int, unused short, void *);
void
queue_window_name(struct window *w)
{
 struct timeval tv;
 tv.tv_sec = 0;
 tv.tv_usec = NAME_INTERVAL * 1000L;
 if (event_initialized(&w->name_timer))
  evtimer_del(&w->name_timer);
 evtimer_set(&w->name_timer, window_name_callback, w);
 evtimer_add(&w->name_timer, &tv);
}
void
window_name_callback(unused int fd, unused short events, void *data)
{
 struct window *w = data;
 char *name;
 if (w->active == NULL)
  return;
 if (!options_get_number(&w->options, "automatic-rename")) {
  if (event_initialized(&w->name_timer))
   event_del(&w->name_timer);
  return;
 }
 queue_window_name(w);
<<<<<<< HEAD
 if (w->active->screen != &w->active->base)
  name = NULL;
 else
  name = osdep_get_name(w->active->fd, w->active->tty);
 if (name == NULL)
  wname = default_window_name(w);
 else {
  if (w->active->cmd != NULL && *w->active->cmd == '\0' &&
      name != NULL && name[0] == '-' && name[1] != '\0')
   wname = parse_window_name(name + 1);
  else
   wname = parse_window_name(name);
  free(name);
 }
 if (w->active->fd == -1) {
  xasprintf(&name, "%s[dead]", wname);
  free(wname);
  wname = name;
 }
 if (strcmp(wname, w->name)) {
  window_set_name(w, wname);
||||||| a9ebb62d
 if (w->active->screen != &w->active->base)
  name = NULL;
 else
  name = get_proc_name(w->active->fd, w->active->tty);
 if (name == NULL)
  wname = default_window_name(w);
 else {
  if (w->active->cmd != NULL && *w->active->cmd == '\0' &&
      name != NULL && name[0] == '-' && name[1] != '\0')
   wname = parse_window_name(name + 1);
  else
   wname = parse_window_name(name);
  free(name);
 }
 if (w->active->fd == -1) {
  xasprintf(&name, "%s[dead]", wname);
  free(wname);
  wname = name;
 }
 if (strcmp(wname, w->name)) {
  window_set_name(w, wname);
=======
 name = format_window_name(w);
 if (strcmp(name, w->name) != 0) {
  window_set_name(w, name);
>>>>>>> 4901d9dd
  server_status_window(w);
 }
 free(name);
}
char *
default_window_name(struct window *w)
{
 if (w->active->cmd != NULL && *w->active->cmd != '\0')
  return (parse_window_name(w->active->cmd));
 return (parse_window_name(w->active->shell));
}
char *
format_window_name(struct window *w)
{
 struct format_tree *ft;
 char *fmt, *name;
 ft = format_create();
 format_window(ft, w);
 format_window_pane(ft, w->active);
 fmt = options_get_string(&w->options, "automatic-rename-format");
 name = format_expand(ft, fmt);
 format_free(ft);
 return (name);
}
char *
parse_window_name(const char *in)
{
 char *copy, *name, *ptr;
 name = copy = xstrdup(in);
 if (strncmp(name, "exec ", (sizeof "exec ") - 1) == 0)
  name = name + (sizeof "exec ") - 1;
 while (*name == ' ' || *name == '-')
  name++;
 if ((ptr = strchr(name, ' ')) != NULL)
  *ptr = '\0';
 if (*name != '\0') {
  ptr = name + strlen(name) - 1;
  while (ptr > name && !isalnum((u_char)*ptr))
   *ptr-- = '\0';
 }
 if (*name == '/')
  name = basename(name);
 name = xstrdup(name);
 free(copy);
 return (name);
}
