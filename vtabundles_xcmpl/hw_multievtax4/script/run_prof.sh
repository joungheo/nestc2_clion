#!/bin/bash
# Run profiling bundles and keep each full output in <Dir>/<suffix>.log:
#   _prof  (default) built in ../build_prof  (-DNESTC_EVTA_PROFILE=ON -DBUNDLE_EXE_SUFFIX=_prof): per VTA run
#   _timed           built in ../build_timed (-DBUNDLE_TIMED=ON -DBUNDLE_EXE_SUFFIX=_timed): per statement,
#                    also <Dir>/timing.csv
# Executable, input and golden come from the "run <dir> <exe> <input> <golden>" lines of run_all.sh. Reloads the bitstream before each run.
# usage: sudo hw/script/run_prof.sh [-s _timed] [-n <runs, default 25>] [-l] [-r] [-t <threads>] Dir [Dir ...]     (or hw_<set>/script/run_prof.sh)
#        sudo hw/script/run_prof.sh [-s _timed] [all|hw_bundles]   (no Dir = every run line of run_all.sh)
# e.g.:  sudo hw_multievtax4/script/run_prof.sh -s _sysprof -n 25 -l -r MultiEVTAResnet18Test
#        sudo hw/script/run_prof.sh -s _systimed -r -t 4 Resnet18Test
#        (-s suffix: _prof _timed _sysprof _systimed _sys; -n runs; -l lockstep (multi-EVTA); -r chrt -f 10;
#         -t OMP_NUM_THREADS, default 1 or OMP_NUM_THREADS of the environment)
# One line per Dir (exit, golden check, total time) is also written to result/<suffix>_summary.txt, printed at the end.
#### JO added (2026-10-26): also in hw_multievtax2/ and hw_multievtax4/ (copied there by collect_hw_bundles.py):
#### build dirs build_<set>_prof / build_<set>_timed. An executable with several timed .cpp (one per EVTA or VTA
#### partition) prints one table each (header names the .cpp) and writes timing_<cpp>.csv each.
set -u
#### JO added (2026-10-29): files written here are not writable by group / others
umask 022
#### JO changed (2026-10-27): this script is in <hw dir>/script/ (the part of the hw dir in git); runs in the hw dir
#cd "$(dirname "$0")"
cd "$(dirname "$0")/.."; RUN_ALL=script/run_all.sh
#### JO added: build dir prefix of this hw dir (hw/ -> ../build, hw_<set>/ -> ../build_<set>)
HWD=$(basename "$PWD"); BLD=../build${HWD#hw}
#### JO changed: bitstream from run_all.sh of this hw dir (hw/: single EVTA, hw_multievtax2/: 2 EVTAs)
#BIN=zcu102_1x16_i8w8a32_16_16_19_18.bin
#### JO changed (2026-10-26): run_all.sh has BIN=${BIN:-<file>} -> take <file>; BIN=<file> in the environment wins
#BIN=$(sed -n "s/^BIN=//p" run_all.sh)
#### JO changed (2026-10-28): after -s (below): -s _sys* takes SYS_BIN of run_all.sh (the systolic bitstream)
#BIN=${BIN:-$(sed -n 's/^BIN=\${BIN:-\(.*\)}$/\1/p; s/^BIN=\([^$].*\)$/\1/p' $RUN_ALL)}
#[ -f /lib/firmware/$BIN ] || { echo "bitstream /lib/firmware/$BIN not found"; exit 1; }
[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
#### JO added (2026-10-26): one EVTA run at a time on the board (the same lock as run_all.sh): each run reloads the
#### PL, and two runs swap bitstreams under each other (SError, bus errors, mismatches)
exec 9>/tmp/evta_run.lock; flock -n 9 || { echo "another EVTA run is active (/tmp/evta_run.lock)"; exit 1; }
SUFFIX=_prof
#### JO changed (2026-10-29): -n <runs> (default 25) = inferences per executable, through BUNDLE_RUNS (timed_bundle.py
#### wrapper in the _prof and _timed builds) in hw/; in hw_<set>/ runs per EVTA copy (MULTI_RUNS, MultiEVTAResnet18Test)
# [ "${1:-}" = -s ] && { SUFFIX=$2; shift 2; }
RUNS=25
#### JO changed (2026-10-29): -l = MULTI_LOCKSTEP=1 (multi-EVTA _prof/_timed: the copies start every EVTA call together)
# while [ "${1:-}" = -s ] || [ "${1:-}" = -n ]; do
#   [ "$1" = -s ] && SUFFIX=$2 || RUNS=$2; shift 2
# done
#### JO changed (2026-10-29): -r = run the executable with SCHED_FIFO priority 10 (chrt -f 10): other processes do not take
#### a core from the EVTA polling threads (a late completion costs a 4 ms tick, CONFIG_HZ=250); the kernel still keeps
#### 5 % of the CPU for the others (sched_rt_runtime_us 950000)
RT=""
#### JO changed (2026-10-31): -t <threads> = OMP_NUM_THREADS of the executable, default 1 for every set (single EVTA too;
#### OMP_NUM_THREADS in the environment is the default instead): the multi-EVTA copies already fill the 4 cores, and the
#### single-EVTA numbers are then comparable with one copy of them; -t 4 for the CPU ops of a single EVTA on 4 cores
# while [ "${1:-}" = -s ] || [ "${1:-}" = -n ] || [ "${1:-}" = -l ] || [ "${1:-}" = -r ]; do
#   case $1 in -l) export MULTI_LOCKSTEP=1; shift;; -r) RT="chrt -f 10"; shift;; -s) SUFFIX=$2; shift 2;; *) RUNS=$2; shift 2;; esac
# done
OMP=${OMP_NUM_THREADS:-1}
while [ "${1:-}" = -s ] || [ "${1:-}" = -n ] || [ "${1:-}" = -l ] || [ "${1:-}" = -r ] || [ "${1:-}" = -t ]; do
  case $1 in -l) export MULTI_LOCKSTEP=1; shift;; -r) RT="chrt -f 10"; shift;; -s) SUFFIX=$2; shift 2;; -t) OMP=$2; shift 2;; *) RUNS=$2; shift 2;; esac
done
export OMP_NUM_THREADS=$OMP
#### JO changed (2026-10-29): hw_<set>/: MULTI_RUNS = runs per EVTA copy of the multi-EVTA mains (4 x for single_thread_main)
# [ "$HWD" = hw ] && export BUNDLE_RUNS=$RUNS || unset BUNDLE_RUNS
if [ "$HWD" = hw ]; then export BUNDLE_RUNS=$RUNS; unset MULTI_RUNS; else export MULTI_RUNS=$RUNS; unset BUNDLE_RUNS; fi
#### JO added (2026-10-28): _sys* executables are built with vtalib-zcu102-systolic (weight-stationary uop order)
#### -> run them on the systolic bitstream SYS_BIN of run_all.sh; BIN=<file> in the environment still wins
V=BIN; case $SUFFIX in _sys*) V=SYS_BIN;; esac
BIN=${BIN:-$(sed -n 's/^'$V'=\${'$V':-\(.*\)}$/\1/p; s/^'$V'=\([^$].*\)$/\1/p' $RUN_ALL)}
[ -n "$BIN" ] && [ -f /lib/firmware/$BIN ] || { echo "bitstream /lib/firmware/$BIN not found ($V in $RUN_ALL)"; exit 1; }
#### JO changed (2026-10-29): set per run line below
# LOG=${SUFFIX#_}.log
#### JO changed: no Dir / "all" / "hw_bundles" runs every bundle listed in run_all.sh
# [ $# -gt 0 ] || { echo "usage: sudo ./run_prof.sh [-s _timed] Dir [Dir ...]"; exit 1; }
if [ $# = 0 ] || [ "$1" = all ] || [ "$1" = hw_bundles ]; then
  set -- $(sed -n "s/^run \([^ ]*\) .*/\1/p" $RUN_ALL | uniq)
fi
#### JO changed (2026-10-27): summaries in result/
#SUM=${SUFFIX#_}_summary.txt; : > $SUM
#### JO changed (2026-10-27): + trace/ (sds_trace_data.dat of the Xlnk reset)
mkdir -p result trace; chown --reference=. result trace; SUM=result/${SUFFIX#_}_summary.txt; : > $SUM

for D in "$@"; do
  #### JO changed: a Dir can have several run lines (multi-EVTA: threaded + single-thread executable)
  # line=$(grep -E "^run $D " run_all.sh) || { echo "$D: not in run_all.sh"; continue; }
  grep -qE "^run $D " $RUN_ALL || { echo "$D: not in $RUN_ALL"; continue; }
  while read -r line <&3; do
  eval "set -- $line"     # -> run dir exe input golden
  EXE=$3$SUFFIX; IN=$4; GOLD=$5
  #### JO added (2026-10-29): a Dir with several run lines (multi-EVTA: threaded + single-thread executable) gets one log
  #### per executable, <suffix>_<exe>.log; with one shared <suffix>.log the second executable overwrote the first
  LOG=${SUFFIX#_}.log
  [ $(grep -cE "^run $D " $RUN_ALL) -gt 1 ] && LOG=${SUFFIX#_}_$3.log
  echo "================ $D ($EXE)"
  #### JO changed: also record it in the summary
  # [ -x $D/$EXE ] || { echo "$D/$EXE not built (cd ../build${SUFFIX} && make $3)"; continue; }
  [ -x $D/$EXE ] || { echo "$D/$EXE not built (cd ${BLD}${SUFFIX} && make $3)"
                      printf "%-30s %-34s NOT BUILT\n" $D $EXE >> $SUM; continue; }
  echo 0 > /sys/class/fpga_manager/fpga0/flags; echo $BIN > /sys/class/fpga_manager/fpga0/firmware
  #### JO changed (2026-10-27): in trace/ (libcma.so writes sds_trace_data.dat to the working dir)
  #python3 -c "from pynq import Xlnk; Xlnk().xlnk_reset()" 2>/dev/null
  ( cd trace && python3 -c "from pynq import Xlnk; Xlnk().xlnk_reset()" 2>/dev/null )
  #### JO changed: keep exit code and golden result for the summary
  # ( cd $D && rm -f output.bin && timeout 300 ./$EXE $IN > $LOG 2>&1; echo "exit=$? (124 = timeout)" )
  # [ -n "$GOLD" ] && [ -f $D/output.bin ] && { cmp -s $D/output.bin $D/$GOLD && echo "RESULT: PASS" || echo "RESULT: MISMATCH"; }
  #### JO changed (2026-10-29): + $RT (-r)
  # ( cd $D && rm -f output.bin && timeout 300 ./$EXE $IN > $LOG 2>&1 ); rc=$?
  ( cd $D && rm -f output.bin && timeout 300 $RT ./$EXE $IN > $LOG 2>&1 ); rc=$?
  echo "exit=$rc (124 = timeout)"
  res="no golden"
  [ -n "$GOLD" ] && { [ -f $D/output.bin ] && cmp -s $D/output.bin $D/$GOLD && res=PASS || res=MISMATCH; }
  echo "RESULT: $res"
  chown -R --reference=. $D
  grep -E "event buffer full|total_time|^run [0-9]+ \(" $D/$LOG
  sed -n "/^==== timing/,\$p" $D/$LOG
  echo "  VTA events: $(grep -c '^VTA_SYNC' $D/$LOG)   full log: $D/$LOG"
  #### JO added (2026-10-29): _prof logs: runs, VTA time per run and GOPS (ops from the bundle .cpp; ../prof_summary.py)
  #### JO changed (2026-10-29): <log>.sum owned like the Dir (run_prof.sh runs as root), group/others read only
  # ps=""; grep -q '^VTA_SYNC' $D/$LOG && { python3 ../prof_summary.py $D $D/$LOG > $D/$LOG.sum 2>&1; cat $D/$LOG.sum; ps=$(tail -1 $D/$LOG.sum); }
  #### JO changed (2026-10-31): SUM: lines of prof_summary.py; one -> on the summary line, several (multi-EVTA) -> a line each below it
  # ps=""; grep -q 'VTA_SYNC' $D/$LOG && { python3 ../prof_summary.py $D $D/$LOG > $D/$LOG.sum 2>&1; chown --reference=$D $D/$LOG.sum
  #                                         chmod 644 $D/$LOG.sum; cat $D/$LOG.sum; ps=$(tail -1 $D/$LOG.sum); }
  ps=""; more=""
  if grep -q 'VTA_SYNC' $D/$LOG; then
    python3 ../prof_summary.py $D $D/$LOG > $D/$LOG.sum 2>&1; chown --reference=$D $D/$LOG.sum; chmod 644 $D/$LOG.sum
    grep -v '^SUM: ' $D/$LOG.sum
    if [ "$(grep -c '^SUM: ' $D/$LOG.sum)" = 1 ]; then ps=$(sed -n 's/^SUM: //p' $D/$LOG.sum)
    else more=$(sed -n 's/^SUM: /    /p' $D/$LOG.sum); fi
  fi
  # summary: total of the last run (_timed) or the VTA total_time line (_prof)
  #### JO changed: a partitioned bundle prints one timing table per VTA partition .cpp -> sum the last run of each
  # t=$(grep -E "^run #[0-9]+: total" $D/$LOG | tail -1 | sed "s/, statements.*//"); [ -n "$t" ] || t=$(grep -m1 total_time $D/$LOG)
  #### JO changed: several tables are also the per-EVTA copies of a multi-EVTA run (concurrent): list each
  # t=$(awk '/^==== timing/ {n++} /^run #[0-9]+: total/ {r[n]=$2; ms[n]=$4}
  #          END {if (n == 1) print "run " r[1] " total " ms[1] " ms";
  #               else if (n > 1) {for (i = 1; i <= n; i++) s += ms[i]; printf "%d timed partitions: sum %.3f ms (CPU .o partitions not timed)\n", n, s}}' $D/$LOG)
  t=$(awk '/^==== timing/ {n++} /^run #[0-9]+: total/ {r[n]=$2; ms[n]=$4}
           END {if (n == 1) print "run " r[1] " total " ms[1] " ms";
                else if (n > 1) {l = ""; for (i = 1; i <= n; i++) {s += ms[i]; l = l (i > 1 ? " / " : "") ms[i]}
                                 printf "%d timed .cpp, last run: %s ms (sum %.3f)\n", n, l, s}}' $D/$LOG)
  #### JO added: VTA partitions run one after another (sum = the run), multi-EVTA copies at once (max = the run)
  [ -n "$t" ] || t=$(grep -m1 total_time $D/$LOG)
  [ -n "$ps" ] && t="$t; $ps"
  #### JO added (2026-10-29): _timed with one table: EVTA calls / CPU conv/fc of the last run (timed_bundle.py summary)
  [ "$(grep -c '^==== timing' $D/$LOG)" = 1 ] && { e=$(grep '^run #[0-9]* ops:' $D/$LOG | tail -1 | sed 's/^run #[0-9]* ops: //; s/; whole run.*//')
                                                 [ -n "$e" ] && t="$t; $e"; }
  #### JO added (2026-10-31): _timed: whole-run ms of every run (timing csv), then the median of runs 2..n and its GOPS
  #### (../run_times.py); the per-run line goes to the output, the medians to the summary
  if grep -q 'per statement and run:' $D/$LOG; then
    python3 ../run_times.py $D $D/$LOG > $D/$LOG.runs 2>&1; chown --reference=$D $D/$LOG.runs; chmod 644 $D/$LOG.runs
    #### JO changed (2026-10-31): SUM: lines; one -> on the summary line, several (multi-EVTA copies) -> a line each below it
    # cat $D/$LOG.runs; a=$(tail -1 $D/$LOG.runs); [ -n "$a" ] && t="$t; $a"
    grep -v '^SUM: ' $D/$LOG.runs
    if [ "$(grep -c '^SUM: ' $D/$LOG.runs)" = 1 ]; then t="$t; $(sed -n 's/^SUM: //p' $D/$LOG.runs)"
    else more=$(sed -n 's/^SUM: /    /p' $D/$LOG.runs); fi
  fi
  #### JO changed: executable column, end of the per-line loop
  # printf "%-30s exit=%-3s %-10s %s\n" $D $rc "$res" "$t" >> $SUM
  #### JO changed (2026-10-31): + omp=<OMP_NUM_THREADS>
  # printf "%-30s %-34s exit=%-3s %-10s %s\n" $D $EXE $rc "$res" "$t" >> $SUM
  printf "%-30s %-34s exit=%-3s omp=%-2s %-10s %s\n" $D $EXE $rc $OMP "$res" "$t" >> $SUM
  [ -n "$more" ] && echo "$more" >> $SUM
  done 3< <(grep -E "^run $D " $RUN_ALL)
done
echo; echo "== summary ($SUM)"; cat $SUM
chown --reference=. $SUM
