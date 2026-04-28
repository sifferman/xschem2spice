# xschem2spice

A small headless C tool that loads xschem `.sch` / `.sym` files and emits SPICE
netlists, without invoking the xschem Tcl/Tk binary. Implementation is distilled
from [xschem/src/netlist.c](https://github.com/StefanSchippers/xschem) and its
supporting modules — no GUI, no Tcl interpreter, no Cairo, no external runtime
dependencies.

Use it when you want xschem-equivalent SPICE output from automation that can't
afford to spin up an X server or carry the full xschem dependency tree.

## Build

```bash
make
```

Produces `xschem2spice` (the CLI) and `libxschem2spice.a` (embeddable static
library).

## Usage

```bash
xschem2spice [options] <input.sch> [-o <output.spice>]

Options:
  -I <path>           Add a symbol search path (repeatable)
  --xschemrc <file>   Load XSCHEM_LIBRARY_PATH from an xschemrc file
  --flat              Emit a flat netlist instead of wrapping in .subckt
  --info              Print parsed schematic info; do not emit SPICE
  -h, --help          Show usage
```

Example with the sky130 PDK:

```bash
xschem2spice \
    --xschemrc $PDK_ROOT/sky130A/libs.tech/xschem/xschemrc \
    -o /tmp/dfxtp.spice \
    test/schematics/sky130_schematics/schematics/dfxtp/sky130_fd_sc_hd__dfxtp_1.sch
```

## Verification

The `test/` harness runs both the real `xschem` binary and `xschem2spice` over a
collection of `.sch` files and compares results with `netgen` LVS. The test
schematics are sourced from two git submodules (sparse-checked out so the full
xschem tree isn't pulled in):

- `test/schematics/xschem` — xschem's own `xschem_library/examples/`
- `test/schematics/sky130_schematics` — real sky130 standard cells

```bash
make -C test init      # init + sparse-checkout the submodules
make -C test           # run xschem + xschem2spice + netgen LVS over the schematics
make -C test summary   # print "N / M schematics pass LVS"
```
