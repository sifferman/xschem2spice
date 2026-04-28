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

#include "xschemrc.h"
#include "strutil.h"

#include <ctype.h>
#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int file_exists(const char *path)
{
    struct stat st;
    return path && stat(path, &st) == 0;
}

static char *parent_directory(const char *path)
{
    char *dup = xs_strdup(path);
    char *r   = xs_strdup(dirname(dup));
    free(dup);
    return r;
}

void xs_library_path_init(xs_library_path *lp)
{
    lp->paths      = NULL;
    lp->path_count = 0;
}

void xs_library_path_free(xs_library_path *lp)
{
    for (int i = 0; i < lp->path_count; i++) free(lp->paths[i]);
    free(lp->paths);
    lp->paths      = NULL;
    lp->path_count = 0;
}

void xs_library_path_add(xs_library_path *lp, const char *path)
{
    if (!path || !*path) return;
    for (int i = 0; i < lp->path_count; i++) {
        if (strcmp(lp->paths[i], path) == 0) return;
    }
    lp->paths = xs_xrealloc(lp->paths, sizeof(char *) * (size_t)(lp->path_count + 1));
    lp->paths[lp->path_count++] = xs_strdup(path);
}

/* ---------------- Tcl-style variable / command expansion ---------------- */
/*
 * Recognises just enough of Tcl to handle real xschemrc files in the wild:
 *   ${VAR}, $VAR, $::env(VAR), [file dirname [info script]], [pwd]
 * Anything else is passed through verbatim.
 */

static const char *resolve_named_variable(const char *name,
                                          const char *share_dir,
                                          const char *user_conf_dir)
{
    if (strcmp(name, "XSCHEM_SHAREDIR") == 0) return share_dir;
    if (strcmp(name, "USER_CONF_DIR")   == 0) return user_conf_dir;
    return getenv(name);
}

static char *expand_tcl_variables(const char *src,
                                  const char *rcfile_dir,
                                  const char *share_dir,
                                  const char *user_conf_dir)
{
    xs_string_buffer out;
    xs_string_buffer_init(&out);

    for (const char *p = src; *p; ) {
        if (p[0] == '$' && p[1] == '{') {
            const char *end = strchr(p + 2, '}');
            if (end) {
                char *name = xs_strndup(p + 2, (size_t)(end - (p + 2)));
                const char *val = resolve_named_variable(name, share_dir, user_conf_dir);
                if (val) xs_string_buffer_append(&out, val);
                free(name);
                p = end + 1;
                continue;
            }
        }
        if (strncmp(p, "$::env(", 7) == 0) {
            const char *end = strchr(p + 7, ')');
            if (end) {
                char *name = xs_strndup(p + 7, (size_t)(end - (p + 7)));
                const char *val = getenv(name);
                if (val) xs_string_buffer_append(&out, val);
                free(name);
                p = end + 1;
                continue;
            }
        }
        if (p[0] == '$' && (isalpha((unsigned char)p[1]) || p[1] == '_')) {
            const char *q = p + 1;
            while (*q && (isalnum((unsigned char)*q) || *q == '_')) q++;
            char *name = xs_strndup(p + 1, (size_t)(q - (p + 1)));
            const char *val = resolve_named_variable(name, share_dir, user_conf_dir);
            if (val) xs_string_buffer_append(&out, val);
            free(name);
            p = q;
            continue;
        }
        if (strncmp(p, "[file dirname [info script]]", 28) == 0) {
            xs_string_buffer_append(&out, rcfile_dir);
            p += 28;
            continue;
        }
        if (strncmp(p, "[pwd]", 5) == 0) {
            xs_string_buffer_append(&out, rcfile_dir);
            p += 5;
            continue;
        }
        xs_string_buffer_append_char(&out, *p++);
    }

    return out.buffer ? out.buffer : xs_strdup("");
}

/* Read a Tcl word: a `{...}` block, a "..."-quoted string, or a bare token. */
static char *read_tcl_word(const char **cursor)
{
    const char *p = *cursor;
    while (*p == ' ' || *p == '\t') p++;

    xs_string_buffer out;
    xs_string_buffer_init(&out);

    if (*p == '{') {
        p++;
        int depth = 1;
        while (*p && depth > 0) {
            if (*p == '\\' && p[1]) {
                xs_string_buffer_append_char(&out, *p);
                xs_string_buffer_append_char(&out, p[1]);
                p += 2;
                continue;
            }
            if (*p == '{') { depth++; xs_string_buffer_append_char(&out, *p++); continue; }
            if (*p == '}') {
                depth--;
                if (depth == 0) { p++; break; }
                xs_string_buffer_append_char(&out, *p++);
                continue;
            }
            xs_string_buffer_append_char(&out, *p++);
        }
    } else if (*p == '"') {
        p++;
        while (*p && *p != '"') {
            if (*p == '\\' && p[1]) {
                xs_string_buffer_append_char(&out, *p);
                xs_string_buffer_append_char(&out, p[1]);
                p += 2;
                continue;
            }
            xs_string_buffer_append_char(&out, *p++);
        }
        if (*p == '"') p++;
    } else {
        while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r' &&
               *p != ';' && *p != '#') {
            xs_string_buffer_append_char(&out, *p++);
        }
    }

    *cursor = p;
    return out.buffer ? out.buffer : xs_strdup("");
}

/* Split a colon-delimited path list and append each non-empty trimmed
 * segment to lp. */
static void append_colon_separated_paths(xs_library_path *lp, const char *list)
{
    for (const char *p = list; *p; ) {
        const char *start = p;
        while (*p && *p != ':') p++;
        if (p > start) {
            char *segment = xs_strndup(start, (size_t)(p - start));
            char *q = segment;
            while (*q == ' ' || *q == '\t') q++;
            char *e = segment + strlen(segment);
            while (e > q && (e[-1] == ' ' || e[-1] == '\t')) e--;
            *e = '\0';
            if (*q) xs_library_path_add(lp, q);
            free(segment);
        }
        if (*p == ':') p++;
    }
}

static char *find_default_share_dir(void)
{
    static const char *candidates[] = {
        "/home/ethan/Utils/xschem/share/xschem",
        "/usr/local/share/xschem",
        "/usr/share/xschem",
        NULL,
    };
    for (int i = 0; candidates[i]; i++) {
        if (file_exists(candidates[i])) return xs_strdup(candidates[i]);
    }
    return NULL;
}

int xs_library_path_load_xschemrc(xs_library_path *lp, const char *xschemrc_path)
{
    if (!xschemrc_path) return 0;

    char *rcfile_dir          = parent_directory(xschemrc_path);
    char *share_dir           = NULL;
    const char *env_share_dir = getenv("XSCHEM_SHAREDIR");
    if (env_share_dir)        share_dir = xs_strdup(env_share_dir);
    else                      share_dir = find_default_share_dir();

    char        user_conf_dir[1024];
    const char *home = getenv("HOME");
    if (home) snprintf(user_conf_dir, sizeof user_conf_dir, "%s/.xschem", home);
    else      snprintf(user_conf_dir, sizeof user_conf_dir, "/tmp/.xschem");

    FILE *f = fopen(xschemrc_path, "r");
    if (!f) {
        free(rcfile_dir);
        free(share_dir);
        return 0;
    }

    char line[4096];
    while (fgets(line, sizeof line, f)) {
        const char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\0') continue;

        const char *cmd_keyword;
        if      (strncmp(p, "set ",    4) == 0)    { cmd_keyword = "set";    p += 4; }
        else if (strncmp(p, "append ", 7) == 0)    { cmd_keyword = "append"; p += 7; }
        else if (strncmp(p, "source ", 7) == 0)    { cmd_keyword = "source"; p += 7; }
        else                                       continue;

        while (*p == ' ' || *p == '\t') p++;

        if (strcmp(cmd_keyword, "source") == 0) {
            char *val      = read_tcl_word(&p);
            char *expanded = expand_tcl_variables(val, rcfile_dir,
                                                  share_dir ? share_dir : "",
                                                  user_conf_dir);
            xs_library_path_load_xschemrc(lp, expanded);
            free(expanded);
            free(val);
            continue;
        }

        const char *name_start = p;
        while (*p && (isalnum((unsigned char)*p) || *p == '_')) p++;
        char *name = xs_strndup(name_start, (size_t)(p - name_start));
        char *val  = read_tcl_word(&p);

        if (strcmp(name, "XSCHEM_LIBRARY_PATH") == 0) {
            if (strcmp(cmd_keyword, "set") == 0) {
                xs_library_path_free(lp);
                xs_library_path_init(lp);
            }
            if (*val) {
                char *expanded = expand_tcl_variables(val, rcfile_dir,
                                                      share_dir ? share_dir : "",
                                                      user_conf_dir);
                append_colon_separated_paths(lp, expanded);
                free(expanded);
            }
        } else if (strcmp(name, "XSCHEM_SHAREDIR") == 0 &&
                   strcmp(cmd_keyword, "set") == 0) {
            char *expanded = expand_tcl_variables(val, rcfile_dir,
                                                  share_dir ? share_dir : "",
                                                  user_conf_dir);
            free(share_dir);
            share_dir = expanded;
        }

        free(name);
        free(val);
    }
    fclose(f);

    /* The sky130 PDK's xschemrc uses `[file dirname [info script]]` to point
     * at its own directory for sky130_fd_pr/. Add that fallback. */
    xs_library_path_add(lp, rcfile_dir);

    /* The two paths every install resolves through ${XSCHEM_SHAREDIR}; add
     * them so a minimal xschemrc still finds the device library. We
     * deliberately do NOT add share/doc/xschem wildcard paths; XSCHEM only uses those
     * when no XSCHEM_LIBRARY_PATH was set, and silently resolving symbols
     * the real binary can't see would cause our netlist to differ. */
    if (share_dir) {
        char buf[4096];
        snprintf(buf, sizeof buf, "%s/xschem_library", share_dir);
        xs_library_path_add(lp, buf);
        snprintf(buf, sizeof buf, "%s/xschem_library/devices", share_dir);
        xs_library_path_add(lp, buf);
    }

    free(rcfile_dir);
    free(share_dir);
    return 0;
}

static char *try_resolve_against_paths(const xs_library_path *lp, const char *symref)
{
    if (symref[0] == '/') {
        return file_exists(symref) ? xs_strdup(symref) : NULL;
    }
    char buf[4096];
    for (int i = 0; i < lp->path_count; i++) {
        snprintf(buf, sizeof buf, "%s/%s", lp->paths[i], symref);
        if (file_exists(buf)) return xs_strdup(buf);
    }
    return NULL;
}

char *xs_library_path_resolve(const xs_library_path *lp, const char *symref)
{
    if (!symref) return NULL;

    size_t len = strlen(symref);
    if (len > 4 && strcmp(symref + len - 4, ".sch") == 0) {
        char *as_sym = xs_strdup(symref);
        memcpy(as_sym + len - 4, ".sym", 4);
        char *resolved = try_resolve_against_paths(lp, as_sym);
        free(as_sym);
        if (resolved) return resolved;
    }
    return try_resolve_against_paths(lp, symref);
}
