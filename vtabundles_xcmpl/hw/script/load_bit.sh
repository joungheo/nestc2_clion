#!/bin/bash
# #### JO added (2026-10-27): reload an EVTA bitstream by hand, as run_all.sh / run_prof.sh do before each run:
# fpga_manager firmware write, then Xlnk().xlnk_reset() in hw/trace/ (PYNQ libcma.so writes sds_trace_data.dat
# to the working directory at exit). Takes the same lock as the run scripts while it reloads.
# usage: sudo hw/script/load_bit.sh <file in /lib/firmware>     e.g. vtahw_multi4_y219.bin
set -u
cd "$(dirname "$0")/.."
BIN=${1:?usage: sudo $0 <file in /lib/firmware>}
[ -f /lib/firmware/$BIN ] || { echo "bitstream /lib/firmware/$BIN not found"; exit 1; }
[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
exec 9>/tmp/evta_run.lock; flock -n 9 || { echo "another EVTA run is active (/tmp/evta_run.lock)"; exit 1; }
echo 0 > /sys/class/fpga_manager/fpga0/flags; echo $BIN > /sys/class/fpga_manager/fpga0/firmware
mkdir -p trace; chown --reference=. trace
( cd trace && python3 -c "from pynq import Xlnk; Xlnk().xlnk_reset()" )
echo "$BIN: fpga state $(cat /sys/class/fpga_manager/fpga0/state)"
