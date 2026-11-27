#!/bin/bash
#### JO added (2026-11-27, docs/evta_hw_versions.md §4.2): run a saved executable set (save_exe_set.sh) on a bitstream: reloads it
#### before each executable (as run_all.sh), runs every executable of the given Dirs from the set's own dir (its weights and i8
#### inputs; sets in $BITDIR/exe_sets, default /home/xilinx/JO/npuprg/bit), prints the result and THROUGHPUT lines. Board, as root:
####   sudo /bin/sh -c "BIN=<file in /lib/firmware> vtabundles_xcmpl/run_exe_set.sh <set name|dir> [-n <runs, 25>] [hw*/Dir ...]"
####   EVTA_RES_CHECK=1 etc. pass through. No Dir = every Dir of the set. Logs: <set>/<hw dir>/<Dir>/run_<bitstream tag>_<exe>.log
set -u
TREE=$(cd "$(dirname "$0")/.." && pwd)
[ $# -ge 1 ] || { sed -n '2,6p' "$0" | sed 's/^#### \{0,1\}//'; exit 1; }
S=$1; shift; [ -d "$S" ] || S=${BITDIR:-/home/xilinx/JO/npuprg/bit}/exe_sets/$S
[ -f $S/MANIFEST.txt ] || { echo "$S: not an executable set"; exit 1; }
RUNS=25; [ "${1:-}" = -n ] && { RUNS=$2; shift 2; }
: ${BIN:?BIN=<bitstream in /lib/firmware>}
[ -f /lib/firmware/$BIN ] || { echo "/lib/firmware/$BIN not found"; exit 1; }
[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
exec 9>/tmp/evta_run.lock; flock -n 9 || { echo "another EVTA run is active (/tmp/evta_run.lock)"; exit 1; }
DIRS=${*:-$(cd $S && ls -d hw*/*/ | sed 's#/$##')}
export OMP_NUM_THREADS=${OMP_NUM_THREADS:-1} MULTI_RUNS=$RUNS
T=${BIN#vtahw_zcu102_1x16_i8w8a32_}; T=${T%.bin}
. $TREE/vtabundles_xcmpl/bininfo.sh; echo "== run_exe_set.sh $S $(date '+%F %T')"; bin_info $BIN
for p in $DIRS; do
  case $p in *Vgg16*) IMG=$TREE/examples/bundles/vgg16_cifar10/cifar10/test/airplane/0001.png;;
             *) IMG=$TREE/glow/tests/images/imagenet/cat_285.png;; esac
  for e in $(cd $S/$p && find . -maxdepth 1 -type f -perm -u+x -name '*Bundle_*' ! -name '*.log' | sed 's#^\./##' | sort); do
    echo 0 > /sys/class/fpga_manager/fpga0/flags
    if for i in $(seq 24); do echo $BIN > /sys/class/fpga_manager/fpga0/firmware 2>/dev/null && break; sleep 5; false; done
    then echo $BIN > /tmp/evta_bitstream; else echo "FAILED $BIN" > /tmp/evta_bitstream; echo "bitstream load failed"; exit 1; fi
    ( mkdir -p /tmp/xlnk_trace && cd /tmp/xlnk_trace && python3 -c "from pynq import Xlnk; Xlnk().xlnk_reset()" 2>/dev/null )
    L=$S/$p/run_${T}_$e.log
    ( cd $S/$p && timeout -k 10 600 ./$e $IMG > $L 2>&1; echo "rc $?" >> $L )
    echo "$p/$e: $(grep -m1 -oE 'Result: [0-9]+( Confidence: [0-9.]+)?|top1: [0-9]+' $L) $(grep -oE '[0-9]+ of [0-9]+ differ' $L | awk '{s+=$1;n++} END {if (n) print "| " n " checks " s " differ"}') | $(tail -1 $L)"
    grep -h -e 'THROUGHPUT' -e '^Inference time' $L | sed 's/^/    /'
  done
done
