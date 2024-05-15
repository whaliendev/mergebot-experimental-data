#include <sys/types.h>
#include <stdlib.h>
#include <string.h>
#include "tmux.h"
static int utf8_width(wchar_t);
void
utf8_set(struct utf8_data *ud, u_char ch)
{
 u_int i;
 *ud->data = ch;
 ud->have = 1;
 ud->size = 1;
 ud->width = 1;
 for (i = ud->size; i < sizeof ud->data; i++)
  ud->data[i] = '\0';
}
void
utf8_copy(struct utf8_data *to, const struct utf8_data *from)
{
 u_int i;
 memcpy(to, from, sizeof *to);
 for (i = to->size; i < sizeof to->data; i++)
  to->data[i] = '\0';
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
 wchar_t wc;
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
 if (utf8_combine(ud, &wc) != UTF8_DONE)
  return (UTF8_ERROR);
 if ((width = utf8_width(wc)) < 0)
  return (UTF8_ERROR);
 ud->width = width;
 return (UTF8_DONE);
}
static int
utf8_width(wchar_t wc)
{
 int width;
 width = wcwidth(wc);
 if (width < 0 || width > 0xff)
  return (-1);
 return (width);
}
enum utf8_state
utf8_combine(const struct utf8_data *ud, wchar_t *wc)
{
 switch (mbtowc(wc, ud->data, ud->size)) {
 case -1:
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
int
utf8_strvis(char *dst, const char *src, size_t len, int flag)
{
 struct utf8_data ud;
 const char *start, *end;
 enum utf8_state more;
 size_t i;
 start = dst;
 end = src + len;
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
  if (src < end - 1)
   dst = vis(dst, src[0], flag, src[1]);
  else if (src < end)
   dst = vis(dst, src[0], flag, '\0');
  src++;
 }
 *dst = '\0';
 return (dst - start);
}
char *
utf8_sanitize(const char *src)
{
 char *dst;
 size_t n;
 enum utf8_state more;
 struct utf8_data ud;
 u_int i;
 dst = NULL;
 n = 0;
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
struct utf8_data *
utf8_fromcstr(const char *src)
{
 struct utf8_data *dst;
 size_t n;
 enum utf8_state more;
 dst = NULL;
 n = 0;
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
 char *dst;
 size_t n;
 dst = NULL;
 n = 0;
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
utf8_trimcstr(const char *s, u_int width)
{
 struct utf8_data *tmp, *next;
 char *out;
 u_int at;
 tmp = utf8_fromcstr(s);
 at = 0;
 for (next = tmp; next->size != 0; next++) {
  if (at + next->width > width) {
   next->size = 0;
   break;
  }
  at += next->width;
 }
 out = utf8_tocstr(tmp);
 free(tmp);
 return (out);
}
char *
utf8_rtrimcstr(const char *s, u_int width)
{
 struct utf8_data *tmp, *next, *end;
 char *out;
 u_int at;
 tmp = utf8_fromcstr(s);
 for (end = tmp; end->size != 0; end++)
               ;
 if (end == tmp) {
  free(tmp);
  return (xstrdup(""));
 }
 next = end - 1;
 at = 0;
 for (;;)
 {
  if (at + next->width > width) {
   next++;
   break;
  }
  at += next->width;
  if (next == tmp)
   break;
  next--;
 }
 out = utf8_tocstr(next);
 free(tmp);
 return (out);
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
