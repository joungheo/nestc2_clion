#!/bin/bash
#### JO added (2026-11-27): MobileNetV2Tf option ablation on one EVTA (docs/mobilenetv2_evta_paper.md): one bundle per set of
#### model-compiler options (hw/AblMnv2_<tag>, built as vtaAblMnv2_<tag>Bundle), runtime variables and bitstreams switched per run,
#### every configuration of a list measured alternating in R rounds (the same time, as the other comparisons).
#### usage (on the board, from vtabundles_xcmpl/):
####   ablation_mnv2.sh gen                 make every bundle of BUNDLES below (model-compiler of Debug2_remote, ~1 min each)
####   ablation_mnv2.sh build               build them: build_sysabl (systolic vtalib, -s _sysabl) and build_abl (HLS vta/vtalib, -s _abl)
####   sudo ablation_mnv2.sh run <list>     measure the configurations of <list> (ladder | loo | hls), R rounds (default 3)
####   OUT=<dir> for the logs (default ~/JO/ubun_wt/abl_<list>_<date>); the summary is <dir>/summary.txt
set -u
H=$(cd "$(dirname "$0")" && pwd); cd $H
B=$(cd $H/../Debug2_remote && pwd)
YAML=$B/vta/bundles/MobileNetV2TfTest/mobilenetv2_tf_calib_n200_b2048.yaml
SRC=$H/hw/MobileNetV2TfTest
IMG=$H/../glow/tests/images/imagenet/cat_285.png

# model-compiler options of every bundle; gen_calib_bundle.sh adds -enable-channelwise-depthwise -vta-round-shift (accuracy, always on)
# and -vta-evta-depthwise (switched here). "off" = every EVTA option that touches this model off.
OFF="-vta-keep-layout=0 -vta-fuse-residual=0 -vta-fuse-residual-norelu=0 -vta-conv-epilogue=0 -vta-evta-fc=0 -vta-fuse-preprocess=0
 -vta-conv0-pack=0 -vta-evta-depthwise=0 -vta-pad-channels=0 -vta-evta-elemadd=0 -vta-skip-evta-invalidate=0"
opts_of() {   # <tag> -> options: c0 = all off, c<k> = the first k of the compiler ladder on, all = defaults, no_<opt> = defaults but one off
  local t=$1 o="$OFF" k
  local L="pad-channels fuse-preprocess conv0-pack keep-layout evta-fc evta-depthwise fuse-residual skip-evta-invalidate"
  case $t in
    all) echo ""; return;;
    no_*) echo "-vta-${t#no_}=0"; [ ${t#no_} = fuse-residual ] && echo "-vta-fuse-residual-norelu=0 -vta-conv-epilogue=0 -vta-evta-elemadd=0"; return;;
    c*) k=${t#c}; set -- $L
        while [ $k -gt 0 ] && [ $# -gt 0 ]; do
          o=$(echo "$o" | sed "s/-vta-$1=0//")
          [ $1 = fuse-residual ] && o=$(echo "$o" | sed "s/-vta-fuse-residual-norelu=0//; s/-vta-conv-epilogue=0//; s/-vta-evta-elemadd=0//")
          shift; k=$((k - 1)); done
        echo $o;;
  esac
}
BUNDLES="c0 c1 c2 c3 c4 c5 c6 c7 c8 all no_pad-channels no_fuse-preprocess no_conv0-pack no_keep-layout no_evta-fc no_evta-depthwise no_fuse-residual no_skip-evta-invalidate"

gen() {
  for t in ${1:-$BUNDLES}; do
    D=AblMnv2_$t; O=$B/vta/bundles/$D; o=$(opts_of $t)
    echo "== $D: $o"
    OUT=$O $H/gen_calib_bundle.sh MobileNetV2TfTest $YAML $o || { echo "$D failed ($O/gen.log)"; continue; }
    mkdir -p hw/$D
    cp $O/mobilenetv2.cpp $O/mobilenetv2.h $O/mobilenetv2.weights.bin hw/$D/
    cp $SRC/mobilenetv2Main.cpp $SRC/multi_threads_main.cpp $SRC/multi_model.h hw/$D/
    [ -f $O/VTARuntime.h ] && cp $O/VTARuntime.h hw/$D/
    echo "$o" > hw/$D/options.txt
  done
}

build() {
  for b in "build_sysabl $H/../vtalib-zcu102-systolic _sysabl" "build_abl $H/../vta/vtalib _abl"; do
    set -- $b
    mkdir -p $1; ( cd $1 && cmake -DVTALIB=$2 -DBUNDLE_EXE_SUFFIX=$3 .. > cmake.log 2>&1 || { echo "cmake $1 failed"; exit 1; }
      for t in ${TAGS:-$BUNDLES}; do make -j2 vtaAblMnv2_${t}Bundle > make_$t.log 2>&1 && echo "$1 $t ok" || echo "$1 $t FAILED ($1/make_$t.log)"; done )
  done
}

# runtime variables: R0 = every runtime feature off; the ladder turns them on one by one in this order
RT_ALL_OFF="EVTA_BUF_POOL_MB=0 EVTA_REPLAY=0 EVTA_INPUT_STREAM=0 EVTA_CONV_BURST=0 EVTA_LOAD_AHEAD=0 EVTA_REPLAY_VARIANTS=0 EVTA_BUF_POOL_SHARED=1 EVTA_WGT_FIT=0"
rt_of() {   # r<k>: the first k runtime features on
  local k=${1#r} e="$RT_ALL_OFF"; set -- EVTA_BUF_POOL_MB EVTA_REPLAY EVTA_INPUT_STREAM EVTA_CONV_BURST EVTA_LOAD_AHEAD "EVTA_REPLAY_VARIANTS EVTA_BUF_POOL_SHARED" EVTA_WGT_FIT
  while [ $k -gt 0 ] && [ $# -gt 0 ]; do for v in $1; do e=$(echo "$e" | sed -E "s/$v=[0-9]+//"); done; shift; k=$((k - 1)); done
  echo $e
}
S=vtahw_zcu102_1x16_i8w8a32_16_16_19_18
B0=${S}_sa_400mhz.bin
# configurations: "<name>|<bundle tag>|<suffix>|<bitstream>|<variables>"
list_ladder() {
  for k in 0 1 2 3 4 5 6 7 8; do echo "c$k|c$k|_sysabl|$B0|$(rt_of r0) INPUT_INT8=0"; done
  for k in 1 2 3 4 5 6 7; do echo "c8+r$k|c8|_sysabl|$B0|$(rt_of r$k) INPUT_INT8=0"; done
  for b in sa_400mhz_alu1_epr sa_400mhz_bias sa_400mhz_rq sa_alux_400mhz_rq sa_alux2_400mhz_rq sa_alux3_400mhz_rq sa_alux3_dw_387mhz_rq; do
    echo "hw_$b|c8|_sysabl|${S}_$b.bin|INPUT_INT8=0"; done
  echo "hw_dw+i8|c8|_sysabl|${S}_sa_alux3_dw_387mhz_rq.bin|INPUT_INT8=1"
  echo "hw_dw+p1|c8|_sysabl|${S}_sa_alux3_dw_387mhz_rq.bin|INPUT_PIPELINE=1"
}
list_loo() {   # on the single default bitstream (_alux3 400) and on _alux3_dw 387.5: everything on, one thing off
  for b in sa_alux3_400mhz_rq sa_alux3_dw_387mhz_rq; do
    echo "all@$b|all|_sysabl|${S}_$b.bin|INPUT_INT8=0"
    for t in $BUNDLES; do case $t in no_*) echo "$t@$b|$t|_sysabl|${S}_$b.bin|INPUT_INT8=0";; esac; done
    for v in EVTA_BUF_POOL_MB EVTA_REPLAY EVTA_INPUT_STREAM EVTA_CONV_BURST EVTA_LOAD_AHEAD EVTA_REPLAY_VARIANTS EVTA_WGT_FIT \
             EVTA_REQUANT EVTA_BIAS_RESET EVTA_ALUX EVTA_DWU; do
      echo "$v=0@$b|all|_sysabl|${S}_$b.bin|$v=0 INPUT_INT8=0"; done
  done
}
list_hls() {   # the HLS GEMM bitstreams with vta/vtalib (-s _abl), all compiler options on and all off
  for b in "zcu102_1x16_i8w8a32_16_16_19_18.bin" "${S}.bin" "${S}_380mhz.bin"; do
    for t in c0 all; do echo "hls_$t@${b%.bin}|$t|_abl|$b|INPUT_INT8=0"; done; done
}

runlk() { local l=$1; shift; while :; do "$@" > $l 2>&1; grep -q 'another EVTA run is active' $l || return 0; sleep 30; done; }
run() {
  local L=$1 R=${R:-3}
  local OUT=${OUT:-/home/xilinx/JO/ubun_wt/abl_${L}_$(date +%y%m%d_%H%M)}; mkdir -p $OUT
  local RL=$OUT/run_list.sh; : > $RL
  for t in $BUNDLES; do echo "run AblMnv2_$t vtaAblMnv2_${t}Bundle $IMG ''" >> $RL; done
  log() { echo "$*" | tee -a $OUT/summary.txt; }
  log "== $(date '+%F %T') git $(git -C .. log -1 --format=%h) list $L R $R"
  list_$L > $OUT/configs.txt
  for r in $(seq $R); do
    while IFS='|' read -r n t s b e; do
      l=$OUT/${n//[@=+]/_}_r$r.log
      runlk $l.run env $e BIN=$b RUN_LIST=$RL hw/script/run_prof.sh -s $s -n ${N:-25} AblMnv2_$t
      cp hw/AblMnv2_$t/${s#_}.log $l 2>/dev/null
      log "$n r$r: $(grep -h 'THROUGHPUT 1' $l | sed -E 's/.*\(([0-9.]+) ms.*/\1 ms/'); $(grep -ho 'Result: [0-9]* Confidence: [0-9.]*' $l | head -1); $(grep -h 'wrong' $l | awk '{s+=$(NF-1)} END {print s+0}') wrong"
    done < $OUT/configs.txt
  done
  [ -n "${SUDO_USER:-}" ] && chown -R $SUDO_USER $OUT
  log "== done"
}

case ${1:-} in
  gen) gen "${2:-}";; build) build;; run) run ${2:?list: ladder loo hls};;
  opts) opts_of ${2:?tag};; list) list_${2:?list};;
  *) sed -n 's/^####   //p' $0; exit 1;;
esac
