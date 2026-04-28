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

#include "parser.h"
#include "strutil.h"

#include <ctype.h>
#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------- file slurp ---------------- */

static char *read_entire_file(const char *path, size_t *out_length)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long size = ftell(f);
    if (size < 0) { fclose(f); return NULL; }
    rewind(f);
    char *buf = xs_xmalloc((size_t)size + 1);
    size_t got = fread(buf, 1, (size_t)size, f);
    buf[got] = '\0';
    fclose(f);
    if (out_length) *out_length = got;
    return buf;
}

/* ---------------- tokenizer ---------------- */

typedef struct {
    const char *cursor;
    const char *end;
    const char *file_path;
    int         line_number;
} parse_cursor;

static void cursor_init(parse_cursor *p, const char *buf, size_t length, const char *path)
{
    p->cursor      = buf;
    p->end         = buf + length;
    p->file_path   = path;
    p->line_number = 1;
}

static int cursor_eof(const parse_cursor *p) { return p->cursor >= p->end; }

static void skip_whitespace(parse_cursor *p)
{
    while (p->cursor < p->end) {
        char c = *p->cursor;
        if (c == '\n') { p->line_number++; p->cursor++; continue; }
        if (c == ' ' || c == '\t' || c == '\r') { p->cursor++; continue; }
        break;
    }
}

static int peek_record_tag(parse_cursor *p)
{
    skip_whitespace(p);
    if (cursor_eof(p)) return -1;
    return (unsigned char)*p->cursor;
}

static int read_required_double_token(parse_cursor *p, double *out)
{
    skip_whitespace(p);
    if (cursor_eof(p)) return -1;
    char *end;
    double v = strtod(p->cursor, &end);
    if (end == p->cursor) return -1;
    p->cursor = end;
    *out = v;
    return 0;
}

static int read_required_integer_token(parse_cursor *p, int *out)
{
    double v;
    if (read_required_double_token(p, &v) != 0) return -1;
    *out = (int)v;
    return 0;
}

/* Read a `{...}` block. Brace balance is honoured; `\\` escapes the next
 * character (so `\}` doesn't close the block). Returns a malloc'd copy of
 * the block contents (without outer braces) or NULL on syntax error. */
static char *read_brace_block(parse_cursor *p)
{
    skip_whitespace(p);
    if (cursor_eof(p) || *p->cursor != '{') return NULL;
    p->cursor++;
    const char *start = p->cursor;
    int depth = 1;
    while (p->cursor < p->end && depth > 0) {
        char c = *p->cursor;
        if (c == '\\' && p->cursor + 1 < p->end) {
            if (p->cursor[1] == '\n') p->line_number++;
            p->cursor += 2;
            continue;
        }
        if (c == '{') depth++;
        else if (c == '}') { depth--; if (depth == 0) break; }
        else if (c == '\n') p->line_number++;
        p->cursor++;
    }
    if (depth != 0) return NULL;
    char *contents = xs_strndup(start, (size_t)(p->cursor - start));
    p->cursor++;
    return contents;
}

static int skip_brace_block(parse_cursor *p)
{
    char *s = read_brace_block(p);
    if (!s) return -1;
    free(s);
    return 0;
}

/* ---------------- key=value property parsing ---------------- */
/*
 * Mirrors get_tok_value() in xschem
 *   https://github.com/StefanSchippers/xschem/blob/3.4.7/src/token.c#L438-L532
 * (we accept the same tokens it produces; full Tcl-list-style nesting is not
 * needed for any record we care about).
 */
char *xs_prop_get(const char *property_block, const char *key)
{
    if (!property_block || !key) return NULL;
    size_t key_length = strlen(key);

    for (const char *p = property_block; *p; ) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
        if (!*p) break;

        const char *current_key_start = p;
        while (*p && (isalnum((unsigned char)*p) || *p == '_')) p++;
        size_t current_key_length = (size_t)(p - current_key_start);
        int    is_match = (current_key_length == key_length &&
                           memcmp(current_key_start, key, key_length) == 0);

        if (*p != '=') {
            /* Bare key with no value (treat as empty). */
            while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') p++;
            if (is_match) return xs_strdup("");
            continue;
        }
        p++;

        xs_string_buffer value;
        xs_string_buffer_init(&value);
        if (*p == '"') {
            p++;
            while (*p && *p != '"') {
                if (*p == '\\' && p[1]) {
                    char next = p[1];
                    if      (next == 'n')  xs_string_buffer_append_char(&value, '\n');
                    else if (next == 't')  xs_string_buffer_append_char(&value, '\t');
                    else if (next == '"')  xs_string_buffer_append_char(&value, '"');
                    else if (next == '\\') xs_string_buffer_append_char(&value, '\\');
                    else                   xs_string_buffer_append_char(&value, next);
                    p += 2;
                } else {
                    xs_string_buffer_append_char(&value, *p++);
                }
            }
            if (*p == '"') p++;
        } else {
            while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') {
                if (*p == '\\' && p[1]) {
                    xs_string_buffer_append_char(&value, p[1]);
                    p += 2;
                } else {
                    xs_string_buffer_append_char(&value, *p++);
                }
            }
        }

        if (is_match) {
            char *result = value.buffer ? value.buffer : xs_strdup("");
            return result;
        }
        xs_string_buffer_free(&value);
    }
    return NULL;
}

/* ---------------- coordinate transform ---------------- */
/*
 * XSCHEM 3.4.7 `ROTATION` macro from xschem.h:
 *   https://github.com/StefanSchippers/xschem/blob/3.4.7/src/xschem.h#L339-L346
 *
 *     xxtmp = flip ? 2*x0 - x : x;
 *     rot==0: rx = xxtmp;          ry = y;
 *     rot==1: rx = x0 - y + y0;    ry = y0 + xxtmp - x0;
 *     rot==2: rx = 2*x0 - xxtmp;   ry = 2*y0 - y;
 *     rot==3: rx = x0 + y - y0;    ry = y0 - xxtmp + x0;
 *
 * Specialised here to (x0, y0) = (0, 0); callers add the instance origin.
 *
 * Note: there is a *different* rotation macro spelled the same way inside
 * xschem/src/netlist.c — that one is buggy when `flip` is set (uses the
 * pre-flip x for the rotated y component). xschem.h's macro is the version
 * called by `get_inst_pin_coord` and is the authoritative one to mirror.
 */
void xs_transform_pin_to_global(int rotation, int flip,
                                double x_in, double y_in,
                                double *x_out, double *y_out)
{
    double x_after_flip = flip ? -x_in : x_in;
    switch (rotation) {
        case 0:  *x_out =  x_after_flip; *y_out =  y_in;        break;
        case 1:  *x_out = -y_in;         *y_out =  x_after_flip; break;
        case 2:  *x_out = -x_after_flip; *y_out = -y_in;        break;
        default: *x_out =  y_in;         *y_out = -x_after_flip; break;
    }
}

/* ---------------- generic record skippers ---------------- */

static int skip_text_record_body(parse_cursor *p)
{
    /* T {text} x y rot flip xs ys {props} */
    if (skip_brace_block(p) != 0) return -1;
    double v;
    for (int i = 0; i < 6; i++) if (read_required_double_token(p, &v) != 0) return -1;
    return skip_brace_block(p);
}
static int skip_line_or_box_record_body(parse_cursor *p)
{
    /* L color x1 y1 x2 y2 {props}; B color x1 y1 x2 y2 {props} */
    double v;
    for (int i = 0; i < 5; i++) if (read_required_double_token(p, &v) != 0) return -1;
    return skip_brace_block(p);
}
static int skip_arc_record_body(parse_cursor *p)
{
    /* A color x y r a b {props} */
    double v;
    for (int i = 0; i < 6; i++) if (read_required_double_token(p, &v) != 0) return -1;
    return skip_brace_block(p);
}
static int skip_polygon_record_body(parse_cursor *p)
{
    /* P color N x1 y1 ... xN yN {props} */
    int color, point_count;
    if (read_required_integer_token(p, &color) != 0)       return -1;
    if (read_required_integer_token(p, &point_count) != 0) return -1;
    double v;
    for (int i = 0; i < 2 * point_count; i++)
        if (read_required_double_token(p, &v) != 0) return -1;
    return skip_brace_block(p);
}

static char *basename_without_extension(const char *path, const char *extension)
{
    const char *slash = strrchr(path, '/');
    const char *base  = slash ? slash + 1 : path;
    size_t len = strlen(base);
    size_t ext = extension ? strlen(extension) : 0;
    if (ext && len > ext && strcmp(base + len - ext, extension) == 0) len -= ext;
    return xs_strndup(base, len);
}

/* ---------------- schematic parsing ---------------- */

static void schematic_append_wire(xs_schematic *s, xs_wire wire)
{
    s->wires = xs_xrealloc(s->wires, sizeof(xs_wire) * (size_t)(s->wire_count + 1));
    s->wires[s->wire_count++] = wire;
}

static void schematic_append_instance(xs_schematic *s, xs_instance instance)
{
    s->instances = xs_xrealloc(s->instances,
                               sizeof(xs_instance) * (size_t)(s->instance_count + 1));
    s->instances[s->instance_count++] = instance;
}

static int parse_schematic_buffer(const char *buf, size_t length,
                                  const char *path, xs_schematic *out)
{
    parse_cursor p;
    cursor_init(&p, buf, length, path);

    while (!cursor_eof(&p)) {
        int tag_int = peek_record_tag(&p);
        if (tag_int < 0) break;
        char tag = (char)tag_int;
        p.cursor++;

        switch (tag) {
        case 'v': case 'G': case 'K': case 'V': case 'S': case 'E': case 'F':
            if (skip_brace_block(&p) != 0) {
                fprintf(stderr, "%s:%d: expected {} after '%c'\n",
                        path, p.line_number, tag);
                return -1;
            }
            break;

        case 'N': {
            xs_wire w = {0};
            if (read_required_double_token(&p, &w.x1) != 0 ||
                read_required_double_token(&p, &w.y1) != 0 ||
                read_required_double_token(&p, &w.x2) != 0 ||
                read_required_double_token(&p, &w.y2) != 0) {
                fprintf(stderr, "%s:%d: bad N record\n", path, p.line_number);
                return -1;
            }
            w.prop_block = read_brace_block(&p);
            schematic_append_wire(out, w);
            break;
        }

        case 'C': {
            char *symref = read_brace_block(&p);
            if (!symref) {
                fprintf(stderr, "%s:%d: bad C record (missing symref)\n",
                        path, p.line_number);
                return -1;
            }
            xs_instance ins = {0};
            ins.symref = symref;
            if (read_required_double_token(&p, &ins.x) != 0 ||
                read_required_double_token(&p, &ins.y) != 0 ||
                read_required_integer_token(&p, &ins.rotation) != 0 ||
                read_required_integer_token(&p, &ins.flip) != 0) {
                fprintf(stderr, "%s:%d: bad C record (numbers)\n",
                        path, p.line_number);
                free(symref);
                return -1;
            }
            ins.prop_block = read_brace_block(&p);
            schematic_append_instance(out, ins);
            break;
        }

        case 'T':
            if (skip_text_record_body(&p) != 0) return -1;
            break;
        case 'L':
        case 'B':
            if (skip_line_or_box_record_body(&p) != 0) return -1;
            break;
        case 'A':
            if (skip_arc_record_body(&p) != 0) return -1;
            break;
        case 'P':
            if (skip_polygon_record_body(&p) != 0) return -1;
            break;
        case '#':
            while (!cursor_eof(&p) && *p.cursor != '\n') p.cursor++;
            break;

        default:
            fprintf(stderr, "%s:%d: unknown record '%c'\n",
                    path, p.line_number, tag);
            return -1;
        }
    }
    return 0;
}

int xs_parse_schematic(const char *path, xs_schematic *out)
{
    memset(out, 0, sizeof *out);
    size_t length;
    char *buf = read_entire_file(path, &length);
    if (!buf) {
        fprintf(stderr, "xschem2spice: cannot open %s\n", path);
        return -1;
    }
    out->path      = xs_strdup(path);
    out->cell_name = basename_without_extension(path, ".sch");
    int rc = parse_schematic_buffer(buf, length, path, out);
    free(buf);
    return rc;
}

void xs_free_schematic(xs_schematic *s)
{
    if (!s) return;
    for (int i = 0; i < s->wire_count;     i++) free(s->wires[i].prop_block);
    free(s->wires);
    for (int i = 0; i < s->instance_count; i++) {
        free(s->instances[i].symref);
        free(s->instances[i].prop_block);
    }
    free(s->instances);
    free(s->path);
    free(s->cell_name);
    memset(s, 0, sizeof *s);
}

/* ---------------- symbol parsing ---------------- */

static void symbol_append_pin(xs_symbol *sym, xs_symbol_pin pin)
{
    sym->pins = xs_xrealloc(sym->pins,
                            sizeof(xs_symbol_pin) * (size_t)(sym->pin_count + 1));
    sym->pins[sym->pin_count++] = pin;
}

/* xschem stashes the canonical metadata in K, but `devices/code.sym` puts it
 * in G; honour either, with first-wins semantics so K beats G. */
static void absorb_kg_block_into_symbol(xs_symbol *sym, const char *kprops)
{
    #define ABSORB_FIELD(field, key) \
        do { if (!sym->field) sym->field = xs_prop_get(kprops, key); } while (0)
    ABSORB_FIELD(type,         "type");
    ABSORB_FIELD(format,       "format");
    ABSORB_FIELD(lvs_format,   "lvs_format");
    ABSORB_FIELD(template_,    "template");
    ABSORB_FIELD(extra,        "extra");
    ABSORB_FIELD(spice_ignore, "spice_ignore");
    #undef ABSORB_FIELD
}

static int parse_symbol_buffer(const char *buf, size_t length,
                               const char *path, xs_symbol *sym)
{
    parse_cursor p;
    cursor_init(&p, buf, length, path);

    while (!cursor_eof(&p)) {
        int tag_int = peek_record_tag(&p);
        if (tag_int < 0) break;
        char tag = (char)tag_int;
        p.cursor++;

        switch (tag) {
        case 'v': case 'V': case 'S': case 'E': case 'F':
            if (skip_brace_block(&p) != 0) {
                fprintf(stderr, "%s:%d: expected {} after '%c'\n",
                        path, p.line_number, tag);
                return -1;
            }
            break;

        case 'K': case 'G': {
            char *kprops = read_brace_block(&p);
            if (!kprops) return -1;
            absorb_kg_block_into_symbol(sym, kprops);
            free(kprops);
            break;
        }

        case 'B': {
            int    color;
            double x1, y1, x2, y2;
            if (read_required_integer_token(&p, &color) != 0 ||
                read_required_double_token(&p, &x1) != 0 ||
                read_required_double_token(&p, &y1) != 0 ||
                read_required_double_token(&p, &x2) != 0 ||
                read_required_double_token(&p, &y2) != 0) {
                fprintf(stderr, "%s:%d: bad B record\n", path, p.line_number);
                return -1;
            }
            char *props    = read_brace_block(&p);
            char *pin_name = xs_prop_get(props, "name");
            char *pin_dir  = xs_prop_get(props, "dir");
            if (pin_name) {
                xs_symbol_pin pin;
                pin.name = pin_name;
                pin.dir  = pin_dir ? pin_dir : xs_strdup("inout");
                pin.x    = (x1 + x2) / 2.0;
                pin.y    = (y1 + y2) / 2.0;
                symbol_append_pin(sym, pin);
            } else {
                free(pin_name);
                free(pin_dir);
            }
            free(props);
            break;
        }

        case 'L':
            if (skip_line_or_box_record_body(&p) != 0) return -1;
            break;
        case 'T':
            if (skip_text_record_body(&p) != 0) return -1;
            break;
        case 'A':
            if (skip_arc_record_body(&p) != 0) return -1;
            break;
        case 'P':
            if (skip_polygon_record_body(&p) != 0) return -1;
            break;
        case 'N': {
            double v;
            for (int i = 0; i < 4; i++)
                if (read_required_double_token(&p, &v) != 0) return -1;
            if (skip_brace_block(&p) != 0) return -1;
            break;
        }
        case 'C': {
            if (skip_brace_block(&p) != 0) return -1;
            double v; int iv;
            if (read_required_double_token(&p, &v) != 0 ||
                read_required_double_token(&p, &v) != 0 ||
                read_required_integer_token(&p, &iv) != 0 ||
                read_required_integer_token(&p, &iv) != 0) return -1;
            if (skip_brace_block(&p) != 0) return -1;
            break;
        }
        case '#':
            while (!cursor_eof(&p) && *p.cursor != '\n') p.cursor++;
            break;

        default:
            fprintf(stderr, "%s:%d: unknown symbol record '%c'\n",
                    path, p.line_number, tag);
            return -1;
        }
    }
    return 0;
}

int xs_parse_symbol(const char *path, xs_symbol *out)
{
    memset(out, 0, sizeof *out);
    size_t length;
    char *buf = read_entire_file(path, &length);
    if (!buf) {
        fprintf(stderr, "xschem2spice: cannot open symbol %s\n", path);
        return -1;
    }
    out->path = xs_strdup(path);
    out->name = basename_without_extension(path, ".sym");
    int rc = parse_symbol_buffer(buf, length, path, out);
    free(buf);
    return rc;
}

void xs_free_symbol(xs_symbol *s)
{
    if (!s) return;
    for (int i = 0; i < s->pin_count; i++) {
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
