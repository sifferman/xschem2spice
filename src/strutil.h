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

#ifndef XS_STRUTIL_H
#define XS_STRUTIL_H

#include <stdarg.h>
#include <stddef.h>

/* malloc/realloc that abort on out-of-memory — internal code never has to
 * check the return value. */
void *xs_xmalloc(size_t bytes);
void *xs_xrealloc(void *ptr, size_t bytes);

/* xs_strdup(NULL) returns NULL; otherwise behaves like POSIX strdup. */
char *xs_strdup(const char *s);
char *xs_strndup(const char *s, size_t n);

/* Append-only growable string buffer, used for in-place text assembly. */
typedef struct {
    char  *buffer;
    size_t length;
    size_t capacity;
} xs_string_buffer;

void xs_string_buffer_init(xs_string_buffer *b);
void xs_string_buffer_free(xs_string_buffer *b);
void xs_string_buffer_append_char(xs_string_buffer *b, char c);
void xs_string_buffer_append(xs_string_buffer *b, const char *s);
void xs_string_buffer_append_n(xs_string_buffer *b, const char *s, size_t n);
void xs_string_buffer_appendf(xs_string_buffer *b, const char *fmt, ...);

#endif
