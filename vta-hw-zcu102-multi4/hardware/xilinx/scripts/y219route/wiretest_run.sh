#!/bin/bash
# #### JO added (2026-10-25): run one Y219 wire test design on the lab ZCU102 from the host PC.
# For v = 0, 1: <out>_<v>.bit -> .bin, load it with fpga_manager on the board (the design has no PS connection, so no
# AXI access can hang the board), wait for the SRLs to fill, read the PL back over JTAG and evaluate with wiretest.py.
# Runs hwtest/nocpuidle.sh on the board first: without it any JTAG access freezes Linux (BOARD_TODO 2026-10-22).
# usage: wiretest_run.sh <work dir> <wt name> <out name>     e.g. wiretest_run.sh build/wiretest wt_small wtb_small
set -e
W=$(cd "$1" && pwd); WT=$2; OUT=$3
S=$(cd "$(dirname "$0")" && pwd)
H=$(cd "$S/../../../../../vtabundles_xcmpl/hwtest" && pwd)
BIT=/home/xilinx/JO/npuprg/bit/y219_wiretest_$(date +%y%m%d)
ssh zcu102 "mkdir -p $BIT; uptime"
ssh zcu102 "cd /home/xilinx/JO/npuprg/nestc2_clion/nest-compiler/vtabundles_xcmpl/hwtest && sudo -n /bin/sh -c ./nocpuidle.sh" | tail -2
for v in 0 1; do
  python3 $H/bit2bin.py $W/${OUT}_$v.bit $W/${OUT}_$v.bin
  scp -q $W/${OUT}_$v.bin zcu102:$BIT/
  cat > $W/load.sh <<EOF
cp $BIT/${OUT}_$v.bin /lib/firmware/y219_wiretest.bin
echo 0 > /sys/class/fpga_manager/fpga0/flags
echo y219_wiretest.bin > /sys/class/fpga_manager/fpga0/firmware
cat /sys/class/fpga_manager/fpga0/state
EOF
  scp -q $W/load.sh zcu102:/tmp/wt_load.sh
  timeout 60 ssh zcu102 'sudo -n /bin/sh -c "bash /tmp/wt_load.sh"'
  sleep 2
  rm -f $W/${OUT}_$v.rb.bin
  docker exec -u ubuntu -w $W vivado2020-build bash -c "source /tools/Xilinx/v2020.1/Vivado/2020.1/settings64.sh; \
    vivado -mode batch -nojournal -nolog -source $H/brambist/readback.tcl -tclargs ${OUT}_$v.rb > ${OUT}_$v.rb.log 2>&1"
  ls -la $W/${OUT}_$v.rb.bin
  timeout 20 ssh zcu102 uptime
done
python3 $S/wiretest.py $W/$WT.txt $W/$OUT $W/${OUT}_0.rb.bin $W/${OUT}_1.rb.bin > $W/${OUT}.bad.txt
head -20 $W/${OUT}.bad.txt
