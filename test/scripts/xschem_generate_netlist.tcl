
# Loads an arbitrary .sch (absolute or relative path) and writes a SPICE
# netlist into a chosen directory. Driven by env vars so the same script
# can be reused across schematics:
#   SCH_PATH    – path to the .sch
#   NETLIST_DIR – directory to drop netlist into (must exist)
#
# Notes:
#   * Sets `lvs_netlist 1` and `netlist_type spice` before netlisting.
#   * Calls `xschem exit closewindow force` when done.

if {[catch {set sch_path $env(SCH_PATH)}]} { puts stderr "Set SCH_PATH"; exit 1 }
if {[catch {set ndir     $env(NETLIST_DIR)}]} { puts stderr "Set NETLIST_DIR"; exit 1 }

set netlist_dir $ndir
xschem load $sch_path
set lvs_netlist 1
set netlist_type spice
xschem netlist
xschem exit closewindow force
