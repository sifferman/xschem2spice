#ifndef XS_NETLIST_H
#define XS_NETLIST_H

#include <stdio.h>

#include "parser.h"
#include "xschemrc.h"

typedef struct {
    int          lvs_mode;       /* if true, prefer lvs_format and strip '#' */
    int          flat;           /* unused for now: emit flat netlist */
    xs_libpath  *lib;
    /* cache of loaded symbols, keyed by symref string */
    struct xs_hash *sym_cache;
} xs_netlister;

void xs_netlister_init(xs_netlister *nl, xs_libpath *lib, int lvs_mode);
void xs_netlister_free(xs_netlister *nl);

/* Resolve every instance->sym in the schematic by loading symbols from disk. */
int  xs_netlister_resolve_symbols(xs_netlister *nl, xs_schematic *sch);

/* Try to load <cell>.sym alongside <cell>.sch and use its B-record pin
 * order as the canonical port list. May return NULL if no companion .sym
 * exists. Caller does not own. */
struct xs_symbol *xs_netlister_self_symbol(xs_netlister *nl,
                                           const xs_schematic *sch);

/* Write a SPICE .subckt for the given (already-parsed) schematic to `out`.
 * Returns 0 on success. */
int  xs_netlister_emit_spice(xs_netlister *nl, const xs_schematic *sch,
                             FILE *out);

#endif
