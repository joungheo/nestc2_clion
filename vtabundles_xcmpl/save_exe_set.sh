#!/bin/bash
#### JO added (2026-11-27, docs/evta_hw_versions.md §4.2): keep a set of tested executables with what they need to run (weights,
#### int8 inputs) beside the bitstreams, so an old bitstream can be run later with the executables it was measured with, without
#### a rebuild. The set goes to $BITDIR/exe_sets/<name>/<hw dir>/<Dir>/ (BITDIR default /home/xilinx/JO/npuprg/bit, the board's
#### bitstream archive) with MANIFEST.txt (per executable: md5, the NESTC_BUILD stamp = vtalib commit, the bundle's model-compiler
#### line); run it with run_exe_set.sh. Board, no sudo needed:
####   vtabundles_xcmpl/save_exe_set.sh [-s <suffix, _sys>] <name> <hw*/Dir> ...
####   e.g. vtabundles_xcmpl/save_exe_set.sh exe_261127_38e3da6 hw/MobileNetV2TfTest hw_multievtax4/MultiEVTAMobileNetV2TfTest
#### Then note the set in the bitstreams' build_info.txt (utils/hwver/build_info.sh add <info> exe "<set>: ...").
set -u
cd "$(dirname "$0")"
SUF=_sys; [ "${1:-}" = -s ] && { SUF=$2; shift 2; }
[ $# -ge 2 ] || { sed -n '2,10p' "$0" | sed 's/^#### \{0,1\}//'; exit 1; }
NAME=$1; shift
D=${BITDIR:-/home/xilinx/JO/npuprg/bit}/exe_sets/$NAME; mkdir -p $D
M=$D/MANIFEST.txt
[ -f $M ] || { echo "# executable set $NAME (vtabundles_xcmpl/save_exe_set.sh, docs/evta_hw_versions.md)"; echo "saved: $(date '+%F %T') tree $(git -C .. log -1 --format='%h %ad' --date=format:'%F %T')"; } > $M
for p in "$@"; do
  p=${p%/}; [ -d $p ] || { echo "$p: no such dir"; continue; }
  exes=$(ls $p/*Bundle$SUF 2>/dev/null); [ -n "$exes" ] || { echo "$p: no *Bundle$SUF"; continue; }
  mkdir -p $D/$p; cp -p $exes $p/*.weights.bin $p/multi_model.h $D/$p/ 2>/dev/null
  [ -d $p/i8 ] && cp -rp $p/i8 $D/$p/
  for e in $exes; do
    mc=$(grep -h -m1 -o 'model-compiler: .*' $p/*.cpp 2>/dev/null | head -1 | cut -c1-90)
    echo "$p/$(basename $e): md5 $(md5sum < $e | cut -c1-12) | $(LC_ALL=C grep -a -o -m1 'NESTC_BUILD [ -~]*' $e | head -1 | sed 's/".*//') | ${mc:-no model-compiler line}" >> $M
  done
  echo "$p: $(du -sh $D/$p | cut -f1)"
done
echo "== $D ($(du -sh $D | cut -f1)); df /: $(df -h / | tail -1 | awk '{print $4 " free"}')"
