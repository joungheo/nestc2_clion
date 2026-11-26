#!/bin/bash
#### JO added (2026-11-26, docs/evta_perf_latest.md): the record of the latest times of the seven models on the board, in one way for
#### every bitstream: the systolic _sys executables (generic main, int8 input), OMP 1, 4 x 25 inferences per executable alone
#### (THROUGHPUT 1 copies: ms per image) and 25 per copy with the 4 copies at once (THROUGHPUT 4 copies: images/s), rounds
#### alternating between the bitstreams given. Steps (any order, default all):
####   i8      <Dir>/i8/<image>.i8 for every Dir that has none (INPUT_INT8_SAVE, the bundle's own quantize; needs the executable)
####   check   EVTA_RES_CHECK=1 on every Dir (every fused EVTA call against the CPU): checks / differ / class per executable
####   m4      the 4-EVTA Dirs on each of $M4_BITS (default: SYS_BIN of hw_multievtax4/script/run_all.sh)
####   single  the 1-EVTA Dirs on each of $S1_BITS (default: SYS_BIN of hw/script/run_all.sh)
#### Logs and summary.txt in $OUT (default /tmp/perf_record_<date>). Run on the board as root:
####   sudo /bin/sh -c "vtabundles_xcmpl/perf_record.sh [-r <rounds, 3>] [step ...]"
####   e.g. M4_BITS="<new>.bin <old>.bin" S1_BITS="<a>.bin <b>.bin" ... perf_record.sh -r 3 m4 single
#### run_prof.sh does the bitstream load, the lock and the per-executable log; times are read from those logs.
set -u
cd "$(dirname "$0")"
R=3
[ "${1:-}" = -r ] && { R=$2; shift 2; }
STEPS=${*:-i8 check m4 single}
OUT=${OUT:-/tmp/perf_record_$(date +%y%m%d_%H%M)}; mkdir -p $OUT
M4="MultiEVTAResnet18Test MultiEVTAResnet50Test MultiEVTAResnet50TvTest MultiEVTAVgg16Cifar10Test MultiEVTAMobileNetV2Test MultiEVTAMobileNetV2TfTest MultiEVTADeiTTest"
S1="Resnet18Test Resnet50Test Resnet50TvTest Vgg16Cifar10Test MobileNetV2Test MobileNetV2TfTest DeiTTest"
sysbin() { sed -n 's/^SYS_BIN=\${SYS_BIN:-\(.*\)}$/\1/p' $1/script/run_all.sh; }
M4_BITS=${M4_BITS:-$(sysbin hw_multievtax4)}
S1_BITS=${S1_BITS:-$(sysbin hw)}
IMG=$(cd .. && pwd)/glow/tests/images/imagenet/cat_285.png
VIMG=$(cd .. && pwd)/examples/bundles/vgg16_cifar10/cifar10/test/airplane/0001.png
export OMP_NUM_THREADS=1
tag() { local t=${1#vtahw_zcu102_1x16_i8w8a32_}; echo ${t%.bin}; }
log() { echo "$*" | tee -a $OUT/summary.txt; }
#### JO added (2026-11-26, ubun): run_prof.sh / load_bit.sh exit at once when another EVTA run holds /tmp/evta_run.lock, and the
#### <Dir> logs of an earlier run stay where they were, so the steps read old times as new. Now every run waits for the lock
#### (retry every 30 s, up to LOCK_WAIT seconds, default 1800) and only logs written after the run started are read (fresh);
#### a Dir with no fresh log prints "no new log".
LOCK_WAIT=${LOCK_WAIT:-1800}
STAMP=$OUT/.stamp
locked() { grep -q 'another EVTA run is active' $1; }
runlk() { # <output file> <command...>: the command, again while the lock is held; 1 if it never got the lock
  local l=$1 w=0; shift
  while :; do
    touch $STAMP; "$@" > $l 2>&1
    locked $l || return 0
    [ $w -ge $LOCK_WAIT ] && { log "ERROR: $l: /tmp/evta_run.lock still held after $LOCK_WAIT s (another EVTA run), not run"; return 1; }
    [ $w = 0 ] && log "wait: /tmp/evta_run.lock held by another EVTA run ($l)"
    sleep 30; w=$((w + 30))
  done
}
fresh() { for f; do [ -f "$f" ] && [ ! "$f" -ot $STAMP ] && echo $f; done; }  # not older than the run start
log "== perf_record.sh $(date '+%F %T')  rounds $R  steps $STEPS  git $(git -C .. log -1 --format=%h)"
for step in $STEPS; do case $step in
i8)
  for p in $(for d in $M4; do echo hw_multievtax4/$d; done) $(for d in $S1; do echo hw/$d; done); do
    im=$IMG; case $p in *Vgg16*) im=$VIMG;; esac; i=$p/i8/$(basename ${im%.png}).i8
    [ -s $i ] && continue
    case $p in hw/*) e=$(ls $p/vta*Bundle_sys 2>/dev/null | head -1); b=$(echo $S1_BITS | cut -d' ' -f1);;
               *) e=$(ls $p/singlevta*Bundle_sys 2>/dev/null | head -1); b=$(echo $M4_BITS | cut -d' ' -f1);; esac
    [ -n "$e" ] || { log "i8: $p has no _sys executable"; continue; }
    runlk $OUT/load_bit.log hw/script/load_bit.sh $b || continue
    log "i8: $p: $(cd $p && INPUT_INT8_SAVE=i8 timeout 300 ./$(basename $e) $im 2>&1 | tail -1)"
  done ;;
check)
  for b in $(echo $M4_BITS | cut -d' ' -f1); do
    runlk $OUT/check_m4.log env EVTA_RES_CHECK=1 BIN=$b hw_multievtax4/script/run_prof.sh -s _sys -n 2 $M4 || continue
    for d in $M4; do fs=$(fresh hw_multievtax4/$d/sys_*.log); [ -n "$fs" ] || log "check $(tag $b) hw_multievtax4/$d: no new log"
    for f in $fs; do
      log "check $(tag $b) $f: $(grep -c differ $f) checks, $(grep -oE '[0-9]+ of [0-9]+ differ' $f | awk '{s+=$1} END {print s+0}') differ, $(grep -m1 -oE 'Result: [0-9]+|top1: [0-9]+' $f)"
    done; done
  done
  for b in $(echo $S1_BITS | cut -d' ' -f1); do
    runlk $OUT/check_s1.log env EVTA_RES_CHECK=1 BIN=$b hw/script/run_prof.sh -s _sys -n 2 $S1 || continue
    for d in $S1; do f=$(fresh hw/$d/sys.log); [ -n "$f" ] || { log "check $(tag $b) hw/$d: no new log"; continue; }
      log "check $(tag $b) $f: $(grep -c differ $f) checks, $(grep -oE '[0-9]+ of [0-9]+ differ' $f | awk '{s+=$1} END {print s+0}') differ, $(grep -m1 -oE 'Result: [0-9]+|top1: [0-9]+' $f)"
    done
  done ;;
m4)
  for r in $(seq $R); do for b in $M4_BITS; do t=$(tag $b)
    runlk $OUT/m4_${t}_r$r.log env BIN=$b hw_multievtax4/script/run_prof.sh -s _sys -n 25 $M4 || continue
    for d in $M4; do
      fm=$(fresh hw_multievtax4/$d/sys_multivta*.log); fs=$(fresh hw_multievtax4/$d/sys_singlevta*.log)
      m="no new log"; [ -n "$fm" ] && m=$(grep -h 'THROUGHPUT 4' $fm | sed -E 's/.*= ([0-9.]+) images.*\(([0-9.]+) ms.*/\1 fps \2 ms/')
      s="no new log"; [ -n "$fs" ] && s=$(grep -h 'THROUGHPUT 1' $fs | sed -E 's/.*\(([0-9.]+) ms.*/\1 ms/')
      log "m4 $t r$r $d: 4 at once $m per copy; alone $s"
    done
  done; done ;;
single)
  for r in $(seq $R); do for b in $S1_BITS; do t=$(tag $b)
    runlk $OUT/s1_${t}_r$r.log env BIN=$b hw/script/run_prof.sh -s _sys -n 25 $S1 || continue
    for d in $S1; do
      f=$(fresh hw/$d/sys.log); [ -n "$f" ] || { log "s1 $t r$r $d: no new log"; continue; }
      log "s1 $t r$r $d: $(grep -h 'THROUGHPUT 1' $f | sed -E 's/.*: ([0-9]+) images.*\(([0-9.]+) ms.*/\2 ms (\1 images)/')"
    done
  done; done ;;
*) log "unknown step $step";;
esac; done
[ -n "${SUDO_USER:-}" ] && chown -R $SUDO_USER $OUT
log "== done: $OUT/summary.txt"
