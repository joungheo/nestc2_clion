#!/bin/sh
# #### JO added (2026-11-11, ALU extension, docs/evta_isa_ext.md 8.8): the HLS C simulation of sim/vta_test.cc (with
# sim/alux_test.cc) compiled with g++ in the docker container vivado2020-build, without vivado_hls (make ip CSIM=1 runs the
# same testbench but then synthesizes all four stages). One core, a few minutes. Usage (on the host):
#   sim/run_csim.sh [ALUX=1|0] [SYSTOLIC=1|0]     environment VTA_ALUX_SEEDS = random programs (default 20)
set -e
cd "$(dirname "$0")/.."
X=$(pwd)
ALUX=1; SYSTOLIC=1
for a in "$@"; do eval "$a"; done
CF=$(python3 ../../config/vta_config.py --cflags)
D=""
[ "$SYSTOLIC" = 1 ] && D="$D -DVTA_SYSTOLIC"
[ "$ALUX" = 1 ] && D="$D -DVTA_ALUX"
OUT=/tmp/vta_csim_$$
docker start vivado2020-build >/dev/null
docker exec -u ubuntu -e VTA_ALUX_SEEDS="${VTA_ALUX_SEEDS:-20}" vivado2020-build sh -c "
  cd $X && nice g++ -std=c++11 -O1 -w -I/tools/Xilinx/v2020.1/Vivado/2020.1/include $CF $D \
    src/vta.cc sim/vta_test.cc sim/alux_test.cc ../../tests/hardware/common/test_lib.cc -o $OUT && nice $OUT; r=\$?; rm -f $OUT; exit \$r"
