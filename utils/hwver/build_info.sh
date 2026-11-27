#!/bin/bash
#### JO added (2026-11-27, docs/evta_hw_versions.md): what a bitstream was built from, kept next to it (export dir, then the
#### board's /lib/firmware/bit/<dir>/build_info.txt), so an old bitstream can be paired with its sources and vtalib later.
#### usage:
####   build_info.sh src <stamp> [path ...]       git commit + uncommitted changes of the hardware sources now (make writes one
####                                              when HLS starts and one when Vivado starts; a dirty tree also gets <stamp>.diff)
####   build_info.sh write <bin> [--src <stamp>]... [--timing <rpt>] [--util <rpt>] [--step <text>]... [--out <file>]
####                                              <bin>.build_info.txt (or --out): name, md5, stamps, VTA_* make variables,
####                                              timing summary, resources, steps
####   build_info.sh add <info> <key> <text>      one more line "<key>: <date> <text>" (a later step, board name, tests, vtalib)
#### Paths in a stamp are relative to the nest-compiler tree. Runs in the vivado2020-build container (git, python3) or on the host.
set -u
TREE=$(cd "$(dirname "$0")/../.." && pwd)
HWPATHS="vta-hw-zcu102-systolic/hardware vta-hw-zcu102-systolic/src vta-hw-zcu102-systolic/include vta-hw-zcu102-systolic/config
 vta-hw-zcu102-multi4/hardware vta-hw-zcu102-multi4/src vta-hw-zcu102-multi4/include vta-hw-zcu102-multi4/config
 vta-hw-zcu102-single/hardware vta-hw-zcu102-single/src vta-hw-zcu102-single/include vta-hw-zcu102-single/config
 vta-hw-zcu102-multi2/hardware vta-hw-zcu102-multi2/src vta-hw-zcu102-multi2/include vta-hw-zcu102-multi2/config"
now() { date '+%F %T'; }
case "${1:-}" in
src)
  S=$2; shift 2; P=${*:-$HWPATHS}
  cd "$TREE" || exit 1
  { echo "stamp: $(now) $(hostname) $(id -un)"
    echo "commit: $(git log -1 --format='%H %h %ad %s' --date=format:'%F %T' 2>/dev/null | cut -c1-200)"
    d=$(git status --porcelain -- $P 2>/dev/null | grep -v '^??' | grep -v '/build/')
    if [ -n "$d" ]; then echo "dirty: $(echo "$d" | wc -l) files (diff in $(basename $S).diff)"; echo "$d" | sed 's/^/  /'
      git diff HEAD -- $P > "$S.diff" 2>/dev/null
    else echo "dirty: no"; rm -f "$S.diff"; fi; } > "$S"
  ;;
write)
  B=$2; shift 2; SRC=(); TIM=; UTIL=; STEPS=(); OUT="${B%.bin}.build_info.txt"
  while [ $# -gt 0 ]; do case $1 in
    --src) SRC+=("$2"); shift 2;; --timing) TIM=$2; shift 2;; --util) UTIL=$2; shift 2;;
    --step) STEPS+=("$2"); shift 2;; --out) OUT=$2; shift 2;; *) echo "unknown $1"; exit 1;; esac; done
  mkdir -p "$(dirname "$OUT")"
  {
    echo "# build_info.txt (utils/hwver/build_info.sh, docs/evta_hw_versions.md)"
    echo "name: $(basename "$B")"
    [ -f "$B" ] && echo "md5: $(md5sum < "$B" | cut -c1-32)"
    echo "written: $(now) $(hostname)"
    for s in "${SRC[@]}"; do [ -f "$s" ] || continue
      echo "== source ($(basename "$s"))"; cat "$s"
      [ -f "$s.diff" ] && cp "$s.diff" "$(dirname "$OUT")/$(basename "${OUT%.txt}")_$(basename "$s").diff"; done
    v=$(env | grep -E '^VTA_[A-Z0-9_]+=.' | sort)
    [ -n "$v" ] && { echo "== make variables"; echo "$v"; }
    if [ -n "$TIM" ] && [ -f "$TIM" ]; then
      echo "== timing ($(basename "$TIM"))"
      grep -A3 'WNS(ns)' "$TIM" | sed -n 3p | awk '{print "WNS " $1 " ns, TNS " $2 " ns, failing endpoints " $3 ", WHS " $5 " ns, THS " $6 " ns"}'
      awk '/^\| Clock Summary/{p=1} p&&/^ *clk_out[^ ]* +\{/{print "clock " $1 " " $5 " MHz"} /^\| Intra Clock/{exit}' "$TIM" | head -12
    fi
    if [ -n "$UTIL" ] && [ -f "$UTIL" ]; then
      echo "== resources ($(basename "$UTIL"))"
      for k in 'CLB LUTs' 'CLB Registers' 'Block RAM Tile' 'DSPs'; do
        grep -m1 -E "^\| $k +\|" "$UTIL" | awk -F'|' -v k="$k" '{gsub(/ /,"",$3); print k ": " $3}'; done
    fi
    for s in "${STEPS[@]}"; do echo "step: $s"; done
  } > "$OUT"
  echo "$OUT"
  ;;
add)
  echo "$3: $(now) $4" >> "$2" ;;
*) sed -n '2,12p' "$0" | sed 's/^#### \{0,1\}//'; exit 1;;
esac
