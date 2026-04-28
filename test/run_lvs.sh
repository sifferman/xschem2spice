#!/usr/bin/env bash
# Run reference xschem netlister, run xschem2spice, then run netgen LVS.
# Args:
#   $1 – absolute path to schematic .sch
#   $2 – report file path (will be moved to .failed on mismatch)
#   $3 – xschemrc path
#
# Exits 0 on LVS pass, non-zero on any failure.

set -u

if [ "$#" -lt 3 ]; then
    echo "Usage: $0 <sch-path> <report-path> <xschemrc>" >&2
    exit 2
fi

SCH="$1"
REPORT="$2"
XSCHEMRC="$3"

# When EXPECTED_FAIL=1 the cell is on the allowlist of known broken
# examples — a non-pass becomes a non-gating "xfail" (and an unexpected
# pass becomes "xpass" so we know to remove it from the allowlist).
EXPECTED_FAIL="${EXPECTED_FAIL:-0}"

if [ -z "${PDK_ROOT:-}" ]; then
    echo "PDK_ROOT must be set" >&2
    exit 2
fi

# Derive paths
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
TOP_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
X2S="$TOP_DIR/xschem2spice"
GEN_TCL="$SCRIPT_DIR/scripts/xschem_generate_netlist.tcl"
NETGEN_TCL="$SCRIPT_DIR/scripts/netgen_lvs.tcl"

if [ ! -x "$X2S" ]; then
    echo "xschem2spice binary not found at $X2S" >&2
    exit 2
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

mkdir -p "$(dirname "$REPORT")"
LOG="${REPORT%.report}.log"
: >"$LOG"

# Cell name (basename without .sch)
CELL="$(basename "$SCH" .sch)"

# 1) Reference netlist via real xschem
REF_DIR="$WORK/ref"
mkdir -p "$REF_DIR"
(
    cd "$WORK" && \
    SCH_PATH="$SCH" NETLIST_DIR="$REF_DIR" \
        xschem --no_x --rcfile "$XSCHEMRC" \
            --log "$WORK/xschem.log" \
            --script "$GEN_TCL"
) >>"$LOG" 2>&1

REF_SPICE="$REF_DIR/$CELL.spice"
REPORT_BASE="${REPORT%.report}"
KEEP_REF="${REPORT_BASE%/*}/$CELL.xschem.spice"
KEEP_OUR="${REPORT_BASE%/*}/$CELL.xschem2spice.spice"

# If xschem itself can't produce a netlist (e.g., a schematic that depends
# on tcleval(...) of a missing var, or a top-level testbench with no
# .subckt), the schematic is not a fair LVS target — skip it. Skips count
# separately from failures and do not gate the test.
if [ ! -f "$REF_SPICE" ]; then
    echo "SKIP: xschem failed to produce reference netlist for $CELL" >>"$LOG"
    cp "$LOG" "${REPORT_BASE}.skipped"
    echo "[skip:xschem-fail] $SCH"
    exit 0
fi
cp "$REF_SPICE" "$KEEP_REF"

# Top-level testbench files have no .subckt — netgen LVS can't compare
# them, so treat as skip.
if ! grep -q '^\.subckt' "$REF_SPICE"; then
    echo "SKIP: reference netlist for $CELL has no .subckt (top-level testbench)" >>"$LOG"
    cp "$LOG" "${REPORT_BASE}.skipped"
    echo "[skip:no-subckt] $SCH"
    exit 0
fi

# 2) Our netlist
OUR_SPICE="$WORK/our.spice"
"$X2S" --xschemrc "$XSCHEMRC" -o "$OUR_SPICE" "$SCH" >>"$LOG" 2>&1
if [ ! -f "$OUR_SPICE" ]; then
    echo "FAIL: xschem2spice failed to produce netlist for $CELL" >>"$LOG"
    if [ "$EXPECTED_FAIL" = "1" ]; then
        cp "$LOG" "${REPORT_BASE}.xfail"
        echo "[xfail:x2s-fail] $SCH"
        exit 0
    fi
    cp "$LOG" "${REPORT_BASE}.failed"
    echo "[x2s-fail] $SCH" >&2
    exit 4
fi
cp "$OUR_SPICE" "$KEEP_OUR"

# 3) Check that our .subckt port list matches xschem's. The netgen_lvs.tcl
#    we use does this internally too, but checking here gives a clearer
#    error message.
ref_subckt="$(awk '/^\.subckt/{print; exit}' "$REF_SPICE")"
our_subckt="$(awk '/^\.subckt/{print; exit}' "$OUR_SPICE")"
if [ -z "$ref_subckt" ] || [ -z "$our_subckt" ]; then
    echo "FAIL: missing .subckt line (ref='$ref_subckt' our='$our_subckt')" >>"$LOG"
    if [ "$EXPECTED_FAIL" = "1" ]; then
        cp "$LOG" "${REPORT_BASE}.xfail"
        echo "[xfail:no-subckt] $SCH"
        exit 0
    fi
    cp "$LOG" "${REPORT%.report}.failed"
    echo "[no-subckt] $SCH" >&2
    exit 5
fi

# 4) Run netgen LVS
REF_CELL="$(printf '%s' "$ref_subckt" | awk '{print $2}')"
OUR_CELL="$(printf '%s' "$our_subckt" | awk '{print $2}')"

REFERENCE_SPICE_FILE="$REF_SPICE" \
REFERENCE_CELL_NAME="$REF_CELL" \
XSCHEM_SPICE_FILE="$OUR_SPICE" \
XSCHEM_CELL_NAME="$OUR_CELL" \
REPORT_FILE="$REPORT" \
PDK_ROOT="$PDK_ROOT" \
    netgen -batch source "$NETGEN_TCL" >>"$LOG" 2>&1

# netgen_lvs.tcl renames .report → .failed on mismatch and exits 1.
if [ -f "${REPORT_BASE}.failed" ]; then
    if [ "$EXPECTED_FAIL" = "1" ]; then
        mv "${REPORT_BASE}.failed" "${REPORT_BASE}.xfail"
        echo "[xfail:lvs-fail] $SCH"
        exit 0
    fi
    echo "[lvs-fail] $SCH" >&2
    exit 1
fi
if [ ! -f "$REPORT" ]; then
    if [ "$EXPECTED_FAIL" = "1" ]; then
        cp "$LOG" "${REPORT_BASE}.xfail"
        echo "[xfail:no-report] $SCH"
        exit 0
    fi
    echo "FAIL: report file not produced" >>"$LOG"
    cp "$LOG" "${REPORT_BASE}.failed"
    echo "[no-report] $SCH" >&2
    exit 1
fi
if [ "$EXPECTED_FAIL" = "1" ]; then
    mv "$REPORT" "${REPORT_BASE}.xpass"
    echo "[xpass] $SCH (was on allowlist; consider removing)"
    exit 0
fi
echo "[ok] $SCH"
