#!/bin/bash
# #### JO added (2026-10-25): multi4 flow after `make bin` (README.md): re-place with rows Y218-220 empty (place_d.tcl),
# reroute every net with a PIP in row Y219 with RapidWright (Y219RWRoute.java, all row PIPs banned), let Vivado
# finish (route_s.tcl: must print 0 nets with any PIP in the row) and print the timing summary.
# Run from vta-hw-zcu102-multi4/build/y219.   usage: run_flow.sh <tag>   -> routed_<tag>, rws_<tag>, rs_<tag>.{dcp,bit}
set -e
T=$1
F=$(cd "$(dirname "$0")" && pwd)
RW=/home/ubuntu/tools/rapidwright
#### JO changed (2026-10-27): VTA_IMPL_DIR (place_d.tcl input, relative to build/y219) is passed into docker
#viv() { docker exec -u ubuntu -w $PWD vivado2020-build bash -c "source /tools/Xilinx/v2020.1/Vivado/2020.1/settings64.sh; vivado -mode batch -nojournal -log $1.log -source $2 -tclargs ${*:3} > /dev/null 2>&1"; }
viv() { docker exec -e VTA_IMPL_DIR=${VTA_IMPL_DIR:-} -u ubuntu -w $PWD vivado2020-build bash -c "source /tools/Xilinx/v2020.1/Vivado/2020.1/settings64.sh; vivado -mode batch -nojournal -log $1.log -source $2 -tclargs ${*:3} > /dev/null 2>&1"; }
viv place_$T $F/place_d.tcl routed_$T
grep -A3 "WNS(ns)" routed_$T.timing.rpt | sed -n 3p
RAPIDWRIGHT_PATH=$RW/d2025.2 nice -n 10 taskset -c 0-7 java -XX:ActiveProcessorCount=8 -Dcrit=0.95 -Xmx64g \
  -cp $RW/rapidwright-2025.2.2-standalone-lin64.jar $F/../Y219RWRoute.java routed_$T.dcp rws_$T.dcp > rws_$T.log 2>&1
grep -E "nets with a PIP|sinks left" rws_$T.log
viv route_s_$T $F/route_s.tcl rws_$T.dcp rs_$T
grep "^CHK" route_s_$T.log
grep -A3 "WNS(ns)" rs_$T.timing.rpt | sed -n 3p
