#include <sys/types.h>
#include <sys/wait.h>
#include <stdlib.h>
#include <string.h>
#include "tmux.h"
enum cmd_retval cmd_if_shell_exec(struct cmd *, struct cmd_q *);
void cmd_if_shell_callback(struct job *);
void cmd_if_shell_done(struct cmd_q *);
void cmd_if_shell_free(void *);
const struct cmd_entry cmd_if_shell_entry = {
 "if-shell", "if",
<<<<<<< HEAD
 "b", 2, 3,
 "[-b] shell-command command [command]",
||||||| 357da035
 "", 2, 3,
 "shell-command command [command]",
=======
 "t:", 2, 3,
 CMD_TARGET_PANE_USAGE " shell-command command [command]",
>>>>>>> 243244d2
 0,
 NULL,
 NULL,
 cmd_if_shell_exec
};
struct cmd_if_shell_data {
 char *cmd_if;
 char *cmd_else;
 struct cmd_q *cmdq;
 int bflag;
 int started;
};
enum cmd_retval
cmd_if_shell_exec(struct cmd *self, struct cmd_q *cmdq)
{
 struct args *args = self->args;
 struct cmd_if_shell_data *cdata;
 const char *shellcmd;
 struct session *s;
 struct winlink *wl;
 struct window_pane *wp;
 struct format_tree *ft;
 wl = cmd_find_pane(ctx, args_get(args, 't'), &s, &wp);
 if (wl == NULL)
  return (CMD_RETURN_ERROR);
 ft = format_create();
 format_session(ft, s);
 format_winlink(ft, s, wl);
 format_window_pane(ft, wp);
 shellcmd = format_expand(ft, args->argv[0]);
 format_free(ft);
 cdata = xmalloc(sizeof *cdata);
 cdata->cmd_if = xstrdup(args->argv[1]);
 if (args->argc == 3)
  cdata->cmd_else = xstrdup(args->argv[2]);
 else
  cdata->cmd_else = NULL;
 cdata->bflag = args_has(args, 'b');
 cdata->started = 0;
 cdata->cmdq = cmdq;
 cmdq->references++;
 job_run(shellcmd, cmd_if_shell_callback, cmd_if_shell_free, cdata);
 free(shellcmd);
 if (cdata->bflag)
  return (CMD_RETURN_NORMAL);
 return (CMD_RETURN_WAIT);
}
void
cmd_if_shell_callback(struct job *job)
{
 struct cmd_if_shell_data *cdata = job->data;
 struct cmd_q *cmdq = cdata->cmdq, *cmdq1;
 struct cmd_list *cmdlist;
 char *cause, *cmd;
 if (cmdq->dead)
  return;
 if (!WIFEXITED(job->status) || WEXITSTATUS(job->status) != 0)
  cmd = cdata->cmd_else;
 else
  cmd = cdata->cmd_if;
 if (cmd == NULL)
  return;
 if (cmd_string_parse(cmd, &cmdlist, NULL, 0, &cause) != 0) {
  if (cause != NULL) {
   cmdq_error(cmdq, "%s", cause);
   free(cause);
  }
  return;
 }
 cdata->started = 1;
 cmdq1 = cmdq_new(cmdq->client);
 cmdq1->emptyfn = cmd_if_shell_done;
 cmdq1->data = cdata;
 cmdq_run(cmdq1, cmdlist);
 cmd_list_free(cmdlist);
}
void
cmd_if_shell_done(struct cmd_q *cmdq1)
{
 struct cmd_if_shell_data *cdata = cmdq1->data;
 struct cmd_q *cmdq = cdata->cmdq;
 if (!cmdq_free(cmdq) && !cdata->bflag)
  cmdq_continue(cmdq);
 cmdq_free(cmdq1);
 free(cdata->cmd_else);
 free(cdata->cmd_if);
 free(cdata);
}
void
cmd_if_shell_free(void *data)
{
 struct cmd_if_shell_data *cdata = data;
 struct cmd_q *cmdq = cdata->cmdq;
 if (cdata->started)
  return;
 if (!cmdq_free(cmdq) && !cdata->bflag)
  cmdq_continue(cmdq);
 free(cdata->cmd_else);
 free(cdata->cmd_if);
 free(cdata);
}
