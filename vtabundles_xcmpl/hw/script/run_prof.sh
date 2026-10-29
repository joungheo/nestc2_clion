#!/bin/bash
# Run profiling bundles and keep each full output in <Dir>/<suffix>.log:
#   _prof  (default) built in ../build_prof  (-DNESTC_EVTA_PROFILE=ON -DBUNDLE_EXE_SUFFIX=_prof): per VTA run
#   _timed           built in ../build_timed (-DBUNDLE_TIMED=ON -DBUNDLE_EXE_SUFFIX=_timed): per statement,
#                    also <Dir>/timing.csv
# Executable, input and golden come from the "run <dir> <exe> <input> <golden>" lines of run_all.sh. Reloads the bitstream before each run.
# usage: sudo hw/script/run_prof.sh [-s _timed] [-n <runs, default 25>] Dir [Dir ...]     (or hw_<set>/script/run_prof.sh)
#        sudo hw/script/run_prof.sh [-s _timed] [all|hw_bundles]   (no Dir = every run line of run_all.sh)
# One line per Dir (exit, golden check, total time) is also written to result/<suffix>_summary.txt, printed at the end.
#### JO added (2026-10-26): also in hw_multievtax2/ and hw_multievtax4/ (copied there by collect_hw_bundles.py):
#### build dirs build_<set>_prof / build_<set>_timed. An executable with several timed .cpp (one per EVTA or VTA
#### partition) prints one table each (header names the .cpp) and writes timing_<cpp>.csv each.
set -u
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
#### wrapper in the _prof and _timed builds); only for hw/ (single set): the multi-EVTA mains loop by themselves
# [ "${1:-}" = -s ] && { SUFFIX=$2; shift 2; }
RUNS=25
while [ "${1:-}" = -s ] || [ "${1:-}" = -n ]; do
  [ "$1" = -s ] && SUFFIX=$2 || RUNS=$2; shift 2
done
[ "$HWD" = hw ] && export BUNDLE_RUNS=$RUNS || unset BUNDLE_RUNS
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
  ( cd $D && rm -f output.bin && timeout 300 ./$EXE $IN > $LOG 2>&1 ); rc=$?
  echo "exit=$rc (124 = timeout)"
  res="no golden"
  [ -n "$GOLD" ] && { [ -f $D/output.bin ] && cmp -s $D/output.bin $D/$GOLD && res=PASS || res=MISMATCH; }
  echo "RESULT: $res"
  chown -R --reference=. $D
  grep -E "event buffer full|total_time|^run [0-9]+ \(" $D/$LOG
  sed -n "/^==== timing/,\$p" $D/$LOG
  echo "  VTA events: $(grep -c '^VTA_SYNC' $D/$LOG)   full log: $D/$LOG"
  #### JO added (2026-10-29): _prof logs: runs, VTA time per run and GOPS (ops from the bundle .cpp; ../prof_summary.py)
  ps=""; grep -q '^VTA_SYNC' $D/$LOG && { python3 ../prof_summary.py $D $D/$LOG > $D/$LOG.sum 2>&1; cat $D/$LOG.sum; ps=$(tail -1 $D/$LOG.sum); }
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
  #### JO changed: executable column, end of the per-line loop
  # printf "%-30s exit=%-3s %-10s %s\n" $D $rc "$res" "$t" >> $SUM
  printf "%-30s %-34s exit=%-3s %-10s %s\n" $D $EXE $rc "$res" "$t" >> $SUM
  done 3< <(grep -E "^run $D " $RUN_ALL)
done
echo; echo "== summary ($SUM)"; cat $SUM
chown --reference=. $SUM
