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
 * Minimal demonstration CLI for libxschem2spice. The library is the real
 * deliverable; this front-end just reads three flags and calls
 * xs_write_spice_netlist().
 */

#include "netlist.h"

#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    const char *schematic_path = NULL;
    const char *xschemrc_path  = NULL;
    const char *output_path    = NULL;

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (!strcmp(arg, "--xschemrc") && i + 1 < argc) xschemrc_path = argv[++i];
        else if (!strcmp(arg, "-o")    && i + 1 < argc) output_path   = argv[++i];
        else if (arg[0] != '-' && !schematic_path)      schematic_path = arg;
        else {
            fprintf(stderr, "Usage: %s [--xschemrc <rc>] [-o <out.spice>] <input.sch>\n",
                    argv[0]);
            return 2;
        }
    }
    if (!schematic_path) {
        fprintf(stderr, "Usage: %s [--xschemrc <rc>] [-o <out.spice>] <input.sch>\n",
                argv[0]);
        return 2;
    }

    FILE *out = stdout;
    if (output_path && !(out = fopen(output_path, "w"))) {
        fprintf(stderr, "xschem2spice: cannot open %s for write\n", output_path);
        return 1;
    }

    int status = xs_write_spice_netlist(schematic_path, xschemrc_path, out);
    if (out != stdout) fclose(out);
    return status == 0 ? 0 : 1;
}
