#!/bin/bash
# Run rawtest 1 / 32 / 16, reloading the EVTA bitstream before each case so every case starts
# from a clean PL state (previous hung runs leave the auto-restart blocks waiting on queues).
# usage: sudo ./run_rawtests.sh
set -u
cd "$(dirname "$0")"
#### JO changed: bitstream can be given, e.g. sudo BIN=zcu102_2_1x16_i8w8a32_16_15_18_17.bin ./run_rawtests.sh
#BIN=zcu102_1x16_i8w8a32_16_16_19_18.bin   # in /lib/firmware
BIN=${BIN:-zcu102_1x16_i8w8a32_16_16_19_18.bin}   # in /lib/firmware

if [ "$(id -u)" != 0 ]; then echo "run with sudo"; exit 1; fi

for mode in 1 32 16; do
  echo "================ rawtest $mode ================"
  echo 0 > /sys/class/fpga_manager/fpga0/flags
  echo $BIN > /sys/class/fpga_manager/fpga0/firmware
  echo "fpga state: $(cat /sys/class/fpga_manager/fpga0/state)"
  #### JO changed (2026-10-27): in trace/ (libcma.so writes sds_trace_data.dat to the working dir)
  #python3 -c "from pynq import Xlnk; Xlnk().xlnk_reset()" 2>/dev/null
  ( mkdir -p trace && cd trace && python3 -c "from pynq import Xlnk; Xlnk().xlnk_reset()" 2>/dev/null )
  ./rawtest $mode
done
