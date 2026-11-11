#!/bin/sh
#### JO added (2026-11-11, ALU extension, docs/evta_isa_ext.md 8.9): builds this vtalib for the simulator (cmake, make -j2 in a
#### temporary dir) and runs tests/lut_conv_test.cc (the conv output LUT as an ALU pass against the CPU table). One or two
#### cores, about a minute. usage: vtalib-zcu102-systolic/tests/run_lut_conv_test.sh   (needs llvm-8 headers for VTABundle.cpp)
set -eu
H=$(cd "$(dirname "$0")" && pwd)
V=$(cd "$H/.." && pwd)
R=$(cd "$V/.." && pwd)
O=${LUT_TEST_BUILD:-$(mktemp -d)}
L=${LLVM_INC:-/usr/lib/llvm-8/include}
cd "$O"
cmake "$V" -DNESTC_EVTA_RUN_ON_ZCU102=OFF -DVTALIB_WITHOUT_NESTC=ON -DCMAKE_CXX_FLAGS="-I$L -I$R -w -DLLVM_DISABLE_ABI_BREAKING_CHECKS_ENFORCING=1" > cmake.log
nice make -j2 VTABundle vta_runtime > make.log
${CXX:-c++} -std=c++14 -O1 -w -DLLVM_DISABLE_ABI_BREAKING_CHECKS_ENFORCING=1 -DNESTC_USE_VTASIM=1 -DPROFILE_ENABLE=0 -DVTALIB_ONLY=1 -I"$V/include" -I"$V/include/simulator" \
  -I"$V/include/simulator/vta" -I"$V/3rdparty/dlpack/include" -I"$V/3rdparty/dmlc-core/include" -I"$L" -I"$R" \
  "$H/lut_conv_test.cc" -o lut_conv_test libVTABundle.a libvta_runtime.a -lpthread
./lut_conv_test
