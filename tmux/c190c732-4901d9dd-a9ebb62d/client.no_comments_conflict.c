#include <sys/types.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <errno.h>
#include <event.h>
#include <fcntl.h>
#include <pwd.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "tmux.h"
struct imsgbuf client_ibuf;
struct event client_event;
struct event client_stdin;
enum {
 CLIENT_EXIT_NONE,
 CLIENT_EXIT_DETACHED,
 CLIENT_EXIT_DETACHED_HUP,
 CLIENT_EXIT_LOST_TTY,
 CLIENT_EXIT_TERMINATED,
 CLIENT_EXIT_LOST_SERVER,
 CLIENT_EXIT_EXITED,
 CLIENT_EXIT_SERVER_EXITED,
} client_exitreason = CLIENT_EXIT_NONE;
int client_exitval;
enum msgtype client_exittype;
const char *client_exitsession;
int client_attached;
int client_get_lock(char *);
int client_connect(char *, int);
void client_send_identify(int);
int client_write_one(enum msgtype, int, const void *, size_t);
int client_write_server(enum msgtype, const void *, size_t);
void client_update_event(void);
void client_signal(int, short, void *);
void client_stdin_callback(int, short, void *);
void client_write(int, const char *, size_t);
void client_callback(int, short, void *);
int client_dispatch_attached(void);
int client_dispatch_wait(void *);
const char *client_exit_message(void);
int
client_get_lock(char *lockfile)
{
 int lockfd;
 if ((lockfd = open(lockfile, O_WRONLY|O_CREAT, 0600)) == -1)
  fatal("open failed");
 if (lockf(lockfd, F_TLOCK, 0) == -1 && errno == EAGAIN) {
  while (lockf(lockfd, F_LOCK, 0) == -1 && errno == EINTR)
                ;
  close(lockfd);
  return (-1);
 }
 return (lockfd);
}
int
client_connect(char *path, int start_server)
{
 struct sockaddr_un sa;
 size_t size;
 int fd, lockfd;
 char *lockfile;
 memset(&sa, 0, sizeof sa);
 sa.sun_family = AF_UNIX;
 size = strlcpy(sa.sun_path, path, sizeof sa.sun_path);
 if (size >= sizeof sa.sun_path) {
  errno = ENAMETOOLONG;
  return (-1);
 }
retry:
 if ((fd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1)
  fatal("socket failed");
 if (connect(fd, (struct sockaddr *) &sa, SUN_LEN(&sa)) == -1) {
  if (errno != ECONNREFUSED && errno != ENOENT)
   goto failed;
  if (!start_server)
   goto failed;
  close(fd);
  xasprintf(&lockfile, "%s.lock", path);
  if ((lockfd = client_get_lock(lockfile)) == -1)
   goto retry;
  if (unlink(path) != 0 && errno != ENOENT)
   return (-1);
  fd = server_start(lockfd, lockfile);
  free(lockfile);
  close(lockfd);
 }
 setblocking(fd, 0);
 return (fd);
failed:
 close(fd);
 return (-1);
}
const char *
client_exit_message(void)
{
 static char msg[256];
 switch (client_exitreason) {
 case CLIENT_EXIT_NONE:
  break;
 case CLIENT_EXIT_DETACHED:
  if (client_exitsession != NULL) {
   xsnprintf(msg, sizeof msg, "detached "
       "(from session %s)", client_exitsession);
   return (msg);
  }
  return ("detached");
 case CLIENT_EXIT_DETACHED_HUP:
  if (client_exitsession != NULL) {
   xsnprintf(msg, sizeof msg, "detached and SIGHUP "
       "(from session %s)", client_exitsession);
   return (msg);
  }
  return ("detached and SIGHUP");
 case CLIENT_EXIT_LOST_TTY:
  return ("lost tty");
 case CLIENT_EXIT_TERMINATED:
  return ("terminated");
 case CLIENT_EXIT_LOST_SERVER:
  return ("lost server");
 case CLIENT_EXIT_EXITED:
  return ("exited");
 case CLIENT_EXIT_SERVER_EXITED:
  return ("server exited");
 }
 return ("unknown reason");
}
int
client_main(int argc, char **argv, int flags)
{
 struct cmd *cmd;
 struct cmd_list *cmdlist;
 struct msg_command_data *data;
 int cmdflags, fd, i;
 pid_t ppid;
 enum msgtype msg;
 char *cause;
 struct termios tio, saved_tio;
 size_t size;
 cmdflags = 0;
 if (shell_cmd != NULL) {
  msg = MSG_SHELL;
  cmdflags = CMD_STARTSERVER;
 } else if (argc == 0) {
  msg = MSG_COMMAND;
  cmdflags = CMD_STARTSERVER|CMD_CANTNEST;
 } else {
  msg = MSG_COMMAND;
  cmdlist = cmd_list_parse(argc, argv, NULL, 0, &cause);
  if (cmdlist == NULL) {
   fprintf(stderr, "%s\n", cause);
   return (1);
  }
  cmdflags &= ~CMD_STARTSERVER;
  TAILQ_FOREACH(cmd, &cmdlist->list, qentry) {
   if (cmd->entry->flags & CMD_STARTSERVER)
    cmdflags |= CMD_STARTSERVER;
   if (cmd->entry->flags & CMD_CANTNEST)
    cmdflags |= CMD_CANTNEST;
  }
  cmd_list_free(cmdlist);
 }
 if (shell_cmd == NULL && environ_path != NULL &&
     (cmdflags & CMD_CANTNEST) &&
     strcmp(socket_path, environ_path) == 0) {
  fprintf(stderr, "sessions should be nested with care, "
      "unset $TMUX to force\n");
  return (1);
 }
 fd = client_connect(socket_path, cmdflags & CMD_STARTSERVER);
 if (fd == -1) {
  fprintf(stderr, "failed to connect to server\n");
  return (1);
 }
#ifdef HAVE_SETPROCTITLE
 setproctitle("client (%s)", socket_path);
#endif
 logfile("client");
 imsg_init(&client_ibuf, fd);
 event_set(&client_event, fd, EV_READ, client_callback, shell_cmd);
 setblocking(STDIN_FILENO, 0);
 event_set(&client_stdin, STDIN_FILENO, EV_READ|EV_PERSIST,
     client_stdin_callback, NULL);
 if (flags & CLIENT_CONTROLCONTROL) {
  if (tcgetattr(STDIN_FILENO, &saved_tio) != 0) {
   fprintf(stderr, "tcgetattr failed: %s\n",
       strerror(errno));
   return (1);
  }
  cfmakeraw(&tio);
  tio.c_iflag = ICRNL|IXANY;
  tio.c_oflag = OPOST|ONLCR;
#ifdef NOKERNINFO
  tio.c_lflag = NOKERNINFO;
#endif
  tio.c_cflag = CREAD|CS8|HUPCL;
  tio.c_cc[VMIN] = 1;
  tio.c_cc[VTIME] = 0;
  cfsetispeed(&tio, cfgetispeed(&saved_tio));
  cfsetospeed(&tio, cfgetospeed(&saved_tio));
  tcsetattr(STDIN_FILENO, TCSANOW, &tio);
 }
 set_signals(client_signal);
 client_send_identify(flags);
 if (msg == MSG_COMMAND) {
  size = 0;
  for (i = 0; i < argc; i++)
   size += strlen(argv[i]) + 1;
  data = xmalloc((sizeof *data) + size);
  data->argc = argc;
  if (cmd_pack_argv(argc, argv, (char*)(data + 1), size) != 0) {
   fprintf(stderr, "command too long\n");
   free(data);
   return (1);
  }
  size += sizeof *data;
  if (client_write_server(msg, data, size) != 0) {
   fprintf(stderr, "failed to send command\n");
   free(data);
   return (1);
  }
  free(data);
 } else if (msg == MSG_SHELL)
  client_write_server(msg, NULL, 0);
 client_update_event();
 event_dispatch();
 if (client_attached) {
  if (client_exitreason != CLIENT_EXIT_NONE && !login_shell)
   printf("[%s]\n", client_exit_message());
  ppid = getppid();
  if (client_exittype == MSG_DETACHKILL && ppid > 1)
   kill(ppid, SIGHUP);
 } else if (flags & CLIENT_CONTROLCONTROL) {
  if (client_exitreason != CLIENT_EXIT_NONE)
   printf("%%exit %s\n", client_exit_message());
  else
   printf("%%exit\n");
  printf("\033\\");
  tcsetattr(STDOUT_FILENO, TCSAFLUSH, &saved_tio);
 }
 setblocking(STDIN_FILENO, 1);
 return (client_exitval);
}
void
client_send_identify(int flags)
{
 const char *s;
 char **ss;
 int fd;
 client_write_one(MSG_IDENTIFY_FLAGS, -1, &flags, sizeof flags);
 if ((s = getenv("TERM")) == NULL)
  s = "";
 client_write_one(MSG_IDENTIFY_TERM, -1, s, strlen(s) + 1);
 if ((s = ttyname(STDIN_FILENO)) == NULL)
  s = "";
 client_write_one(MSG_IDENTIFY_TTYNAME, -1, s, strlen(s) + 1);
 if ((fd = open(".", O_RDONLY)) == -1)
  fd = open("/", O_RDONLY);
 client_write_one(MSG_IDENTIFY_CWD, fd, NULL, 0);
#ifdef __CYGWIN__
 snprintf(&data.ttyname, sizeof data.ttyname, "%s",
     ttyname(STDIN_FILENO));
#else
 if ((fd = dup(STDIN_FILENO)) == -1)
  fatal("dup failed");
<<<<<<< HEAD
#endif
 imsg_compose(&client_ibuf,
     MSG_IDENTIFY, PROTOCOL_VERSION, -1, fd, &data, sizeof data);
||||||| a9ebb62d
 imsg_compose(&client_ibuf,
     MSG_IDENTIFY, PROTOCOL_VERSION, -1, fd, &data, sizeof data);
=======
 client_write_one(MSG_IDENTIFY_STDIN, fd, NULL, 0);
 for (ss = environ; *ss != NULL; ss++)
  client_write_one(MSG_IDENTIFY_ENVIRON, -1, *ss, strlen(*ss) + 1);
 client_write_one(MSG_IDENTIFY_DONE, -1, NULL, 0);
>>>>>>> 4901d9dd
 client_update_event();
}
int
client_write_one(enum msgtype type, int fd, const void *buf, size_t len)
{
 int retval;
 retval = imsg_compose(&client_ibuf, type, PROTOCOL_VERSION, -1, fd,
     (void*)buf, len);
 if (retval != 1)
  return (-1);
 return (0);
}
int
client_write_server(enum msgtype type, const void *buf, size_t len)
{
 int retval;
 retval = client_write_one(type, -1, buf, len);
 if (retval == 0)
  client_update_event();
 return (retval);
}
void
client_update_event(void)
{
 short events;
 event_del(&client_event);
 events = EV_READ;
 if (client_ibuf.w.queued > 0)
  events |= EV_WRITE;
 event_set(
     &client_event, client_ibuf.fd, events, client_callback, shell_cmd);
 event_add(&client_event, NULL);
}
void
client_signal(int sig, unused short events, unused void *data)
{
 struct sigaction sigact;
 int status;
 if (!client_attached) {
  switch (sig) {
  case SIGCHLD:
   waitpid(WAIT_ANY, &status, WNOHANG);
   break;
  case SIGTERM:
   event_loopexit(NULL);
   break;
  }
 } else {
  switch (sig) {
  case SIGHUP:
   client_exitreason = CLIENT_EXIT_LOST_TTY;
   client_exitval = 1;
   client_write_server(MSG_EXITING, NULL, 0);
   break;
  case SIGTERM:
   client_exitreason = CLIENT_EXIT_TERMINATED;
   client_exitval = 1;
   client_write_server(MSG_EXITING, NULL, 0);
   break;
  case SIGWINCH:
   client_write_server(MSG_RESIZE, NULL, 0);
   break;
  case SIGCONT:
   memset(&sigact, 0, sizeof sigact);
   sigemptyset(&sigact.sa_mask);
   sigact.sa_flags = SA_RESTART;
   sigact.sa_handler = SIG_IGN;
   if (sigaction(SIGTSTP, &sigact, NULL) != 0)
    fatal("sigaction failed");
   client_write_server(MSG_WAKEUP, NULL, 0);
   break;
  }
 }
 client_update_event();
}
void
client_callback(unused int fd, short events, void *data)
{
 ssize_t n;
 int retval;
 if (events & EV_READ) {
  if ((n = imsg_read(&client_ibuf)) == -1 || n == 0)
   goto lost_server;
  if (client_attached)
   retval = client_dispatch_attached();
  else
   retval = client_dispatch_wait(data);
  if (retval != 0) {
   event_loopexit(NULL);
   return;
  }
 }
 if (events & EV_WRITE) {
  if (msgbuf_write(&client_ibuf.w) < 0)
   goto lost_server;
 }
 client_update_event();
 return;
lost_server:
 client_exitreason = CLIENT_EXIT_LOST_SERVER;
 client_exitval = 1;
 event_loopexit(NULL);
}
void
client_stdin_callback(unused int fd, unused short events, unused void *data1)
{
 struct msg_stdin_data data;
 data.size = read(STDIN_FILENO, data.data, sizeof data.data);
 if (data.size < 0 && (errno == EINTR || errno == EAGAIN))
  return;
 client_write_server(MSG_STDIN, &data, sizeof data);
 if (data.size <= 0)
  event_del(&client_stdin);
 client_update_event();
}
void
client_write(int fd, const char *data, size_t size)
{
 ssize_t used;
 while (size != 0) {
  used = write(fd, data, size);
  if (used == -1) {
   if (errno == EINTR || errno == EAGAIN)
    continue;
   break;
  }
  data += used;
  size -= used;
 }
}
int
client_dispatch_wait(void *data0)
{
 struct imsg imsg;
 char *data;
 ssize_t n, datalen;
 struct msg_stdout_data stdoutdata;
 struct msg_stderr_data stderrdata;
 int retval;
 for (;;) {
  if ((n = imsg_get(&client_ibuf, &imsg)) == -1)
   fatalx("imsg_get failed");
  if (n == 0)
   return (0);
  data = imsg.data;
  datalen = imsg.hdr.len - IMSG_HEADER_SIZE;
  log_debug("got %d from server", imsg.hdr.type);
  switch (imsg.hdr.type) {
  case MSG_EXIT:
  case MSG_SHUTDOWN:
   if (datalen != sizeof retval && datalen != 0)
    fatalx("bad MSG_EXIT size");
   if (datalen == sizeof retval) {
    memcpy(&retval, data, sizeof retval);
    client_exitval = retval;
   }
   imsg_free(&imsg);
   return (-1);
  case MSG_READY:
   if (datalen != 0)
    fatalx("bad MSG_READY size");
   event_del(&client_stdin);
   client_attached = 1;
   client_write_server(MSG_RESIZE, NULL, 0);
   break;
  case MSG_STDIN:
   if (datalen != 0)
    fatalx("bad MSG_STDIN size");
   event_add(&client_stdin, NULL);
   break;
  case MSG_STDOUT:
   if (datalen != sizeof stdoutdata)
    fatalx("bad MSG_STDOUT size");
   memcpy(&stdoutdata, data, sizeof stdoutdata);
   client_write(STDOUT_FILENO, stdoutdata.data,
       stdoutdata.size);
   break;
  case MSG_STDERR:
   if (datalen != sizeof stderrdata)
    fatalx("bad MSG_STDERR size");
   memcpy(&stderrdata, data, sizeof stderrdata);
   client_write(STDERR_FILENO, stderrdata.data,
       stderrdata.size);
   break;
  case MSG_VERSION:
   if (datalen != 0)
    fatalx("bad MSG_VERSION size");
   fprintf(stderr, "protocol version mismatch "
       "(client %u, server %u)\n", PROTOCOL_VERSION,
       imsg.hdr.peerid);
   client_exitval = 1;
   imsg_free(&imsg);
   return (-1);
  case MSG_SHELL:
   if (datalen == 0 || data[datalen - 1] != '\0')
    fatalx("bad MSG_SHELL string");
   clear_signals(0);
   shell_exec(data, data0);
  case MSG_DETACH:
  case MSG_DETACHKILL:
   client_write_server(MSG_EXITING, NULL, 0);
   break;
  case MSG_EXITED:
   imsg_free(&imsg);
   return (-1);
  }
  imsg_free(&imsg);
 }
}
int
client_dispatch_attached(void)
{
 struct imsg imsg;
 struct sigaction sigact;
 char *data;
 ssize_t n, datalen;
 for (;;) {
  if ((n = imsg_get(&client_ibuf, &imsg)) == -1)
   fatalx("imsg_get failed");
  if (n == 0)
   return (0);
  data = imsg.data;
  datalen = imsg.hdr.len - IMSG_HEADER_SIZE;
  log_debug("got %d from server", imsg.hdr.type);
  switch (imsg.hdr.type) {
  case MSG_DETACH:
  case MSG_DETACHKILL:
   if (datalen == 0 || data[datalen - 1] != '\0')
    fatalx("bad MSG_DETACH string");
   client_exitsession = xstrdup(data);
   client_exittype = imsg.hdr.type;
   if (imsg.hdr.type == MSG_DETACHKILL)
    client_exitreason = CLIENT_EXIT_DETACHED_HUP;
   else
    client_exitreason = CLIENT_EXIT_DETACHED;
   client_write_server(MSG_EXITING, NULL, 0);
   break;
  case MSG_EXIT:
   if (datalen != 0 && datalen != sizeof (int))
    fatalx("bad MSG_EXIT size");
   client_write_server(MSG_EXITING, NULL, 0);
   client_exitreason = CLIENT_EXIT_EXITED;
   break;
  case MSG_EXITED:
   if (datalen != 0)
    fatalx("bad MSG_EXITED size");
   imsg_free(&imsg);
   return (-1);
  case MSG_SHUTDOWN:
   if (datalen != 0)
    fatalx("bad MSG_SHUTDOWN size");
   client_write_server(MSG_EXITING, NULL, 0);
   client_exitreason = CLIENT_EXIT_SERVER_EXITED;
   client_exitval = 1;
   break;
  case MSG_SUSPEND:
   if (datalen != 0)
    fatalx("bad MSG_SUSPEND size");
   memset(&sigact, 0, sizeof sigact);
   sigemptyset(&sigact.sa_mask);
   sigact.sa_flags = SA_RESTART;
   sigact.sa_handler = SIG_DFL;
   if (sigaction(SIGTSTP, &sigact, NULL) != 0)
    fatal("sigaction failed");
   kill(getpid(), SIGTSTP);
   break;
  case MSG_LOCK:
   if (datalen == 0 || data[datalen - 1] != '\0')
    fatalx("bad MSG_LOCK string");
   system(data);
   client_write_server(MSG_UNLOCK, NULL, 0);
   break;
  }
  imsg_free(&imsg);
 }
}
