#include <sys/types.h>
#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "tmux.h"
struct utf8_item {
 u_int offset;
 RB_ENTRY(utf8_item) entry;
 char data[UTF8_SIZE];
 u_char size;
};
RB_HEAD(utf8_tree, utf8_item);
static int
utf8_cmp(struct utf8_item *ui1, struct utf8_item *ui2)
{
 if (ui1->size < ui2->size)
  return (-1);
 if (ui1->size > ui2->size)
  return (1);
 return (memcmp(ui1->data, ui2->data, ui1->size));
}
RB_GENERATE_STATIC(utf8_tree, utf8_item, entry, utf8_cmp);
static struct utf8_tree utf8_tree = RB_INITIALIZER(utf8_tree);
static struct utf8_item *utf8_list;
static u_int utf8_list_size;
static u_int utf8_list_used;
union utf8_map {
 utf8_char uc;
 struct {
  u_char flags;
#define UTF8_FLAG_SIZE 0x1f
#define UTF8_FLAG_WIDTH2 0x20
  u_char data[3];
 };
} __packed;
static const union utf8_map utf8_space1 = {
 .flags = 1,
 .data = " "
};
static const union utf8_map utf8_space2 = {
 .flags = UTF8_FLAG_WIDTH2|2,
 .data = "  "
};
static struct utf8_item *
utf8_get_item(const char *data, size_t size)
{
 struct utf8_item ui;
 memcpy(ui.data, data, size);
 ui.size = size;
 return (RB_FIND(utf8_tree, &utf8_tree, &ui));
}
static int
utf8_expand_list(void)
{
 if (utf8_list_size == 0xffffff)
  return (-1);
 if (utf8_list_size == 0)
  utf8_list_size = 256;
 else if (utf8_list_size > 0x7fffff)
  utf8_list_size = 0xffffff;
 else
  utf8_list_size *= 2;
 utf8_list = xreallocarray(utf8_list, utf8_list_size, sizeof *utf8_list);
 return (0);
}
static int
utf8_put_item(const char *data, size_t size, u_int *offset)
{
 struct utf8_item *ui;
 ui = utf8_get_item(data, size);
 if (ui != NULL) {
  *offset = ui->offset;
  log_debug("%s: have %.*s at %u", __func__, (int)size, data,
      *offset);
  return (0);
 }
 if (utf8_list_used == utf8_list_size && utf8_expand_list() != 0)
  return (-1);
 *offset = utf8_list_used++;
 ui = &utf8_list[*offset];
 ui->offset = *offset;
 memcpy(ui->data, data, size);
 ui->size = size;
 RB_INSERT(utf8_tree, &utf8_tree, ui);
 log_debug("%s: added %.*s at %u", __func__, (int)size, data, *offset);
 return (0);
}
enum utf8_state
utf8_from_data(const struct utf8_data *ud, utf8_char *uc)
{
 union utf8_map m = { .uc = 0 };
 u_int offset;
 if (ud->width != 1 && ud->width != 2)
  return (utf8_space1.uc);
 if (ud->size > UTF8_FLAG_SIZE)
  goto fail;
 if (ud->size == 1)
  return (utf8_build_one(ud->data[0], 1));
 m.flags = ud->size;
 if (ud->width == 2)
  m.flags |= UTF8_FLAG_WIDTH2;
 if (ud->size <= 3)
  memcpy(m.data, ud->data, ud->size);
 else {
  if (utf8_put_item(ud->data, ud->size, &offset) != 0)
   goto fail;
  m.data[0] = (offset & 0xff);
  m.data[1] = (offset >> 8) & 0xff;
  m.data[2] = (offset >> 16);
 }
 *uc = m.uc;
 return (UTF8_DONE);
fail:
 if (ud->width == 1)
  *uc = utf8_space1.uc;
 else
  *uc = utf8_space2.uc;
 return (UTF8_ERROR);
}
void
utf8_to_data(utf8_char uc, struct utf8_data *ud)
{
 union utf8_map m = { .uc = uc };
 struct utf8_item *ui;
 u_int offset;
 memset(ud, 0, sizeof *ud);
 ud->size = ud->have = (m.flags & UTF8_FLAG_SIZE);
 if (m.flags & UTF8_FLAG_WIDTH2)
  ud->width = 2;
 else
  ud->width = 1;
 if (ud->size <= 3) {
  memcpy(ud->data, m.data, ud->size);
  return;
 }
 offset = ((u_int)m.data[2] << 16)|((u_int)m.data[1] << 8)|m.data[0];
 if (offset >= utf8_list_used)
  memset(ud->data, ' ', ud->size);
 else {
  ui = &utf8_list[offset];
  memcpy(ud->data, ui->data, ud->size);
 }
}
u_int
utf8_build_one(char c, u_int width)
{
 union utf8_map m = { .flags = 1, .data[0] = c };
 if (width == 2)
  m.flags |= UTF8_FLAG_WIDTH2;
 return (m.uc);
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
 switch (mbtowc(&wc, ud->data, ud->size)) {
 case -1:
  log_debug("UTF-8 %.*s, mbtowc() %d", (int)ud->size, ud->data,
      errno);
  mbtowc(NULL, NULL, MB_CUR_MAX);
  return (UTF8_ERROR);
 case 0:
  return (UTF8_ERROR);
 }
 *width = wcwidth(wc);
 if (*width < 0 || *width > 0xff) {
  log_debug("UTF-8 %.*s, wcwidth() %d", (int)ud->size, ud->data,
      *width);
  return (UTF8_ERROR);
 }
 return (UTF8_DONE);
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
<<<<<<< HEAD
static int
utf8_width(wchar_t wc)
{
 int width;
#ifdef HAVE_UTF8PROC
 width = utf8proc_wcwidth(wc);
#else
 width = wcwidth(wc);
#endif
 if (width < 0 || width > 0xff) {
  log_debug("Unicode %04lx, wcwidth() %d", (long)wc, width);
#ifndef __OpenBSD__
  if (width < 0)
   return (1);
#endif
  return (-1);
 }
 return (width);
}
enum utf8_state
utf8_combine(const struct utf8_data *ud, wchar_t *wc)
{
#ifdef HAVE_UTF8PROC
 switch (utf8proc_mbtowc(wc, ud->data, ud->size)) {
#else
 switch (mbtowc(wc, ud->data, ud->size)) {
#endif
 case -1:
  log_debug("UTF-8 %.*s, mbtowc() %d", (int)ud->size, ud->data,
      errno);
  mbtowc(NULL, NULL, MB_CUR_MAX);
  return (UTF8_ERROR);
 case 0:
  return (UTF8_ERROR);
 default:
  return (UTF8_DONE);
 }
}
enum utf8_state
utf8_split(wchar_t wc, struct utf8_data *ud)
{
 char s[MB_LEN_MAX];
 int slen;
#ifdef HAVE_UTF8PROC
 slen = utf8proc_wctomb(s, wc);
#else
 slen = wctomb(s, wc);
#endif
 if (slen <= 0 || slen > (int)sizeof ud->data)
  return (UTF8_ERROR);
 memcpy(ud->data, s, slen);
 ud->size = slen;
 ud->width = utf8_width(wc);
 return (UTF8_DONE);
}
||||||| bbfb44e9
static int
utf8_width(wchar_t wc)
{
 int width;
 width = wcwidth(wc);
 if (width < 0 || width > 0xff) {
  log_debug("Unicode %04lx, wcwidth() %d", (long)wc, width);
  return (-1);
 }
 return (width);
}
enum utf8_state
utf8_combine(const struct utf8_data *ud, wchar_t *wc)
{
 switch (mbtowc(wc, ud->data, ud->size)) {
 case -1:
  log_debug("UTF-8 %.*s, mbtowc() %d", (int)ud->size, ud->data,
      errno);
  mbtowc(NULL, NULL, MB_CUR_MAX);
  return (UTF8_ERROR);
 case 0:
  return (UTF8_ERROR);
 default:
  return (UTF8_DONE);
 }
}
enum utf8_state
utf8_split(wchar_t wc, struct utf8_data *ud)
{
 char s[MB_LEN_MAX];
 int slen;
 slen = wctomb(s, wc);
 if (slen <= 0 || slen > (int)sizeof ud->data)
  return (UTF8_ERROR);
 memcpy(ud->data, s, slen);
 ud->size = slen;
 ud->width = utf8_width(wc);
 return (UTF8_DONE);
}
=======
>>>>>>> 6f03e49e
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
