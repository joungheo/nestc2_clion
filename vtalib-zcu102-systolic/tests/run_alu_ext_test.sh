#!/bin/sh
#### JO added (2026-11-11, ALU extension, docs/evta_isa_ext.md 8.7): builds and runs alu_ext_test.cc (the integer layer norm
#### as an ALU instruction program against the bundles' CPU function). Light: one core, a few seconds.
#### usage: vtalib-zcu102-systolic/tests/run_alu_ext_test.sh [rows] [seed]   (any machine with g++; NESTC root = ../..)
set -eu
H=$(cd "$(dirname "$0")" && pwd)
R=$(cd "$H/../.." && pwd)
O=$(mktemp -d)
trap 'rm -rf "$O"' EXIT
python3 - "$R/lib/Backends/VTA/VTASave.cpp" "$O/cpuops.cpp" <<'PY'
import sys
s = open(sys.argv[1]).read()
a = s.index('NESTC_CPU_OPS = R"~(') + len('NESTC_CPU_OPS = R"~(')
open(sys.argv[2], 'w').write(s[a:s.index(')~"', a)])
PY
D="-DVTA_LOG_INP_WIDTH=3 -DVTA_LOG_WGT_WIDTH=3 -DVTA_LOG_ACC_WIDTH=5 -DVTA_LOG_BATCH=0 -DVTA_LOG_BLOCK=4
   -DVTA_LOG_UOP_BUFF_SIZE=16 -DVTA_LOG_BLOCK_IN=4 -DVTA_LOG_BLOCK_OUT=4 -DVTA_LOG_OUT_WIDTH=3 -DVTA_LOG_OUT_BUFF_SIZE=16
   -DVTA_LOG_BUS_WIDTH=7 -DVTA_LOG_INP_BUFF_SIZE=16 -DVTA_LOG_WGT_BUFF_SIZE=19 -DVTA_LOG_ACC_BUFF_SIZE=18"
${CXX:-g++} -std=c++14 -O2 -w $D -I"$H/../include/zcu102" -I"$O" -o "$O/alu_ext_test" "$H/alu_ext_test.cc"
OMP_NUM_THREADS=1 "$O/alu_ext_test" "$@"
