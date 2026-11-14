#!/bin/sh
#### JO added (2026-11-14, docs/evta_alux_epilogue.md): builds this vtalib for the simulator (as run_lut_conv_test.sh) and runs
#### tests/epi_conv_test.cc (the conv epilogue on the EVTA against the plain conv + the same program on the CPU) three ways:
#### the requantize off (in-acc ALU passes), on (EVTA_REQUANT=1: GEMM / ALU rq_*, the simulator keeps an out buffer as the
#### hardware), and without the extended ALU (EVTA_ALUX=0: tables on the CPU). EVTA_EPI_CHECK=1 prints how each conv was
#### lowered. usage: vtalib-zcu102-systolic/tests/run_epi_test.sh   (EPI_TEST_BUILD=<dir> keeps the build)
set -eu
H=$(cd "$(dirname "$0")" && pwd)
V=$(cd "$H/.." && pwd)
R=$(cd "$V/.." && pwd)
O=${EPI_TEST_BUILD:-$(mktemp -d)}
L=${LLVM_INC:-/usr/lib/llvm-8/include}
cd "$O"
cmake "$V" -DNESTC_EVTA_RUN_ON_ZCU102=OFF -DVTALIB_WITHOUT_NESTC=ON -DCMAKE_CXX_FLAGS="-I$L -I$R -w -DLLVM_DISABLE_ABI_BREAKING_CHECKS_ENFORCING=1" > cmake.log
nice make -j${JOBS:-2} VTABundle vta_runtime > make.log
${CXX:-c++} -std=c++14 -O1 -w -DLLVM_DISABLE_ABI_BREAKING_CHECKS_ENFORCING=1 -DNESTC_USE_VTASIM=1 -DPROFILE_ENABLE=0 -DVTALIB_ONLY=1 -I"$V/include" -I"$V/include/simulator" \
  -I"$V/include/simulator/vta" -I"$V/3rdparty/dlpack/include" -I"$V/3rdparty/dmlc-core/include" -I"$L" -I"$R" \
  "$H/epi_conv_test.cc" -o epi_conv_test libVTABundle.a libvta_runtime.a -lpthread
rc=0
echo "== requantize off (ALU passes in acc)"
EVTA_REQUANT=0 EVTA_BIAS_RESET=0 ./epi_conv_test || rc=1
echo "== requantize on (EVTA_REQUANT=1)"
EVTA_REQUANT=1 EVTA_BIAS_RESET=0 ./epi_conv_test || rc=1
echo "== without the extended ALU (EVTA_ALUX=0)"
EVTA_ALUX=0 EVTA_REQUANT=1 EVTA_BIAS_RESET=0 ./epi_conv_test || rc=1
exit $rc
