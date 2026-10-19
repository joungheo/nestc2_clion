#!/bin/bash
# Run convvar (vtalib convolution() on EVTA vs CPU reference) for several conv shapes,
# each on a freshly loaded bitstream. All variants pass on the simulator.
# usage: sudo ./run_convvar.sh
set -u
cd "$(dirname "$0")"
BIN=zcu102_1x16_i8w8a32_16_16_19_18.bin   # in /lib/firmware
if [ "$(id -u)" != 0 ]; then echo "run with sudo"; exit 1; fi

for a in "1 0 0" "1 0 1" "3 0 0" "3 1 0" "3 1 1" "3 1 1 32 32 14"; do
  echo "================ convvar $a ================"
  echo 0 > /sys/class/fpga_manager/fpga0/flags
  echo $BIN > /sys/class/fpga_manager/fpga0/firmware
  python3 -c "from pynq import Xlnk; Xlnk().xlnk_reset()" 2>/dev/null
  timeout 20 ./convvar $a; rc=$?
  [ $rc = 124 ] && echo "RESULT convvar $a: TIMEOUT"
done 2>&1 | tee convvar.log
echo; echo "== summary"; grep -E "^K=|RESULT" convvar.log
