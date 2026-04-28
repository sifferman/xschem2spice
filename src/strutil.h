#ifndef XS_STRUTIL_H
#define XS_STRUTIL_H

#include <stddef.h>
#include <stdarg.h>

void *xs_xmalloc(size_t n);
void *xs_xrealloc(void *p, size_t n);
char *xs_strdup(const char *s);
char *xs_strndup(const char *s, size_t n);

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} xs_str;

void xs_str_init(xs_str *s);
void xs_str_free(xs_str *s);
void xs_str_clear(xs_str *s);
void xs_str_putc(xs_str *s, char c);
void xs_str_puts(xs_str *s, const char *t);
void xs_str_putn(xs_str *s, const char *t, size_t n);
void xs_str_printf(xs_str *s, const char *fmt, ...);

int  xs_str_endswith(const char *s, const char *suffix);
int  xs_str_startswith(const char *s, const char *prefix);

#endif
