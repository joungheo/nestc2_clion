# #### JO added (2026-11-13, ALU extension, docs/evta_isa_ext.md 8.11): C/RTL co-simulation of compute() with the
# random ALU programs (sim/alux_cosim_tb.cc + sim/alux_test.cc, -DALUX_COSIM). Same cflags and compute directives as
# scripts/hls.tcl with SYSTOLIC=1 ALUX=1. Run through sim/run_cosim.sh.
#   vivado_hls -f cosim_compute.tcl -tclargs <root_dir> <vta_config.tcl>
set root_dir   [lindex $argv 2]
set vta_config [lindex $argv 3]
set src_dir "$root_dir/hardware/xilinx/src"
set sim_dir "$root_dir/hardware/xilinx/sim"
set test_dir "$root_dir/tests/hardware/common"
source $vta_config
set cflags "$CFLAGS -DVTA_SYSTOLIC -DVTA_ALUX"

open_project -reset vta_cosim_compute
set_top compute
add_files $src_dir/vta.cc -cflags $cflags
add_files -tb $sim_dir/alux_cosim_tb.cc -cflags "$cflags -DALUX_COSIM"
add_files -tb $sim_dir/alux_test.cc -cflags "$cflags -DALUX_COSIM"
add_files -tb $test_dir/test_lib.cc -cflags "$cflags -DALUX_COSIM"
open_solution -reset soln
set_part $FPGA_DEVICE
create_clock -period $FPGA_PERIOD -name default
set_directive_array_reshape -type block -factor $INP_MEM_AXI_RATIO -dim 2 "compute" inp_mem
if {$INP_MEM_BANKS > 1} { set_directive_array_partition -type block -factor $INP_MEM_BANKS -dim 2 "compute" inp_mem }
set_directive_array_reshape -type block -factor $WGT_MEM_AXI_RATIO -dim 2 "compute" wgt_mem
if {$WGT_MEM_BANKS > 1} { set_directive_array_partition -type block -factor $WGT_MEM_BANKS -dim 2 "compute" wgt_mem }
set_directive_array_reshape -type block -factor $OUT_MEM_AXI_RATIO -dim 2 "compute" out_mem
if {$OUT_MEM_BANKS > 1} { set_directive_array_partition -type block -factor $OUT_MEM_BANKS -dim 2 "compute" out_mem }
# cosim only: depths of the m_axi ports (alux_cosim_tb.cc kUopDepth / kBiasDepth); no effect on the RTL
set_directive_interface -mode m_axi -depth 1024 -offset slave -bundle uop_port "compute" uops
set_directive_interface -mode m_axi -depth 4096 -offset slave -bundle data_port "compute" biases
csynth_design
cosim_design -rtl verilog -tool xsim
exit
