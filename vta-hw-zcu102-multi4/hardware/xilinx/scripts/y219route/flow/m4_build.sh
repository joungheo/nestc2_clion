#!/bin/bash
# #### JO added (2026-11-04): multi4 bitstream in one go: make bin up to opt_design (IMPL_TO_STEP) -> run_flow.sh <tag>.
# run_flow.sh only reads impl_1/vta_wrapper_opt.dcp, so placing and routing in impl_1 is wasted time (4 h of route for
# alu1r, 2026-11-03). As a fallback, the impl_1 Vivado is stopped if it gets to place_design anyway.
# Run on the host (docker exec inside).  usage: m4_build.sh <tag> [make vars], e.g.
#   m4_build.sh alu1r SYSTOLIC=1 FREQ=400 IMPL_STRATEGY=Performance_ExtraTimingOpt
# -> build/m4_<tag>.log (make), build/m4_<tag>_flow.log, build/y219/{routed,rws,rs}_m<tag>.*; prints ALL_DONE at the end.
T=$1; shift
F=$(cd "$(dirname "$0")" && pwd)
M=$(cd "$F/../../../../.." && pwd)
V="source /tools/Xilinx/v2020.1/Vivado/2020.1/settings64.sh && cd $M/hardware/xilinx"
docker exec -u ubuntu vivado2020-build bash -c "$V && make bin JOBS=4 BUILD_TAG=$T IMPL_TO_STEP=opt_design $*" > $M/build/m4_$T.log 2>&1 &
MK=$!
I=$(ls -d $M/build/hardware/xilinx/vivado/*_$T 2>/dev/null)
while kill -0 $MK 2> /dev/null; do
  sleep 60
  [ -z "$I" ] && I=$(ls -d $M/build/hardware/xilinx/vivado/*_$T 2>/dev/null)
  R=$I/vta.runs/impl_1
  if [ -n "$I" ] && [ -f $R/vta_wrapper_opt.dcp ] && grep -qs "^Command: place_design" $R/runme.log; then
    echo "impl_1 started place_design: stopping it"
    for p in $(pgrep -f '[u]nwrapped.*vta_wrapper.vdi'); do
      [ "$(readlink /proc/$p/cwd)" = "$R" ] && kill $p
    done
  fi
done
wait $MK; echo "make bin rc=$?"
R=$I/vta.runs/impl_1
ls -la $R/vta_wrapper_opt.dcp || { echo ALL_DONE; exit 1; }
mkdir -p $M/build/y219
cd $M/build/y219 && VTA_IMPL_DIR=$(realpath --relative-to=. $R) $F/run_flow.sh m$T > $M/build/m4_${T}_flow.log 2>&1
echo "flow rc=$?"
tail -8 $M/build/m4_${T}_flow.log
echo ALL_DONE
