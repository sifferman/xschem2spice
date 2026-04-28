#include "parser.h"
#include "strutil.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------- file slurp ---------------- */

static char *slurp(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return NULL; }
    char *buf = xs_xmalloc((size_t)n + 1);
    size_t got = fread(buf, 1, (size_t)n, f);
    buf[got] = '\0';
    fclose(f);
    if (out_len) *out_len = got;
    return buf;
}

/* ---------------- tokenizer ---------------- */

typedef struct {
    const char *p;       /* current pointer */
    const char *end;     /* one past end */
    const char *path;    /* for error msgs */
    int line;
} tok_t;

static void tok_init(tok_t *t, const char *buf, size_t len, const char *path)
{
    t->p = buf;
    t->end = buf + len;
    t->path = path;
    t->line = 1;
}

static int tok_eof(tok_t *t) { return t->p >= t->end; }

static void tok_skip_ws(tok_t *t)
{
    while (t->p < t->end) {
        char c = *t->p;
        if (c == ' ' || c == '\t' || c == '\r') { t->p++; continue; }
        if (c == '\n') { t->line++; t->p++; continue; }
        break;
    }
}

/* Skip whitespace including newlines until we see start of next record. */
static int tok_peek_record_start(tok_t *t)
{
    tok_skip_ws(t);
    if (tok_eof(t)) return -1;
    return (unsigned char)*t->p;
}

/* Read a numeric token (double). Whitespace-skipped first. */
static int tok_number(tok_t *t, double *out)
{
    tok_skip_ws(t);
    if (tok_eof(t)) return -1;
    char *end;
    double v = strtod(t->p, &end);
    if (end == t->p) return -1;
    t->p = end;
    *out = v;
    return 0;
}

static int tok_int(tok_t *t, int *out)
{
    double v;
    if (tok_number(t, &v) != 0) return -1;
    *out = (int)v;
    return 0;
}

/*
 * Read a brace-delimited block. Skip whitespace; expect '{'.
 * Returns malloc'd contents (without the outer braces).
 * Inside, count balance of '{' and '}', honor '\\', '\{', '\}'.
 * (xschem uses Tcl-like list semantics; this approximation suffices
 *  for the records we care about.)
 */
static char *tok_brace(tok_t *t)
{
    tok_skip_ws(t);
    if (tok_eof(t) || *t->p != '{') return NULL;
    t->p++;                       /* consume '{' */
    const char *start = t->p;
    int depth = 1;
    while (t->p < t->end && depth > 0) {
        char c = *t->p;
        if (c == '\\' && t->p + 1 < t->end) {
            if (t->p[1] == '\n') t->line++;
            t->p += 2;
            continue;
        }
        if (c == '{') depth++;
        else if (c == '}') { depth--; if (depth == 0) break; }
        else if (c == '\n') t->line++;
        t->p++;
    }
    if (depth != 0) return NULL;
    size_t n = (size_t)(t->p - start);
    char *r = xs_strndup(start, n);
    t->p++;                       /* consume final '}' */
    return r;
}

/* Skip a brace-delimited block. */
static int tok_skip_brace(tok_t *t)
{
    char *s = tok_brace(t);
    if (!s) return -1;
    free(s);
    return 0;
}

/* Skip rest of line (used to skip records we don't care about whose syntax
 * we don't fully parse — but we DO need to consume their brace block.) */

/* ---------------- property block parsing ---------------- */

/*
 * The property block format is a Tcl-list-ish "key=value key=value ..." —
 * key is alnum/underscore, value is bare token (no whitespace) OR a
 * "..."-quoted string with \" / \\ escapes. Separators are whitespace
 * (incl. newlines).
 *
 * xs_prop_get returns a malloc'd, escape-decoded copy of the value or NULL.
 */
char *xs_prop_get(const char *prop, const char *key)
{
    if (!prop || !key) return NULL;
    size_t klen = strlen(key);
    const char *p = prop;
    while (*p) {
        /* skip whitespace */
        while (*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
            p++;
        if (!*p) break;
        /* read key */
        const char *kstart = p;
        while (*p && (isalnum((unsigned char)*p) || *p == '_')) p++;
        size_t curkl = (size_t)(p - kstart);
        int matched = (curkl == klen && memcmp(kstart, key, klen) == 0);
        /* skip optional '=' */
        if (*p != '=') {
            /* bare key with no value — skip to next whitespace */
            while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r')
                p++;
            if (matched) return xs_strdup("");
            continue;
        }
        p++; /* '=' */
        /* read value: bare or quoted */
        xs_str val;
        xs_str_init(&val);
        if (*p == '"') {
            p++;
            while (*p && *p != '"') {
                if (*p == '\\' && p[1]) {
                    char nx = p[1];
                    if (nx == 'n') xs_str_putc(&val, '\n');
                    else if (nx == 't') xs_str_putc(&val, '\t');
                    else if (nx == '"') xs_str_putc(&val, '"');
                    else if (nx == '\\') xs_str_putc(&val, '\\');
                    else { xs_str_putc(&val, nx); }
                    p += 2;
                } else {
                    xs_str_putc(&val, *p);
                    p++;
                }
            }
            if (*p == '"') p++;
        } else {
            /* bare value: until whitespace */
            while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') {
                if (*p == '\\' && p[1]) {
                    xs_str_putc(&val, p[1]);
                    p += 2;
                } else {
                    xs_str_putc(&val, *p);
                    p++;
                }
            }
        }
        if (matched) {
            char *r = val.buf ? val.buf : xs_strdup("");
            /* don't xs_str_free; we're handing buf out (if buf alloc'd) */
            if (!val.buf) r = xs_strdup("");
            return r;
        }
        xs_str_free(&val);
    }
    return NULL;
}

char *xs_props_merge(const char *defaults, const char *overrides)
{
    xs_str s;
    xs_str_init(&s);
    if (overrides && *overrides) {
        xs_str_puts(&s, overrides);
    }
    if (defaults && *defaults) {
        /* For each key in defaults, append only if missing in overrides */
        const char *p = defaults;
        while (*p) {
            while (*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
            if (!*p) break;
            const char *kstart = p;
            while (*p && (isalnum((unsigned char)*p) || *p == '_')) p++;
            size_t klen = (size_t)(p - kstart);
            if (klen == 0) {
                /* skip to whitespace */
                while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') p++;
                continue;
            }
            char *kbuf = xs_strndup(kstart, klen);
            char *existing = xs_prop_get(overrides, kbuf);
            /* now scan past the value to know where this defaults entry ends */
            const char *vstart = p;
            if (*p == '=') {
                p++;
                if (*p == '"') {
                    p++;
                    while (*p && *p != '"') {
                        if (*p == '\\' && p[1]) p += 2;
                        else p++;
                    }
                    if (*p == '"') p++;
                } else {
                    while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') {
                        if (*p == '\\' && p[1]) p += 2;
                        else p++;
                    }
                }
            }
            if (!existing) {
                /* append "<key>=<rest>" */
                if (s.len > 0) xs_str_putc(&s, '\n');
                xs_str_putn(&s, kstart, (size_t)(p - kstart));
                /* if key alone with no value, fine */
                if (vstart == p) {
                    /* no value — nothing more to add */
                }
            }
            free(existing);
            free(kbuf);
        }
    }
    return s.buf ? s.buf : xs_strdup("");
}

/* ---------------- coordinate transform ---------------- */

void xs_apply_xform(int rot, int flip, double sx, double sy,
                    double *gx, double *gy);
void xs_apply_xform(int rot, int flip, double sx, double sy,
                    double *gx, double *gy)
{
    /* Mirror upstream xschem ROTATION macro from xschem.h, with x0=y0=0:
     *
     *   xxtmp = flip ? -sx : sx
     *   rot==0: rx = xxtmp,  ry = sy
     *   rot==1: rx = -sy,    ry = xxtmp
     *   rot==2: rx = -xxtmp, ry = -sy
     *   rot==3: rx = sy,     ry = -xxtmp
     *
     * (There is a different `ROTATION` macro elsewhere in netlist.c that
     * uses pre-flip `xx` for the y-component — DON'T use it; xschem.h's is
     * the authoritative one used by `get_inst_pin_coord`.) */
    double xxtmp = flip ? -sx : sx;
    double rx = 0, ry = 0;
    if (rot == 0)      { rx = xxtmp;  ry = sy; }
    else if (rot == 1) { rx = -sy;    ry = xxtmp; }
    else if (rot == 2) { rx = -xxtmp; ry = -sy; }
    else /* rot==3 */  { rx = sy;     ry = -xxtmp; }
    *gx = rx;
    *gy = ry;
}

/* ---------------- record skippers ---------------- */

/* T {text} x y rot flip xs ys {props} */
static int skip_record_T(tok_t *t)
{
    if (tok_skip_brace(t) != 0) return -1;
    double v;
    for (int i = 0; i < 6; i++) if (tok_number(t, &v) != 0) return -1;
    return tok_skip_brace(t);
}

/* L color x1 y1 x2 y2 {props}; B color x1 y1 x2 y2 {props} */
static int skip_record_LB(tok_t *t)
{
    double v;
    for (int i = 0; i < 5; i++) if (tok_number(t, &v) != 0) return -1;
    return tok_skip_brace(t);
}

/* A color x y r a b {props} */
static int skip_record_A(tok_t *t)
{
    double v;
    for (int i = 0; i < 6; i++) if (tok_number(t, &v) != 0) return -1;
    return tok_skip_brace(t);
}

/* P color N x1 y1 ... xN yN {props} */
static int skip_record_P(tok_t *t)
{
    double v;
    int color, n;
    if (tok_int(t, &color) != 0) return -1;
    if (tok_int(t, &n) != 0) return -1;
    for (int i = 0; i < 2 * n; i++) if (tok_number(t, &v) != 0) return -1;
    return tok_skip_brace(t);
}

/* ---------------- schematic parsing ---------------- */

static void push_wire(xs_schematic *sch, xs_wire w)
{
    sch->wires = xs_xrealloc(sch->wires,
                             sizeof(xs_wire) * (size_t)(sch->nwires + 1));
    sch->wires[sch->nwires++] = w;
}
static void push_inst(xs_schematic *sch, xs_instance i)
{
    sch->instances = xs_xrealloc(sch->instances,
                                 sizeof(xs_instance) * (size_t)(sch->ninstances + 1));
    sch->instances[sch->ninstances++] = i;
}

static int parse_schematic_buf(const char *buf, size_t len,
                               const char *path,
                               xs_schematic *sch)
{
    tok_t t;
    tok_init(&t, buf, len, path);

    while (!tok_eof(&t)) {
        int ch = tok_peek_record_start(&t);
        if (ch < 0) break;
        char tag = (char)ch;
        t.p++;                /* consume tag char */

        switch (tag) {
        case 'v': case 'G': case 'K': case 'V': case 'S': case 'E': case 'F':
            if (tok_skip_brace(&t) != 0) {
                fprintf(stderr, "%s:%d: expected {} after '%c'\n",
                        path, t.line, tag);
                return -1;
            }
            break;
        case 'N': {
            double x1, y1, x2, y2;
            if (tok_number(&t, &x1) != 0 || tok_number(&t, &y1) != 0 ||
                tok_number(&t, &x2) != 0 || tok_number(&t, &y2) != 0) {
                fprintf(stderr, "%s:%d: bad N record\n", path, t.line);
                return -1;
            }
            char *prop = tok_brace(&t);
            xs_wire w = {x1, y1, x2, y2, prop};
            push_wire(sch, w);
            break;
        }
        case 'C': {
            char *symref = tok_brace(&t);
            if (!symref) {
                fprintf(stderr, "%s:%d: bad C record (no symref)\n",
                        path, t.line);
                return -1;
            }
            double x, y;
            int rot = 0, flip = 0;
            if (tok_number(&t, &x) != 0 || tok_number(&t, &y) != 0 ||
                tok_int(&t, &rot) != 0 || tok_int(&t, &flip) != 0) {
                fprintf(stderr, "%s:%d: bad C record (numbers)\n",
                        path, t.line);
                free(symref);
                return -1;
            }
            char *prop = tok_brace(&t);
            xs_instance ins = {0};
            ins.symref = symref;
            ins.x = x; ins.y = y;
            ins.rot = rot; ins.flip = flip;
            ins.prop = prop;
            push_inst(sch, ins);
            break;
        }
        case 'T':
            if (skip_record_T(&t) != 0) return -1;
            break;
        case 'L': case 'B':
            if (skip_record_LB(&t) != 0) return -1;
            break;
        case 'A':
            if (skip_record_A(&t) != 0) return -1;
            break;
        case 'P':
            if (skip_record_P(&t) != 0) return -1;
            break;
        case '#':
            /* comment to end-of-line — not part of xschem .sch but be lenient */
            while (!tok_eof(&t) && *t.p != '\n') t.p++;
            break;
        default:
            fprintf(stderr, "%s:%d: unknown record '%c'\n", path, t.line, tag);
            return -1;
        }
    }
    return 0;
}

static char *basename_no_ext(const char *path, const char *ext)
{
    const char *slash = strrchr(path, '/');
    const char *base = slash ? slash + 1 : path;
    size_t bl = strlen(base);
    size_t el = ext ? strlen(ext) : 0;
    if (el && bl > el && strcmp(base + bl - el, ext) == 0) bl -= el;
    return xs_strndup(base, bl);
}

int xs_parse_schematic(const char *path, xs_schematic *out)
{
    memset(out, 0, sizeof *out);
    size_t len;
    char *buf = slurp(path, &len);
    if (!buf) {
        fprintf(stderr, "xschem2spice: cannot open %s\n", path);
        return -1;
    }
    out->path = xs_strdup(path);
    out->cell_name = basename_no_ext(path, ".sch");
    int rc = parse_schematic_buf(buf, len, path, out);
    free(buf);
    return rc;
}

void xs_free_schematic(xs_schematic *s)
{
    if (!s) return;
    for (int i = 0; i < s->nwires; i++) free(s->wires[i].prop);
    free(s->wires);
    for (int i = 0; i < s->ninstances; i++) {
        free(s->instances[i].symref);
        free(s->instances[i].prop);
    }
    free(s->instances);
    free(s->path);
    free(s->cell_name);
    memset(s, 0, sizeof *s);
}

/* ---------------- symbol parsing ---------------- */

static void push_pin(xs_symbol *sym, xs_pin pin)
{
    sym->pins = xs_xrealloc(sym->pins,
                            sizeof(xs_pin) * (size_t)(sym->npins + 1));
    sym->pins[sym->npins++] = pin;
}

static int parse_symbol_buf(const char *buf, size_t len,
                            const char *path,
                            xs_symbol *sym)
{
    tok_t t;
    tok_init(&t, buf, len, path);

    while (!tok_eof(&t)) {
        int ch = tok_peek_record_start(&t);
        if (ch < 0) break;
        char tag = (char)ch;
        t.p++;

        switch (tag) {
        case 'v': case 'V': case 'S': case 'E': case 'F':
            if (tok_skip_brace(&t) != 0) {
                fprintf(stderr, "%s:%d: expected {} after '%c'\n",
                        path, t.line, tag);
                return -1;
            }
            break;
        case 'K': case 'G': {
            /* Both K and G blocks may carry type/format/template metadata
             * (devices/code.sym uses G; most symbols use K). Honor whichever
             * is non-empty; later blocks fill in fields not yet set. */
            char *kprops = tok_brace(&t);
            if (!kprops) return -1;
#define ABSORB(field, key) do { \
                if (!sym->field) sym->field = xs_prop_get(kprops, key); \
            } while (0)
            ABSORB(type,         "type");
            ABSORB(format,       "format");
            ABSORB(lvs_format,   "lvs_format");
            ABSORB(template_,    "template");
            ABSORB(extra,        "extra");
            ABSORB(spice_ignore, "spice_ignore");
#undef ABSORB
            free(kprops);
            break;
        }
        case 'B': {
            int color;
            double x1, y1, x2, y2;
            if (tok_int(&t, &color) != 0 ||
                tok_number(&t, &x1) != 0 || tok_number(&t, &y1) != 0 ||
                tok_number(&t, &x2) != 0 || tok_number(&t, &y2) != 0) {
                fprintf(stderr, "%s:%d: bad B record\n", path, t.line);
                return -1;
            }
            char *prop = tok_brace(&t);
            /* In symbols, color==5 marks a pin box (typically). To be safe,
             * include any B with a name= property as a pin. */
            char *pname = xs_prop_get(prop, "name");
            char *pdir = xs_prop_get(prop, "dir");
            if (pname) {
                xs_pin p;
                p.name = pname;          /* takes ownership */
                p.dir  = pdir ? pdir : xs_strdup("inout");
                p.x = (x1 + x2) / 2.0;
                p.y = (y1 + y2) / 2.0;
                push_pin(sym, p);
            } else {
                free(pname);
                free(pdir);
            }
            free(prop);
            break;
        }
        case 'L':
            if (skip_record_LB(&t) != 0) return -1;
            break;
        case 'T':
            if (skip_record_T(&t) != 0) return -1;
            break;
        case 'A':
            if (skip_record_A(&t) != 0) return -1;
            break;
        case 'P':
            if (skip_record_P(&t) != 0) return -1;
            break;
        case 'N': {
            /* symbols can technically have wires too — skip them */
            double v;
            for (int i = 0; i < 4; i++) if (tok_number(&t, &v) != 0) return -1;
            if (tok_skip_brace(&t) != 0) return -1;
            break;
        }
        case 'C': {
            /* nested instance refs in a symbol — skip for now */
            if (tok_skip_brace(&t) != 0) return -1;
            double v; int iv;
            if (tok_number(&t, &v) != 0 || tok_number(&t, &v) != 0 ||
                tok_int(&t, &iv) != 0 || tok_int(&t, &iv) != 0) return -1;
            if (tok_skip_brace(&t) != 0) return -1;
            break;
        }
        case '#':
            while (!tok_eof(&t) && *t.p != '\n') t.p++;
            break;
        default:
            fprintf(stderr, "%s:%d: unknown symbol record '%c'\n", path, t.line, tag);
            return -1;
        }
    }
    return 0;
}

int xs_parse_symbol(const char *path, xs_symbol *out)
{
    memset(out, 0, sizeof *out);
    size_t len;
    char *buf = slurp(path, &len);
    if (!buf) {
        fprintf(stderr, "xschem2spice: cannot open symbol %s\n", path);
        return -1;
    }
    out->path = xs_strdup(path);
    out->name = basename_no_ext(path, ".sym");
    int rc = parse_symbol_buf(buf, len, path, out);
    free(buf);
    return rc;
}

void xs_free_symbol(xs_symbol *s)
{
    if (!s) return;
    for (int i = 0; i < s->npins; i++) {
        free(s->pins[i].name);
        free(s->pins[i].dir);
    }
    free(s->pins);
    free(s->name);
    free(s->path);
    free(s->type);
    free(s->format);
    free(s->lvs_format);
    free(s->template_);
    free(s->extra);
    free(s->spice_ignore);
    memset(s, 0, sizeof *s);
}
