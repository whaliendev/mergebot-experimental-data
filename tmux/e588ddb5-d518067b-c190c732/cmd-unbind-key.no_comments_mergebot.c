#include <sys/types.h>
#include <stdlib.h>
#include "tmux.h"
enum cmd_retval cmd_unbind_key_exec(struct cmd *, struct cmd_q *);
enum cmd_retval cmd_unbind_key_table(struct cmd *, struct cmd_q *, int);
const struct cmd_entry cmd_unbind_key_entry = {"unbind-key",
                                               "unbind",
                                               "acnt:",
                                               0,
                                               1,
                                               "[-acn] [-t key-table] key",
                                               0,
                                               NULL,
                                               cmd_unbind_key_exec};
enum cmd_retval cmd_unbind_key_exec(struct cmd *self, struct cmd_q *cmdq) {
  struct args *args = self->args;
  struct key_binding *bd;
  int key;
  if (!args_has(args, 'a')) {
<<<<<<< HEAD
    if (args->argc != 1) {
      cmdq_error(cmdq, "missing key");
      return (CMD_RETURN_ERROR);
    }
    return (CMD_RETURN_ERROR);
|||||||
=======
    if (args->argc != 1) {
      cmdq_error(cmdq, "missing key");
      return (CMD_RETURN_ERROR);
    }
>>>>>>> d518067be6220757a9101ca27fff14d5f599c410
    key = key_string_lookup_string(args->argv[0]);
    if (key == KEYC_NONE) {
      cmdq_error(cmdq, "unknown key: %s", args->argv[0]);
      return (CMD_RETURN_ERROR);
    }
  } else {
    if (args->argc != 0) {
      cmdq_error(cmdq, "key given with -a");
      return (CMD_RETURN_ERROR);
    }
    key = KEYC_NONE;
  }
  if (args_has(args, 't')) return (cmd_unbind_key_table(self, cmdq, key));
  if (key == KEYC_NONE) {
    while (!RB_EMPTY(&key_bindings)) {
      bd = RB_ROOT(&key_bindings);
      key_bindings_remove(bd->key);
    }
    return (CMD_RETURN_NORMAL);
  }
  if (!args_has(args, 'n')) key |= KEYC_PREFIX;
  key_bindings_remove(key);
  return (CMD_RETURN_NORMAL);
}
enum cmd_retval cmd_unbind_key_table(struct cmd *self, struct cmd_q *cmdq,
                                     int key) {
  struct args *args = self->args;
  const char *tablename;
  const struct mode_key_table *mtab;
  struct mode_key_binding *mbind, mtmp;
  tablename = args_get(args, 't');
  if ((mtab = mode_key_findtable(tablename)) == NULL) {
    cmdq_error(cmdq, "unknown key table: %s", tablename);
    return (CMD_RETURN_ERROR);
  }
  if (key == KEYC_NONE) {
    while (!RB_EMPTY(mtab->tree)) {
      mbind = RB_ROOT(mtab->tree);
      RB_REMOVE(mode_key_tree, mtab->tree, mbind);
      free(mbind);
    }
    return (CMD_RETURN_NORMAL);
  }
  mtmp.key = key;
  mtmp.mode = !!args_has(args, 'c');
  if ((mbind = RB_FIND(mode_key_tree, mtab->tree, &mtmp)) != NULL) {
    RB_REMOVE(mode_key_tree, mtab->tree, mbind);
    free(mbind);
  }
  return (CMD_RETURN_NORMAL);
}
