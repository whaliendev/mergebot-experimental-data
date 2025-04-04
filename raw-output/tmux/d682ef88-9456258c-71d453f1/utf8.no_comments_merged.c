#include <sys/types.h>
#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "tmux.h"
struct utf8_item {
 RB_ENTRY(utf8_item) index_entry;
 u_int index;
 RB_ENTRY(utf8_item) data_entry;
 char data[UTF8_SIZE];
 u_char size;
};
static int
utf8_data_cmp(struct utf8_item *ui1, struct utf8_item *ui2)
{
 if (ui1->size < ui2->size)
  return (-1);
 if (ui1->size > ui2->size)
  return (1);
 return (memcmp(ui1->data, ui2->data, ui1->size));
}
RB_HEAD(utf8_data_tree, utf8_item);
RB_GENERATE_STATIC(utf8_data_tree, utf8_item, data_entry, utf8_data_cmp);
static struct utf8_data_tree utf8_data_tree = RB_INITIALIZER(utf8_data_tree);
static int
utf8_index_cmp(struct utf8_item *ui1, struct utf8_item *ui2)
{
 if (ui1->index < ui2->index)
  return (-1);
 if (ui1->index > ui2->index)
  return (1);
 return (0);
}
RB_HEAD(utf8_index_tree, utf8_item);
RB_GENERATE_STATIC(utf8_index_tree, utf8_item, index_entry, utf8_index_cmp);
static struct utf8_index_tree utf8_index_tree = RB_INITIALIZER(utf8_index_tree);
static u_int utf8_next_index;
#define UTF8_GET_SIZE(uc) (((uc) >> 24) & 0x1f)
#define UTF8_GET_WIDTH(uc) (((uc) >> 29) - 1)
#define UTF8_SET_SIZE(size) (((utf8_char)(size)) << 24)
#define UTF8_SET_WIDTH(width) ((((utf8_char)(width)) + 1) << 29)
static struct utf8_item *
utf8_item_by_data(const u_char *data, size_t size)
{
 struct utf8_item ui;
 memcpy(ui.data, data, size);
 ui.size = size;
 return (RB_FIND(utf8_data_tree, &utf8_data_tree, &ui));
}
static struct utf8_item *
utf8_item_by_index(u_int index)
{
 struct utf8_item ui;
 ui.index = index;
 return (RB_FIND(utf8_index_tree, &utf8_index_tree, &ui));
}
static int
utf8_put_item(const u_char *data, size_t size, u_int *index)
{
 struct utf8_item *ui;
 ui = utf8_item_by_data(data, size);
 if (ui != NULL) {
  *index = ui->index;
  log_debug("%s: found %.*s = %u", __func__, (int)size, data,
      *index);
  return (0);
 }
 if (utf8_next_index == 0xffffff + 1)
  return (-1);
 ui = xcalloc(1, sizeof *ui);
 ui->index = utf8_next_index++;
 RB_INSERT(utf8_index_tree, &utf8_index_tree, ui);
 memcpy(ui->data, data, size);
 ui->size = size;
 RB_INSERT(utf8_data_tree, &utf8_data_tree, ui);
 *index = ui->index;
 log_debug("%s: added %.*s = %u", __func__, (int)size, data, *index);
 return (0);
}
enum utf8_state
utf8_from_data(const struct utf8_data *ud, utf8_char *uc)
{
 u_int index;
 if (ud->width > 2)
  fatalx("invalid UTF-8 width: %u", ud->width);
 if (ud->size > UTF8_SIZE)
  goto fail;
 if (ud->size <= 3) {
  index = (((utf8_char)ud->data[2] << 16)|
     ((utf8_char)ud->data[1] << 8)|
     ((utf8_char)ud->data[0]));
 } else if (utf8_put_item(ud->data, ud->size, &index) != 0)
  goto fail;
 *uc = UTF8_SET_SIZE(ud->size)|UTF8_SET_WIDTH(ud->width)|index;
 log_debug("%s: (%d %d %.*s) -> %08x", __func__, ud->width, ud->size,
     (int)ud->size, ud->data, *uc);
 return (UTF8_DONE);
fail:
 if (ud->width == 0)
  *uc = UTF8_SET_SIZE(0)|UTF8_SET_WIDTH(0);
 else if (ud->width == 1)
  *uc = UTF8_SET_SIZE(1)|UTF8_SET_WIDTH(1)|0x20;
 else
  *uc = UTF8_SET_SIZE(1)|UTF8_SET_WIDTH(1)|0x2020;
 return (UTF8_ERROR);
}
void
utf8_to_data(utf8_char uc, struct utf8_data *ud)
{
 struct utf8_item *ui;
 u_int index;
 memset(ud, 0, sizeof *ud);
 ud->size = ud->have = UTF8_GET_SIZE(uc);
 ud->width = UTF8_GET_WIDTH(uc);
 if (ud->size <= 3) {
  ud->data[2] = (uc >> 16);
  ud->data[1] = ((uc >> 8) & 0xff);
  ud->data[0] = (uc & 0xff);
 } else {
  index = (uc & 0xffffff);
  if ((ui = utf8_item_by_index(index)) == NULL)
   memset(ud->data, ' ', ud->size);
  else
   memcpy(ud->data, ui->data, ud->size);
 }
 log_debug("%s: %08x -> (%d %d %.*s)", __func__, uc, ud->width, ud->size,
     (int)ud->size, ud->data);
}
u_int
utf8_build_one(u_char ch)
{
 return (UTF8_SET_SIZE(1)|UTF8_SET_WIDTH(1)|ch);
}
void
utf8_set(struct utf8_data *ud, u_char ch)
{
 static const struct utf8_data empty = { { 0 }, 1, 1, 1 };
 memcpy(ud, &empty, sizeof *ud);
 *ud->data = ch;
}
void
utf8_copy(struct utf8_data *to, const struct utf8_data *from)
{
 u_int i;
 memcpy(to, from, sizeof *to);
 for (i = to->size; i < sizeof to->data; i++)
  to->data[i] = '\0';
}
static enum utf8_state
utf8_width(struct utf8_data *ud, int *width)
{
 wchar_t wc;
#ifdef HAVE_UTF8PROC
 switch (utf8proc_mbtowc(&wc, ud->data, ud->size)) {
#else
 switch (mbtowc(&wc, ud->data, ud->size)) {
#endif
 case -1:
  log_debug("UTF-8 %.*s, mbtowc() %d", (int)ud->size, ud->data,
      errno);
  mbtowc(NULL, NULL, MB_CUR_MAX);
  return (UTF8_ERROR);
 case 0:
  return (UTF8_ERROR);
 }
<<<<<<< HEAD
 log_debug("UTF-8 %.*s is %08X", (int)ud->size, ud->data, (u_int)wc);
#ifdef HAVE_UTF8PROC
 *width = utf8proc_wcwidth(wc);
 log_debug("utf8proc_wcwidth(%08X) returned %d", (u_int)wc, *width);
#else
||||||| 71d453f1
 log_debug("UTF-8 %.*s is %08X", (int)ud->size, ud->data, (u_int)wc);
=======
 log_debug("UTF-8 %.*s is %05X", (int)ud->size, ud->data, (u_int)wc);
>>>>>>> 9456258c
 *width = wcwidth(wc);
 log_debug("wcwidth(%05X) returned %d", (u_int)wc, *width);
 if (*width < 0) {
  *width = (wc >= 0x80 && wc <= 0x9f) ? 0 : 1;
 }
#endif
 if (*width >= 0 && *width <= 0xff)
  return (UTF8_DONE);
 return (UTF8_ERROR);
}
enum utf8_state
utf8_open(struct utf8_data *ud, u_char ch)
{
 memset(ud, 0, sizeof *ud);
 if (ch >= 0xc2 && ch <= 0xdf)
  ud->size = 2;
 else if (ch >= 0xe0 && ch <= 0xef)
  ud->size = 3;
 else if (ch >= 0xf0 && ch <= 0xf4)
  ud->size = 4;
 else
  return (UTF8_ERROR);
 utf8_append(ud, ch);
 return (UTF8_MORE);
}
enum utf8_state
utf8_append(struct utf8_data *ud, u_char ch)
{
 int width;
 if (ud->have >= ud->size)
  fatalx("UTF-8 character overflow");
 if (ud->size > sizeof ud->data)
  fatalx("UTF-8 character size too large");
 if (ud->have != 0 && (ch & 0xc0) != 0x80)
  ud->width = 0xff;
 ud->data[ud->have++] = ch;
 if (ud->have != ud->size)
  return (UTF8_MORE);
 if (ud->width == 0xff)
  return (UTF8_ERROR);
 if (utf8_width(ud, &width) != UTF8_DONE)
  return (UTF8_ERROR);
 ud->width = width;
 return (UTF8_DONE);
}
int
utf8_strvis(char *dst, const char *src, size_t len, int flag)
{
 struct utf8_data ud;
 const char *start = dst, *end = src + len;
 enum utf8_state more;
 size_t i;
 while (src < end) {
  if ((more = utf8_open(&ud, *src)) == UTF8_MORE) {
   while (++src < end && more == UTF8_MORE)
    more = utf8_append(&ud, *src);
   if (more == UTF8_DONE) {
    for (i = 0; i < ud.size; i++)
     *dst++ = ud.data[i];
    continue;
   }
   src -= ud.have;
  }
  if (src[0] == '$' && src < end - 1) {
   if (isalpha((u_char)src[1]) ||
       src[1] == '_' ||
       src[1] == '{')
    *dst++ = '\\';
   *dst++ = '$';
  } else if (src < end - 1)
   dst = vis(dst, src[0], flag, src[1]);
  else if (src < end)
   dst = vis(dst, src[0], flag, '\0');
  src++;
 }
 *dst = '\0';
 return (dst - start);
}
int
utf8_stravis(char **dst, const char *src, int flag)
{
 char *buf;
 int len;
 buf = xreallocarray(NULL, 4, strlen(src) + 1);
 len = utf8_strvis(buf, src, strlen(src), flag);
 *dst = xrealloc(buf, len + 1);
 return (len);
}
int
utf8_stravisx(char **dst, const char *src, size_t srclen, int flag)
{
 char *buf;
 int len;
 buf = xreallocarray(NULL, 4, srclen + 1);
 len = utf8_strvis(buf, src, srclen, flag);
 *dst = xrealloc(buf, len + 1);
 return (len);
}
int
utf8_isvalid(const char *s)
{
 struct utf8_data ud;
 const char *end;
 enum utf8_state more;
 end = s + strlen(s);
 while (s < end) {
  if ((more = utf8_open(&ud, *s)) == UTF8_MORE) {
   while (++s < end && more == UTF8_MORE)
    more = utf8_append(&ud, *s);
   if (more == UTF8_DONE)
    continue;
   return (0);
  }
  if (*s < 0x20 || *s > 0x7e)
   return (0);
  s++;
 }
 return (1);
}
char *
utf8_sanitize(const char *src)
{
 char *dst = NULL;
 size_t n = 0;
 enum utf8_state more;
 struct utf8_data ud;
 u_int i;
 while (*src != '\0') {
  dst = xreallocarray(dst, n + 1, sizeof *dst);
  if ((more = utf8_open(&ud, *src)) == UTF8_MORE) {
   while (*++src != '\0' && more == UTF8_MORE)
    more = utf8_append(&ud, *src);
   if (more == UTF8_DONE) {
    dst = xreallocarray(dst, n + ud.width,
        sizeof *dst);
    for (i = 0; i < ud.width; i++)
     dst[n++] = '_';
    continue;
   }
   src -= ud.have;
  }
  if (*src > 0x1f && *src < 0x7f)
   dst[n++] = *src;
  else
   dst[n++] = '_';
  src++;
 }
 dst = xreallocarray(dst, n + 1, sizeof *dst);
 dst[n] = '\0';
 return (dst);
}
size_t
utf8_strlen(const struct utf8_data *s)
{
 size_t i;
 for (i = 0; s[i].size != 0; i++)
               ;
 return (i);
}
u_int
utf8_strwidth(const struct utf8_data *s, ssize_t n)
{
 ssize_t i;
 u_int width = 0;
 for (i = 0; s[i].size != 0; i++) {
  if (n != -1 && n == i)
   break;
  width += s[i].width;
 }
 return (width);
}
struct utf8_data *
utf8_fromcstr(const char *src)
{
 struct utf8_data *dst = NULL;
 size_t n = 0;
 enum utf8_state more;
 while (*src != '\0') {
  dst = xreallocarray(dst, n + 1, sizeof *dst);
  if ((more = utf8_open(&dst[n], *src)) == UTF8_MORE) {
   while (*++src != '\0' && more == UTF8_MORE)
    more = utf8_append(&dst[n], *src);
   if (more == UTF8_DONE) {
    n++;
    continue;
   }
   src -= dst[n].have;
  }
  utf8_set(&dst[n], *src);
  n++;
  src++;
 }
 dst = xreallocarray(dst, n + 1, sizeof *dst);
 dst[n].size = 0;
 return (dst);
}
char *
utf8_tocstr(struct utf8_data *src)
{
 char *dst = NULL;
 size_t n = 0;
 for(; src->size != 0; src++) {
  dst = xreallocarray(dst, n + src->size, 1);
  memcpy(dst + n, src->data, src->size);
  n += src->size;
 }
 dst = xreallocarray(dst, n + 1, 1);
 dst[n] = '\0';
 return (dst);
}
u_int
utf8_cstrwidth(const char *s)
{
 struct utf8_data tmp;
 u_int width;
 enum utf8_state more;
 width = 0;
 while (*s != '\0') {
  if ((more = utf8_open(&tmp, *s)) == UTF8_MORE) {
   while (*++s != '\0' && more == UTF8_MORE)
    more = utf8_append(&tmp, *s);
   if (more == UTF8_DONE) {
    width += tmp.width;
    continue;
   }
   s -= tmp.have;
  }
  if (*s > 0x1f && *s != 0x7f)
   width++;
  s++;
 }
 return (width);
}
char *
utf8_padcstr(const char *s, u_int width)
{
 size_t slen;
 char *out;
 u_int n, i;
 n = utf8_cstrwidth(s);
 if (n >= width)
  return (xstrdup(s));
 slen = strlen(s);
 out = xmalloc(slen + 1 + (width - n));
 memcpy(out, s, slen);
 for (i = n; i < width; i++)
  out[slen++] = ' ';
 out[slen] = '\0';
 return (out);
}
char *
utf8_rpadcstr(const char *s, u_int width)
{
 size_t slen;
 char *out;
 u_int n, i;
 n = utf8_cstrwidth(s);
 if (n >= width)
  return (xstrdup(s));
 slen = strlen(s);
 out = xmalloc(slen + 1 + (width - n));
 for (i = 0; i < width - n; i++)
  out[i] = ' ';
 memcpy(out + i, s, slen);
 out[i + slen] = '\0';
 return (out);
}
int
utf8_cstrhas(const char *s, const struct utf8_data *ud)
{
 struct utf8_data *copy, *loop;
 int found = 0;
 copy = utf8_fromcstr(s);
 for (loop = copy; loop->size != 0; loop++) {
  if (loop->size != ud->size)
   continue;
  if (memcmp(loop->data, ud->data, loop->size) == 0) {
   found = 1;
   break;
  }
 }
 free(copy);
 return (found);
}
