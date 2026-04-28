#include "netlist.h"
#include "hash.h"
#include "strutil.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Implemented in parser.c */
void xs_apply_xform(int rot, int flip, double sx, double sy,
                    double *gx, double *gy);

void xs_netlister_init(xs_netlister *nl, xs_libpath *lib, int lvs_mode)
{
    nl->lvs_mode = lvs_mode;
    nl->flat = 0;
    nl->lib = lib;
    nl->sym_cache = (struct xs_hash *)xs_hash_new(64);
}

static void free_symbol_val(void *p)
{
    if (!p) return;
    xs_free_symbol((xs_symbol *)p);
    free(p);
}

void xs_netlister_free(xs_netlister *nl)
{
    if (nl->sym_cache) xs_hash_free((xs_hash *)nl->sym_cache, free_symbol_val);
    nl->sym_cache = NULL;
}

struct xs_symbol *xs_netlister_self_symbol(xs_netlister *nl,
                                           const xs_schematic *sch)
{
    /* Build candidate path: same dir as the .sch, basename swap .sch -> .sym */
    if (!sch->path) return NULL;
    size_t l = strlen(sch->path);
    if (l < 4 || strcmp(sch->path + l - 4, ".sch") != 0) return NULL;
    char *path = xs_strdup(sch->path);
    memcpy(path + l - 4, ".sym", 4);
    /* check direct path; fall back to library lookup using just the basename */
    xs_symbol *sym = NULL;
    {
        FILE *f = fopen(path, "r");
        if (f) {
            fclose(f);
            sym = xs_xmalloc(sizeof *sym);
            if (xs_parse_symbol(path, sym) != 0) {
                free(sym);
                sym = NULL;
            }
        }
    }
    if (!sym) {
        const char *slash = strrchr(path, '/');
        const char *base = slash ? slash + 1 : path;
        char *resolved = xs_libpath_resolve(nl->lib, base);
        if (resolved) {
            sym = xs_xmalloc(sizeof *sym);
            if (xs_parse_symbol(resolved, sym) != 0) {
                free(sym);
                sym = NULL;
            }
            free(resolved);
        }
    }
    free(path);
    if (!sym) return NULL;
    /* Cache so we don't leak — use a sentinel key */
    char cachekey[512];
    snprintf(cachekey, sizeof cachekey, "__self__:%s", sch->cell_name);
    void *prev = xs_hash_put((xs_hash *)nl->sym_cache, cachekey, sym);
    if (prev) free_symbol_val(prev);
    return sym;
}

int xs_netlister_resolve_symbols(xs_netlister *nl, xs_schematic *sch)
{
    /* Cached "missing" placeholder shared across all unresolved instances. */
    xs_symbol *missing_proto = NULL;

    for (int i = 0; i < sch->ninstances; i++) {
        xs_instance *ins = &sch->instances[i];
        xs_symbol *sym = (xs_symbol *)xs_hash_get((xs_hash *)nl->sym_cache,
                                                  ins->symref);
        if (!sym) {
            char *path = xs_libpath_resolve(nl->lib, ins->symref);
            if (!path) {
                /* xschem in the same situation substitutes the symbol with
                 * `systemlib/missing.sym`, whose format prints
                 *   `*  @name -  @symname  IS MISSING !!!!`
                 * Match that behavior so netgen LVS sees the same top-level
                 * graph. */
                if (!missing_proto) {
                    missing_proto = xs_xmalloc(sizeof *missing_proto);
                    memset(missing_proto, 0, sizeof *missing_proto);
                    missing_proto->name = xs_strdup("missing");
                    missing_proto->path = xs_strdup("(missing)");
                    missing_proto->type = xs_strdup("missing");
                    missing_proto->format =
                        xs_strdup("*  @name -  @symname  IS MISSING !!!!");
                    xs_hash_put((xs_hash *)nl->sym_cache,
                                "__missing__", missing_proto);
                }
                ins->sym = missing_proto;
                fprintf(stderr,
                        "xschem2spice: warning: symbol '%s' not in libpath, treating as missing\n",
                        ins->symref);
                continue;
            }
            sym = xs_xmalloc(sizeof *sym);
            if (xs_parse_symbol(path, sym) != 0) {
                free(path);
                free(sym);
                return -1;
            }
            free(path);
            xs_hash_put((xs_hash *)nl->sym_cache, ins->symref, sym);
        }
        ins->sym = sym;
    }
    return 0;
}

/* ---------------- vertex / union-find ---------------- */

typedef struct {
    double x, y;
    int    inst;     /* -1 for wire endpoint */
    int    pin;      /* index into instance's symbol pins; -1 for wire endpoint */
} vertex_t;

typedef struct {
    int *parent;
    int  n;
} ufd_t;

static int uf_find(ufd_t *u, int i)
{
    while (u->parent[i] != i) {
        u->parent[i] = u->parent[u->parent[i]];
        i = u->parent[i];
    }
    return i;
}

static void uf_unite(ufd_t *u, int a, int b)
{
    int ra = uf_find(u, a);
    int rb = uf_find(u, b);
    if (ra != rb) u->parent[ra] = rb;
}

static int dbleq(double a, double b)
{
    return fabs(a - b) < 1e-6;
}

/* Returns 1 if (px, py) lies on segment (ax,ay)–(bx,by) (incl. endpoints). */
static int point_on_segment(double px, double py,
                            double ax, double ay,
                            double bx, double by)
{
    double cross = (px - ax) * (by - ay) - (py - ay) * (bx - ax);
    if (fabs(cross) > 1e-6) return 0;
    /* dot of (P-A) with (B-A) gives projection */
    double dot = (px - ax) * (bx - ax) + (py - ay) * (by - ay);
    if (dot < -1e-9) return 0;
    double len2 = (bx - ax) * (bx - ax) + (by - ay) * (by - ay);
    if (dot > len2 + 1e-9) return 0;
    return 1;
}

/* ---------------- net naming ---------------- */

typedef struct {
    int   root;
    char *label;       /* canonical label, NULL if auto-named */
    char *auto_name;   /* malloc'd "netN" if used */
} net_info_t;

/* per-vertex; also a bag of labels collected during pass */
typedef struct {
    int   nvertices;
    int  *root_to_idx;  /* root vertex id -> compact index into nets[] */
    int   nnets;
    net_info_t *nets;
} netmap_t;

static int is_label_type(const char *type)
{
    if (!type) return 0;
    return (strcmp(type, "ipin") == 0 ||
            strcmp(type, "opin") == 0 ||
            strcmp(type, "iopin") == 0 ||
            strcmp(type, "label") == 0);
}

static int is_port_type(const char *type)
{
    if (!type) return 0;
    return (strcmp(type, "ipin") == 0 ||
            strcmp(type, "opin") == 0 ||
            strcmp(type, "iopin") == 0);
}

/* ---------------- net name pretty-print ---------------- */

/* In LVS mode, xschem strips a leading '#'. Also we always strip surrounding
 * whitespace and unify case-style. */
static char *prettify_label(const char *raw, int lvs_mode)
{
    if (!raw) return NULL;
    const char *p = raw;
    while (*p == ' ' || *p == '\t') p++;
    if (lvs_mode && *p == '#') p++;
    char *r = xs_strdup(p);
    /* trim trailing whitespace */
    char *e = r + strlen(r);
    while (e > r && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n' || e[-1] == '\r')) e--;
    *e = '\0';
    return r;
}

/* ---------------- main netlist builder ---------------- */

/* Get prop value from instance; if missing or empty, fall back to template default.
 * Returns malloc'd string (possibly empty). Never returns NULL. */
static char *get_resolved_prop(const xs_instance *ins, const char *key)
{
    char *v = xs_prop_get(ins->prop, key);
    if (v && *v) return v;
    free(v);
    if (ins->sym && ins->sym->template_) {
        char *t = xs_prop_get(ins->sym->template_, key);
        if (t) return t;
    }
    return xs_strdup("");
}

/* Strip `tcleval(<balanced>)` substrings from `s` and write the rest to `out`.
 * xschem evaluates these as Tcl; without an embedded interpreter the literal
 * text would otherwise leak through and confuse netgen LVS, which tries to
 * parse `if/elseif/return` inside the tcleval body as device names. */
static void fputs_strip_tcleval(const char *s, FILE *out)
{
    if (!s) return;
    while (*s) {
        if (strncmp(s, "tcleval(", 8) == 0) {
            s += 8;
            int depth = 1;
            while (*s && depth > 0) {
                char c = *s++;
                if (c == '\\' && *s) { s++; continue; }
                if (c == '(') depth++;
                else if (c == ')') { depth--; if (depth == 0) break; }
            }
            continue;
        }
        fputc(*s, out);
        s++;
    }
}

/* Substitute @<key> tokens in the format string.
 *
 * `name_override` (optional) replaces the value of @name; used for bus-name
 * instance expansion where each replica gets a per-bit name like `R5[3]`.
 */
static void emit_subst_format_one(const char *fmt,
                                  const xs_instance *ins,
                                  char **pin_nets,
                                  const char *symname,
                                  const char *name_override,
                                  FILE *out_real);

static void emit_subst_format(const char *fmt,
                              const xs_instance *ins,
                              char **pin_nets,
                              const char *symname,
                              FILE *out_real)
{
    emit_subst_format_one(fmt, ins, pin_nets, symname, NULL, out_real);
}

static void emit_subst_format_one(const char *fmt,
                                  const xs_instance *ins,
                                  char **pin_nets,
                                  const char *symname,
                                  const char *name_override,
                                  FILE *out_real)
{
    if (!fmt) return;
    /* Substitute into an in-memory tmpfile() first, then post-process to strip
     * unevaluable `tcleval(...)` blocks before writing to the real output. */
    FILE *out = tmpfile();
    if (!out) {
        /* fall back to direct write if tmpfile fails */
        out = out_real;
    }
    const char *p = fmt;
    while (*p) {
        if (*p == '\\' && p[1]) {
            /* xschem template escape — \" → ", \\ → \ — but inside format
             * string we usually only see \\ for backslashes. Pass through. */
            fputc(p[1], out);
            p += 2;
            continue;
        }
        if (*p == '@') {
            /* @@<pinname> — net connected to the symbol pin named <pinname>. */
            if (p[1] == '@' && (isalpha((unsigned char)p[2]) || p[2] == '_')) {
                const char *q = p + 2;
                while (*q && (isalnum((unsigned char)*q) || *q == '_')) q++;
                size_t kl = (size_t)(q - (p + 2));
                char *name = xs_strndup(p + 2, kl);
                int found = -1;
                int npins = ins->sym ? ins->sym->npins : 0;
                for (int j = 0; j < npins; j++) {
                    if (ins->sym->pins[j].name &&
                        strcmp(ins->sym->pins[j].name, name) == 0) {
                        found = j; break;
                    }
                }
                if (found >= 0 && pin_nets[found]) fputs(pin_nets[found], out);
                else fputs("?", out);
                free(name);
                p = q;
                continue;
            }
            /* Pin-net references: @#<N> or @#<N>:<attr> */
            if (p[1] == '#' && isdigit((unsigned char)p[2])) {
                const char *q = p + 2;
                int idx = 0;
                while (isdigit((unsigned char)*q)) { idx = idx * 10 + (*q - '0'); q++; }
                /* optional :<attr> */
                if (*q == ':') {
                    const char *as = q + 1;
                    const char *ae = as;
                    while (*ae && (isalnum((unsigned char)*ae) || *ae == '_')) ae++;
                    /* We only support :net_name (and treat :<anything> as net) */
                    q = ae;
                }
                int npins = ins->sym ? ins->sym->npins : 0;
                if (idx >= 0 && idx < npins && pin_nets[idx]) {
                    fputs(pin_nets[idx], out);
                } else {
                    fputs("?", out);
                }
                p = q;
                continue;
            }
            const char *q = p + 1;
            /* alnum/underscore/dot? Just alnum/underscore. */
            while (*q && (isalnum((unsigned char)*q) || *q == '_')) q++;
            size_t kl = (size_t)(q - (p + 1));
            if (kl == 0) { fputc('@', out); p++; continue; }
            char *key = xs_strndup(p + 1, kl);
            if (strcmp(key, "name") == 0) {
                if (name_override) {
                    fputs(name_override, out);
                } else {
                    char *v = xs_prop_get(ins->prop, "name");
                    fputs(v ? v : "", out);
                    free(v);
                }
            } else if (strcmp(key, "spiceprefix") == 0) {
                char *v = xs_prop_get(ins->prop, "spiceprefix");
                if (!v && ins->sym && ins->sym->template_) {
                    v = xs_prop_get(ins->sym->template_, "spiceprefix");
                }
                fputs(v ? v : "", out);
                free(v);
            } else if (strcmp(key, "pinlist") == 0) {
                int npins = ins->sym ? ins->sym->npins : 0;
                for (int i = 0; i < npins; i++) {
                    if (i) fputc(' ', out);
                    fputs(pin_nets[i] ? pin_nets[i] : "?", out);
                }
            } else if (strcmp(key, "symname") == 0) {
                fputs(symname ? symname : "", out);
            } else if (strcmp(key, "path") == 0) {
                /* hierarchical path — empty at top level */
            } else if (strcmp(key, "savecurrent") == 0 ||
                       strcmp(key, "spice_ignore") == 0) {
                /* marker properties — xschem absorbs these and emits any
                 * side effects (.save i(@name)) elsewhere. We just skip. */
            } else {
                /* lookup instance prop, then template default */
                char *v = get_resolved_prop(ins, key);
                fputs(v ? v : "", out);
                free(v);
            }
            free(key);
            p = q;
            continue;
        }
        fputc(*p, out);
        p++;
    }
    if (out != out_real) {
        long n = ftell(out);
        if (n < 0) n = 0;
        char *contents = xs_xmalloc((size_t)n + 1);
        rewind(out);
        size_t got = fread(contents, 1, (size_t)n, out);
        contents[got] = '\0';
        fclose(out);
        fputs_strip_tcleval(contents, out_real);
        free(contents);
    }
}

/* Determine pin0 (the connection pin) for a label-bearing symbol like
 * ipin/opin/lab_pin. We just take the first pin in the symbol. */
static int label_pin_index(const xs_symbol *sym)
{
    return sym && sym->npins > 0 ? 0 : -1;
}

/* Compute global pin coordinate. */
static void inst_pin_global(const xs_instance *ins, int pinix,
                            double *gx, double *gy)
{
    const xs_pin *p = &ins->sym->pins[pinix];
    double rx, ry;
    xs_apply_xform(ins->rot, ins->flip, p->x, p->y, &rx, &ry);
    *gx = ins->x + rx;
    *gy = ins->y + ry;
}

/* Bus-name syntax kinds, distinguishing xschem's two index forms:
 *   `:` (colon)  → scalar names use brackets, e.g. DATA[3]
 *   `..` (dotdot)→ scalar names omit brackets, e.g. DIN3
 * (See xschem/src/expandlabel.y: expandlabel_strbus vs strbus_nobracket.) */
enum xs_bus_kind { XS_BUS_NONE = 0, XS_BUS_COLON = 1, XS_BUS_DOTDOT = 2 };

/* Parse "<base>[<hi>:<lo>]" or "<base>[<hi>..<lo>]". Returns the multiplicity
 * (|hi-lo|+1) on match, or 0. Out params: *base_len = strlen(base); hi/lo
 * indices; kind = which syntax. */
static int parse_bus_name(const char *s, size_t *base_len,
                          int *hi, int *lo, int *kind)
{
    if (kind) *kind = XS_BUS_NONE;
    if (!s) return 0;
    const char *lb = strchr(s, '[');
    if (!lb) return 0;
    int h, l;
    if (sscanf(lb, "[%d..%d]", &h, &l) == 2) {
        if (kind) *kind = XS_BUS_DOTDOT;
    } else if (sscanf(lb, "[%d:%d]", &h, &l) == 2) {
        if (kind) *kind = XS_BUS_COLON;
    } else {
        return 0;
    }
    *base_len = (size_t)(lb - s);
    *hi = h;
    *lo = l;
    return (h >= l) ? (h - l + 1) : (l - h + 1);
}

/* Render the i-th scalar of a bus, given the kind and base. */
static void render_bus_scalar(char *buf, size_t bufsz,
                              const char *base, size_t base_len,
                              int idx, int kind)
{
    if (kind == XS_BUS_DOTDOT) {
        snprintf(buf, bufsz, "%.*s%d", (int)base_len, base, idx);
    } else {
        snprintf(buf, bufsz, "%.*s[%d]", (int)base_len, base, idx);
    }
}

/* Symbol shortname: drop any directory and .sym/.sch suffix. */
static char *symbol_short_name(const char *symref)
{
    const char *slash = strrchr(symref, '/');
    const char *base = slash ? slash + 1 : symref;
    size_t bl = strlen(base);
    if (bl >= 4 && (strcmp(base + bl - 4, ".sym") == 0 ||
                    strcmp(base + bl - 4, ".sch") == 0))
        bl -= 4;
    return xs_strndup(base, bl);
}

int xs_netlister_emit_spice(xs_netlister *nl, const xs_schematic *sch, FILE *out)
{
    /* 1. Resolve symbols (caller must have called resolve_symbols). */
    for (int i = 0; i < sch->ninstances; i++) {
        if (!sch->instances[i].sym) {
            fprintf(stderr, "xschem2spice: instance %d (%s) has no resolved symbol\n",
                    i, sch->instances[i].symref);
            return -1;
        }
    }

    /* 2. Build vertex list: every wire endpoint + every instance pin. */
    int max_v = sch->nwires * 2;
    for (int i = 0; i < sch->ninstances; i++) max_v += sch->instances[i].sym->npins;
    vertex_t *V = xs_xmalloc(sizeof(vertex_t) * (size_t)(max_v + 1));
    int nv = 0;

    /* per-instance pin-vertex offset (into V). */
    int *inst_pin_off = xs_xmalloc(sizeof(int) * (size_t)(sch->ninstances + 1));
    int *wire_a = xs_xmalloc(sizeof(int) * (size_t)(sch->nwires + 1));
    int *wire_b = xs_xmalloc(sizeof(int) * (size_t)(sch->nwires + 1));

    for (int i = 0; i < sch->nwires; i++) {
        const xs_wire *w = &sch->wires[i];
        wire_a[i] = nv;
        V[nv].x = w->x1; V[nv].y = w->y1; V[nv].inst = -1; V[nv].pin = -1;
        nv++;
        wire_b[i] = nv;
        V[nv].x = w->x2; V[nv].y = w->y2; V[nv].inst = -1; V[nv].pin = -1;
        nv++;
    }
    for (int i = 0; i < sch->ninstances; i++) {
        inst_pin_off[i] = nv;
        const xs_instance *ins = &sch->instances[i];
        for (int j = 0; j < ins->sym->npins; j++) {
            double gx, gy;
            inst_pin_global(ins, j, &gx, &gy);
            V[nv].x = gx; V[nv].y = gy; V[nv].inst = i; V[nv].pin = j;
            nv++;
        }
    }

    /* 3. Union-find: same coord. */
    ufd_t U;
    U.n = nv;
    U.parent = xs_xmalloc(sizeof(int) * (size_t)nv);
    for (int i = 0; i < nv; i++) U.parent[i] = i;

    /* coincident vertices */
    for (int i = 0; i < nv; i++) {
        for (int j = i + 1; j < nv; j++) {
            if (dbleq(V[i].x, V[j].x) && dbleq(V[i].y, V[j].y)) {
                uf_unite(&U, i, j);
            }
        }
    }
    /* type=short instances electrically merge their pins. */
    for (int i = 0; i < sch->ninstances; i++) {
        const xs_instance *ins = &sch->instances[i];
        if (!ins->sym->type || strcmp(ins->sym->type, "short") != 0) continue;
        if (ins->sym->npins < 2) continue;
        int v0 = inst_pin_off[i] + 0;
        for (int j = 1; j < ins->sym->npins; j++) {
            uf_unite(&U, v0, inst_pin_off[i] + j);
        }
    }
    /* wire interior contacts */
    for (int wi = 0; wi < sch->nwires; wi++) {
        const xs_wire *w = &sch->wires[wi];
        /* both endpoints are already in V */
        int a = wire_a[wi], b = wire_b[wi];
        uf_unite(&U, a, b);
        for (int v = 0; v < nv; v++) {
            if (v == a || v == b) continue;
            if (point_on_segment(V[v].x, V[v].y,
                                 w->x1, w->y1, w->x2, w->y2)) {
                uf_unite(&U, v, a);
            }
        }
    }

    /* 4. Build root -> compact net index */
    int *root_idx = xs_xmalloc(sizeof(int) * (size_t)nv);
    for (int i = 0; i < nv; i++) root_idx[i] = -1;
    int nnets = 0;
    for (int i = 0; i < nv; i++) {
        int r = uf_find(&U, i);
        if (root_idx[r] == -1) root_idx[r] = nnets++;
    }
    char **net_label = xs_xmalloc(sizeof(char *) * (size_t)(nnets + 1));
    /* Priority of the current label per net:
     *   3 = port label (ipin/opin/iopin) — must appear in .subckt port list
     *   2 = lab_pin or other label-typed instance
     *   1 = wire lab= annotation
     *   0 = none (use auto-name)
     * Higher priority wins; on equal priority, alphabetically-earlier wins
     * for determinism. */
    int *net_label_prio = xs_xmalloc(sizeof(int) * (size_t)(nnets + 1));
    for (int i = 0; i < nnets; i++) {
        net_label[i] = NULL;
        net_label_prio[i] = 0;
    }

    /* Apply labels from ipin/opin/iopin/label-typed instances.
     *
     * Note: xschem does NOT treat the wire-level `lab=NAME` annotation as
     * authoritative — those values are cached annotations from a prior
     * netlist run. Authoritative names come exclusively from instance pins
     * (lab_pin / ipin / opin / iopin / lab_wire). Trying to propagate them
     * here causes false-merges of nets that xschem keeps distinct. */
    for (int i = 0; i < sch->ninstances; i++) {
        const xs_instance *ins = &sch->instances[i];
        if (!is_label_type(ins->sym->type)) continue;
        int pinix = label_pin_index(ins->sym);
        if (pinix < 0) continue;
        int v = inst_pin_off[i] + pinix;
        int net = root_idx[uf_find(&U, v)];
        char *lab = xs_prop_get(ins->prop, "lab");
        if (!lab) continue;
        char *pretty = prettify_label(lab, nl->lvs_mode);
        free(lab);
        int prio = is_port_type(ins->sym->type) ? 3 : 2;
        if (net_label_prio[net] < prio ||
            (net_label_prio[net] == prio && net_label[net] &&
             strcmp(net_label[net], pretty) > 0)) {
            free(net_label[net]);
            net_label[net] = pretty;
            net_label_prio[net] = prio;
        } else {
            free(pretty);
        }
    }

    /* 5c. Process bus_tap instances. A bus_tap symbol has two pins:
     *
     *   pin 0 (`tap`)  — scalar net side
     *   pin 1 (`bus`)  — bus net side; e.g. carries `DATA[15:0]`
     *
     * The instance carries `lab=[<bit>]`. xschem derives the tap-side
     * scalar name as <basename>[<bit>], where <basename> is taken from the
     * bus pin's resolved net (e.g. `DATA` from `DATA[15:0]`). This is what
     * lets a wire labelled by a `lab_pin` `lab=DATA[15:0]` translate to a
     * scalar `DATA[3]` net at the tap pin (which downstream instances then
     * reference). */
    for (int i = 0; i < sch->ninstances; i++) {
        const xs_instance *ins = &sch->instances[i];
        if (!ins->sym->type || strcmp(ins->sym->type, "bus_tap") != 0) continue;
        if (ins->sym->npins < 2) continue;

        /* Identify which pin is "tap" (scalar) and which is "bus" by name.
         * Default to the convention from xschem's bus_tap.sym (tap=0, bus=1)
         * if names are missing. */
        int tap_idx = 0, bus_idx = 1;
        for (int j = 0; j < ins->sym->npins; j++) {
            const char *nm = ins->sym->pins[j].name;
            if (!nm) continue;
            if (strcmp(nm, "tap") == 0) tap_idx = j;
            else if (strcmp(nm, "bus") == 0) bus_idx = j;
        }

        int v_bus = inst_pin_off[i] + bus_idx;
        int v_tap = inst_pin_off[i] + tap_idx;
        int net_bus = root_idx[uf_find(&U, v_bus)];
        int net_tap = root_idx[uf_find(&U, v_tap)];
        const char *bus_name = net_label[net_bus];
        if (!bus_name) continue;  /* bus side unnamed; can't derive scalar */

        /* tap is the per-instance lab, e.g. "[3]" or "ENAB". */
        char *tap_lab = xs_prop_get(ins->prop, "lab");
        if (!tap_lab) {
            /* fall back to the symbol template's default */
            if (ins->sym->template_)
                tap_lab = xs_prop_get(ins->sym->template_, "lab");
        }
        if (!tap_lab || !*tap_lab) { free(tap_lab); continue; }

        /* xschem's rule (xschem/src/netlist.c instcheck): if the tap label
         * starts with `[` or is purely digits, treat it as a bit/range slice
         * of the bus's basename; otherwise the tap label is the literal net
         * name. */
        int is_slice = (tap_lab[0] == '[');
        if (!is_slice) {
            int onlydigits = 1;
            for (const char *q = tap_lab; *q; q++) {
                if (!isdigit((unsigned char)*q)) { onlydigits = 0; break; }
            }
            is_slice = onlydigits;
        }

        char *scalar;
        if (is_slice) {
            /* Find the basename xschem-style: walk left from the first `[`
             * until we hit a `,` (or the start of the string), then skip
             * any leading spaces. Lets us extract `ADD` from a comma-list
             * bus name like `CK , S1, ADD[3:0], ENAB`. */
            const char *first_lb = strchr(bus_name, '[');
            const char *base_start;
            if (first_lb) {
                const char *p2 = first_lb;
                while (p2 > bus_name) {
                    p2--;
                    if (*p2 == ',') {
                        do { p2++; } while (*p2 == ' ');
                        break;
                    }
                }
                if (*p2 == ',' || p2 < bus_name) p2 = bus_name;
                base_start = p2;
            } else {
                base_start = bus_name;
            }
            const char *base_end = first_lb ? first_lb : (bus_name + strlen(bus_name));
            size_t base_len = (size_t)(base_end - base_start);
            size_t scalar_len = base_len + strlen(tap_lab);
            scalar = xs_xmalloc(scalar_len + 1);
            memcpy(scalar, base_start, base_len);
            memcpy(scalar + base_len, tap_lab, strlen(tap_lab));
            scalar[scalar_len] = '\0';
        } else {
            scalar = xs_strdup(tap_lab);
        }
        free(tap_lab);

        /* Assign with the same priority as a lab_pin (2). Bus_tap is the
         * authoritative source of scalar net names derived from buses. */
        const int prio = 2;
        if (net_label_prio[net_tap] < prio ||
            (net_label_prio[net_tap] == prio && net_label[net_tap] &&
             strcmp(net_label[net_tap], scalar) > 0)) {
            free(net_label[net_tap]);
            net_label[net_tap] = scalar;
            net_label_prio[net_tap] = prio;
        } else {
            free(scalar);
        }
    }

    /* 6. Generate auto names for unlabeled nets that are actually used.
     *    A net is "used" if at least one non-label instance pin connects to it.
     */
    int *net_used = xs_xmalloc(sizeof(int) * (size_t)nnets);
    for (int i = 0; i < nnets; i++) net_used[i] = 0;
    for (int i = 0; i < sch->ninstances; i++) {
        const xs_instance *ins = &sch->instances[i];
        if (is_label_type(ins->sym->type)) continue;
        for (int j = 0; j < ins->sym->npins; j++) {
            int v = inst_pin_off[i] + j;
            int net = root_idx[uf_find(&U, v)];
            net_used[net] = 1;
        }
    }
    int auto_n = 0;
    for (int i = 0; i < nnets; i++) {
        if (net_label[i] || !net_used[i]) continue;
        auto_n++;
        char buf[32];
        snprintf(buf, sizeof buf, "net%d", auto_n);
        net_label[i] = xs_strdup(buf);
    }

    /* 7. Determine port list. Preferred source: companion <cell>.sym's
     *    B-record pin order (with bus-name expansion). xschem creates symbols
     *    from schematics with this ordering convention, and the parent's
     *    instance-level pinlist substitution depends on it — so the .subckt
     *    line must match the .sym's pin order, not the .sch's ipin order. */
    int  ports_cap = sch->ninstances + 16;
    char **port_names = xs_xmalloc(sizeof(char *) * (size_t)ports_cap);
    char **port_kinds = xs_xmalloc(sizeof(char *) * (size_t)ports_cap);
    int nports = 0;

    xs_symbol *self_sym = xs_netlister_self_symbol(nl, sch);
    /* Build a map from port name -> kind ("ipin"/"opin"/"iopin") from the
     * schematic's own ipin/opin instances, so we can produce a PININFO line
     * even when the .sym drives the port list. */
    typedef struct { char *name; char kind; } portkind_t;
    portkind_t *pks = xs_xmalloc(sizeof(portkind_t) * (size_t)(sch->ninstances + 1));
    int npks = 0;
    for (int i = 0; i < sch->ninstances; i++) {
        const xs_instance *ins = &sch->instances[i];
        if (!ins->sym->type) continue;
        if (!is_port_type(ins->sym->type)) continue;
        char *lab = xs_prop_get(ins->prop, "lab");
        if (!lab || !*lab) { free(lab); continue; }
        char *pretty = prettify_label(lab, nl->lvs_mode);
        free(lab);
        char k = (strcmp(ins->sym->type, "ipin") == 0) ? 'I' :
                 (strcmp(ins->sym->type, "opin") == 0) ? 'O' : 'B';
        pks[npks].name = pretty;
        pks[npks].kind = k;
        npks++;
    }

    if (self_sym && self_sym->npins > 0) {
        /* Use .sym pin order, expanding bus names like "A[3:0]" or "A[3..0]" */
        for (int i = 0; i < self_sym->npins; i++) {
            const char *nm = self_sym->pins[i].name;
            if (!nm) continue;
            /* Check for bus form: "<base>[<hi>:<lo>]" or "<base>[<hi>..<lo>]" */
            const char *lb = strchr(nm, '[');
            const char *colon = NULL;
            const char *rb = NULL;
            int hi = 0, lo = 0, is_bus = 0;
            if (lb) {
                rb = strchr(lb, ']');
                colon = strchr(lb, ':');
                const char *dotdot = strstr(lb, "..");
                if (rb && colon && colon < rb) {
                    is_bus = sscanf(lb, "[%d:%d]", &hi, &lo) == 2;
                } else if (rb && dotdot && dotdot < rb) {
                    is_bus = sscanf(lb, "[%d..%d]", &hi, &lo) == 2;
                }
            }
            if (is_bus) {
                size_t base_len = (size_t)(lb - nm);
                int step = (hi >= lo) ? -1 : 1;
                int n = (hi >= lo) ? (hi - lo + 1) : (lo - hi + 1);
                int kind = (colon && colon < rb) ? XS_BUS_COLON : XS_BUS_DOTDOT;
                for (int k = 0; k < n; k++) {
                    int idx = hi + step * k;
                    char buf[256];
                    render_bus_scalar(buf, sizeof buf, nm, base_len, idx, kind);
                    int dup = 0;
                    for (int p = 0; p < nports; p++)
                        if (strcmp(port_names[p], buf) == 0) { dup = 1; break; }
                    if (dup) continue;
                    if (nports >= ports_cap) {
                        ports_cap *= 2;
                        port_names = xs_xrealloc(port_names,
                                                 sizeof(char *) * (size_t)ports_cap);
                        port_kinds = xs_xrealloc(port_kinds,
                                                 sizeof(char *) * (size_t)ports_cap);
                    }
                    port_names[nports] = xs_strdup(buf);
                    port_kinds[nports] = NULL;
                    nports++;
                }
            } else {
                int dup = 0;
                for (int p = 0; p < nports; p++)
                    if (strcmp(port_names[p], nm) == 0) { dup = 1; break; }
                if (dup) continue;
                if (nports >= ports_cap) {
                    ports_cap *= 2;
                    port_names = xs_xrealloc(port_names,
                                             sizeof(char *) * (size_t)ports_cap);
                    port_kinds = xs_xrealloc(port_kinds,
                                             sizeof(char *) * (size_t)ports_cap);
                }
                port_names[nports] = xs_strdup(nm);
                port_kinds[nports] = NULL;
                nports++;
            }
        }
    } else {
        /* Fallback: ipins, opins, iopins in source order. */
        for (int pass = 0; pass < 3; pass++) {
            const char *want = (pass == 0) ? "ipin" : (pass == 1) ? "opin" : "iopin";
            for (int i = 0; i < sch->ninstances; i++) {
                const xs_instance *ins = &sch->instances[i];
                if (!ins->sym->type) continue;
                if (strcmp(ins->sym->type, want) != 0) continue;
                char *lab = xs_prop_get(ins->prop, "lab");
                if (!lab || !*lab) { free(lab); continue; }
                char *pretty = prettify_label(lab, nl->lvs_mode);
                free(lab);
                int dup = 0;
                for (int j = 0; j < nports; j++) {
                    if (strcmp(port_names[j], pretty) == 0) { dup = 1; break; }
                }
                if (dup) { free(pretty); continue; }
                if (nports >= ports_cap) {
                    ports_cap *= 2;
                    port_names = xs_xrealloc(port_names,
                                             sizeof(char *) * (size_t)ports_cap);
                    port_kinds = xs_xrealloc(port_kinds,
                                             sizeof(char *) * (size_t)ports_cap);
                }
                port_names[nports] = pretty;
                port_kinds[nports] = NULL;
                nports++;
            }
        }
    }
    /* Annotate kinds */
    for (int p = 0; p < nports; p++) {
        for (int k = 0; k < npks; k++) {
            if (strcmp(port_names[p], pks[k].name) == 0) {
                static char buf[2][2] = {{'I',0},{'O',0}};
                (void)buf;
                char *s = xs_xmalloc(2);
                s[0] = pks[k].kind; s[1] = '\0';
                port_kinds[p] = s;
                break;
            }
        }
    }
    for (int k = 0; k < npks; k++) free(pks[k].name);
    free(pks);

    /* 8. Emit SPICE */
    fprintf(out, "** sch_path: %s\n", sch->path);
    fprintf(out, ".subckt %s", sch->cell_name);
    for (int i = 0; i < nports; i++) {
        fprintf(out, " %s", port_names[i]);
    }
    fprintf(out, "\n");

    /* PININFO line */
    fprintf(out, "*.PININFO");
    for (int i = 0; i < nports; i++) {
        char k = port_kinds[i] ? port_kinds[i][0] : 'B';
        fprintf(out, " %s:%c", port_names[i], k);
    }
    fprintf(out, "\n");

    /* Body: emit each non-port, non-label instance's format.
     * Skip instances whose effective spice_ignore is "true". Defer
     * type=netlist_commands instances (.control blocks etc.) to after
     * the device list. */
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < sch->ninstances; i++) {
            const xs_instance *ins = &sch->instances[i];
            if (is_label_type(ins->sym->type)) continue;

            int is_netlist_cmd = (ins->sym->type &&
                                  strcmp(ins->sym->type, "netlist_commands") == 0);
            if (pass == 0 && is_netlist_cmd) continue;
            if (pass == 1 && !is_netlist_cmd) continue;

            /* spice_ignore truthy → skip this device. Truthy values
             * mirror xschem's `strboolcmp`: "true", "1", "yes" etc. The
             * special value "short" is also a skip-with-comment. Symbol-
             * level spice_ignore (in the K block) applies to every instance
             * unless the instance explicitly overrides it. */
            char *si = xs_prop_get(ins->prop, "spice_ignore");
            if (!si && ins->sym->spice_ignore)
                si = xs_strdup(ins->sym->spice_ignore);
            int ignore = 0;
            if (si) {
                if (strcmp(si, "true") == 0 || strcmp(si, "short") == 0 ||
                    strcmp(si, "1") == 0    || strcmp(si, "yes") == 0)
                    ignore = 1;
            }
            free(si);
            if (ignore) continue;

            /* For type=subcircuit instances we don't recurse — xschem emits
             * a placeholder comment in this case (`* <name> - <sym> IS
             * MISSING !!!!`). Match that exactly so netgen LVS sees the same
             * top-level graph. */
            if (ins->sym->type && strcmp(ins->sym->type, "subcircuit") == 0) {
                char *iname = xs_prop_get(ins->prop, "name");
                char *sn = symbol_short_name(ins->symref);
                fprintf(out, "*  %s -  %s  IS MISSING !!!!\n",
                        iname ? iname : "?", sn ? sn : "?");
                free(iname);
                free(sn);
                continue;
            }

            const char *fmt = NULL;
            if (nl->lvs_mode && ins->sym->lvs_format && *ins->sym->lvs_format)
                fmt = ins->sym->lvs_format;
            else if (ins->sym->format && *ins->sym->format)
                fmt = ins->sym->format;
            if (!fmt) {
                /* informational only; not all symbols emit netlist content */
                continue;
            }

            char **pin_nets = xs_xmalloc(sizeof(char *) *
                                         (size_t)(ins->sym->npins + 1));
            for (int j = 0; j < ins->sym->npins; j++) {
                int v = inst_pin_off[i] + j;
                int net = root_idx[uf_find(&U, v)];
                pin_nets[j] = net_label[net];
            }
            char *symshort = symbol_short_name(ins->symref);

            /* Bus-name instance expansion: when an instance is named like
             * "R5[3:0]", xschem emits 4 separate device lines (R5[3], R5[2],
             * R5[1], R5[0]) — and for any pin that connects to an equally-
             * sized bus net (e.g. DATA[3:0] or DATA[15:12]), each replica
             * gets the matching scalar bit. Replica iteration goes from hi
             * to lo, matching xschem. */
            char *iname = xs_prop_get(ins->prop, "name");
            int inst_hi = 0, inst_lo = 0, inst_kind = XS_BUS_NONE;
            size_t inst_base_len = 0;
            int inst_mult = iname ? parse_bus_name(iname, &inst_base_len,
                                                   &inst_hi, &inst_lo,
                                                   &inst_kind) : 0;
            if (inst_mult <= 1) {
                emit_subst_format(fmt, ins, pin_nets, symshort, out);
                fputc('\n', out);
            } else {
                int step = (inst_hi >= inst_lo) ? -1 : 1;
                /* Pre-parse each pin's net to detect bus form. */
                int *pin_mult = xs_xmalloc(sizeof(int)    * (size_t)ins->sym->npins);
                int *pin_hi   = xs_xmalloc(sizeof(int)    * (size_t)ins->sym->npins);
                int *pin_lo   = xs_xmalloc(sizeof(int)    * (size_t)ins->sym->npins);
                int *pin_kind = xs_xmalloc(sizeof(int)    * (size_t)ins->sym->npins);
                size_t *pbl   = xs_xmalloc(sizeof(size_t) * (size_t)ins->sym->npins);
                for (int j = 0; j < ins->sym->npins; j++) {
                    pin_mult[j] = pin_nets[j] ?
                        parse_bus_name(pin_nets[j], &pbl[j],
                                       &pin_hi[j], &pin_lo[j], &pin_kind[j]) : 0;
                }
                char **rep_nets = xs_xmalloc(sizeof(char *) * (size_t)ins->sym->npins);
                for (int k = 0; k < inst_mult; k++) {
                    int bit = inst_hi + step * k;
                    char rep_name[256];
                    render_bus_scalar(rep_name, sizeof rep_name, iname,
                                      inst_base_len, bit, inst_kind);
                    /* Per-replica pin nets */
                    for (int j = 0; j < ins->sym->npins; j++) {
                        if (pin_mult[j] == inst_mult) {
                            int pstep = (pin_hi[j] >= pin_lo[j]) ? -1 : 1;
                            int pbit = pin_hi[j] + pstep * k;
                            char buf[256];
                            render_bus_scalar(buf, sizeof buf, pin_nets[j],
                                              pbl[j], pbit, pin_kind[j]);
                            rep_nets[j] = xs_strdup(buf);
                        } else {
                            rep_nets[j] = NULL;  /* signal to use original */
                        }
                    }
                    /* Build a temporary pin_nets array with overrides. */
                    char **rpn = xs_xmalloc(sizeof(char *) * (size_t)ins->sym->npins);
                    for (int j = 0; j < ins->sym->npins; j++) {
                        rpn[j] = rep_nets[j] ? rep_nets[j] : pin_nets[j];
                    }
                    emit_subst_format_one(fmt, ins, rpn, symshort, rep_name, out);
                    fputc('\n', out);
                    for (int j = 0; j < ins->sym->npins; j++) free(rep_nets[j]);
                    free(rpn);
                }
                free(pin_mult);
                free(pin_hi);
                free(pin_lo);
                free(pin_kind);
                free(pbl);
                free(rep_nets);
            }
            free(iname);

            /* If the instance has savecurrent=true (or symbol template default
             * resolves to true), emit a .save line — this matches xschem's
             * behavior for vsource/ammeter when savecurrent is enabled. */
            char *sc = get_resolved_prop(ins, "savecurrent");
            if (sc && strcmp(sc, "true") == 0) {
                char *iname2 = xs_prop_get(ins->prop, "name");
                if (iname2 && *iname2) {
                    /* lower-case name for SPICE i(...) */
                    fputs(".save i(", out);
                    for (char *q = iname2; *q; q++) fputc(tolower((unsigned char)*q), out);
                    fputs(")\n", out);
                }
                free(iname2);
            }
            free(sc);
            free(symshort);
            free(pin_nets);
        }
    }

    fprintf(out, ".ends\n");
    fprintf(out, ".end\n");

    /* cleanup */
    for (int i = 0; i < nnets; i++) free(net_label[i]);
    free(net_label);
    free(net_label_prio);
    free(net_used);
    for (int i = 0; i < nports; i++) {
        free(port_names[i]);
        free(port_kinds[i]);
    }
    free(port_names);
    free(port_kinds);
    free(root_idx);
    free(U.parent);
    free(wire_a);
    free(wire_b);
    free(inst_pin_off);
    free(V);
    return 0;
}
