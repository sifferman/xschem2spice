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
 * On-disk format mirrors XSCHEM's per-record reader/writer (both live in
 * src/save.c despite the name):
 *   read_xschem_file():
 *     https://github.com/StefanSchippers/xschem/blob/3.4.7/src/save.c#L3058-L3202
 *   write_xschem_file():
 *     https://github.com/StefanSchippers/xschem/blob/3.4.7/src/save.c#L2705-L2759
 */

#ifndef XS_PARSER_H
#define XS_PARSER_H

#include <stddef.h>

typedef struct {
    char  *name;              /* e.g. "D" */
    char  *dir;               /* "in", "out", or "inout" */
    double x, y;              /* pin centre in symbol-local coordinates */
} xs_symbol_pin;

typedef struct {
    char         *name;          /* basename without .sym, e.g. "nfet_01v8" */
    char         *path;          /* file the symbol was loaded from */
    char         *type;          /* K-block type=:  nmos|pmos|subcircuit|ipin|opin|label|... */
    char         *format;        /* K-block format= raw string, possibly NULL */
    char         *lvs_format;    /* K-block lvs_format= raw string, possibly NULL */
    char         *template_;    /* K-block template= raw string, possibly NULL */
    char         *extra;         /* K-block extra= (manual subckt port list), possibly NULL */
    char         *spice_ignore;  /* K-block spice_ignore=, possibly NULL */
    xs_symbol_pin *pins;
    int           pin_count;
} xs_symbol;

typedef struct {
    double x1, y1, x2, y2;
    char  *prop_block;            /* raw `{...}` from the N-record */
} xs_wire;

typedef struct {
    char       *symref;           /* exact .sch reference, e.g. "sky130_fd_pr/nfet_01v8.sym" */
    double      x, y;             /* placement origin in schematic coordinates */
    int         rotation;         /* xschem rotation, 0..3 (×90° CCW) */
    int         flip;             /* xschem flip, 0 or 1 */
    char       *prop_block;       /* raw `{...}` from the C-record */
    xs_symbol  *resolved_symbol;  /* filled by xs_netlister_resolve_symbols */
} xs_instance;

typedef struct {
    xs_wire      *wires;
    int           wire_count;
    xs_instance  *instances;
    int           instance_count;
    char         *path;           /* path the schematic was loaded from */
    char         *cell_name;      /* basename without .sch */
} xs_schematic;

int  xs_parse_schematic(const char *path, xs_schematic *out);
void xs_free_schematic(xs_schematic *s);

int  xs_parse_symbol(const char *path, xs_symbol *out);
void xs_free_symbol(xs_symbol *s);

/* Look up `key` in an xschem `key=value key=value …` property block. Values
 * may be bare tokens or "..."-quoted with \"/\\ escapes. Returns a malloc'd
 * value (caller frees) or NULL when missing. */
char *xs_prop_get(const char *property_block, const char *key);

/* Apply an instance's (rotation, flip) to a symbol-local point and write the
 * result through `*x_out`, `*y_out`. The caller adds the instance origin to
 * get global coordinates. Mirrors XSCHEM's ROTATION macro
 *   https://github.com/StefanSchippers/xschem/blob/3.4.7/src/xschem.h#L339-L346
 * with `(x0, y0) = (0, 0)`. */
void xs_transform_pin_to_global(int rotation, int flip,
                                double x_in, double y_in,
                                double *x_out, double *y_out);

#endif
