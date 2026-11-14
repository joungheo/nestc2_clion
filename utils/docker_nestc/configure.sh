#!/bin/bash
# Configure (once) and build the simulator build Debug2_remote of this checkout inside the nestc-build container,
# with the options of the board's Debug2_remote (CMakeCache.txt there, 2026-11-14).
#   usage (host): docker exec -u ubuntu nestc-build <tree>/utils/docker_nestc/configure.sh [make targets, default: model-compiler image-classifier]
set -e
N=$(cd "$(dirname "$(readlink -f "$0")")/../.." && pwd)
B=$N/Debug2_remote
export TVM_HOME=$N/tvm PYTHONPATH=$N/tvm/python TVM_LIBRARY_PATH=$B/tvm
mkdir -p $B && cd $B
if [ ! -f CMakeCache.txt ]; then
  cmake -G "Unix Makefiles" $N -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=clang-8 -DCMAKE_CXX_COMPILER=clang++-8 -DLLVM_DIR=/usr/lib/llvm-8/lib/cmake/llvm \
    -DNESTC_WITH_EVTA=ON -DNESTC_EVTA_BUNDLE_TEST=ON -DGLOW_WITH_BUNDLES=ON \
    -DNESTC_USE_VTASIM=ON -DNESTC_EVTA_RUN_ON_ZCU102=OFF -DNESTC_EVTA_RUN_WITH_GENERIC_BUNDLE=ON \
    -DNESTC_USE_PRECOMPILED_EVTA_LIBRARY=OFF -DNESTC_USE_PRECOMPILED_BUNDLE=OFF -DNESTC_USE_PRECOMPILED_BUNDLE_FROM_AWS=OFF \
    -DVTA_RESNET18_WITH_SKIPQUANT0=OFF
fi
make -j${JOBS:-16} ${@:-model-compiler image-classifier}
