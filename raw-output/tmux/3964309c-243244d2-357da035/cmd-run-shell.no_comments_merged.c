#include <sys/types.h>
#include <sys/wait.h>
#include <stdlib.h>
#include <string.h>
#include "tmux.h"
enum cmd_retval cmd_run_shell_exec(struct cmd *, struct cmd_q *);
void cmd_run_shell_callback(struct job *);
void cmd_run_shell_free(void *);
void cmd_run_shell_print(struct job *, const char *);
const struct cmd_entry cmd_run_shell_entry = {
 "run-shell", "run",
 "bt:", 1, 1,
 "[-b] " CMD_TARGET_PANE_USAGE " shell-command",
 0,
 NULL,
 NULL,
 cmd_run_shell_exec
};
struct cmd_run_shell_data {
 char *cmd;
 struct cmd_q *cmdq;
 int bflag;
 u_int wp_id;
};
void
cmd_run_shell_print(struct job *job, const char *msg)
{
 struct cmd_run_shell_data *cdata = job->data;
 struct window_pane *wp;
 wp = window_pane_find_by_id(cdata->wp_id);
 if (wp == NULL) {
  cmdq_print(cdata->cmdq, "%s", msg);
  return;
 }
 if (window_pane_set_mode(wp, &window_copy_mode) == 0)
  window_copy_init_for_output(wp);
 if (wp->mode == &window_copy_mode)
  window_copy_add(wp, "%s", msg);
}
enum cmd_retval
cmd_run_shell_exec(struct cmd *self, struct cmd_q *cmdq)
{
 struct args *args = self->args;
 struct cmd_run_shell_data *cdata;
 const char *shellcmd;
 struct session *s;
 struct winlink *wl;
 struct window_pane *wp;
 struct format_tree *ft;
<<<<<<< HEAD
 if (cmd_find_pane(cmdq, args_get(args, 't'), NULL, &wp) == NULL)
||||||| 357da035
 if (cmd_find_pane(ctx, args_get(args, 't'), NULL, &wp) == NULL)
=======
 wl = cmd_find_pane(ctx, args_get(args, 't'), &s, &wp);
 if (wl == NULL)
>>>>>>> 243244d2
  return (CMD_RETURN_ERROR);
 ft = format_create();
 format_session(ft, s);
 format_winlink(ft, s, wl);
 format_window_pane(ft, wp);
 shellcmd = format_expand(ft, args->argv[0]);
 format_free(ft);
 cdata = xmalloc(sizeof *cdata);
<<<<<<< HEAD
 cdata->cmd = xstrdup(args->argv[0]);
 cdata->bflag = args_has(args, 'b');
||||||| 357da035
 cdata->cmd = xstrdup(args->argv[0]);
=======
 cdata->cmd = shellcmd;
>>>>>>> 243244d2
 cdata->wp_id = wp->id;
 cdata->cmdq = cmdq;
 cmdq->references++;
 job_run(shellcmd, cmd_run_shell_callback, cmd_run_shell_free, cdata);
 if (cdata->bflag)
  return (CMD_RETURN_NORMAL);
 return (CMD_RETURN_WAIT);
}
void
cmd_run_shell_callback(struct job *job)
{
 struct cmd_run_shell_data *cdata = job->data;
 struct cmd_q *cmdq = cdata->cmdq;
 char *cmd, *msg, *line;
 size_t size;
 int retcode;
 u_int lines;
 if (cmdq->dead)
  return;
 cmd = cdata->cmd;
 lines = 0;
 do {
  if ((line = evbuffer_readline(job->event->input)) != NULL) {
   cmd_run_shell_print(job, line);
   free(line);
   lines++;
  }
 } while (line != NULL);
 size = EVBUFFER_LENGTH(job->event->input);
 if (size != 0) {
  line = xmalloc(size + 1);
  memcpy(line, EVBUFFER_DATA(job->event->input), size);
  line[size] = '\0';
  cmd_run_shell_print(job, line);
  lines++;
  free(line);
 }
 msg = NULL;
 if (WIFEXITED(job->status)) {
  if ((retcode = WEXITSTATUS(job->status)) != 0)
   xasprintf(&msg, "'%s' returned %d", cmd, retcode);
 } else if (WIFSIGNALED(job->status)) {
  retcode = WTERMSIG(job->status);
  xasprintf(&msg, "'%s' terminated by signal %d", cmd, retcode);
 }
 if (msg != NULL) {
  if (lines == 0)
   cmdq_info(cmdq, "%s", msg);
  else
   cmd_run_shell_print(job, msg);
  free(msg);
 }
}
void
cmd_run_shell_free(void *data)
{
 struct cmd_run_shell_data *cdata = data;
 struct cmd_q *cmdq = cdata->cmdq;
 if (!cmdq_free(cmdq) && !cdata->bflag)
  cmdq_continue(cmdq);
 free(cdata->cmd);
 free(cdata);
}
