#ifndef XS_HASH_H
#define XS_HASH_H

#include <stddef.h>

typedef struct xs_hash_entry {
    char *key;
    void *val;
    struct xs_hash_entry *next;
} xs_hash_entry;

typedef struct {
    xs_hash_entry **buckets;
    size_t nbuckets;
    size_t nentries;
} xs_hash;

xs_hash *xs_hash_new(size_t nbuckets);
void     xs_hash_free(xs_hash *h, void (*free_val)(void *));
void    *xs_hash_get(xs_hash *h, const char *key);
/* puts a copy of key; returns previous value (or NULL) */
void    *xs_hash_put(xs_hash *h, const char *key, void *val);
/* iterate entries */
void     xs_hash_iter(xs_hash *h,
                      void (*fn)(const char *key, void *val, void *user),
                      void *user);

#endif
