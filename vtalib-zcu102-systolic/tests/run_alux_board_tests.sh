#!/bin/sh
#### JO added (2026-11-14, ALU extension step 4, docs/evta_isa_ext.md 8.12): the vtalib ALU-extension tests of
#### run_lut_conv_test.sh (lut_conv_test, ln_evta_test, softmax_evta_test: EVTA ALU path against the CPU, byte by byte) built
#### for the board against vtabundles_xcmpl/build_sys's vtalib (rebuilt here first, same flags) and run on the EVTA.
#### On the board, with the bitstream loaded (load_bit.sh writes /tmp/evta_bitstream; _alux in its name turns the paths on):
####   sudo -n /bin/sh -c "vtalib-zcu102-systolic/tests/run_alux_board_tests.sh [build|run|all]"   (default all)
#### EVTA_ALUX=0 runs the CPU fallback of the same binaries (must also be 0 differ).
set -eu
H=$(cd "$(dirname "$0")" && pwd)
V=$(cd "$H/.." && pwd)
R=$(cd "$V/.." && pwd)
B=${BUILD_SYS:-$R/vtabundles_xcmpl/build_sys}
O=${ALUX_TEST_DIR:-$B/alux_tests}
STEP=${1:-all}
if [ "$STEP" != run ]; then
  F=$B/vtalib/CMakeFiles/VTABundle.dir/flags.make
  [ -f "$F" ] || { echo "no $F: configure $B first (vtabundles_xcmpl README)"; exit 1; }
  make -C "$B" VTABundle vta_runtime > /dev/null
  DEFS=$(sed -n 's/^CXX_DEFINES = //p' "$F")
  INCS=$(sed -n 's/^CXX_INCLUDES = //p' "$F")
  mkdir -p "$O"
  python3 - "$R/lib/Backends/VTA/VTASave.cpp" "$O/cpuops.cpp" <<'PY'
import sys
s = open(sys.argv[1]).read()
a = s.index('NESTC_CPU_OPS = R"~(') + len('NESTC_CPU_OPS = R"~(')
open(sys.argv[2], 'w').write(s[a:s.index(')~"', a)])
PY
  for t in lut_conv_test ln_evta_test softmax_evta_test; do
    g++ -std=c++14 -O2 -w -fopenmp $DEFS $INCS -I"$O" "$H/$t.cc" -o "$O/$t" \
      "$B/vtalib/libVTABundle.a" "$B/vtalib/libvta_runtime.a" -lcma -lpthread -lgomp
  done
  echo "built in $O"
fi
if [ "$STEP" != build ]; then
  echo "bitstream: $(cat /tmp/evta_bitstream 2>/dev/null)"
  cd "$O"
  ./lut_conv_test
  OMP_NUM_THREADS=1 ./ln_evta_test
  OMP_NUM_THREADS=1 ./ln_evta_test after-conv
  OMP_NUM_THREADS=1 ./softmax_evta_test
fi
