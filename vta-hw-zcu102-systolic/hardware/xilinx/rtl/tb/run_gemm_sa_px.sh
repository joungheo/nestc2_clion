#!/bin/bash
# #### JO added (2026-11-29, pixel pair experiment): xsim of gemm_sa_px against gemm_sa on random programs
# (tb_gemm_sa_px.sv).  usage: rtl/tb/run_gemm_sa_px.sh [seeds...]  (default 1 2 3); GAP=<% idle>. Work dir: build/rtl_sim_px
set -e
T=$(cd "$(dirname "$0")" && pwd); R=$(cd $T/.. && pwd); W=$(cd $R/../../.. && pwd)/build/rtl_sim_px
mkdir -p $W
SEEDS=${@:-1 2 3}
export PATH=$HOME/bin_sudo_docker:$PATH
docker exec -u ubuntu -w $W vivado2020-build bash -c "source /tools/Xilinx/v2020.1/Vivado/2020.1/settings64.sh >/dev/null; \
  xvlog -sv $R/sa_pe.v $R/sa_array.v $R/sa_ctrl.v $R/sa_rmw.v $R/sa_dw.v $R/gemm_sa.v \
    $R/sa_pe_px.v $R/sa_array_px.v $R/sa_ctrl_px.v $R/gemm_sa_px.v $T/tb_gemm_sa_px.sv \
    \$XILINX_VIVADO/data/verilog/src/glbl.v > xvlog.log 2>&1 || { tail -20 xvlog.log; exit 1; }; \
  xelab -L unisims_ver -timescale 1ns/1ps tb_gemm_sa_px glbl -s tb > xelab.log 2>&1 || { grep -E 'ERROR' xelab.log | head -20; exit 1; }"
for s in $SEEDS; do
  docker exec -u ubuntu -w $W vivado2020-build bash -c "source /tools/Xilinx/v2020.1/Vivado/2020.1/settings64.sh >/dev/null; \
    xsim tb -R -testplusarg seed=$s -testplusarg gap=${GAP:-0} > xsim_s$s.log 2>&1"
  grep -E '^RESULT|^acc|^out|^  ' $W/xsim_s$s.log | grep -v '\*\*' | head -10
done
