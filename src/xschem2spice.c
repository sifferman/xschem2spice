#include "parser.h"
#include "netlist.h"
#include "xschemrc.h"
#include "strutil.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libgen.h>

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [options] <input.sch> [-o <output.spice>]\n"
        "\n"
        "Options:\n"
        "  -I <path>           Add a symbol search path (repeatable)\n"
        "  --xschemrc <file>   Load XSCHEM_LIBRARY_PATH from an xschemrc file\n"
        "  --flat              Emit a flat netlist (currently same as default)\n"
        "  --no-lvs            Disable LVS-mode formatting (use full format=)\n"
        "  --info              Print parsed schematic info; do not emit SPICE\n"
        "  -o <out>            Write SPICE to <out> (default: stdout)\n"
        "  -h, --help          Show usage\n",
        prog);
}

int main(int argc, char **argv)
{
    const char *input = NULL;
    const char *output = NULL;
    const char *xschemrc = NULL;
    int lvs_mode = 1;
    int info_only = 0;
    int flat = 0;

    xs_libpath lib;
    xs_libpath_init(&lib);

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            usage(argv[0]);
            xs_libpath_free(&lib);
            return 0;
        } else if (strcmp(a, "-I") == 0 && i + 1 < argc) {
            xs_libpath_add(&lib, argv[++i]);
        } else if (strcmp(a, "--xschemrc") == 0 && i + 1 < argc) {
            xschemrc = argv[++i];
        } else if (strcmp(a, "-o") == 0 && i + 1 < argc) {
            output = argv[++i];
        } else if (strcmp(a, "--flat") == 0) {
            flat = 1;
            (void)flat;
        } else if (strcmp(a, "--no-lvs") == 0) {
            lvs_mode = 0;
        } else if (strcmp(a, "--info") == 0) {
            info_only = 1;
        } else if (a[0] == '-') {
            fprintf(stderr, "Unknown option: %s\n", a);
            usage(argv[0]);
            xs_libpath_free(&lib);
            return 2;
        } else {
            if (input) {
                fprintf(stderr, "More than one input file: %s and %s\n", input, a);
                xs_libpath_free(&lib);
                return 2;
            }
            input = a;
        }
    }

    if (!input) {
        usage(argv[0]);
        xs_libpath_free(&lib);
        return 2;
    }

    /* Load xschemrc paths first (it may execute `set XSCHEM_LIBRARY_PATH {}`
     * which would wipe paths added before it). xschem itself uses *only*
     * XSCHEM_LIBRARY_PATH to resolve symbols; we deliberately do NOT auto-
     * add the input .sch's directory, so an xschem2spice run with a given
     * xschemrc resolves the exact same symbols xschem would. */
    if (xschemrc) {
        xs_libpath_load_xschemrc(&lib, xschemrc);
    }

    xs_schematic sch;
    if (xs_parse_schematic(input, &sch) != 0) {
        xs_libpath_free(&lib);
        return 1;
    }

    xs_netlister nl;
    xs_netlister_init(&nl, &lib, lvs_mode);

    if (xs_netlister_resolve_symbols(&nl, &sch) != 0) {
        xs_netlister_free(&nl);
        xs_free_schematic(&sch);
        xs_libpath_free(&lib);
        return 1;
    }

    if (info_only) {
        printf("# Schematic: %s\n", sch.path);
        printf("# Cell:      %s\n", sch.cell_name);
        printf("# Wires:     %d\n", sch.nwires);
        printf("# Instances: %d\n", sch.ninstances);
        for (int i = 0; i < sch.ninstances; i++) {
            const xs_instance *ins = &sch.instances[i];
            printf("# Instance[%d]: %s @ (%g,%g) rot=%d flip=%d type=%s\n",
                   i, ins->symref, ins->x, ins->y, ins->rot, ins->flip,
                   ins->sym->type ? ins->sym->type : "(none)");
        }
        xs_netlister_free(&nl);
        xs_free_schematic(&sch);
        xs_libpath_free(&lib);
        return 0;
    }

    FILE *out = stdout;
    if (output) {
        out = fopen(output, "w");
        if (!out) {
            fprintf(stderr, "xschem2spice: cannot open %s for write\n", output);
            xs_netlister_free(&nl);
            xs_free_schematic(&sch);
            xs_libpath_free(&lib);
            return 1;
        }
    }
    int rc = xs_netlister_emit_spice(&nl, &sch, out);
    if (out != stdout) fclose(out);

    xs_netlister_free(&nl);
    xs_free_schematic(&sch);
    xs_libpath_free(&lib);
    return rc == 0 ? 0 : 1;
}
