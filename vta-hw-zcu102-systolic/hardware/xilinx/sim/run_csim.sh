#!/bin/sh
# #### JO added (2026-11-11, ALU extension, docs/evta_isa_ext.md 8.8): the HLS C simulation compiled with g++ in the docker
# container vivado2020-build, without vivado_hls (make ip CSIM=1 runs the same testbench but then synthesizes all four
# stages). One core, seconds to a few minutes. Usage (on the host):
#   sim/run_csim.sh [ALUX=1|0] [SYSTOLIC=1|0] [NOIMM=1|STEP1=1] [AMUL=1 [BC=1]] [DWU=1]      sim/vta_test.cc (with sim/alux_test.cc); VTA_ALUX_SEEDS = random
#                                                   programs (default 20)
#   sim/run_csim.sh PROG=1 [rows] [seed]            the integer layer norm / softmax ALU programs of
#                                                   vtalib-zcu102-systolic/tests/alu_ext_test.cc on the HLS ALU
#                                                   (sim/alux_prog_hls.cc), compared with the bundles' CPU functions
set -e
cd "$(dirname "$0")/.."
X=$(pwd)
ALUX=1; SYSTOLIC=1; PROG=0
while [ $# -gt 0 ]; do case "$1" in *=*) eval "$1"; shift;; *) break;; esac; done
CF=$(python3 ../../config/vta_config.py --cflags)
D=""
[ "$SYSTOLIC" = 1 ] && D="$D -DVTA_SYSTOLIC"
[ "$ALUX" = 1 ] && D="$D -DVTA_ALUX"
#### JO added (2026-11-22): NOIMM=1 -> VTA_ALUX_NO_IMM_LOOP (imm ops in the slot loop, Makefile ALUX_NOIMM=1)
[ "${NOIMM:-0}" = 1 ] && D="$D -DVTA_ALUX_NO_IMM_LOOP"
#### JO added (2026-11-23): AMUL=1 -> VTA_ALU_MUL without VTA_ALUX (Makefile ALUMUL=1: MUL with an immediate only)
[ "${AMUL:-0}" = 1 ] && D="$(echo "$D" | sed 's/ -DVTA_ALUX//') -DVTA_ALU_MUL"
#### JO added (2026-11-23): BC=1 -> VTA_ALU_BC (Makefile ALUBC=1: broadcast vector ops in the imm loop), with AMUL=1
[ "${BC:-0}" = 1 ] && D="$D -DVTA_ALU_BC"
#### JO added (2026-11-23): STEP1=1 -> src/vta_alux1.cc (Makefile ALUX_STEP1=1: the ALU extension step 1, operations 0-11, II 2
#### vector loop), the random programs with operations 0-11 only (VTA_ALUX_OPS=12) and no slot hazard filter (VTA_ALUX_II2)
#### JO added (2026-11-24): DWU=1 -> VTA_DWU (Makefile DWU=1: the depthwise window mode, sim/dwu_test.cc; docs/evta_dw_unit.md)
[ "${DWU:-0}" = 1 ] && D="$D -DVTA_DWU"
VSRC=src/vta.cc; NOPS=18
[ "${STEP1:-0}" = 1 ] && VSRC=src/vta_alux1.cc && NOPS=12 && D="$D -DVTA_ALUX_II2"
HI=-I/tools/Xilinx/v2020.1/Vivado/2020.1/include
W=$X/sim/.csim_$$          # inside the tree: the container mounts it at the same path
mkdir -p $W && chmod 777 $W   # (run with sudo: the container user must write it)
trap 'rm -rf $W' EXIT
docker start vivado2020-build >/dev/null
if [ "$PROG" = 1 ]; then
  N=$(cd ../../.. && pwd)
  T=$N/vtalib-zcu102-systolic/tests
  python3 - $N/lib/Backends/VTA/VTASave.cpp $W/cpuops.cpp <<'PY'
import sys
s = open(sys.argv[1]).read()
a = s.index('NESTC_CPU_OPS = R"~(') + len('NESTC_CPU_OPS = R"~(')
open(sys.argv[2], 'w').write(s[a:s.index(')~"', a)])
PY
  docker exec -u ubuntu vivado2020-build sh -c "
    cd $X && nice g++ -std=c++14 -O2 -w $(echo "$CF" | tr ' ' '\n' | grep '^-D' | tr '\n' ' ') -DALU_EXT_HLS \
      -I$N/vtalib-zcu102-systolic/include/zcu102 -I$W -c $T/alu_ext_test.cc -o $W/t.o &&
    nice g++ -std=c++11 -O1 -w $HI $CF $D -c $VSRC -o $W/v.o &&
    nice g++ -std=c++11 -O1 -w $HI $CF $D -c sim/alux_prog_hls.cc -o $W/p.o &&
    nice g++ -std=c++11 -O1 -w $HI $CF $D -c ../../tests/hardware/common/test_lib.cc -o $W/l.o &&
    g++ $W/t.o $W/v.o $W/p.o $W/l.o -o $W/a && nice $W/a $*"
else
  docker exec -u ubuntu -e VTA_ALUX_SEEDS="${VTA_ALUX_SEEDS:-20}" -e VTA_ALUX_OPS="${VTA_ALUX_OPS:-$NOPS}" vivado2020-build sh -c "
    cd $X && nice g++ -std=c++11 -O1 -w $HI $CF $D \
      $VSRC sim/vta_test.cc sim/alux_test.cc sim/dwu_test.cc ../../tests/hardware/common/test_lib.cc -o $W/a && nice $W/a"
fi
