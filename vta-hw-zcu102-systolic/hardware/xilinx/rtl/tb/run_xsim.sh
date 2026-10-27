#!/bin/bash
# #### JO added (2026-10-27, systolic Phase D): compile gemm_sa + tb with xsim (Vivado 2020.1 in docker) and run it on
# model vectors.  usage: rtl/tb/run_xsim.sh <vector dir> [<vector dir> ...]   (make vectors: sim/systolic_model dump <dir> <seed>)
# env: GAP, BP (% idle uop / done backpressure). Work dir: build/rtl_sim (git-ignored).
set -e
T=$(cd "$(dirname "$0")" && pwd); R=$(cd $T/.. && pwd); W=$(cd $R/../../.. && pwd)/build/rtl_sim
mkdir -p $W
docker exec -u ubuntu -w $W vivado2020-build bash -c "source /tools/Xilinx/v2020.1/Vivado/2020.1/settings64.sh >/dev/null; \
  xvlog -sv $R/sa_pe.v $R/sa_array.v $R/sa_ctrl.v $R/sa_rmw.v $R/gemm_sa.v $T/tb_gemm_sa.v \
    \$XILINX_VIVADO/data/verilog/src/glbl.v > xvlog.log 2>&1 || { tail -20 xvlog.log; exit 1; }; \
  xelab -L unisims_ver -timescale 1ns/1ps tb_gemm_sa glbl -s tb > xelab.log 2>&1 || { grep -E 'ERROR|WARNING: \[VRFC' xelab.log | head -20; exit 1; }"
for d in "$@"; do
  docker exec -u ubuntu -w $W vivado2020-build bash -c "source /tools/Xilinx/v2020.1/Vivado/2020.1/settings64.sh >/dev/null; \
    xsim tb -R -testplusarg dir=$d -testplusarg gap=${GAP:-0} -testplusarg bp=${BP:-0} -testplusarg dbg=${DBG:-0} > xsim_\$(basename $d).log 2>&1"
  grep -E '^RESULT|^acc\[|^out\[|  got|  exp' $W/xsim_$(basename $d).log | head -12
done
