#ifndef XS_PARSER_H
#define XS_PARSER_H

#include <stddef.h>

typedef struct xs_pin {
    char *name;          /* e.g. "D" */
    char *dir;           /* "in", "out", "inout" */
    double x, y;         /* center of pin box, in symbol coords */
} xs_pin;

typedef struct xs_symbol {
    char *name;          /* canonical name w/o .sym, e.g. "sky130_fd_pr/nfet_01v8" */
    char *path;          /* loaded-from file path */
    char *type;          /* from K-block: "nmos","pmos","subcircuit","ipin","opin","label","iopin", ... */
    char *format;        /* K-block format= raw string, may be NULL */
    char *lvs_format;    /* K-block lvs_format= raw string, may be NULL */
    char *template_;     /* K-block template= raw string, may be NULL */
    char *extra;         /* K-block extra= (subckt port list) */
    char *spice_ignore;  /* K-block spice_ignore= ("true" → ignore symbol entirely) */
    xs_pin *pins;
    int npins;
} xs_symbol;

typedef struct xs_wire {
    double x1, y1, x2, y2;
    char  *prop;         /* raw {} block from .sch */
} xs_wire;

typedef struct xs_instance {
    char  *symref;       /* exact reference from .sch, e.g. "ipin.sym" or "sky130_fd_pr/nfet_01v8.sym" */
    double x, y;
    int    rot;
    int    flip;
    char  *prop;         /* raw {} block from .sch */
    xs_symbol *sym;      /* resolved symbol */
} xs_instance;

typedef struct xs_schematic {
    xs_wire     *wires;
    int          nwires;
    xs_instance *instances;
    int          ninstances;
    char        *path;       /* schematic path */
    char        *cell_name;  /* basename without .sch */
} xs_schematic;

/* parse .sch file from disk into struct (mallocs). Returns 0 on success. */
int  xs_parse_schematic(const char *path, xs_schematic *out);
void xs_free_schematic(xs_schematic *s);

/* parse a .sym file from disk into struct (mallocs). Returns 0 on success. */
int  xs_parse_symbol(const char *path, xs_symbol *out);
void xs_free_symbol(xs_symbol *s);

/* helper: read the value of a token "key=value" from a property string.
 * Returns malloc'd value (caller frees) or NULL if not found. Whitespace-,
 * newline-, or end-separated; supports "..."-quoted values with \" / \\. */
char *xs_prop_get(const char *prop, const char *key);

/* build properties string by concatenating defaults and overrides, with overrides
 * taking precedence. Returns malloc'd combined string. */
char *xs_props_merge(const char *defaults, const char *overrides);

#endif
