#include <sys/types.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "tmux.h"
TAILQ_HEAD(args_values, args_value);
struct args_entry {
 u_char flag;
 struct args_values values;
 u_int count;
 RB_ENTRY(args_entry) entry;
};
struct args {
 struct args_tree tree;
 u_int count;
 struct args_value *values;
};
static struct args_entry *args_find(struct args *, u_char);
static int args_cmp(struct args_entry *, struct args_entry *);
RB_GENERATE_STATIC(args_tree, args_entry, entry, args_cmp);
static int
args_cmp(struct args_entry *a1, struct args_entry *a2)
{
 return (a1->flag - a2->flag);
}
static struct args_entry *
args_find(struct args *args, u_char flag)
{
 struct args_entry entry;
 entry.flag = flag;
 return (RB_FIND(args_tree, &args->tree, &entry));
}
static void
args_copy_value(struct args_value *to, struct args_value *from)
{
 to->type = from->type;
 switch (from->type) {
 case ARGS_NONE:
  break;
 case ARGS_COMMANDS:
  to->cmdlist = from->cmdlist;
  to->cmdlist->references++;
  break;
 case ARGS_STRING:
  to->string = xstrdup(from->string);
  break;
 }
}
static const char *
args_value_as_string(struct args_value *value)
{
 switch (value->type) {
 case ARGS_NONE:
  return ("");
 case ARGS_COMMANDS:
  if (value->cached == NULL)
   value->cached = cmd_list_print(value->cmdlist, 0);
  return (value->cached);
 case ARGS_STRING:
  return (value->string);
 }
}
struct args *
args_create(void)
{
 struct args *args;
 args = xcalloc(1, sizeof *args);
 RB_INIT(&args->tree);
 return (args);
}
struct args *
args_parse(const struct args_parse *parse, struct args_value *values,
    u_int count)
{
 struct args *args;
 u_int i;
 struct args_value *value, *new;
 u_char flag, argument;
 const char *found, *string, *s;
 if (count == 0)
  return (args_create());
 args = args_create();
 for (i = 1; i < count; ) {
  value = &values[i];
  if (value->type != ARGS_STRING)
   break;
  string = value->string;
  if (*string++ != '-' || *string == '\0')
   break;
  i++;
  if (string[0] == '-' && string[1] == '\0')
   break;
  for (;;) {
   flag = *string++;
   if (flag == '\0')
    break;
   if (!isalnum(flag)) {
    args_free(args);
    return (NULL);
   }
   found = strchr(parse->template, flag);
   if (found == NULL) {
    args_free(args);
    return (NULL);
   }
   argument = *++found;
   if (argument != ':') {
    log_debug("%s: add -%c", __func__, flag);
    args_set(args, flag, NULL);
    continue;
   }
   new = xcalloc(1, sizeof *value);
   if (*string != '\0') {
    new->type = ARGS_STRING;
    new->string = xstrdup(string);
   } else {
    if (i == count) {
     args_free(args);
     return (NULL);
    }
    args_copy_value(new, &values[i++]);
   }
   s = args_value_as_string(new);
   log_debug("%s: add -%c = %s", __func__, flag, s);
   args_set(args, flag, new);
   break;
  }
 }
 log_debug("%s: flags end at %u of %u", __func__, i, count);
 if (i != count) {
  for ( ; i < count; i++) {
   value = &values[i];
   s = args_value_as_string(value);
   log_debug("%s: %u = %s", __func__, i, s);
   args->values = xrecallocarray(args->values,
       args->count, args->count + 1, sizeof *args->values);
   args_copy_value(&args->values[args->count++], value);
  }
 }
 if ((parse->lower != -1 && args->count < (u_int)parse->lower) ||
     (parse->upper != -1 && args->count > (u_int)parse->upper)) {
  args_free(args);
  return (NULL);
 }
 return (args);
}
void
args_free_value(struct args_value *value)
{
 switch (value->type) {
 case ARGS_NONE:
  break;
 case ARGS_STRING:
  free(value->string);
  break;
 case ARGS_COMMANDS:
  cmd_list_free(value->cmdlist);
  break;
 }
 free(value->cached);
}
void
args_free(struct args *args)
{
 struct args_entry *entry;
 struct args_entry *entry1;
 struct args_value *value;
 struct args_value *value1;
 u_int i;
 for (i = 0; i < args->count; i++)
  args_free_value(&args->values[i]);
 free(args->values);
 RB_FOREACH_SAFE(entry, args_tree, &args->tree, entry1) {
  RB_REMOVE(args_tree, &args->tree, entry);
  TAILQ_FOREACH_SAFE(value, &entry->values, entry, value1) {
   TAILQ_REMOVE(&entry->values, value, entry);
   args_free_value(value);
   free(value);
  }
  free(entry);
 }
 free(args);
}
void
args_vector(struct args *args, int *argc, char ***argv)
{
 struct args_value *value;
 u_int i;
 *argc = 0;
 *argv = NULL;
 for (i = 0; i < args->count; i++) {
  value = &args->values[i];
  cmd_append_argv(argc, argv, args_value_as_string(value));
 }
}
static void printflike(3, 4)
args_print_add(char **buf, size_t *len, const char *fmt, ...)
{
 va_list ap;
 char *s;
 size_t slen;
 va_start(ap, fmt);
 slen = xvasprintf(&s, fmt, ap);
 va_end(ap);
 *len += slen;
 *buf = xrealloc(*buf, *len);
 strlcat(*buf, s, *len);
 free(s);
}
static void
args_print_add_value(char **buf, size_t *len, struct args_value *value)
{
 char *expanded = NULL;
 if (**buf != '\0')
  args_print_add(buf, len, " ");
 switch (value->type) {
 case ARGS_NONE:
  break;
 case ARGS_COMMANDS:
  expanded = cmd_list_print(value->cmdlist, 0);
  args_print_add(buf, len, "{ %s }", expanded);
  break;
 case ARGS_STRING:
  expanded = args_escape(value->string);
  args_print_add(buf, len, "%s", expanded);
  break;
 }
 free(expanded);
}
char *
args_print(struct args *args)
{
 size_t len;
 char *buf;
 u_int i, j;
 struct args_entry *entry;
 struct args_value *value;
 len = 1;
 buf = xcalloc(1, len);
 RB_FOREACH(entry, args_tree, &args->tree) {
  if (!TAILQ_EMPTY(&entry->values))
   continue;
  if (*buf == '\0')
   args_print_add(&buf, &len, "-");
  for (j = 0; j < entry->count; j++)
   args_print_add(&buf, &len, "%c", entry->flag);
 }
 RB_FOREACH(entry, args_tree, &args->tree) {
  TAILQ_FOREACH(value, &entry->values, entry) {
   if (*buf != '\0')
    args_print_add(&buf, &len, " -%c", entry->flag);
   else
    args_print_add(&buf, &len, "-%c", entry->flag);
   args_print_add_value(&buf, &len, value);
  }
 }
 for (i = 0; i < args->count; i++)
  args_print_add_value(&buf, &len, &args->values[i]);
 return (buf);
}
char *
args_escape(const char *s)
{
 static const char dquoted[] = " #';${}";
 static const char squoted[] = " \"";
 char *escaped, *result;
 int flags, quotes = 0;
 if (*s == '\0') {
  xasprintf(&result, "''");
  return (result);
 }
 if (s[strcspn(s, dquoted)] != '\0')
  quotes = '"';
 else if (s[strcspn(s, squoted)] != '\0')
  quotes = '\'';
 if (s[0] != ' ' &&
     s[1] == '\0' &&
     (quotes != 0 || s[0] == '~')) {
  xasprintf(&escaped, "\\%c", s[0]);
  return (escaped);
 }
 flags = VIS_OCTAL|VIS_CSTYLE|VIS_TAB|VIS_NL;
 if (quotes == '"')
  flags |= VIS_DQ;
 utf8_stravis(&escaped, s, flags);
 if (quotes == '\'')
  xasprintf(&result, "'%s'", escaped);
 else if (quotes == '"') {
  if (*escaped == '~')
   xasprintf(&result, "\"\\%s\"", escaped);
  else
   xasprintf(&result, "\"%s\"", escaped);
 } else {
  if (*escaped == '~')
   xasprintf(&result, "\\%s", escaped);
  else
   result = xstrdup(escaped);
 }
 free(escaped);
 return (result);
}
int
args_has(struct args *args, u_char flag)
{
 struct args_entry *entry;
 entry = args_find(args, flag);
 if (entry == NULL)
  return (0);
 return (entry->count);
}
void
args_set(struct args *args, u_char flag, struct args_value *value)
{
 struct args_entry *entry;
 entry = args_find(args, flag);
 if (entry == NULL) {
  entry = xcalloc(1, sizeof *entry);
  entry->flag = flag;
  entry->count = 1;
  TAILQ_INIT(&entry->values);
  RB_INSERT(args_tree, &args->tree, entry);
 } else
  entry->count++;
 if (value != NULL && value->type != ARGS_NONE)
  TAILQ_INSERT_TAIL(&entry->values, value, entry);
}
const char *
args_get(struct args *args, u_char flag)
{
 struct args_entry *entry;
 if ((entry = args_find(args, flag)) == NULL)
  return (NULL);
 if (TAILQ_EMPTY(&entry->values))
  return (NULL);
 return (TAILQ_LAST(&entry->values, args_values)->string);
}
u_char
args_first(struct args *args, struct args_entry **entry)
{
 *entry = RB_MIN(args_tree, &args->tree);
 if (*entry == NULL)
  return (0);
 return ((*entry)->flag);
}
u_char
args_next(struct args_entry **entry)
{
 *entry = RB_NEXT(args_tree, &args->tree, *entry);
 if (*entry == NULL)
  return (0);
 return ((*entry)->flag);
}
u_int
args_count(struct args *args)
{
 return (args->count);
}
struct args_value *
args_value(struct args *args, u_int idx)
{
 if (idx >= args->count)
  return (NULL);
 return (&args->values[idx]);
}
const char *
args_string(struct args *args, u_int idx)
{
 if (idx >= args->count)
  return (NULL);
 return (args_value_as_string(&args->values[idx]));
}
struct args_value *
args_first_value(struct args *args, u_char flag)
{
 struct args_entry *entry;
 if ((entry = args_find(args, flag)) == NULL)
  return (NULL);
 return (TAILQ_FIRST(&entry->values));
}
struct args_value *
args_next_value(struct args_value *value)
{
 return (TAILQ_NEXT(value, entry));
}
long long
args_strtonum(struct args *args, u_char flag, long long minval,
    long long maxval, char **cause)
{
 const char *errstr;
 long long ll;
 struct args_entry *entry;
 struct args_value *value;
 if ((entry = args_find(args, flag)) == NULL) {
  *cause = xstrdup("missing");
  return (0);
 }
 value = TAILQ_LAST(&entry->values, args_values);
 ll = strtonum(value->string, minval, maxval, &errstr);
 if (errstr != NULL) {
  *cause = xstrdup(errstr);
  return (0);
 }
 *cause = NULL;
 return (ll);
}
long long
args_percentage(struct args *args, u_char flag, long long minval,
    long long maxval, long long curval, char **cause)
{
 const char *value;
 struct args_entry *entry;
 if ((entry = args_find(args, flag)) == NULL) {
  *cause = xstrdup("missing");
  return (0);
 }
 value = TAILQ_LAST(&entry->values, args_values)->string;
 return (args_string_percentage(value, minval, maxval, curval, cause));
}
long long
args_string_percentage(const char *value, long long minval, long long maxval,
    long long curval, char **cause)
{
 const char *errstr;
 long long ll;
 size_t valuelen = strlen(value);
 char *copy;
 if (value[valuelen - 1] == '%') {
  copy = xstrdup(value);
  copy[valuelen - 1] = '\0';
  ll = strtonum(copy, 0, 100, &errstr);
  free(copy);
  if (errstr != NULL) {
   *cause = xstrdup(errstr);
   return (0);
  }
  ll = (curval * ll) / 100;
  if (ll < minval) {
   *cause = xstrdup("too small");
   return (0);
  }
  if (ll > maxval) {
   *cause = xstrdup("too large");
   return (0);
  }
 } else {
  ll = strtonum(value, minval, maxval, &errstr);
  if (errstr != NULL) {
   *cause = xstrdup(errstr);
   return (0);
  }
 }
 *cause = NULL;
 return (ll);
}
