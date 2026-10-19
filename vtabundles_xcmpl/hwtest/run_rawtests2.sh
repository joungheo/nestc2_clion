#!/bin/bash
# Run every rawtest2 case on a freshly loaded EVTA bitstream.
# usage: sudo ./run_rawtests2.sh [case ...]   (default: all cases)
set -u
cd "$(dirname "$0")"
#### JO changed: bitstream can be given, e.g. sudo BIN=zcu102_2_1x16_i8w8a32_16_15_18_17.bin ./run_rawtests2.sh gemm_x2
#BIN=zcu102_1x16_i8w8a32_16_16_19_18.bin   # in /lib/firmware
BIN=${BIN:-zcu102_1x16_i8w8a32_16_16_19_18.bin}   # in /lib/firmware

if [ "$(id -u)" != 0 ]; then echo "run with sudo"; exit 1; fi

# add_test / add_test_nc: the vtalib runtime test, re-run here on a fresh bitstream
# (earlier runs were done on a PL left in a hung state by the previous run).
CASES=${@:-uop acc l2c c2s inp store storeacc add add_test_nc}
for c in $CASES; do
  echo "================ $c ================"
  echo 0 > /sys/class/fpga_manager/fpga0/flags
  echo $BIN > /sys/class/fpga_manager/fpga0/firmware
  python3 -c "from pynq import Xlnk; Xlnk().xlnk_reset()" 2>/dev/null
  case $c in
    #### JO changed: any built vtalib HW test binary (add_test*, gemm*, conv13*, multi_*)
    # add_test|add_test_nc|gemm|gemm_nc|conv13|conv13_nc)
    add_test*|gemm*|conv13*|multi_*)
      timeout ${TMO:-10} ./$c > $c.out 2>&1; rc=$?
      tail -4 $c.out
      [ $rc = 0 ] && echo "RESULT case=$c: DONE" || echo "RESULT case=$c: TIMEOUT/FAIL (rc=$rc)";;
    #### JO changed: rawtest2 binary selectable, e.g. RT=rawtest2_x2m3a3 (built by build.sh with BUFF/ISA)
    # *) ./rawtest2 $c;;
    *) ./${RT:-rawtest2} $c;;
  esac
done 2>&1 | tee rawtests2.log
echo; echo "== summary"; grep RESULT rawtests2.log
