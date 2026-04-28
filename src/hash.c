#include "hash.h"
#include "strutil.h"

#include <stdlib.h>
#include <string.h>

static unsigned long fnv1a(const char *s)
{
    unsigned long h = 1469598103934665603UL;
    while (*s) {
        h ^= (unsigned char)*s++;
        h *= 1099511628211UL;
    }
    return h;
}

xs_hash *xs_hash_new(size_t nbuckets)
{
    if (nbuckets < 16) nbuckets = 16;
    xs_hash *h = xs_xmalloc(sizeof *h);
    h->buckets = xs_xmalloc(nbuckets * sizeof *h->buckets);
    for (size_t i = 0; i < nbuckets; i++) h->buckets[i] = NULL;
    h->nbuckets = nbuckets;
    h->nentries = 0;
    return h;
}

void xs_hash_free(xs_hash *h, void (*free_val)(void *))
{
    if (!h) return;
    for (size_t i = 0; i < h->nbuckets; i++) {
        xs_hash_entry *e = h->buckets[i];
        while (e) {
            xs_hash_entry *n = e->next;
            if (free_val) free_val(e->val);
            free(e->key);
            free(e);
            e = n;
        }
    }
    free(h->buckets);
    free(h);
}

void *xs_hash_get(xs_hash *h, const char *key)
{
    unsigned long b = fnv1a(key) % h->nbuckets;
    for (xs_hash_entry *e = h->buckets[b]; e; e = e->next) {
        if (strcmp(e->key, key) == 0) return e->val;
    }
    return NULL;
}

void *xs_hash_put(xs_hash *h, const char *key, void *val)
{
    unsigned long b = fnv1a(key) % h->nbuckets;
    for (xs_hash_entry *e = h->buckets[b]; e; e = e->next) {
        if (strcmp(e->key, key) == 0) {
            void *old = e->val;
            e->val = val;
            return old;
        }
    }
    xs_hash_entry *e = xs_xmalloc(sizeof *e);
    e->key = xs_strdup(key);
    e->val = val;
    e->next = h->buckets[b];
    h->buckets[b] = e;
    h->nentries++;
    return NULL;
}

void xs_hash_iter(xs_hash *h,
                  void (*fn)(const char *key, void *val, void *user),
                  void *user)
{
    for (size_t i = 0; i < h->nbuckets; i++) {
        for (xs_hash_entry *e = h->buckets[i]; e; e = e->next) {
            fn(e->key, e->val, user);
        }
    }
}
