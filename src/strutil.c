/*
 * xschem2spice - a headless C tool that converts xschem .sch/.sym files into SPICE netlists
 * Copyright (C) 2026 Ethan Sifferman
 *
 * Portions of this file are derived from xschem
 * Copyright (C) 1998-2021 Stefan Frederik Schippers
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, see <https://www.gnu.org/licenses/>.
 */

#include "strutil.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void *xs_xmalloc(size_t bytes)
{
    void *p = malloc(bytes ? bytes : 1);
    if (!p) {
        fprintf(stderr, "xschem2spice: out of memory (malloc %zu)\n", bytes);
        exit(2);
    }
    return p;
}

void *xs_xrealloc(void *ptr, size_t bytes)
{
    void *p = realloc(ptr, bytes ? bytes : 1);
    if (!p) {
        fprintf(stderr, "xschem2spice: out of memory (realloc %zu)\n", bytes);
        exit(2);
    }
    return p;
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

void xs_string_buffer_init(xs_string_buffer *b)
{
    b->buffer   = NULL;
    b->length   = 0;
    b->capacity = 0;
}

void xs_string_buffer_free(xs_string_buffer *b)
{
    free(b->buffer);
    b->buffer   = NULL;
    b->length   = 0;
    b->capacity = 0;
}

static void xs_string_buffer_reserve(xs_string_buffer *b, size_t additional)
{
    if (b->length + additional + 1 <= b->capacity) return;
    size_t new_capacity = b->capacity ? b->capacity : 64;
    while (b->length + additional + 1 > new_capacity) new_capacity *= 2;
    b->buffer   = xs_xrealloc(b->buffer, new_capacity);
    b->capacity = new_capacity;
}

void xs_string_buffer_append_char(xs_string_buffer *b, char c)
{
    xs_string_buffer_reserve(b, 1);
    b->buffer[b->length++] = c;
    b->buffer[b->length]   = '\0';
}

void xs_string_buffer_append(xs_string_buffer *b, const char *s)
{
    if (s) xs_string_buffer_append_n(b, s, strlen(s));
}

void xs_string_buffer_append_n(xs_string_buffer *b, const char *s, size_t n)
{
    xs_string_buffer_reserve(b, n);
    memcpy(b->buffer + b->length, s, n);
    b->length += n;
    b->buffer[b->length] = '\0';
}

void xs_string_buffer_appendf(xs_string_buffer *b, const char *fmt, ...)
{
    char small[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(small, sizeof small, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n < sizeof small) {
        xs_string_buffer_append_n(b, small, (size_t)n);
        return;
    }
    char *big = xs_xmalloc((size_t)n + 1);
    va_start(ap, fmt);
    vsnprintf(big, (size_t)n + 1, fmt, ap);
    va_end(ap);
    xs_string_buffer_append_n(b, big, (size_t)n);
    free(big);
}
