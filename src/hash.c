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

#include "hash.h"
#include "strutil.h"

#include <stdlib.h>
#include <string.h>

static unsigned long fnv1a_hash(const char *s)
{
    unsigned long h = 1469598103934665603UL;
    while (*s) {
        h ^= (unsigned char)*s++;
        h *= 1099511628211UL;
    }
    return h;
}

xs_hash *xs_hash_new(size_t bucket_count)
{
    if (bucket_count < 16) bucket_count = 16;
    xs_hash *h    = xs_xmalloc(sizeof *h);
    h->buckets    = xs_xmalloc(bucket_count * sizeof *h->buckets);
    for (size_t i = 0; i < bucket_count; i++) h->buckets[i] = NULL;
    h->bucket_count = bucket_count;
    h->entry_count  = 0;
    return h;
}

void xs_hash_free(xs_hash *h, void (*free_value)(void *))
{
    if (!h) return;
    for (size_t i = 0; i < h->bucket_count; i++) {
        for (xs_hash_entry *e = h->buckets[i], *next; e; e = next) {
            next = e->next;
            if (free_value) free_value(e->value);
            free(e->key);
            free(e);
        }
    }
    free(h->buckets);
    free(h);
}

void *xs_hash_get(xs_hash *h, const char *key)
{
    size_t bucket = fnv1a_hash(key) % h->bucket_count;
    for (xs_hash_entry *e = h->buckets[bucket]; e; e = e->next) {
        if (strcmp(e->key, key) == 0) return e->value;
    }
    return NULL;
}

void *xs_hash_put(xs_hash *h, const char *key, void *value)
{
    size_t bucket = fnv1a_hash(key) % h->bucket_count;
    for (xs_hash_entry *e = h->buckets[bucket]; e; e = e->next) {
        if (strcmp(e->key, key) == 0) {
            void *prev = e->value;
            e->value   = value;
            return prev;
        }
    }
    xs_hash_entry *e = xs_xmalloc(sizeof *e);
    e->key   = xs_strdup(key);
    e->value = value;
    e->next  = h->buckets[bucket];
    h->buckets[bucket] = e;
    h->entry_count++;
    return NULL;
}

void xs_hash_iter(xs_hash *h,
                  void (*visit)(const char *key, void *value, void *user),
                  void *user)
{
    for (size_t i = 0; i < h->bucket_count; i++) {
        for (xs_hash_entry *e = h->buckets[i]; e; e = e->next) {
            visit(e->key, e->value, user);
        }
    }
}
