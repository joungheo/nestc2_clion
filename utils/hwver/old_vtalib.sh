#!/bin/bash
#### JO added (2026-11-27, docs/evta_hw_versions.md §4.3): executables with an OLD vtalib-zcu102-systolic, for a bitstream the
#### current vtalib no longer runs as it did (an ISA field moved, a feature changed under the same name). The main tree stays as
#### it is: the old commit is checked out as a git worktree beside the tree, and a new vtabundles_xcmpl build dir points VTALIB
#### at its vtalib (the bundles in hw*/<Dir>/ are today's; when they call a function the old vtalib lacks, the link fails, see
#### the doc). usage:
####   old_vtalib.sh <tag|commit> [single|multievtax4] <target> ...      e.g. old_vtalib.sh hw-multi4-amul-261123 single vtaMobileNetV2TfBundle
#### board (aarch64): build dir vtabundles_xcmpl/build[_multievtax4]_sysv_<name>, executables hw*/<Dir>/<target>_sysv_<name>,
####   run: sudo hw*/script/run_prof.sh -s _sysv_<name> ... (BIN=<old bitstream>)
#### PC: in the nestc-build container, build_pc[_m4]_pcsysv_<name>, executables <target>_pcsysv_<name> (scp them to the board)
#### remove: git worktree remove <tree>/../nestc_hwver/<name>; rm -rf the build dir
set -eu
TREE=$(cd "$(dirname "$0")/../.." && pwd)
[ $# -ge 2 ] || { sed -n '2,13p' "$0" | sed 's/^#### \{0,1\}//'; exit 1; }
REV=$1; shift
SET=single; case $1 in single|multievtax4) SET=$1; shift;; esac
[ $# -ge 1 ] || { echo "no target"; exit 1; }
git -C "$TREE" rev-parse -q --verify "$REV^{commit}" > /dev/null || { echo "$REV: no such tag / commit (git fetch --tags?)"; exit 1; }
if git -C "$TREE" show-ref -q --tags "$REV"; then NAME=$REV; else NAME=$(git -C "$TREE" rev-parse --short "$REV"); fi
NAME=$(echo "$NAME" | tr -c 'A-Za-z0-9_.\n-' '_')
WT=$(cd "$TREE/.." && pwd)/nestc_hwver/$NAME
if [ ! -d "$WT" ]; then mkdir -p "$(dirname "$WT")"; git -C "$TREE" worktree add --detach "$WT" "$REV"; fi
VL=$WT/vtalib-zcu102-systolic
[ -d "$VL" ] || { echo "$VL missing (that commit has no systolic vtalib)"; exit 1; }
echo "vtalib: $VL ($(git -C "$WT" log -1 --format='%h %ad %s' --date=format:'%F %T' -- vtalib-zcu102-systolic | cut -c1-120))"
M4=; B4=; H4=; P4=
if [ $SET = multievtax4 ]; then M4="-DBUNDLE_SET=multievtax4 -DNESTC_EVTA_MULTI=ON -DNESTC_EVTA_MAX_XFER=4194304"; B4=_multievtax4; H4=_multievtax4; P4=_m4; fi
if [ "$(uname -m)" = aarch64 ]; then
  SUF=_sysv_$NAME; BD=$TREE/vtabundles_xcmpl/build$B4$SUF
  mkdir -p "$BD"; cd "$BD"
  [ -f CMakeCache.txt ] || cmake -DCMAKE_C_COMPILER=clang-8 -DCMAKE_CXX_COMPILER=clang++-8 $M4 -DBUNDLE_EXE_SUFFIX=$SUF -DVTALIB="$VL" ..
  make -j2 "$@"
  echo "run: cd $TREE/vtabundles_xcmpl && sudo /bin/sh -c \"BIN=<bitstream> hw$H4/script/run_prof.sh -s $SUF <Dir>\""
else
  SUF=_pcsysv_$NAME; BD=$TREE/vtabundles_xcmpl/build_pc$P4$SUF
  export PATH=$HOME/bin_sudo_docker:$PATH
  docker exec -u ubuntu nestc-build bash -c "set -e; mkdir -p $BD && cd $BD && { [ -f CMakeCache.txt ] || cmake \
    -DCMAKE_TOOLCHAIN_FILE=$TREE/utils/docker_nestc/zcu102_toolchain.cmake $M4 -DBUNDLE_EXE_SUFFIX=$SUF -DVTALIB=$VL ..; } && make -j16 $*"
  echo "then scp vtabundles_xcmpl/hw*/<Dir>/<target>$SUF (and <model>.weights.bin if the bundle changed) to the board;"
  echo "run: sudo /bin/sh -c \"BIN=<bitstream> hw*/script/run_prof.sh -s $SUF <Dir>\""
fi
