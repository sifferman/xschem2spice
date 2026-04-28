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
 * Minimal xschemrc parsing for the XSCHEM_LIBRARY_PATH, XSCHEM_SHAREDIR, and
 * source commands used by real PDK setup files.
 */

#ifndef XS_XSCHEMRC_H
#define XS_XSCHEMRC_H

typedef struct {
    char **paths;
    int    path_count;
} xs_library_path;

void xs_library_path_init(xs_library_path *lp);
void xs_library_path_free(xs_library_path *lp);
void xs_library_path_add(xs_library_path *lp, const char *path);

/* Read `xschemrc_path`, append to `lp` everything on the resulting
 * XSCHEM_LIBRARY_PATH. Returns 0 on success; missing file is non-fatal. */
int  xs_library_path_load_xschemrc(xs_library_path *lp, const char *xschemrc_path);

/* Search `lp` for `symref` (e.g. `"sky130_fd_pr/nfet_01v8.sym"`). When
 * `symref` ends in `.sch` we prefer the same-basename `.sym` (every real
 * hierarchical block has both, and only the .sym carries pin geometry).
 * Returns a malloc'd absolute path, or NULL when nothing matches. */
char *xs_library_path_resolve(const xs_library_path *lp, const char *symref);

#endif
