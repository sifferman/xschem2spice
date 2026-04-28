# xschem2spice

A pure-C library — with a small demo CLI — that converts
[xschem](https://github.com/StefanSchippers/xschem) `.sch` / `.sym` files
into SPICE netlists without invoking the xschem Tcl/Tk binary. The parser,
connectivity engine and SPICE formatter are distilled from xschem 3.4.7
(GPL-2+); files that mirror upstream functions cite them by permalink.

There is no runtime dependency on Tcl, Cairo, or X11.

## Build

```bash
make
```

Produces:

- **`libxschem2spice.a`** — the static library (the primary deliverable).
- `xschem2spice` — a tiny demo CLI (~50 lines, used by the test harness).

## Use as a library

The recommended entry point is the one-shot helper:

```c
#include "netlist.h"
#include <stdio.h>

int main(void) {
    /* xschemrc_path may be NULL to use built-in defaults */
    return xs_write_spice_netlist(
        "my_circuit.sch",
        "/path/to/sky130A/libs.tech/xschem/xschemrc",
        stdout);
}
```

For more control (multiple schematics, custom search paths, custom output),
use the explicit pipeline:

```c
#include "netlist.h"
#include "parser.h"
#include "xschemrc.h"

int main(void) {
    xs_library_path library_path;
    xs_library_path_init(&library_path);
    xs_library_path_load_xschemrc(&library_path,
        "/path/to/sky130A/libs.tech/xschem/xschemrc");
    /* xs_library_path_add(&library_path, "/extra/symbols"); */

    xs_schematic schematic;
    xs_parse_schematic("my_circuit.sch", &schematic);

    xs_netlister netlister;
    xs_netlister_init(&netlister, &library_path, /*lvs_mode=*/1);
    xs_netlister_resolve_symbols(&netlister, &schematic);
    xs_netlister_emit_spice(&netlister, &schematic, stdout);

    xs_netlister_free(&netlister);
    xs_free_schematic(&schematic);
    xs_library_path_free(&library_path);
    return 0;
}
```

Link against `libxschem2spice.a` and add `src/` to the include path. The
public headers are:

- `parser.h`   — `xs_parse_schematic`, `xs_parse_symbol`, `xs_prop_get`
- `xschemrc.h` — `xs_library_path*` (XSCHEM_LIBRARY_PATH parsing)
- `netlist.h`  — `xs_netlister*` and `xs_write_spice_netlist`
- `hash.h` / `strutil.h` — internal helpers (you can ignore these)

## CLI (demo)

The `xschem2spice` binary is a thin wrapper around `xs_write_spice_netlist`:

```bash
xschem2spice [--xschemrc <rc>] [-o <out.spice>] <input.sch>
```

Example:

```bash
xschem2spice \
    --xschemrc $PDK_ROOT/sky130A/libs.tech/xschem/xschemrc \
    -o /tmp/dfxtp.spice \
    test/schematics/sky130_schematics/schematics/dfxtp/sky130_fd_sc_hd__dfxtp_1.sch
```

## Verification

The `test/` harness runs both the real `xschem` binary and `xschem2spice`
over every `.sch` it can find, then compares the two with `netgen` LVS:

```bash
make -C test init      # init + sparse-checkout the submodules
make -C test           # run xschem + xschem2spice + netgen LVS
make -C test summary   # print "N / M schematics pass LVS"
```

Test schematics come from two sparse-checked-out submodules:

- `test/schematics/xschem` — xschem's own `xschem_library/examples/`
- `test/schematics/sky130_schematics` — real sky130 standard cells

## Licence

GPL-2.0-or-later (see `LICENSE`). xschem2spice is a derivative work of
xschem; see per-file headers and `LICENSE` for details.
