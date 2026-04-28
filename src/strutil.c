#include "strutil.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void *xs_xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) {
        fprintf(stderr, "xschem2spice: out of memory (malloc %zu)\n", n);
        exit(2);
    }
    return p;
}

void *xs_xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q) {
        fprintf(stderr, "xschem2spice: out of memory (realloc %zu)\n", n);
        exit(2);
    }
    return q;
}

char *xs_strdup(const char *s)
{
    if (!s) return NULL;
    size_t n = strlen(s);
    char *r = xs_xmalloc(n + 1);
    memcpy(r, s, n + 1);
    return r;
}

char *xs_strndup(const char *s, size_t n)
{
    char *r = xs_xmalloc(n + 1);
    memcpy(r, s, n);
    r[n] = '\0';
    return r;
}

void xs_str_init(xs_str *s)
{
    s->buf = NULL;
    s->len = 0;
    s->cap = 0;
}

void xs_str_free(xs_str *s)
{
    free(s->buf);
    s->buf = NULL;
    s->len = 0;
    s->cap = 0;
}

void xs_str_clear(xs_str *s)
{
    s->len = 0;
    if (s->buf) s->buf[0] = '\0';
}

static void xs_str_grow(xs_str *s, size_t need)
{
    if (s->len + need + 1 <= s->cap) return;
    size_t nc = s->cap ? s->cap : 64;
    while (s->len + need + 1 > nc) nc *= 2;
    s->buf = xs_xrealloc(s->buf, nc);
    s->cap = nc;
}

void xs_str_putc(xs_str *s, char c)
{
    xs_str_grow(s, 1);
    s->buf[s->len++] = c;
    s->buf[s->len] = '\0';
}

void xs_str_puts(xs_str *s, const char *t)
{
    if (!t) return;
    size_t n = strlen(t);
    xs_str_putn(s, t, n);
}

void xs_str_putn(xs_str *s, const char *t, size_t n)
{
    xs_str_grow(s, n);
    memcpy(s->buf + s->len, t, n);
    s->len += n;
    s->buf[s->len] = '\0';
}

void xs_str_printf(xs_str *s, const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n < sizeof buf) {
        xs_str_putn(s, buf, (size_t)n);
        return;
    }
    char *big = xs_xmalloc((size_t)n + 1);
    va_start(ap, fmt);
    vsnprintf(big, (size_t)n + 1, fmt, ap);
    va_end(ap);
    xs_str_putn(s, big, (size_t)n);
    free(big);
}

int xs_str_endswith(const char *s, const char *suffix)
{
    if (!s || !suffix) return 0;
    size_t ls = strlen(s), lsx = strlen(suffix);
    if (lsx > ls) return 0;
    return strcmp(s + ls - lsx, suffix) == 0;
}

int xs_str_startswith(const char *s, const char *prefix)
{
    if (!s || !prefix) return 0;
    size_t lp = strlen(prefix);
    return strncmp(s, prefix, lp) == 0;
}
