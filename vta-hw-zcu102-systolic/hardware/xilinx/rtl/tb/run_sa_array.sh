#!/bin/bash
# #### JO added (2026-11-28, DSP packing experiment): xsim of sa_array (PK=0), sa_array_pk (PK=1) and sa_array_px (PK=2) alone against the
# reference in tb_sa_array.sv.  usage: rtl/tb/run_sa_array.sh [seeds...]   (default seeds 1 2 3). Work dir: build/rtl_sim_array
set -e
T=$(cd "$(dirname "$0")" && pwd); R=$(cd $T/.. && pwd); W=$(cd $R/../../.. && pwd)/build/rtl_sim_array
mkdir -p $W
SEEDS=${@:-1 2 3}
export PATH=$HOME/bin_sudo_docker:$PATH
docker exec -u ubuntu -w $W vivado2020-build bash -c "source /tools/Xilinx/v2020.1/Vivado/2020.1/settings64.sh >/dev/null; \
  xvlog -sv $R/sa_pe.v $R/sa_array.v $R/sa_pe_pk.v $R/sa_array_pk.v $R/sa_pe_px.v $R/sa_array_px.v $T/tb_sa_array.sv \
    \$XILINX_VIVADO/data/verilog/src/glbl.v > xvlog.log 2>&1 || { tail -20 xvlog.log; exit 1; }; \
  for pk in ${MODES:-0 1 2}; do xelab -L unisims_ver -timescale 1ns/1ps -generic_top PK=\$pk tb_sa_array glbl -s tb\$pk > xelab\$pk.log 2>&1 \
    || { grep -E 'ERROR' xelab\$pk.log | head -20; exit 1; }; done"
for pk in ${MODES:-0 1 2}; do for s in $SEEDS; do
  docker exec -u ubuntu -w $W vivado2020-build bash -c "source /tools/Xilinx/v2020.1/Vivado/2020.1/settings64.sh >/dev/null; \
xelab -L unisims_ver -timescale 1ns/1ps -generic_top PK=$pk -generic_top SEED=$s tb_sa_array glbl -s tbs > /dev/null 2>&1; \
    xsim tbs -R > xsim_pk${pk}_s$s.log 2>&1"
  grep -E '^RESULT|^  ' $W/xsim_pk${pk}_s$s.log | head -10
done; done
