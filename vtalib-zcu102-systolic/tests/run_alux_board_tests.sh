#!/bin/sh
#### JO added (2026-11-14, ALU extension step 4, docs/evta_isa_ext.md 8.12): the vtalib ALU-extension tests of
#### run_lut_conv_test.sh (lut_conv_test, ln_evta_test, softmax_evta_test: EVTA ALU path against the CPU, byte by byte) built
#### for the board against vtabundles_xcmpl/build_sys's vtalib (rebuilt here first, same flags) and run on the EVTA.
#### On the board, with the bitstream loaded (load_bit.sh writes /tmp/evta_bitstream; _alux in its name turns the paths on):
####   sudo -n /bin/sh -c "vtalib-zcu102-systolic/tests/run_alux_board_tests.sh [build|run|all]"   (default all)
#### EVTA_ALUX=0 runs the CPU fallback of the same binaries (must also be 0 differ).
#### JO added (2026-11-14): epi_conv_test (the conv epilogue, docs/evta_alux_epilogue.md). CROSS=1 builds on the host PC in the
#### nestc-build container against a cross build dir (BUILD_SYS, e.g. vtabundles_xcmpl/build_pc_systimed; utils/docker_nestc,
#### docs/host_pc_build.md) with its toolchain; copy ALUX_TEST_DIR to the board and run it there with the step run.
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
  X=
  [ "${CROSS:-0}" = 1 ] && X="--target=aarch64-linux-gnu --sysroot=${ZCU102_SYSROOT:-$HOME/zcu102_sysroot} -B/usr/bin/aarch64-linux-gnu- -Wl,-L${ZCU102_SYSROOT:-$HOME/zcu102_sysroot}/usr/lib/llvm-8/lib -Wl,--hash-style=both"
  for t in lut_conv_test ln_evta_test softmax_evta_test epi_conv_test; do
    # the compiler and libraries of build_sys's bundle executables (clang++-8, its OpenMP, ACL, LLVMSupport)
    clang++-8 $X -std=c++14 -O2 -w -fopenmp $DEFS $INCS -I"$O" "$H/$t.cc" -o "$O/$t" \
      "$B/vtalib/libVTABundle.a" -lpng -ltinfo -ldl -lz "$B/vtalib/libvta_runtime.a" \
      $V/lib/acl/libarm_compute-static.a $V/lib/acl/libarm_compute_core-static.a $V/lib/acl/libarm_compute_graph-static.a \
      -lLLVMSupport -lpthread -lglog -lgflags -lgomp -lcma
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
  EVTA_EPI_CHECK=1 ./epi_conv_test
fi
