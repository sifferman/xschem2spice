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
 * spice_netlist() / global_spice_netlist():
 * https://github.com/StefanSchippers/xschem/blob/3.4.7/src/spice_netlist.c#L171-L249
 * https://github.com/StefanSchippers/xschem/blob/3.4.7/src/spice_netlist.c#L252-L607
 */

#include "netlist.h"
#include "hash.h"
#include "strutil.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NET_LABEL_PRIO_NONE     0
#define NET_LABEL_PRIO_LAB_PIN  2
#define NET_LABEL_PRIO_PORT     3

/* ============================================================ *
 * Symbol-type predicates and small helpers
 * ============================================================ */

static int symbol_type_is_port(const char *type)
{
    return type && (!strcmp(type, "ipin") || !strcmp(type, "opin") ||
                    !strcmp(type, "iopin"));
}

static int symbol_type_is_label_or_pin(const char *type)
{
    return symbol_type_is_port(type) || (type && !strcmp(type, "label"));
}

static int symbol_type_equals(const char *type, const char *want)
{
    return type && strcmp(type, want) == 0;
}

/* xschem's strboolcmp accepts "true", "1", "yes" (case-insensitive) plus the
 * special token "short" for spice_ignore. */
static int spice_ignore_value_is_truthy(const char *v)
{
    return v && (!strcmp(v, "true") || !strcmp(v, "1") ||
                 !strcmp(v, "yes")  || !strcmp(v, "short"));
}

static char *symref_basename_without_extension(const char *symref)
{
    const char *slash = strrchr(symref, '/');
    const char *base  = slash ? slash + 1 : symref;
    size_t len = strlen(base);
    if (len >= 4 && (!strcmp(base + len - 4, ".sym") ||
                     !strcmp(base + len - 4, ".sch"))) len -= 4;
    return xs_strndup(base, len);
}

/* Look up `key` on the instance, then on the symbol's template default. */
static char *lookup_property_with_template_fallback(const xs_instance *ins,
                                                    const char *key)
{
    char *v = xs_prop_get(ins->prop_block, key);
    if (v && *v) return v;
    free(v);
    if (ins->resolved_symbol && ins->resolved_symbol->template_) {
        char *t = xs_prop_get(ins->resolved_symbol->template_, key);
        if (t) return t;
    }
    return xs_strdup("");
}

/* Trim surrounding whitespace; in lvs_mode also strip a leading `#`
 * (XSCHEM's auto-name marker). */
static char *normalize_net_label(const char *raw, int lvs_mode)
{
    if (!raw) return NULL;
    while (*raw == ' ' || *raw == '\t') raw++;
    if (lvs_mode && *raw == '#') raw++;
    char *r = xs_strdup(raw);
    char *e = r + strlen(r);
    while (e > r && (e[-1] == ' '  || e[-1] == '\t' ||
                     e[-1] == '\n' || e[-1] == '\r')) e--;
    *e = '\0';
    return r;
}

/* ============================================================ *
 * Bus designators (e.g. "DATA[3:0]" or "DIN[15..0]")
 *
 * Mirrors expansion done by XSCHEM's expandlabel.y:
 *   strbus           https://github.com/StefanSchippers/xschem/blob/3.4.7/src/expandlabel.y#L199-L218
 *   strbus_nobracket https://github.com/StefanSchippers/xschem/blob/3.4.7/src/expandlabel.y#L230-L249
 * ============================================================ */

typedef enum { BUS_NONE = 0, BUS_COLON, BUS_DOTDOT } bus_kind;

typedef struct {
    bus_kind kind;
    size_t   base_length;   /* characters before `[` */
    int      hi, lo;
    int      multiplicity;
} bus_designator;

static int parse_bus_designator(const char *s, bus_designator *out)
{
    out->kind = BUS_NONE;
    if (!s) return 0;
    const char *lb = strchr(s, '[');
    if (!lb) return 0;
    int hi, lo;
    if (sscanf(lb, "[%d..%d]", &hi, &lo) == 2)      out->kind = BUS_DOTDOT;
    else if (sscanf(lb, "[%d:%d]",  &hi, &lo) == 2) out->kind = BUS_COLON;
    else return 0;
    out->base_length  = (size_t)(lb - s);
    out->hi           = hi;
    out->lo           = lo;
    out->multiplicity = (hi >= lo) ? (hi - lo + 1) : (lo - hi + 1);
    return out->multiplicity;
}

static void format_bus_scalar(char *buf, size_t bufsz,
                              const char *base, const bus_designator *d, int bit)
{
    if (d->kind == BUS_DOTDOT) {
        snprintf(buf, bufsz, "%.*s%d",   (int)d->base_length, base, bit);
    } else {
        snprintf(buf, bufsz, "%.*s[%d]", (int)d->base_length, base, bit);
    }
}

/* ============================================================ *
 * Symbol resolution and missing.sym placeholder
 * ============================================================ */

static void free_cached_symbol(void *p)
{
    if (!p) return;
    xs_free_symbol((xs_symbol *)p);
    free(p);
}

/* When a referenced symbol can't be resolved, XSCHEM substitutes
 * `systemlib/missing.sym` whose format prints `* @name - @symname IS MISSING
 * !!!!`. We mirror that: see XSCHEM's match_symbol() at
 *   https://github.com/StefanSchippers/xschem/blob/3.4.7/src/token.c#L182-L201
 * and missing.sym at
 *   https://github.com/StefanSchippers/xschem/blob/3.4.7/src/systemlib/missing.sym
 */
static xs_symbol *missing_symbol_placeholder(xs_netlister *nl)
{
    xs_symbol *cached = xs_hash_get(nl->symbol_cache, "__missing__");
    if (cached) return cached;
    xs_symbol *m = xs_xmalloc(sizeof *m);
    memset(m, 0, sizeof *m);
    m->name   = xs_strdup("missing");
    m->path   = xs_strdup("(missing)");
    m->type   = xs_strdup("missing");
    m->format = xs_strdup("*  @name -  @symname  IS MISSING !!!!");
    xs_hash_put(nl->symbol_cache, "__missing__", m);
    return m;
}

/* Allocate and parse a symbol at `path`. Returns NULL on parse error. */
static xs_symbol *parse_symbol_or_free(const char *path)
{
    xs_symbol *sym = xs_xmalloc(sizeof *sym);
    if (xs_parse_symbol(path, sym) == 0) return sym;
    free(sym);
    return NULL;
}

/* Try the path verbatim, then fall back to a libpath lookup of its basename. */
static xs_symbol *parse_symbol_with_libpath_fallback(xs_netlister *nl,
                                                     const char *direct_path)
{
    FILE *probe = fopen(direct_path, "r");
    if (probe) { fclose(probe); return parse_symbol_or_free(direct_path); }

    const char *slash = strrchr(direct_path, '/');
    char *resolved = xs_library_path_resolve(nl->library_path,
                                             slash ? slash + 1 : direct_path);
    if (!resolved) return NULL;
    xs_symbol *sym = parse_symbol_or_free(resolved);
    free(resolved);
    return sym;
}

/* Load `<schematic>.sym` (the symbol view of the schematic itself), if one
 * exists. We use its B-record pin order as the .subckt port list because
 * xschem creates symbols-from-schematics with that as the canonical order. */
static xs_symbol *load_companion_sym_for_schematic(xs_netlister *nl,
                                                   const xs_schematic *sch)
{
    if (!sch->path) return NULL;
    size_t len = strlen(sch->path);
    if (len < 4 || strcmp(sch->path + len - 4, ".sch") != 0) return NULL;

    char *candidate_path = xs_strdup(sch->path);
    memcpy(candidate_path + len - 4, ".sym", 4);
    xs_symbol *sym = parse_symbol_with_libpath_fallback(nl, candidate_path);
    free(candidate_path);
    if (!sym) return NULL;

    char cache_key[512];
    snprintf(cache_key, sizeof cache_key, "__self__:%s", sch->cell_name);
    free_cached_symbol(xs_hash_put(nl->symbol_cache, cache_key, sym));
    return sym;
}

void xs_netlister_init(xs_netlister *nl, xs_library_path *lp, int lvs_mode)
{
    nl->lvs_mode     = lvs_mode;
    nl->library_path = lp;
    nl->symbol_cache = xs_hash_new(64);
}

void xs_netlister_free(xs_netlister *nl)
{
    if (nl->symbol_cache) xs_hash_free(nl->symbol_cache, free_cached_symbol);
    nl->symbol_cache = NULL;
}

int xs_netlister_resolve_symbols(xs_netlister *nl, xs_schematic *sch)
{
    for (int i = 0; i < sch->instance_count; i++) {
        xs_instance *ins = &sch->instances[i];
        xs_symbol *cached = xs_hash_get(nl->symbol_cache, ins->symref);
        if (cached) { ins->resolved_symbol = cached; continue; }

        char *path = xs_library_path_resolve(nl->library_path, ins->symref);
        if (!path) {
            fprintf(stderr,
                    "xschem2spice: warning: symbol '%s' not in libpath, treating as missing\n",
                    ins->symref);
            ins->resolved_symbol = missing_symbol_placeholder(nl);
            continue;
        }
        xs_symbol *sym = xs_xmalloc(sizeof *sym);
        if (xs_parse_symbol(path, sym) != 0) { free(path); free(sym); return -1; }
        free(path);
        xs_hash_put(nl->symbol_cache, ins->symref, sym);
        ins->resolved_symbol = sym;
    }
    return 0;
}

/* ============================================================ *
 * Connectivity graph (vertices + union-find)
 *
 * One vertex per wire endpoint and one per instance pin. Vertices at the
 * same coordinate are unioned; type=short instances electrically merge their
 * pins; wires with interior coincident points absorb them too.
 * ============================================================ */

typedef struct {
    double x, y;
    int    instance_index;   /* -1 if vertex is a wire endpoint */
    int    pin_index;        /* -1 if vertex is a wire endpoint */
} graph_vertex;

typedef struct {
    int *parent;
    int  count;
} disjoint_set;

static int  disjoint_set_find_root(disjoint_set *set, int element_index);
static void disjoint_set_union(disjoint_set *set, int first_element, int second_element);

typedef struct {
    graph_vertex *vertices;
    int           vertex_count;

    int          *instance_pin_offset;   /* [inst]  → first vertex index */
    int          *wire_first_endpoint;   /* [wire]  → first endpoint vertex index */

    disjoint_set  connected_vertex_sets;

    int          *vertex_to_net;         /* [vertex] → 0..net_count-1 */
    int           net_count;
} connectivity_graph;

static int disjoint_set_find_root(disjoint_set *set, int element_index)
{
    while (set->parent[element_index] != element_index) {
        set->parent[element_index] = set->parent[set->parent[element_index]];
        element_index = set->parent[element_index];
    }
    return element_index;
}

static void disjoint_set_union(disjoint_set *set, int first_element, int second_element)
{
    int first_root = disjoint_set_find_root(set, first_element);
    int second_root = disjoint_set_find_root(set, second_element);
    if (first_root != second_root) set->parent[first_root] = second_root;
}

static int coordinates_equal(double a, double b) { return fabs(a - b) < 1e-6; }

static int point_lies_on_segment(double px, double py,
                                 double ax, double ay,
                                 double bx, double by)
{
    double cross = (px - ax) * (by - ay) - (py - ay) * (bx - ax);
    if (fabs(cross) > 1e-6) return 0;
    double dot   = (px - ax) * (bx - ax) + (py - ay) * (by - ay);
    if (dot < -1e-9) return 0;
    double len_sq = (bx - ax) * (bx - ax) + (by - ay) * (by - ay);
    if (dot > len_sq + 1e-9) return 0;
    return 1;
}

static void compute_pin_global_position(const xs_instance *ins, int pin_index,
                                        double *x_out, double *y_out)
{
    const xs_symbol_pin *pin = &ins->resolved_symbol->pins[pin_index];
    double rx, ry;
    xs_transform_pin_to_global(ins->rotation, ins->flip, pin->x, pin->y, &rx, &ry);
    *x_out = ins->x + rx;
    *y_out = ins->y + ry;
}

/* Phase 1: lay out one vertex per wire endpoint (first), then one per
 * instance pin. The contiguous-block layout lets us index into the vertex
 * array via cheap offsets later. */
static void allocate_and_populate_vertices(connectivity_graph *g,
                                           const xs_schematic *sch)
{
    int upper_bound = sch->wire_count * 2;
    for (int i = 0; i < sch->instance_count; i++)
        upper_bound += sch->instances[i].resolved_symbol->pin_count;

    g->vertices            = xs_xmalloc(sizeof(graph_vertex) * (size_t)(upper_bound + 1));
    g->instance_pin_offset = xs_xmalloc(sizeof(int) * (size_t)(sch->instance_count + 1));
    g->wire_first_endpoint = xs_xmalloc(sizeof(int) * (size_t)(sch->wire_count + 1));
    g->vertex_count        = 0;

    for (int w_idx = 0; w_idx < sch->wire_count; w_idx++) {
        const xs_wire *w = &sch->wires[w_idx];
        g->wire_first_endpoint[w_idx]  = g->vertex_count;
        g->vertices[g->vertex_count++] = (graph_vertex){ w->x1, w->y1, -1, -1 };
        g->vertices[g->vertex_count++] = (graph_vertex){ w->x2, w->y2, -1, -1 };
    }
    for (int i = 0; i < sch->instance_count; i++) {
        g->instance_pin_offset[i] = g->vertex_count;
        const xs_instance *ins = &sch->instances[i];
        for (int j = 0; j < ins->resolved_symbol->pin_count; j++) {
            double gx, gy;
            compute_pin_global_position(ins, j, &gx, &gy);
            g->vertices[g->vertex_count++] = (graph_vertex){ gx, gy, i, j };
        }
    }
}

/* Phase 2a: vertices that share coordinates are the same physical point. */
static void union_coincident_vertices(connectivity_graph *g)
{
    for (int i = 0; i < g->vertex_count; i++)
        for (int j = i + 1; j < g->vertex_count; j++)
            if (coordinates_equal(g->vertices[i].x, g->vertices[j].x) &&
                coordinates_equal(g->vertices[i].y, g->vertices[j].y))
                disjoint_set_union(&g->connected_vertex_sets, i, j);
}

/* Phase 2b: type=short instances electrically tie all their pins together. */
static void union_short_instance_pins(connectivity_graph *g, const xs_schematic *sch)
{
    for (int i = 0; i < sch->instance_count; i++) {
        const xs_instance *ins = &sch->instances[i];
        if (!symbol_type_equals(ins->resolved_symbol->type, "short")) continue;
        if (ins->resolved_symbol->pin_count < 2) continue;
        int first_pin_vertex = g->instance_pin_offset[i];
        for (int j = 1; j < ins->resolved_symbol->pin_count; j++)
            disjoint_set_union(&g->connected_vertex_sets,
                               first_pin_vertex,
                               g->instance_pin_offset[i] + j);
    }
}

/* Phase 2c: each wire's two endpoints are connected; any vertex that lies on
 * the wire segment's interior is also absorbed into that net. */
static void union_wire_segments_with_interior_points(connectivity_graph *g,
                                                     const xs_schematic *sch)
{
    for (int w_idx = 0; w_idx < sch->wire_count; w_idx++) {
        const xs_wire *w = &sch->wires[w_idx];
        int endpoint_a = g->wire_first_endpoint[w_idx];
        int endpoint_b = endpoint_a + 1;
        disjoint_set_union(&g->connected_vertex_sets, endpoint_a, endpoint_b);
        for (int v = 0; v < g->vertex_count; v++) {
            if (v == endpoint_a || v == endpoint_b) continue;
            if (point_lies_on_segment(g->vertices[v].x, g->vertices[v].y,
                                      w->x1, w->y1, w->x2, w->y2))
                disjoint_set_union(&g->connected_vertex_sets, v, endpoint_a);
        }
    }
}

/* Phase 3: compact disjoint-set roots into dense net ids 0..net_count-1. */
static void assign_dense_net_ids(connectivity_graph *g)
{
    g->vertex_to_net = xs_xmalloc(sizeof(int) * (size_t)g->vertex_count);
    for (int i = 0; i < g->vertex_count; i++) g->vertex_to_net[i] = -1;
    g->net_count = 0;
    for (int i = 0; i < g->vertex_count; i++) {
        int root = disjoint_set_find_root(&g->connected_vertex_sets, i);
        if (g->vertex_to_net[root] == -1) g->vertex_to_net[root] = g->net_count++;
    }
}

static void build_connectivity_graph(connectivity_graph *g, const xs_schematic *sch)
{
    allocate_and_populate_vertices(g, sch);

    g->connected_vertex_sets.parent = xs_xmalloc(sizeof(int) * (size_t)g->vertex_count);
    g->connected_vertex_sets.count  = g->vertex_count;
    for (int i = 0; i < g->vertex_count; i++) g->connected_vertex_sets.parent[i] = i;

    union_coincident_vertices                  (g);
    union_short_instance_pins                  (g, sch);
    union_wire_segments_with_interior_points   (g, sch);
    assign_dense_net_ids                       (g);
}

static void free_connectivity_graph(connectivity_graph *g)
{
    free(g->vertices);
    free(g->instance_pin_offset);
    free(g->wire_first_endpoint);
    free(g->connected_vertex_sets.parent);
    free(g->vertex_to_net);
}

static int net_id_for_pin(const connectivity_graph *g_const, int instance_index, int pin_index)
{
    /* disjoint_set_find_root mutates parent[] for path compression — that's a
     * read-only operation semantically, so cast the const away locally. */
    disjoint_set *connected_vertex_sets = (disjoint_set *)&g_const->connected_vertex_sets;
    int v = g_const->instance_pin_offset[instance_index] + pin_index;
    return g_const->vertex_to_net[disjoint_set_find_root(connected_vertex_sets, v)];
}

/* ============================================================ *
 * Net-label table  —  priority-based net naming
 * ============================================================ */

typedef struct {
    char **labels;
    int   *priorities;
    int    net_count;
} net_label_table;

static void net_label_table_init(net_label_table *t, int net_count)
{
    t->labels     = xs_xmalloc(sizeof(char *) * (size_t)(net_count + 1));
    t->priorities = xs_xmalloc(sizeof(int)    * (size_t)(net_count + 1));
    for (int i = 0; i < net_count; i++) {
        t->labels[i]     = NULL;
        t->priorities[i] = NET_LABEL_PRIO_NONE;
    }
    t->net_count = net_count;
}

static void net_label_table_free(net_label_table *t)
{
    for (int i = 0; i < t->net_count; i++) free(t->labels[i]);
    free(t->labels);
    free(t->priorities);
}

/* Compare-and-assign with priority. Higher priority wins; on tie, the
 * alphabetically-earlier name wins (deterministic). Takes ownership of
 * `name`. */
static void net_label_table_propose(net_label_table *t, int net,
                                    char *name, int priority)
{
    int existing_prio = t->priorities[net];
    int wins = 0;
    if (priority > existing_prio) wins = 1;
    else if (priority == existing_prio && t->labels[net] &&
             strcmp(t->labels[net], name) > 0) wins = 1;
    if (wins) {
        free(t->labels[net]);
        t->labels[net]     = name;
        t->priorities[net] = priority;
    } else {
        free(name);
    }
}

/* For each ipin/opin/iopin/lab_pin/lab_wire instance, the net at its first pin
 * inherits its `lab=` value. Mirrors XSCHEM's
 *   name_nodes_of_pins_labels_and_propagate():
 *     https://github.com/StefanSchippers/xschem/blob/3.4.7/src/netlist.c#L1255-L1380
 * (we don't run XSCHEM's recursive `wirecheck` because our union-find has
 * already merged everything connected at the same coordinate). */
static void apply_instance_label_pins(net_label_table *t,
                                      const connectivity_graph *g,
                                      const xs_schematic *sch, int lvs_mode)
{
    for (int i = 0; i < sch->instance_count; i++) {
        const xs_instance *ins = &sch->instances[i];
        if (!symbol_type_is_label_or_pin(ins->resolved_symbol->type)) continue;
        if (ins->resolved_symbol->pin_count < 1) continue;
        char *raw = xs_prop_get(ins->prop_block, "lab");
        if (!raw) continue;
        char *pretty = normalize_net_label(raw, lvs_mode);
        free(raw);
        int net      = net_id_for_pin(g, i, 0);
        int priority = symbol_type_is_port(ins->resolved_symbol->type)
                       ? NET_LABEL_PRIO_PORT
                       : NET_LABEL_PRIO_LAB_PIN;
        net_label_table_propose(t, net, pretty, priority);
    }
}

/* xschem's bus_tap basename extraction from a comma-list bus name like
 *   "CK , S1, ADD[3:0], ENAB"
 * walks back from the first `[` until a `,` (skip spaces) and returns the
 * preceding identifier (`ADD`). Mirrors instcheck() at
 *   https://github.com/StefanSchippers/xschem/blob/3.4.7/src/netlist.c#L1187-L1196 */
static void copy_bus_basename_around_first_bracket(const char *bus_name,
                                                   const char **base_start_out,
                                                   size_t *base_length_out)
{
    const char *first_bracket = strchr(bus_name, '[');
    if (!first_bracket) {
        *base_start_out  = bus_name;
        *base_length_out = strlen(bus_name);
        return;
    }
    const char *p = first_bracket;
    while (p > bus_name) {
        p--;
        if (*p == ',') { do { p++; } while (*p == ' '); break; }
    }
    if (*p == ',' || p < bus_name) p = bus_name;
    *base_start_out  = p;
    *base_length_out = (size_t)(first_bracket - p);
}

/* For each `type=bus_tap` instance, derive the tap pin's net name from the
 * bus pin's resolved net name plus the instance's `lab=` value. Mirrors
 * instcheck() bus-tap branch at
 *   https://github.com/StefanSchippers/xschem/blob/3.4.7/src/netlist.c#L1173-L1216 */
/* Locate the "tap" and "bus" pin indices on a bus_tap symbol; xschem's
 * convention is tap=0, bus=1 (used as default if pin names are absent). */
static void find_bus_tap_pins(const xs_symbol *sym, int *tap_pin, int *bus_pin)
{
    *tap_pin = 0; *bus_pin = 1;
    for (int j = 0; j < sym->pin_count; j++) {
        const char *name = sym->pins[j].name;
        if (!name) continue;
        if (!strcmp(name, "tap")) *tap_pin = j;
        else if (!strcmp(name, "bus")) *bus_pin = j;
    }
}

static int string_is_only_digits(const char *s)
{
    if (!s || !*s) return 0;
    for (; *s; s++) if (!isdigit((unsigned char)*s)) return 0;
    return 1;
}

/* Combine the bus's basename (e.g. "ADD" extracted from "CK , S1, ADD[3:0]")
 * with the slice token (e.g. "[3:0]") to produce the tap pin's net name. */
static char *splice_basename_with_slice(const char *bus_name, const char *slice)
{
    const char *base_start;
    size_t      base_length;
    copy_bus_basename_around_first_bracket(bus_name, &base_start, &base_length);

    size_t total = base_length + strlen(slice);
    char  *out   = xs_xmalloc(total + 1);
    memcpy(out, base_start, base_length);
    memcpy(out + base_length, slice, strlen(slice));
    out[total] = '\0';
    return out;
}

static void apply_bus_taps(net_label_table *t, const connectivity_graph *g,
                           const xs_schematic *sch)
{
    for (int i = 0; i < sch->instance_count; i++) {
        const xs_instance *ins = &sch->instances[i];
        if (!symbol_type_equals(ins->resolved_symbol->type, "bus_tap")) continue;
        if (ins->resolved_symbol->pin_count < 2) continue;

        int tap_pin, bus_pin;
        find_bus_tap_pins(ins->resolved_symbol, &tap_pin, &bus_pin);

        int         tap_net  = net_id_for_pin(g, i, tap_pin);
        const char *bus_name = t->labels[net_id_for_pin(g, i, bus_pin)];
        if (!bus_name) continue;

        char *tap_lab = xs_prop_get(ins->prop_block, "lab");
        if (!tap_lab && ins->resolved_symbol->template_)
            tap_lab = xs_prop_get(ins->resolved_symbol->template_, "lab");
        if (!tap_lab || !*tap_lab) { free(tap_lab); continue; }

        /* xschem's rule: a tap label that starts with `[` or is purely digits
         * is a bus slice; otherwise it's the literal scalar net name. */
        int   is_bus_slice = (tap_lab[0] == '[') || string_is_only_digits(tap_lab);
        char *scalar       = is_bus_slice
                           ? splice_basename_with_slice(bus_name, tap_lab)
                           : xs_strdup(tap_lab);
        free(tap_lab);

        net_label_table_propose(t, tap_net, scalar, NET_LABEL_PRIO_LAB_PIN);
    }
}

/* Mark "used" any net that some non-label instance pin connects to; used
 * unlabeled nets get a deterministic auto-name like `net1`, `net2`. */
static void apply_auto_names_to_used_unlabeled_nets(net_label_table *t,
                                                    const connectivity_graph *g,
                                                    const xs_schematic *sch)
{
    int *net_is_used = xs_xmalloc(sizeof(int) * (size_t)t->net_count);
    for (int i = 0; i < t->net_count; i++) net_is_used[i] = 0;

    for (int i = 0; i < sch->instance_count; i++) {
        const xs_instance *ins = &sch->instances[i];
        if (symbol_type_is_label_or_pin(ins->resolved_symbol->type)) continue;
        for (int j = 0; j < ins->resolved_symbol->pin_count; j++) {
            int net = net_id_for_pin(g, i, j);
            net_is_used[net] = 1;
        }
    }

    int next_auto_index = 0;
    for (int i = 0; i < t->net_count; i++) {
        if (t->labels[i] || !net_is_used[i]) continue;
        char buf[32];
        snprintf(buf, sizeof buf, "net%d", ++next_auto_index);
        t->labels[i] = xs_strdup(buf);
    }
    free(net_is_used);
}

/* ============================================================ *
 * Subckt port list
 *
 * Preferred source: companion `<cell>.sym`'s B-record pin order (with bus
 * expansion). Fallback when no .sym is available: ipin/opin/iopin instances
 * in source order, by direction.
 * ============================================================ */

typedef struct {
    char **names;
    char  *kinds;     /* 'I' | 'O' | 'B' | 0 (unknown) */
    int    count;
    int    capacity;
} port_list;

static void port_list_init(port_list *p)
{
    p->names    = NULL;
    p->kinds    = NULL;
    p->count    = 0;
    p->capacity = 0;
}

static void port_list_free(port_list *p)
{
    for (int i = 0; i < p->count; i++) free(p->names[i]);
    free(p->names);
    free(p->kinds);
}

static int port_list_contains(const port_list *p, const char *name)
{
    for (int i = 0; i < p->count; i++)
        if (strcmp(p->names[i], name) == 0) return 1;
    return 0;
}

static void port_list_grow(port_list *p)
{
    if (p->count < p->capacity) return;
    int new_cap = p->capacity ? p->capacity * 2 : 16;
    p->names = xs_xrealloc(p->names, sizeof(char *) * (size_t)new_cap);
    p->kinds = xs_xrealloc(p->kinds, sizeof(char)   * (size_t)new_cap);
    p->capacity = new_cap;
}

/* Adds the port if its name is not already present. Takes ownership of
 * `name` (frees it if it would be a duplicate). */
static void port_list_add_unique_taking_ownership(port_list *p, char *name, char kind)
{
    if (port_list_contains(p, name)) { free(name); return; }
    port_list_grow(p);
    p->names[p->count] = name;
    p->kinds[p->count] = kind;
    p->count++;
}

/* Look up the port direction kind ('I'|'O'|'B') from the schematic's own
 * ipin/opin/iopin instances by matching their lab against `name`. */
static char find_port_kind_from_schematic(const xs_schematic *sch,
                                          const char *name, int lvs_mode)
{
    for (int i = 0; i < sch->instance_count; i++) {
        const xs_instance *ins = &sch->instances[i];
        if (!symbol_type_is_port(ins->resolved_symbol->type)) continue;
        char *raw = xs_prop_get(ins->prop_block, "lab");
        if (!raw || !*raw) { free(raw); continue; }
        char *pretty = normalize_net_label(raw, lvs_mode);
        free(raw);
        int matches = strcmp(pretty, name) == 0;
        free(pretty);
        if (matches) {
            const char *t = ins->resolved_symbol->type;
            return !strcmp(t, "ipin") ? 'I' : !strcmp(t, "opin") ? 'O' : 'B';
        }
    }
    return 0;
}

/* Add a single self-symbol pin (or its bus-expansion) to the port list. */
static void add_self_sym_pin_to_ports(port_list *ports, const xs_symbol_pin *pin,
                                      const xs_schematic *sch, int lvs_mode)
{
    bus_designator bus;
    if (parse_bus_designator(pin->name, &bus) > 0) {
        int step = (bus.hi >= bus.lo) ? -1 : 1;
        for (int k = 0; k < bus.multiplicity; k++) {
            char buf[256];
            format_bus_scalar(buf, sizeof buf, pin->name, &bus, bus.hi + step * k);
            char *name = xs_strdup(buf);
            port_list_add_unique_taking_ownership(ports, name,
                find_port_kind_from_schematic(sch, name, lvs_mode));
        }
    } else {
        char *name = xs_strdup(pin->name);
        port_list_add_unique_taking_ownership(ports, name,
            find_port_kind_from_schematic(sch, name, lvs_mode));
    }
}

static void build_subckt_port_list_from_self_sym(port_list *ports,
                                                 const xs_symbol *self_sym,
                                                 const xs_schematic *sch,
                                                 int lvs_mode)
{
    for (int i = 0; i < self_sym->pin_count; i++) {
        if (!self_sym->pins[i].name) continue;
        add_self_sym_pin_to_ports(ports, &self_sym->pins[i], sch, lvs_mode);
    }
}

static void build_subckt_port_list_from_schematic_ports(port_list *ports,
                                                        const xs_schematic *sch,
                                                        int lvs_mode)
{
    /* ipins, then opins, then iopins, in source order. */
    static const char *type_pass_order[] = { "ipin", "opin", "iopin" };
    static const char  kind_per_pass[]   = { 'I',     'O',    'B'   };

    for (int pass = 0; pass < 3; pass++) {
        for (int i = 0; i < sch->instance_count; i++) {
            const xs_instance *ins = &sch->instances[i];
            if (!symbol_type_equals(ins->resolved_symbol->type, type_pass_order[pass]))
                continue;
            char *raw = xs_prop_get(ins->prop_block, "lab");
            if (!raw || !*raw) { free(raw); continue; }
            char *pretty = normalize_net_label(raw, lvs_mode);
            free(raw);
            port_list_add_unique_taking_ownership(ports, pretty, kind_per_pass[pass]);
        }
    }
}

static void build_subckt_port_list(port_list *ports, xs_netlister *nl,
                                   const xs_schematic *sch)
{
    xs_symbol *self_sym = load_companion_sym_for_schematic(nl, sch);
    if (self_sym && self_sym->pin_count > 0)
        build_subckt_port_list_from_self_sym(ports, self_sym, sch, nl->lvs_mode);
    else
        build_subckt_port_list_from_schematic_ports(ports, sch, nl->lvs_mode);
}

/* ============================================================ *
 * Format substitution and emission
 *
 * Mirrors XSCHEM's print_spice_element() at
 *   https://github.com/StefanSchippers/xschem/blob/3.4.7/src/token.c#L2150-L2535
 * ============================================================ */

/* Discard `tcleval(<balanced>)` substrings; without an embedded interpreter
 * the literal text would otherwise leak through and confuse netgen LVS. */
static void write_stripping_unevaluated_tcleval(FILE *out, const char *src)
{
    if (!src) return;
    for (const char *p = src; *p; ) {
        if (strncmp(p, "tcleval(", 8) == 0) {
            p += 8;
            int depth = 1;
            while (*p && depth > 0) {
                char c = *p++;
                if (c == '\\' && *p) { p++; continue; }
                if (c == '(') depth++;
                else if (c == ')' && --depth == 0) break;
            }
            continue;
        }
        fputc(*p++, out);
    }
}

static int find_pin_index_by_name(const xs_symbol *sym, const char *name)
{
    if (!sym) return -1;
    for (int j = 0; j < sym->pin_count; j++)
        if (sym->pins[j].name && strcmp(sym->pins[j].name, name) == 0) return j;
    return -1;
}

static const char *advance_past_identifier(const char *p)
{
    while (*p && (isalnum((unsigned char)*p) || *p == '_')) p++;
    return p;
}

static void append_net_or_qmark(xs_string_buffer *out, const char *net_or_null)
{
    xs_string_buffer_append(out, net_or_null ? net_or_null : "?");
}

/* Substitution shape: bundles every input the @-token expansion needs. */
typedef struct {
    const xs_instance  *instance;
    char  *const       *pin_nets;
    const char         *symbol_short_name;
    const char         *instance_name_override;  /* non-NULL for bus replicas */
} substitution_context;

/* Look up `key` from the instance, then the symbol's template default; for
 * `spiceprefix` xschem additionally falls through to the template. */
static char *fetch_keyed_value(const substitution_context *ctx, const char *key)
{
    if (!strcmp(key, "spiceprefix")) {
        char *v = xs_prop_get(ctx->instance->prop_block, "spiceprefix");
        if (v) return v;
        if (ctx->instance->resolved_symbol &&
            ctx->instance->resolved_symbol->template_)
            return xs_prop_get(ctx->instance->resolved_symbol->template_, "spiceprefix");
        return NULL;
    }
    return lookup_property_with_template_fallback(ctx->instance, key);
}

/* Append the substitution for `@@<pinname>`. Returns the cursor past the
 * consumed token. */
static const char *append_pin_by_name(xs_string_buffer *out,
                                      const char *cursor,
                                      const substitution_context *ctx)
{
    const char *name_start = cursor + 2;
    const char *name_end   = advance_past_identifier(name_start);
    char       *pin_name   = xs_strndup(name_start, (size_t)(name_end - name_start));
    int         pin_index  = find_pin_index_by_name(ctx->instance->resolved_symbol,
                                                    pin_name);
    free(pin_name);
    append_net_or_qmark(out, pin_index >= 0 ? ctx->pin_nets[pin_index] : NULL);
    return name_end;
}

/* Append the substitution for `@#<idx>` (alias `@#<idx>:<attr>`). */
static const char *append_pin_by_index(xs_string_buffer *out,
                                       const char *cursor,
                                       const substitution_context *ctx)
{
    const char *p   = cursor + 2;
    int         idx = 0;
    while (isdigit((unsigned char)*p)) { idx = idx * 10 + (*p - '0'); p++; }
    if (*p == ':') p = advance_past_identifier(p + 1);

    int pin_count = ctx->instance->resolved_symbol
                  ? ctx->instance->resolved_symbol->pin_count : 0;
    append_net_or_qmark(out,
        idx >= 0 && idx < pin_count ? ctx->pin_nets[idx] : NULL);
    return p;
}

/* Append the substitution for a generic `@<key>` token. */
static const char *append_at_key(xs_string_buffer *out,
                                 const char *cursor,
                                 const substitution_context *ctx)
{
    const char *key_start = cursor + 1;
    const char *key_end   = advance_past_identifier(key_start);
    if (key_start == key_end) {
        xs_string_buffer_append_char(out, '@');
        return cursor + 1;
    }
    char *key = xs_strndup(key_start, (size_t)(key_end - key_start));

    if (!strcmp(key, "name") && ctx->instance_name_override) {
        xs_string_buffer_append(out, ctx->instance_name_override);
    } else if (!strcmp(key, "name")) {
        char *v = xs_prop_get(ctx->instance->prop_block, "name");
        if (v) xs_string_buffer_append(out, v);
        free(v);
    } else if (!strcmp(key, "symname")) {
        if (ctx->symbol_short_name) xs_string_buffer_append(out, ctx->symbol_short_name);
    } else if (!strcmp(key, "pinlist")) {
        int pin_count = ctx->instance->resolved_symbol
                      ? ctx->instance->resolved_symbol->pin_count : 0;
        for (int i = 0; i < pin_count; i++) {
            if (i) xs_string_buffer_append_char(out, ' ');
            append_net_or_qmark(out, ctx->pin_nets[i]);
        }
    } else if (!strcmp(key, "path") || !strcmp(key, "savecurrent") ||
               !strcmp(key, "spice_ignore")) {
        /* `path` is the hierarchical prefix (empty at top level). The other
         * two are marker properties that xschem absorbs and emits as side
         * effects (e.g. `.save i(@name)` for savecurrent). */
    } else {
        char *v = fetch_keyed_value(ctx, key);
        if (v) xs_string_buffer_append(out, v);
        free(v);
    }
    free(key);
    return key_end;
}

/*
 * Walk `format`, expanding @-tokens into `out`:
 *   @@<pin>             net at the symbol pin named <pin>
 *   @#<idx>             net at pin index <idx> (alias @#<idx>:net_name)
 *   @name               instance's name (or override, used for bus replicas)
 *   @symname            symbol short name (no directory, no extension)
 *   @pinlist            space-separated nets, in symbol-pin order
 *   @<key>              instance property (falls through to symbol template)
 *   @path/@savecurrent/@spice_ignore are absorbed.
 */
static void substitute_format_into_buffer(xs_string_buffer *out,
                                          const char *format,
                                          const substitution_context *ctx)
{
    for (const char *p = format; *p; ) {
        if (*p == '\\' && p[1]) {                       /* template escape */
            xs_string_buffer_append_char(out, p[1]);
            p += 2;
        } else if (*p != '@') {
            xs_string_buffer_append_char(out, *p++);
        } else if (p[1] == '@' && (isalpha((unsigned char)p[2]) || p[2] == '_')) {
            p = append_pin_by_name (out, p, ctx);
        } else if (p[1] == '#' && isdigit((unsigned char)p[2])) {
            p = append_pin_by_index(out, p, ctx);
        } else {
            p = append_at_key      (out, p, ctx);
        }
    }
}

/* Substitute @-tokens in `format` and write the result to `out`, with
 * unevaluable tcleval(...) blocks elided. */
static void emit_substituted_format(FILE *out, const char *format,
                                    const xs_instance *ins,
                                    char *const *pin_nets,
                                    const char *symname,
                                    const char *name_override)
{
    if (!format) return;
    substitution_context ctx = {
        .instance               = ins,
        .pin_nets               = pin_nets,
        .symbol_short_name      = symname,
        .instance_name_override = name_override,
    };
    xs_string_buffer raw;
    xs_string_buffer_init(&raw);
    substitute_format_into_buffer(&raw, format, &ctx);
    if (raw.buffer) write_stripping_unevaluated_tcleval(out, raw.buffer);
    xs_string_buffer_free(&raw);
}

static void emit_save_current_line_if_requested(FILE *out, const xs_instance *ins)
{
    char *sc = lookup_property_with_template_fallback(ins, "savecurrent");
    if (!sc || strcmp(sc, "true") != 0) { free(sc); return; }
    free(sc);
    char *iname = xs_prop_get(ins->prop_block, "name");
    if (iname && *iname) {
        fputs(".save i(", out);
        for (char *q = iname; *q; q++) fputc(tolower((unsigned char)*q), out);
        fputs(")\n", out);
    }
    free(iname);
}

/* Pick the format string we'll substitute against. lvs_format wins in lvs_mode;
 * otherwise fall back to the regular `format`. */
static const char *select_emission_format(const xs_instance *ins, int lvs_mode)
{
    const xs_symbol *s = ins->resolved_symbol;
    if (lvs_mode && s->lvs_format && *s->lvs_format) return s->lvs_format;
    if (s->format && *s->format)                     return s->format;
    return NULL;
}

/* xschem's bit-iteration convention: replica k corresponds to the bus-bit at
 * (hi + step*k), i.e. msb-first when hi >= lo and lsb-first when hi < lo. */
static int bus_bit_for_replica(const bus_designator *bus, int replica_index)
{
    int step = (bus->hi >= bus->lo) ? -1 : 1;
    return bus->hi + step * replica_index;
}

/* For a bus-named instance like `R5[3:0]`, emit one device line per bit. Pins
 * whose net is a same-multiplicity bus get mapped to the corresponding scalar
 * (e.g. `DATA[15:12]` becomes `DATA[15]`, …, `DATA[12]`); other pins keep
 * their net name across replicas. */
static void emit_bus_expanded_instance(FILE *out, const xs_instance *ins,
                                       const char *format, char **pin_nets,
                                       const char *symbol_short,
                                       const char *instance_name,
                                       const bus_designator *instance_bus)
{
    int pin_count = ins->resolved_symbol->pin_count;

    bus_designator *pin_bus = xs_xmalloc(sizeof(bus_designator) * (size_t)pin_count);
    for (int j = 0; j < pin_count; j++)
        parse_bus_designator(pin_nets[j], &pin_bus[j]);

    char **resolved_pin_nets = xs_xmalloc(sizeof(char *) * (size_t)pin_count);

    for (int replica = 0; replica < instance_bus->multiplicity; replica++) {
        char replica_name[256];
        format_bus_scalar(replica_name, sizeof replica_name,
                          instance_name, instance_bus,
                          bus_bit_for_replica(instance_bus, replica));

        for (int j = 0; j < pin_count; j++) {
            if (pin_bus[j].multiplicity != instance_bus->multiplicity) {
                resolved_pin_nets[j] = pin_nets[j];
                continue;
            }
            char scalar[256];
            format_bus_scalar(scalar, sizeof scalar, pin_nets[j], &pin_bus[j],
                              bus_bit_for_replica(&pin_bus[j], replica));
            resolved_pin_nets[j] = xs_strdup(scalar);
        }

        emit_substituted_format(out, format, ins, resolved_pin_nets,
                                symbol_short, replica_name);
        fputc('\n', out);

        for (int j = 0; j < pin_count; j++) {
            if (resolved_pin_nets[j] != pin_nets[j]) free(resolved_pin_nets[j]);
        }
    }

    free(pin_bus);
    free(resolved_pin_nets);
}

static void emit_one_device(FILE *out, const xs_instance *ins,
                            const connectivity_graph *g,
                            const net_label_table *labels,
                            int instance_index, int lvs_mode)
{
    const xs_symbol *sym = ins->resolved_symbol;

    /* spice_ignore on the instance OR (inherited) on the symbol skips it. */
    char *si = xs_prop_get(ins->prop_block, "spice_ignore");
    if (!si && sym->spice_ignore) si = xs_strdup(sym->spice_ignore);
    int ignore = spice_ignore_value_is_truthy(si);
    free(si);
    if (ignore) return;

    /* type=subcircuit is replaced by `* IS MISSING !!!!` (we never recurse). */
    if (symbol_type_equals(sym->type, "subcircuit")) {
        char *iname = xs_prop_get(ins->prop_block, "name");
        char *sname = symref_basename_without_extension(ins->symref);
        fprintf(out, "*  %s -  %s  IS MISSING !!!!\n",
                iname ? iname : "?", sname ? sname : "?");
        free(iname);
        free(sname);
        return;
    }

    const char *format = select_emission_format(ins, lvs_mode);
    if (!format) return;

    /* Gather pin nets. */
    int npins = sym->pin_count;
    char **pin_nets = xs_xmalloc(sizeof(char *) * (size_t)(npins + 1));
    for (int j = 0; j < npins; j++)
        pin_nets[j] = labels->labels[net_id_for_pin(g, instance_index, j)];

    char *symname = symref_basename_without_extension(ins->symref);
    char *iname   = xs_prop_get(ins->prop_block, "name");
    bus_designator inst_bus;
    int multiplicity = iname ? parse_bus_designator(iname, &inst_bus) : 0;

    if (multiplicity > 1)
        emit_bus_expanded_instance(out, ins, format, pin_nets, symname,
                                   iname, &inst_bus);
    else {
        emit_substituted_format(out, format, ins, pin_nets, symname, NULL);
        fputc('\n', out);
    }

    emit_save_current_line_if_requested(out, ins);

    free(iname);
    free(symname);
    free(pin_nets);
}

static void emit_subckt_header(FILE *out, const xs_schematic *sch, const port_list *ports)
{
    fprintf(out, "** sch_path: %s\n", sch->path);
    fprintf(out, ".subckt %s",        sch->cell_name);
    for (int i = 0; i < ports->count; i++) fprintf(out, " %s", ports->names[i]);
    fputc('\n', out);
}

static void emit_pininfo_line(FILE *out, const port_list *ports)
{
    fputs("*.PININFO", out);
    for (int i = 0; i < ports->count; i++)
        fprintf(out, " %s:%c", ports->names[i],
                ports->kinds[i] ? ports->kinds[i] : 'B');
    fputc('\n', out);
}

/* type=netlist_commands instances (`.control`, `.MODEL`, …) are deferred
 * after the device list, matching XSCHEM's `**** begin user architecture
 * code` section ordering. */
static void emit_devices(FILE *out, const xs_schematic *sch,
                         const connectivity_graph *g,
                         const net_label_table *labels,
                         int lvs_mode)
{
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < sch->instance_count; i++) {
            const xs_instance *ins = &sch->instances[i];
            const xs_symbol   *sym = ins->resolved_symbol;
            if (symbol_type_is_label_or_pin(sym->type)) continue;
            int is_netlist_cmd = symbol_type_equals(sym->type, "netlist_commands");
            if ((pass == 0 &&  is_netlist_cmd) ||
                (pass == 1 && !is_netlist_cmd)) continue;
            emit_one_device(out, ins, g, labels, i, lvs_mode);
        }
    }
}

static void emit_subckt_footer(FILE *out)
{
    fputs(".ends\n.end\n", out);
}

/* ============================================================ *
 * Public entry point — assemble the SPICE netlist
 * ============================================================ */

int xs_netlister_emit_spice(xs_netlister *nl, const xs_schematic *sch, FILE *out)
{
    for (int i = 0; i < sch->instance_count; i++) {
        if (!sch->instances[i].resolved_symbol) {
            fprintf(stderr, "xschem2spice: instance %d (%s) has no resolved symbol\n",
                    i, sch->instances[i].symref);
            return -1;
        }
    }

    connectivity_graph graph;
    build_connectivity_graph(&graph, sch);

    net_label_table labels;
    net_label_table_init(&labels, graph.net_count);
    apply_instance_label_pins(&labels, &graph, sch, nl->lvs_mode);
    apply_bus_taps           (&labels, &graph, sch);
    apply_auto_names_to_used_unlabeled_nets(&labels, &graph, sch);

    port_list ports;
    port_list_init(&ports);
    build_subckt_port_list(&ports, nl, sch);

    emit_subckt_header(out, sch, &ports);
    emit_pininfo_line (out, &ports);
    emit_devices      (out, sch, &graph, &labels, nl->lvs_mode);
    emit_subckt_footer(out);

    port_list_free(&ports);
    net_label_table_free(&labels);
    free_connectivity_graph(&graph);
    return 0;
}

int xs_write_spice_netlist(const char *schematic_path,
                           const char *xschemrc_path,
                           FILE       *out)
{
    xs_library_path library_path;
    xs_library_path_init(&library_path);
    if (xschemrc_path)
        xs_library_path_load_xschemrc(&library_path, xschemrc_path);

    xs_schematic schematic;
    if (xs_parse_schematic(schematic_path, &schematic) != 0) {
        xs_library_path_free(&library_path);
        return -1;
    }

    xs_netlister netlister;
    xs_netlister_init(&netlister, &library_path, /*lvs_mode=*/1);

    int status = xs_netlister_resolve_symbols(&netlister, &schematic);
    if (status == 0) status = xs_netlister_emit_spice(&netlister, &schematic, out);

    xs_netlister_free(&netlister);
    xs_free_schematic(&schematic);
    xs_library_path_free(&library_path);
    return status;
}
