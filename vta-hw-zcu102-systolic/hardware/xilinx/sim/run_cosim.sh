#!/bin/sh
# #### JO added (2026-11-13, ALU extension, docs/evta_isa_ext.md 8.11): C/RTL co-simulation of compute() (SYSTOLIC=1
# ALUX=1) with the random ALU programs of sim/alux_test.cc, compared with the reference model alu_ext_ref.h. Runs
# vivado_hls in the docker container vivado2020-build; work dir build/hardware/xilinx/cosim_alux (git-ignored).
#   sim/run_cosim.sh [seeds (default 3)] [instructions per program (default 48)]
#   VTA_ALUX_WINDOW=16 sim/run_cosim.sh 3   programs with no reuse within 16 steps (work dir cosim_alux_w16)
#   VTA_ALUX_DIST=14,15,16,17,18 sim/run_cosim.sh 1   one vector ADD per reuse distance d (work dir cosim_alux_dist<d,...>; one d per run: a wrong d also spoils the later stores)
#   VTA_ALUX_DIST_OP=muli: the same with a MUL by imm 3 (one slot per step on the II 1 slot loop); VTA_ALUX_WSLOTS=<slots> (default 34): the random programs keep no stale read in that many slots (alu_ext_ref.h hazard_slots)
#   VTA_ALUX_HAZARD_OK=1 sim/run_cosim.sh 3   keeps instructions that break the 17-step rule: must FAIL (work dir cosim_alux_hazard)
# Result: build/hardware/xilinx/cosim_alux/cosim.log ("cosim ALU extension: ... all passed", "C/RTL co-simulation finished: PASS").
set -e
cd "$(dirname "$0")/.."
X=$(pwd)
R=$(cd ../.. && pwd)
CONF=$(python3 $R/config/vta_config.py --cfg-str)
C=$R/build/hardware/xilinx/include/$CONF/vta_config.tcl
[ -f $C ] || make -C $X $C >/dev/null
W=$R/build/hardware/xilinx/cosim_alux${VTA_ALUX_HAZARD_OK:+_hazard}${VTA_ALUX_WINDOW:+_w$VTA_ALUX_WINDOW}${VTA_ALUX_DIST:+_dist$VTA_ALUX_DIST}${VTA_ALUX_DIST_OP:+_$VTA_ALUX_DIST_OP}${VTA_ALUX_WSLOTS:+_s$VTA_ALUX_WSLOTS}
mkdir -p $W
docker start vivado2020-build >/dev/null
docker exec -u ubuntu -w $W -e VTA_ALUX_SEEDS="${1:-3}" -e VTA_ALUX_NINSN="${2:-48}" -e VTA_ALUX_HAZARD_OK="${VTA_ALUX_HAZARD_OK:-0}" -e VTA_ALUX_WINDOW="${VTA_ALUX_WINDOW:-17}" -e VTA_ALUX_DIST="${VTA_ALUX_DIST:-}" -e VTA_ALUX_DIST_OP="${VTA_ALUX_DIST_OP:-}" -e VTA_ALUX_WSLOTS="${VTA_ALUX_WSLOTS:-34}" vivado2020-build bash -c \
  "source /tools/Xilinx/v2020.1/Vivado/2020.1/settings64.sh >/dev/null; vivado_hls -f $X/sim/cosim_compute.tcl -tclargs $R $C > cosim.log 2>&1" || true
grep -E "INFO - (ALU extension test (successful|failed)|cosim)|ERROR|C/RTL co-simulation finished" $W/cosim.log | sort | uniq -c
