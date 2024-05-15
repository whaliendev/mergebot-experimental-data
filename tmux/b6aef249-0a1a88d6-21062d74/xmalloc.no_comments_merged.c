#include <sys/types.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "tmux.h"
char *
xstrdup(const char *s)
{
 char *ptr;
 size_t len;
 len = strlen(s) + 1;
 ptr = xmalloc(len);
 strlcpy(ptr, s, len);
 return (ptr);
}
void *
xcalloc(size_t nmemb, size_t size)
{
 void *ptr;
 if (size == 0 || nmemb == 0)
  fatalx("zero size");
 if (SIZE_MAX / nmemb < size)
  fatalx("nmemb * size > SIZE_MAX");
 if ((ptr = calloc(nmemb, size)) == NULL)
  fatal("xcalloc failed");
 return (ptr);
}
void *
xmalloc(size_t size)
{
 void *ptr;
 if (size == 0)
  fatalx("zero size");
 if ((ptr = malloc(size)) == NULL)
  fatal("xmalloc failed");
 return (ptr);
}
void *
xrealloc(void *oldptr, size_t newsize)
{
 void *newptr;
 if (newsize == 0)
  fatalx("zero size");
 if ((newptr = realloc(oldptr, newsize)) == NULL)
  fatal("xrealloc failed");
 return (newptr);
}
void *
xreallocarray(void *oldptr, size_t nmemb, size_t size)
{
 size_t newsize = nmemb * size;
 void *newptr;
 if (newsize == 0)
  fatalx("zero size");
 if (SIZE_MAX / nmemb < size)
  fatalx("nmemb * size > SIZE_MAX");
 if ((newptr = realloc(oldptr, newsize)) == NULL)
  fatal("xreallocarray failed");
 return (newptr);
}
int
xasprintf(char **ret, const char *fmt, ...)
{
 va_list ap;
 int i;
 va_start(ap, fmt);
 i = xvasprintf(ret, fmt, ap);
 va_end(ap);
 return (i);
}
int
xvasprintf(char **ret, const char *fmt, va_list ap)
{
 int i;
 i = vasprintf(ret, fmt, ap);
 if (i < 0 || *ret == NULL)
  fatal("xvasprintf failed");
 return (i);
}
int
xsnprintf(char *buf, size_t len, const char *fmt, ...)
{
 va_list ap;
 int i;
 va_start(ap, fmt);
 i = xvsnprintf(buf, len, fmt, ap);
 va_end(ap);
 return (i);
}
int
xvsnprintf(char *buf, size_t len, const char *fmt, va_list ap)
{
 int i;
 if (len > INT_MAX)
  fatalx("len > INT_MAX");
 i = vsnprintf(buf, len, fmt, ap);
 if (i < 0)
  fatal("vsnprintf failed");
 return (i);
}
