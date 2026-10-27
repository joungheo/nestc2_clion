#!/bin/sh
# #### JO added (2026-10-27): HW probes of a bitstream, reloading it before each case (what §8 of
# docs/evta_systolic.md calls "HW 프로브"): gemmprobe reset/zero/ident/perm/ones, sramscan wgt/inp (+invert), donerace 16.
# NUM_EVTA=0 (default): single bitstream, the *_nc tests  (VTA_COHERENT=false SUFFIX=_nc ./build.sh gemmprobe sramscan donerace)
# NUM_EVTA=2|4:         multi bitstream, the *_evta<e>_m tests of EVTA 0..NUM_EVTA-1
#                       (for e in 0 1 2 3; do VTA_COHERENT=false SUFFIX=_m BUFF="15 18 17" XFER=8388608 EVTA=$e ./build.sh gemmprobe sramscan donerace; done)
# usage: sudo -n /bin/sh -c 'NUM_EVTA=4 /bin/sh vtabundles_xcmpl/hwtest/probes.sh <file in /lib/firmware> [...]'
# HWTEST=<dir> runs the tests of that hwtest dir (default: the dir of this script), e.g. a copy of this script outside the tree.
# Pass: gemmprobe "0/16 wrong", sramscan "0 bytes wrong", donerace "16/16" on every line.
cd "${HWTEST:-$(dirname "$0")}" || exit 1
exec 9>/tmp/evta_run.lock; flock -n 9 || { echo "another EVTA run is active (/tmp/evta_run.lock)"; exit 1; }
N=${NUM_EVTA:-0}
if [ "$N" = 0 ]; then SFX="_nc"; else SFX=""; e=0; while [ $e -lt "$N" ]; do SFX="$SFX _evta${e}_m"; e=$((e+1)); done; fi
for BIN in "$@"; do
  echo "######## $BIN"
  [ -f /lib/firmware/$BIN ] || { echo "bitstream /lib/firmware/$BIN not found"; exit 1; }
  for s in $SFX; do
    for t in "gemmprobe$s reset" "gemmprobe$s zero" "gemmprobe$s ident" "gemmprobe$s perm" "gemmprobe$s ones" \
             "sramscan$s wgt" "sramscan$s wgt invert" "sramscan$s inp" "sramscan$s inp invert" "donerace$s 16"; do
      echo 0 > /sys/class/fpga_manager/fpga0/flags; echo $BIN > /sys/class/fpga_manager/fpga0/firmware
      mkdir -p /tmp/xlnk_trace; ( cd /tmp/xlnk_trace && python3 -c "from pynq import Xlnk; Xlnk().xlnk_reset()" 2>/dev/null )
      out=$(timeout 60 ./$t 2>&1); rc=$?
      printf "%-30s rc=%s  %s\n" "$t" $rc "$(echo "$out" | grep -E -i 'RESULT|wrong|match|PASS|FAIL' | tail -2 | tr '\n' ' ')"
    done
  done
done
