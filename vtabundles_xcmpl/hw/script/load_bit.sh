#!/bin/bash
# #### JO added (2026-10-27): reload an EVTA bitstream by hand, as run_all.sh / run_prof.sh do before each run:
# fpga_manager firmware write, then Xlnk().xlnk_reset() in hw/trace/ (PYNQ libcma.so writes sds_trace_data.dat
# to the working directory at exit). Takes the same lock as the run scripts while it reloads.
# usage: sudo hw/script/load_bit.sh <file in /lib/firmware>     e.g. vtahw_multi4_y219.bin
set -u
cd "$(dirname "$0")/.."
#### JO added (2026-11-04): --help; prints the bitstream and its features first (../bininfo.sh)
case "${1:-}" in -h|--help) echo "usage: sudo $0 <file in /lib/firmware>   reloads that bitstream (fpga_manager + xlnk_reset)"
  echo "  and records its name in /tmp/evta_bitstream (vtalib reads the EVTA count and the features from it)"; exit 0;; esac
BIN=${1:?usage: sudo $0 <file in /lib/firmware>}
[ -f /lib/firmware/$BIN ] || { echo "bitstream /lib/firmware/$BIN not found"; exit 1; }
[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
exec 9>/tmp/evta_run.lock; flock -n 9 || { echo "another EVTA run is active (/tmp/evta_run.lock)"; exit 1; }
#### JO changed (2026-11-01): record the loaded bitstream in /tmp/evta_bitstream (vtalib's pynq_driver.cc checks the
#### EVTA count of its name before mapping the registers: a missing EVTA is an SError, not a clean error)
# echo 0 > /sys/class/fpga_manager/fpga0/flags; echo $BIN > /sys/class/fpga_manager/fpga0/firmware
echo 0 > /sys/class/fpga_manager/fpga0/flags
if for i in $(seq 24); do echo $BIN > /sys/class/fpga_manager/fpga0/firmware 2>/dev/null && break; [ $i = 1 ] && ( mkdir -p /tmp/xlnk_trace && cd /tmp/xlnk_trace && python3 -c "from pynq import Xlnk; Xlnk().xlnk_reset()" 2>/dev/null ); sleep 5; false; done; then echo $BIN > /tmp/evta_bitstream
else echo "FAILED $BIN" > /tmp/evta_bitstream; echo "bitstream $BIN: load failed"; fi
mkdir -p trace; chown --reference=. trace
( cd trace && python3 -c "from pynq import Xlnk; Xlnk().xlnk_reset()" )
echo "$BIN: fpga state $(cat /sys/class/fpga_manager/fpga0/state)"
. ../bininfo.sh; bin_info $BIN
