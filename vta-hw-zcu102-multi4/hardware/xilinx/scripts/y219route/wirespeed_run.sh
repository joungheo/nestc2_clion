#!/bin/bash
# #### JO added (2026-10-25): run one Y219 at-speed wire test design (WireTest.java -Dspeed=1) on the lab ZCU102 from
# the host PC. Loads <out>_p.bit with fpga_manager, then per frequency: pl_clk0 at <MHz> for a second, clock stopped
# (vtabundles_xcmpl/hwtest/plclkset.py), JTAG readback; pl_clk0 is set back to 100 MHz at the end (also on error).
# nocpuidle.sh first: without it a JTAG access freezes the board's Linux. Evaluates with wirespeed.py.
# usage: wirespeed_run.sh <work dir> <ws name> <out name> [MHz ...]   (default 100 150 187.5 250 300 375 500)
set -e
W=$(cd "$1" && pwd); WS=$2; OUT=$3; shift 3
FREQS=${@:-100 150 187.5 250 300 375 500}
S=$(cd "$(dirname "$0")" && pwd)
H=$(cd "$S/../../../../../vtabundles_xcmpl/hwtest" && pwd)
BH=/home/xilinx/JO/npuprg/nestc2_clion/nest-compiler/vtabundles_xcmpl/hwtest
BIT=/home/xilinx/JO/npuprg/bit/y219_wiretest_$(date +%y%m%d)
bsh() { timeout 60 ssh zcu102 "sudo -n /bin/sh -c \"$1\""; }
trap 'bsh "python3 $BH/plclkset.py 100" || true' EXIT
ssh zcu102 "mkdir -p $BIT; uptime"
scp -q $H/plclkset.py zcu102:$BH/
bsh "cd $BH && ./nocpuidle.sh" | tail -1
python3 $H/bit2bin.py $W/${OUT}_p.bit $W/${OUT}_p.bin
scp -q $W/${OUT}_p.bin zcu102:$BIT/
bsh "cp $BIT/${OUT}_p.bin /lib/firmware/y219_wiretest.bin; echo 0 > /sys/class/fpga_manager/fpga0/flags; echo y219_wiretest.bin > /sys/class/fpga_manager/fpga0/firmware; cat /sys/class/fpga_manager/fpga0/state"
RUNS=""
for f in $FREQS; do
  bsh "python3 $BH/plclkset.py $f; sleep 1; python3 $BH/plclkset.py stop"
  rm -f $W/${OUT}_$f.rb.bin
  docker exec -u ubuntu -w $W vivado2020-build bash -c "source /tools/Xilinx/v2020.1/Vivado/2020.1/settings64.sh; \
    vivado -mode batch -nojournal -nolog -source $H/brambist/readback.tcl -tclargs ${OUT}_$f.rb > ${OUT}_$f.rb.log 2>&1"
  ls $W/${OUT}_$f.rb.bin >/dev/null
  timeout 20 ssh zcu102 uptime
  RUNS="$RUNS $f=$W/${OUT}_$f.rb.bin"
done
python3 $S/wirespeed.py $W/$WS.txt $W/$OUT $RUNS > $W/${OUT}.speed.txt
