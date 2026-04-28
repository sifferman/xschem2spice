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

#ifndef XS_HASH_H
#define XS_HASH_H

#include <stddef.h>

typedef struct xs_hash_entry {
    char                 *key;
    void                 *value;
    struct xs_hash_entry *next;
} xs_hash_entry;

typedef struct {
    xs_hash_entry **buckets;
    size_t          bucket_count;
    size_t          entry_count;
} xs_hash;

xs_hash *xs_hash_new(size_t bucket_count);
void     xs_hash_free(xs_hash *h, void (*free_value)(void *));

void *xs_hash_get(xs_hash *h, const char *key);
/* Returns the previous value for `key` (or NULL). The key string is copied. */
void *xs_hash_put(xs_hash *h, const char *key, void *value);

void xs_hash_iter(xs_hash *h,
                  void (*visit)(const char *key, void *value, void *user),
                  void *user);

#endif
