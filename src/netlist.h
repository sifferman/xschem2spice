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

/*
 * Connectivity and SPICE emission mirror XSCHEM 3.4.7's netlisting pipeline:
 * prepare_netlist_structs():
 * https://github.com/StefanSchippers/xschem/blob/3.4.7/src/netlist.c#L1509-L1549
 * spice_netlist():
 * https://github.com/StefanSchippers/xschem/blob/3.4.7/src/spice_netlist.c#L171-L249
 * global_spice_netlist():
 * https://github.com/StefanSchippers/xschem/blob/3.4.7/src/spice_netlist.c#L252-L607
 */

#ifndef XS_NETLIST_H
#define XS_NETLIST_H

#include <stdio.h>

#include "hash.h"
#include "parser.h"
#include "xschemrc.h"

typedef struct {
    int              lvs_mode;        /* prefer lvs_format and strip leading `#` from auto names */
    xs_library_path *library_path;
    xs_hash         *symbol_cache;    /* keyed by symref (`"sky130_fd_pr/nfet_01v8.sym"`) */
} xs_netlister;

void xs_netlister_init(xs_netlister *nl, xs_library_path *lp, int lvs_mode);
void xs_netlister_free(xs_netlister *nl);

/* For every instance in `sch`, populate `instance.resolved_symbol` by looking
 * up `instance.symref` against `nl->library_path` and parsing the .sym file
 * (cached). Symrefs that don't resolve get a shared "missing" placeholder so
 * we can emit the same `* IS MISSING !!!!` comment XSCHEM does. Returns 0 on
 * success or -1 on hard parse error. */
int  xs_netlister_resolve_symbols(xs_netlister *nl, xs_schematic *sch);

/* Emit a SPICE .subckt for `sch` to `out`. Caller must have already invoked
 * xs_netlister_resolve_symbols(). Returns 0 on success. */
int  xs_netlister_emit_spice    (xs_netlister *nl, const xs_schematic *sch, FILE *out);

/* Convenience: parse `schematic_path`, resolve symbols against
 * `xschemrc_path` (may be NULL), and write a SPICE netlist to `out`. This is
 * the recommended entry point for callers that just want a netlist. */
int  xs_write_spice_netlist(const char *schematic_path,
                            const char *xschemrc_path,
                            FILE       *out);

#endif
