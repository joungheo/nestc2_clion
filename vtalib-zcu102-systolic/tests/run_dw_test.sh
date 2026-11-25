#!/bin/sh
#### JO added (2026-11-24, docs/evta_dw_unit.md 5): builds this vtalib for the simulator (as run_epi_test.sh) and runs
#### tests/dw_evta_test.cc (dw_convolution_evta against a CPU loop) with the depthwise window mode (EVTA_DWU=1) and with the
#### diagonal GEMM (EVTA_DWU=0), both with the requantize on as on the _rq bitstreams.
#### usage: vtalib-zcu102-systolic/tests/run_dw_test.sh   (DW_TEST_BUILD=<dir> keeps the build; in the nestc-build container)
set -eu
H=$(cd "$(dirname "$0")" && pwd)
V=$(cd "$H/.." && pwd)
R=$(cd "$V/.." && pwd)
O=${DW_TEST_BUILD:-$(mktemp -d)}
L=${LLVM_INC:-/usr/lib/llvm-8/include}
cd "$O"
cmake "$V" -DNESTC_EVTA_RUN_ON_ZCU102=OFF -DVTALIB_WITHOUT_NESTC=ON -DCMAKE_CXX_FLAGS="-I$L -I$R -w -DLLVM_DISABLE_ABI_BREAKING_CHECKS_ENFORCING=1" > cmake.log
nice make -j${JOBS:-2} VTABundle vta_runtime > make.log
${CXX:-c++} -std=c++14 -O1 -w -DLLVM_DISABLE_ABI_BREAKING_CHECKS_ENFORCING=1 -DNESTC_USE_VTASIM=1 -DPROFILE_ENABLE=0 -DVTALIB_ONLY=1 -I"$V/include" -I"$V/include/simulator" \
  -I"$V/include/simulator/vta" -I"$V/3rdparty/dlpack/include" -I"$V/3rdparty/dmlc-core/include" -I"$L" -I"$R" \
  "$H/dw_evta_test.cc" -o dw_evta_test libVTABundle.a libvta_runtime.a -lpthread
rc=0
echo "== window mode (EVTA_DWU=1)"
EVTA_DWU=1 EVTA_REQUANT=1 ./dw_evta_test || rc=1
echo "== diagonal GEMM (EVTA_DWU=0)"
EVTA_DWU=0 EVTA_REQUANT=1 ./dw_evta_test || rc=1
exit $rc
