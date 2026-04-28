#ifndef XS_XSCHEMRC_H
#define XS_XSCHEMRC_H

typedef struct {
    char **paths;
    int    npaths;
} xs_libpath;

void xs_libpath_init(xs_libpath *lp);
void xs_libpath_free(xs_libpath *lp);
void xs_libpath_add(xs_libpath *lp, const char *path);

/* Parse `set XSCHEM_LIBRARY_PATH` and `append XSCHEM_LIBRARY_PATH` lines from
 * a Tcl-syntax xschemrc file, expanding ${XSCHEM_SHAREDIR}, $USER_CONF_DIR
 * and `[file dirname [info script]]` to fixed values. Splits the resulting
 * colon-delimited string into paths and appends them to lp. Returns 0 on
 * success even if the file is missing (paths just won't be added). */
int xs_libpath_load_xschemrc(xs_libpath *lp, const char *xschemrc_path);

/* Resolve a symbol reference (e.g. "ipin.sym" or "sky130_fd_pr/nfet_01v8.sym")
 * against the search paths. Returns malloc'd absolute path or NULL. */
char *xs_libpath_resolve(const xs_libpath *lp, const char *symref);

#endif
