#!/bin/bash
# Build vtalib's standalone ZCU102 hardware tests (vta/vtalib/tests/zcu102/tests/*.cpp)
# against the ZCU102 EVTA runtime only (no NEST-C / Glow code), with the same -D macros as
# the NESTC_EVTA_RUN_ON_ZCU102=ON branch of vta/vtalib/CMakeLists.txt.
# Purpose: check that the board + bitstream + runtime work before debugging bundles.
#
# usage: [VTA_COHERENT=false] [SUFFIX=_nc] ./build.sh [test ...]   (default: add_test gemm)
#### JO added: other bitstreams / EVTAs (2026-10-17)
#   BUFF="15 18 17"   log2 INP/WGT/ACC buffer sizes (default "16 19 18"; 2-EVTA bitstream: "15 18 17")
#   UOP=15            log2 UOP buffer size (default 16)
#   XFER=8388608      VTA_MAX_XFER (per-handle CMA queues; default 32 MB)
#   ISA="3 3"         memory_type / alu_opcode bit widths (default from hw_spec.h: 2 2, the 16_16_19_18 bitstream)
#   OLDRT=<libvta_runtime.a>  link a prebuilt etri/nest-data runtime instead (TVM 0.7 API, via old_api_shim.h)
#   EVTA=1            build tests that "#define EVTA_ID 0" for that EVTA (output name gets _evta1)
#   NUM_EVTA=2        multievta_tests written for 4 EVTAs (thread[4], i<4) use this many (output name gets _2evta);
#                     never run a 4-EVTA test on a 2-EVTA bitstream (EVTA2/3 registers do not exist)
#   tests are taken from vta/vtalib/tests/zcu102/tests/ or .../multievta_tests/ (e.g. multi_gemm_gemm)
#        sudo ./add_test ; sudo ./gemm
set -e
cd "$(dirname "$0")"

#### JO changed: ../Conv3Test (hand-built bundle check) was removed; its flag set is copied here
# # Reuse the flag set from the Conv3Test bundle build
# eval "$(sed -n '/^R=/,/^FLAGS=/p' ../Conv3Test/build.sh)"
# flag set formerly in ../Conv3Test/build.sh (same -D macros as vtalib's ZCU102 branch)
#### JO changed: hwtest now lives in nest-compiler/vtabundles_xcmpl/hwtest -> NEST-C root is two levels up (we cd to hwtest above)
#R=/home/xilinx/JO/npuprg/nestc2_clion/nest-compiler
R=$(cd ../.. && pwd)
#### JO added: buffer sizes / queue size from the environment (see usage)
read B_INP B_WGT B_ACC <<< "${BUFF:-16 19 18}"
XFER_DEF=${XFER:+-DVTA_MAX_XFER=$XFER}
if [ -n "${ISA:-}" ]; then read I_MEM I_ALU <<< "$ISA"; XFER_DEF="$XFER_DEF -DVTA_MEMOP_ID_BIT_WIDTH=$I_MEM -DVTA_ALU_OPCODE_BIT_WIDTH=$I_ALU"; fi
V=$R/vta/vtalib
CXX=clang++-8
JOBS=${JOBS:-2}

DEFS="-DNESTC_USE_VTASIM=0 -DDMLC_USE_FOPEN64=0 -DVTA_TARGET=zcu102 -DVTA_HW_VER=0.0.1
 -DVTA_LOG_INP_WIDTH=3 -DVTA_LOG_WGT_WIDTH=3 -DVTA_LOG_ACC_WIDTH=5 -DVTA_LOG_BATCH=0
 -DVTA_LOG_BLOCK=4 -DVTA_LOG_UOP_BUFF_SIZE=${UOP:-16} -DVTA_LOG_BLOCK_IN=4 -DVTA_LOG_BLOCK_OUT=4
 -DVTA_LOG_OUT_WIDTH=3 -DVTA_LOG_OUT_BUFF_SIZE=16 -DVTA_LOG_BUS_WIDTH=7 -DVTA_DEVICE_NUM=4
 -DVTA_IP_REG_MAP_RANGE=0x1000
 -DVTA_FETCH_ADDR=0xA0000000 -DVTA_LOAD_ADDR=0xA0001000 -DVTA_COMPUTE_ADDR=0xA0002000 -DVTA_STORE_ADDR=0xA0003000
 -DVTA_FETCH_ADDR_1=0xA0010000 -DVTA_LOAD_ADDR_1=0xA0011000 -DVTA_COMPUTE_ADDR_1=0xA0012000 -DVTA_STORE_ADDR_1=0xA0013000
 -DVTA_FETCH_ADDR_2=0xA0020000 -DVTA_LOAD_ADDR_2=0xA0021000 -DVTA_COMPUTE_ADDR_2=0xA0022000 -DVTA_STORE_ADDR_2=0xA0023000
 -DVTA_FETCH_ADDR_3=0xA0030000 -DVTA_LOAD_ADDR_3=0xA0031000 -DVTA_COMPUTE_ADDR_3=0xA0032000 -DVTA_STORE_ADDR_3=0xA0033000
 -DVTA_FETCH_INSN_COUNT_OFFSET=16 -DVTA_FETCH_INSN_ADDR_OFFSET=24 -DVTA_LOAD_INP_ADDR_OFFSET=16
 -DVTA_LOAD_WGT_ADDR_OFFSET=24 -DVTA_COMPUTE_DONE_WR_OFFSET=16 -DVTA_COMPUTE_DONE_RD_OFFSET=24
 -DVTA_COMPUTE_UOP_ADDR_OFFSET=32 -DVTA_COMPUTE_BIAS_ADDR_OFFSET=40 -DVTA_STORE_OUT_ADDR_OFFSET=16
 -DVTA_COHERENT_ACCESSES=${VTA_COHERENT:-false} -Dvta_EXPORTS
 -DVTA_LOG_INP_BUFF_SIZE=$B_INP -DVTA_LOG_WGT_BUFF_SIZE=$B_WGT -DVTA_LOG_ACC_BUFF_SIZE=$B_ACC $XFER_DEF
 -DPROFILE_ENABLE=1"
INCS="-I. -I$R -I$V/include -I$V/include/zcu102 -I$V/include/Bundle
 -I$V/3rdparty/dlpack/include -I$V/3rdparty/dmlc-core/include -isystem /usr/lib/llvm-8/include"
FLAGS="-std=c++14 -faligned-new -O3 -fPIC -fopenmp=libgomp -Wno-psabi -mtune=cortex-a53 -march=armv8-a+crc -ftree-vectorize -w"
CC_LINE="$(echo $CXX $FLAGS $DEFS $INCS) -fpermissive -DGLOW_WITH_ZCU102_TEST=1"

OBJ=obj${SUFFIX}; mkdir -p $OBJ
for f in api.cc pynq_driver.cc; do
  [ $OBJ/$f.o -nt $V/lib/zcu102/$f ] || { echo "  CXX $f"; $CC_LINE -c $V/lib/zcu102/$f -o $OBJ/$f.o; }
done

TESTS=${@:-add_test gemm}
for t in $TESTS; do
  #### JO changed: test from tests/ or multievta_tests/, optionally re-targeted to EVTA $EVTA
  src=$V/tests/zcu102/tests/$t.cpp; [ -f $src ] || src=$V/tests/zcu102/multievta_tests/$t.cpp
  [ -f $t.cpp ] && src=$PWD/$t.cpp   # hwtest probes (rawtest2.cpp) with the same flags
  out=$t${EVTA:+_evta$EVTA}${SUFFIX}
  if [ -n "${EVTA:-}" ]; then sed "s/^#define EVTA_ID 0/#define EVTA_ID $EVTA/" $src > $OBJ/$t.evta$EVTA.cpp; src=$OBJ/$t.evta$EVTA.cpp; fi
  if [ -n "${NUM_EVTA:-}" ] && [[ $src == */multievta_tests/* ]]; then
    out=$t${NUM_EVTA:+_${NUM_EVTA}evta}${SUFFIX}
    sed -e "s/for(int i=0;i<4;i++)/for(int i=0;i<$NUM_EVTA;i++)/g" -e "s/pthread_t thread\[4\];/pthread_t thread[$NUM_EVTA];/" \
        -e "s/int a\[4\];/int a[$NUM_EVTA];/" $src > $OBJ/$t.${NUM_EVTA}evta.cpp; src=$OBJ/$t.${NUM_EVTA}evta.cpp
  fi
  echo "  CXX+LINK $out  ($src)"
  # $CC_LINE $V/tests/zcu102/tests/$t.cpp $OBJ/api.cc.o $OBJ/pynq_driver.cc.o -o $t${SUFFIX} \
  if [ -n "${OLDRT:-}" ]; then
    $CC_LINE -include $PWD/old_api_shim.h $src $OLDRT -o $out -lcma -lglog -lgflags -lgomp -lpthread
    continue
  fi
  $CC_LINE $src $OBJ/api.cc.o $OBJ/pynq_driver.cc.o -o $out \
    -lcma -lglog -lgflags -lgomp -lpthread
done
echo "built: $TESTS"
