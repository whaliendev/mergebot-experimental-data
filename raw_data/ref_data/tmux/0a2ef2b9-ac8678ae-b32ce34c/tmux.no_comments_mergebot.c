#include <sys/types.h>
#include <sys/stat.h>
#include <err.h>
#include <errno.h>
#include <event.h>
#include <fcntl.h>
#include <getopt.h>
#include <paths.h>
#include <locale.h>
#include <pwd.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "tmux.h"
#if defined(DEBUG) && defined(__OpenBSD__)
extern char *malloc_options;
#endif
#ifdef DEBUG
extern char *malloc_options;
#endif
struct options *global_options;
struct options *global_s_options;
struct options *global_w_options;
struct environ *global_environ;
struct timeval start_time;
const char *socket_path;
__dead void usage(void);
static char *make_label(const char *);
#ifndef HAVE___PROGNAME
char *__progname = (char *)"tmux";
#endif
__dead void usage(void) {
  fprintf(stderr,
          "usage: %s [-2CluvV] [-c shell-command] [-f file] [-L socket-name]\n"
          "            [-S socket-path] [command [flags]]\n",
          __progname);
  exit(1);
}
const char *getshell(void) {
  struct passwd *pw;
  const char *shell;
  shell = getenv("SHELL");
  if (checkshell(shell)) return (shell);
  pw = getpwuid(getuid());
  if (pw != NULL && checkshell(pw->pw_shell)) return (pw->pw_shell);
  return (_PATH_BSHELL);
}
int checkshell(const char *shell) {
  if (shell == NULL || *shell == '\0' || *shell != '/') return (0);
  if (areshell(shell)) return (0);
  if (access(shell, X_OK) != 0) return (0);
  return (1);
}
int areshell(const char *shell) {
  const char *progname, *ptr;
  if ((ptr = strrchr(shell, '/')) != NULL)
    ptr++;
  else
    ptr = shell;
  progname = __progname;
  if (*progname == '-') progname++;
  if (strcmp(ptr, progname) == 0) return (1);
  return (0);
}
static char *make_label(const char *label) {
  char *base, resolved[PATH_MAX], *path, *s;
  struct stat sb;
  u_int uid;
  int saved_errno;
  if (label == NULL) label = "default";
  uid = getuid();
  if ((s = getenv("TMUX_TMPDIR")) != NULL && *s != '\0')
    xasprintf(&base, "%s/tmux-%u", s, uid);
  else
    xasprintf(&base, "%s/tmux-%u", _PATH_TMP, uid);
  if (mkdir(base, S_IRWXU) != 0 && errno != EEXIST) goto fail;
  if (lstat(base, &sb) != 0) goto fail;
  if (!S_ISDIR(sb.st_mode)) {
    errno = ENOTDIR;
    goto fail;
  }
  if (sb.st_uid != uid || (sb.st_mode & S_IRWXO) != 0) {
    errno = EACCES;
    goto fail;
  }
  if (realpath(base, resolved) == NULL)
    strlcpy(resolved, base, sizeof resolved);
  xasprintf(&path, "%s/%s", resolved, label);
  return (path);
fail:
  saved_errno = errno;
  free(base);
  errno = saved_errno;
  return (NULL);
}
void setblocking(int fd, int state) {
  int mode;
  if ((mode = fcntl(fd, F_GETFL)) != -1) {
    if (!state)
      mode |= O_NONBLOCK;
    else
      mode &= ~O_NONBLOCK;
    fcntl(fd, F_SETFL, mode);
  }
}
const char *find_home(void) {
  struct passwd *pw;
  static const char *home;
  if (home != NULL) return (home);
  home = getenv("HOME");
  if (home == NULL || *home == '\0') {
    pw = getpwuid(getuid());
    if (pw != NULL)
      home = pw->pw_dir;
    else
      home = NULL;
  }
  return (home);
}
int main(int argc, char **argv) {
  char *path, *label, **var, tmp[PATH_MAX], *shellcmd = NULL;
  const char *s;
  int opt, flags, keys;
<<<<<<< HEAD
#if defined(DEBUG) && defined(__OpenBSD__)
  malloc_options = (char *)"AFGJPX";
#endif
  |||||||
#ifdef DEBUG
      malloc_options = (char *)"AFGJPX";
#endif
  =======
>>>>>>> ac8678aefe157d7e40c5bcedd12333eaedf0df92
      setlocale(LC_TIME, "");
  tzset();
  if (**argv == '-')
    flags = CLIENT_LOGIN;
  else
    flags = 0;
  label = path = NULL;
  while ((opt = getopt(argc, argv, "2c:Cdf:lL:qS:uUVv")) != -1) {
    switch (opt) {
      case '2':
        flags |= CLIENT_256COLOURS;
        break;
      case 'c':
        free(shellcmd);
        shellcmd = xstrdup(optarg);
        break;
      case 'C':
        if (flags & CLIENT_CONTROL)
          flags |= CLIENT_CONTROLCONTROL;
        else
          flags |= CLIENT_CONTROL;
        break;
      case 'V':
        printf("%s %s\n", __progname, VERSION);
        exit(0);
      case 'f':
        set_cfg_file(optarg);
        break;
      case 'l':
        flags |= CLIENT_LOGIN;
        break;
      case 'L':
        free(label);
        label = xstrdup(optarg);
        break;
      case 'q':
        break;
      case 'S':
        free(path);
        path = xstrdup(optarg);
        break;
      case 'u':
        flags |= CLIENT_UTF8;
        break;
      case 'v':
        log_add_level();
        break;
      default:
        usage();
    }
  }
  argc -= optind;
  argv += optind;
  if (shellcmd != NULL && argc != 0) usage();
#ifdef __OpenBSD__
  if (pledge("stdio rpath wpath cpath flock fattr unix getpw sendfd "
             "recvfd proc exec tty ps",
             NULL) != 0)
    err(1, "pledge");
#endif
  if (getenv("TMUX") != NULL)
    flags |= CLIENT_UTF8;
  else {
    s = getenv("LC_ALL");
    if (s == NULL || *s == '\0') s = getenv("LC_CTYPE");
    if (s == NULL || *s == '\0') s = getenv("LANG");
    if (s == NULL || *s == '\0') s = "";
    if (strcasestr(s, "UTF-8") != NULL || strcasestr(s, "UTF8") != NULL)
      flags |= CLIENT_UTF8;
  }
  global_environ = environ_create();
  for (var = environ; *var != NULL; var++) environ_put(global_environ, *var);
  if (getcwd(tmp, sizeof tmp) != NULL)
    environ_set(global_environ, "PWD", "%s", tmp);
  global_options = options_create(NULL);
  options_table_populate_tree(OPTIONS_TABLE_SERVER, global_options);
  global_s_options = options_create(NULL);
  options_table_populate_tree(OPTIONS_TABLE_SESSION, global_s_options);
  options_set_string(global_s_options, "default-shell", "%s", getshell());
  global_w_options = options_create(NULL);
  options_table_populate_tree(OPTIONS_TABLE_WINDOW, global_w_options);
  if ((s = getenv("VISUAL")) != NULL || (s = getenv("EDITOR")) != NULL) {
    if (strrchr(s, '/') != NULL) s = strrchr(s, '/') + 1;
    if (strstr(s, "vi") != NULL)
      keys = MODEKEY_VI;
    else
      keys = MODEKEY_EMACS;
    options_set_number(global_s_options, "status-keys", keys);
    options_set_number(global_w_options, "mode-keys", keys);
  }
  if (path == NULL && label == NULL) {
    s = getenv("TMUX");
    if (s != NULL && *s != '\0' && *s != ',') {
      path = xstrdup(s);
      path[strcspn(path, ",")] = '\0';
    }
  }
  if (path == NULL && (path = make_label(label)) == NULL) {
    fprintf(stderr, "can't create socket: %s\n", strerror(errno));
    exit(1);
  }
  socket_path = path;
  free(label);
<<<<<<< HEAD
  exit(client_main(osdep_event_init(), argc, argv, flags));
|||||||
  exit(client_main(event_init(), argc, argv, flags));
=======
  exit(client_main(event_init(), argc, argv, flags, shellcmd));
>>>>>>> ac8678aefe157d7e40c5bcedd12333eaedf0df92
}
