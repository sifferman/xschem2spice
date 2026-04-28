#include "xschemrc.h"
#include "strutil.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <libgen.h>

void xs_libpath_init(xs_libpath *lp)
{
    lp->paths = NULL;
    lp->npaths = 0;
}

void xs_libpath_free(xs_libpath *lp)
{
    for (int i = 0; i < lp->npaths; i++) free(lp->paths[i]);
    free(lp->paths);
    lp->paths = NULL;
    lp->npaths = 0;
}

void xs_libpath_add(xs_libpath *lp, const char *path)
{
    if (!path || !*path) return;
    /* dedupe */
    for (int i = 0; i < lp->npaths; i++) {
        if (strcmp(lp->paths[i], path) == 0) return;
    }
    lp->paths = xs_xrealloc(lp->paths, sizeof(char *) * (size_t)(lp->npaths + 1));
    lp->paths[lp->npaths++] = xs_strdup(path);
}

static int file_exists(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0;
}

static char *parent_dir(const char *path)
{
    char *dup = xs_strdup(path);
    char *d = dirname(dup);
    char *r = xs_strdup(d);
    free(dup);
    return r;
}

/* Substitute ${VAR} and $VAR (limited subset) plus a few Tcl idioms.
 * Specifically:
 *   ${XSCHEM_SHAREDIR}  -> shareDir   (or env XSCHEM_SHAREDIR)
 *   $XSCHEM_SHAREDIR    -> same
 *   ${USER_CONF_DIR}    -> userConfDir
 *   $USER_CONF_DIR      -> same
 *   ${PDK_ROOT}         -> env PDK_ROOT
 *   $PDK_ROOT           -> same
 *   $::env(VAR)         -> env VAR
 *   [file dirname [info script]]  -> rcDir
 *   [pwd]                          -> rcDir
 */
static char *expand_vars(const char *s, const char *rcDir,
                         const char *shareDir, const char *userConfDir)
{
    xs_str out;
    xs_str_init(&out);
    const char *p = s;
    while (*p) {
        if (p[0] == '$' && p[1] == '{') {
            const char *end = strchr(p + 2, '}');
            if (end) {
                size_t nl = (size_t)(end - (p + 2));
                char *name = xs_strndup(p + 2, nl);
                const char *val = NULL;
                if (strcmp(name, "XSCHEM_SHAREDIR") == 0) val = shareDir;
                else if (strcmp(name, "USER_CONF_DIR") == 0) val = userConfDir;
                else val = getenv(name);
                if (val) xs_str_puts(&out, val);
                free(name);
                p = end + 1;
                continue;
            }
        }
        if (p[0] == '$' && p[1] == ':' && p[2] == ':' &&
            strncmp(p, "$::env(", 7) == 0) {
            const char *end = strchr(p + 7, ')');
            if (end) {
                size_t nl = (size_t)(end - (p + 7));
                char *name = xs_strndup(p + 7, nl);
                const char *val = getenv(name);
                if (val) xs_str_puts(&out, val);
                free(name);
                p = end + 1;
                continue;
            }
        }
        if (p[0] == '$' && (isalpha((unsigned char)p[1]) || p[1] == '_')) {
            const char *q = p + 1;
            while (*q && (isalnum((unsigned char)*q) || *q == '_')) q++;
            size_t nl = (size_t)(q - (p + 1));
            char *name = xs_strndup(p + 1, nl);
            const char *val = NULL;
            if (strcmp(name, "XSCHEM_SHAREDIR") == 0) val = shareDir;
            else if (strcmp(name, "USER_CONF_DIR") == 0) val = userConfDir;
            else val = getenv(name);
            if (val) xs_str_puts(&out, val);
            free(name);
            p = q;
            continue;
        }
        if (strncmp(p, "[file dirname [info script]]", 28) == 0) {
            xs_str_puts(&out, rcDir);
            p += 28;
            continue;
        }
        if (strncmp(p, "[pwd]", 5) == 0) {
            xs_str_puts(&out, rcDir);
            p += 5;
            continue;
        }
        xs_str_putc(&out, *p);
        p++;
    }
    return out.buf ? out.buf : xs_strdup("");
}

/* Read a Tcl-quoted string starting at *p (after stripping leading ws).
 * Handles {…}, "…" and bare token. Updates *pp past the consumed text.
 * Returns malloc'd content. */
static char *read_tcl_value(const char **pp)
{
    const char *p = *pp;
    while (*p == ' ' || *p == '\t') p++;
    xs_str out;
    xs_str_init(&out);
    if (*p == '{') {
        p++;
        int depth = 1;
        while (*p && depth > 0) {
            if (*p == '\\' && p[1]) {
                xs_str_putc(&out, *p);
                xs_str_putc(&out, p[1]);
                p += 2;
                continue;
            }
            if (*p == '{') { depth++; xs_str_putc(&out, *p++); continue; }
            if (*p == '}') {
                depth--;
                if (depth == 0) { p++; break; }
                xs_str_putc(&out, *p++);
                continue;
            }
            xs_str_putc(&out, *p++);
        }
    } else if (*p == '"') {
        p++;
        while (*p && *p != '"') {
            if (*p == '\\' && p[1]) {
                xs_str_putc(&out, *p);
                xs_str_putc(&out, p[1]);
                p += 2;
                continue;
            }
            xs_str_putc(&out, *p++);
        }
        if (*p == '"') p++;
    } else {
        while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r' &&
               *p != ';' && *p != '#') {
            xs_str_putc(&out, *p++);
        }
    }
    *pp = p;
    return out.buf ? out.buf : xs_strdup("");
}

static void split_colon(xs_libpath *lp, const char *s)
{
    const char *p = s;
    while (*p) {
        const char *start = p;
        while (*p && *p != ':') p++;
        if (p > start) {
            char *seg = xs_strndup(start, (size_t)(p - start));
            /* trim leading/trailing whitespace */
            char *q = seg;
            while (*q == ' ' || *q == '\t') q++;
            char *e = seg + strlen(seg);
            while (e > q && (e[-1] == ' ' || e[-1] == '\t')) e--;
            *e = '\0';
            if (*q) xs_libpath_add(lp, q);
            free(seg);
        }
        if (*p == ':') p++;
    }
}

int xs_libpath_load_xschemrc(xs_libpath *lp, const char *xschemrc_path)
{
    if (!xschemrc_path) return 0;

    /* Resolve helper paths */
    char *rcDir = parent_dir(xschemrc_path);

    const char *shareDir = getenv("XSCHEM_SHAREDIR");
    char *fallback_share = NULL;
    if (!shareDir) {
        /* Try a few common defaults. */
        const char *candidates[] = {
            "/home/ethan/Utils/xschem/share/xschem",
            "/usr/local/share/xschem",
            "/usr/share/xschem",
            NULL
        };
        for (int i = 0; candidates[i]; i++) {
            if (file_exists(candidates[i])) {
                fallback_share = xs_strdup(candidates[i]);
                shareDir = fallback_share;
                break;
            }
        }
    }

    char userConfBuf[1024];
    const char *home = getenv("HOME");
    if (home) snprintf(userConfBuf, sizeof userConfBuf, "%s/.xschem", home);
    else      strcpy(userConfBuf, "/tmp/.xschem");

    /* nested rcfile sources via `source ...`: resolve once. */
    FILE *f = fopen(xschemrc_path, "r");
    if (!f) {
        free(rcDir);
        free(fallback_share);
        return 0; /* missing rc is non-fatal */
    }

    char line[4096];
    while (fgets(line, sizeof line, f)) {
        const char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\0') continue;

        if (strncmp(p, "set", 3) == 0 && (p[3] == ' ' || p[3] == '\t')) {
            p += 3;
            while (*p == ' ' || *p == '\t') p++;
            const char *name_start = p;
            while (*p && (isalnum((unsigned char)*p) || *p == '_')) p++;
            size_t nl = (size_t)(p - name_start);
            char *name = xs_strndup(name_start, nl);
            char *val = read_tcl_value(&p);
            if (strcmp(name, "XSCHEM_LIBRARY_PATH") == 0) {
                /* reset */
                xs_libpath_free(lp);
                xs_libpath_init(lp);
                if (*val) {
                    char *expanded = expand_vars(val, rcDir, shareDir ? shareDir : "",
                                                 userConfBuf);
                    split_colon(lp, expanded);
                    free(expanded);
                }
            } else if (strcmp(name, "XSCHEM_SHAREDIR") == 0) {
                char *expanded = expand_vars(val, rcDir, shareDir ? shareDir : "",
                                             userConfBuf);
                free(fallback_share);
                fallback_share = expanded;
                shareDir = fallback_share;
            }
            free(name);
            free(val);
            continue;
        }
        if (strncmp(p, "append", 6) == 0 && (p[6] == ' ' || p[6] == '\t')) {
            p += 6;
            while (*p == ' ' || *p == '\t') p++;
            const char *name_start = p;
            while (*p && (isalnum((unsigned char)*p) || *p == '_')) p++;
            size_t nl = (size_t)(p - name_start);
            char *name = xs_strndup(name_start, nl);
            char *val = read_tcl_value(&p);
            if (strcmp(name, "XSCHEM_LIBRARY_PATH") == 0 && *val) {
                char *expanded = expand_vars(val, rcDir, shareDir ? shareDir : "",
                                             userConfBuf);
                split_colon(lp, expanded);
                free(expanded);
            }
            free(name);
            free(val);
            continue;
        }
        if (strncmp(p, "source", 6) == 0 && (p[6] == ' ' || p[6] == '\t')) {
            p += 6;
            while (*p == ' ' || *p == '\t') p++;
            char *val = read_tcl_value(&p);
            char *expanded = expand_vars(val, rcDir, shareDir ? shareDir : "",
                                         userConfBuf);
            xs_libpath_load_xschemrc(lp, expanded);
            free(expanded);
            free(val);
            continue;
        }
    }
    fclose(f);

    /* Always include rcDir itself as a search path — the sky130 xschemrc
     * relies on `[file dirname [info script]]` for sky130_fd_pr/. */
    xs_libpath_add(lp, rcDir);

    /* xschem's `${XSCHEM_SHAREDIR}/xschem_library{,/devices}` are always
     * resolvable from a normal install; many xschemrc files reference them
     * by var. We mirror those two so that even a minimal user xschemrc gets
     * the device library. (We deliberately DO NOT add `share/doc/xschem/*`
     * here — xschem itself only uses those when no XSCHEM_LIBRARY_PATH was
     * set, and copying that fallback in would let us silently resolve
     * symbols that real xschem can't see, producing different netlists.) */
    if (shareDir) {
        char buf[4096];
        snprintf(buf, sizeof buf, "%s/xschem_library", shareDir);
        xs_libpath_add(lp, buf);
        snprintf(buf, sizeof buf, "%s/xschem_library/devices", shareDir);
        xs_libpath_add(lp, buf);
    }

    free(rcDir);
    free(fallback_share);
    return 0;
}

static char *try_resolve(const xs_libpath *lp, const char *symref)
{
    char buf[4096];
    if (symref[0] == '/') {
        if (file_exists(symref)) return xs_strdup(symref);
        return NULL;
    }
    for (int i = 0; i < lp->npaths; i++) {
        snprintf(buf, sizeof buf, "%s/%s", lp->paths[i], symref);
        if (file_exists(buf)) return xs_strdup(buf);
    }
    return NULL;
}

char *xs_libpath_resolve(const xs_libpath *lp, const char *symref)
{
    if (!symref) return NULL;

    /* If a .sch was referenced (a hierarchical sub-schematic), prefer the
     * companion .sym — its B-pin records give us the pin positions/order
     * needed to wire the parent's instance correctly. The .sch alone has
     * ipin/opin instances at canvas positions that are *not* the pin
     * positions on the parent's instance footprint. */
    size_t l = strlen(symref);
    if (l > 4 && strcmp(symref + l - 4, ".sch") == 0) {
        char *alt = xs_strdup(symref);
        memcpy(alt + l - 4, ".sym", 4);
        char *r = try_resolve(lp, alt);
        free(alt);
        if (r) return r;
    }
    return try_resolve(lp, symref);
}
