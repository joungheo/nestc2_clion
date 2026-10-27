# #### JO added (2026-10-27, systolic Phase D4): out-of-context implementation of gemm_sa on the ZCU102 part, placed in
# the y219 fence region of vivado.tcl (below tile row Y219), to see the clock the RTL closes at.
# usage: vivado -mode batch -source ooc.tcl -tclargs <period ns> <out prefix>
set period [lindex $argv 0]; set out [lindex $argv 1]
set R [file normalize [file join [file dirname [info script]] ..]]
read_verilog [list $R/sa_pe.v $R/sa_array.v $R/sa_ctrl.v $R/sa_rmw.v $R/gemm_sa.v]
synth_design -top gemm_sa -part xczu9eg-ffvb1156-2-e -mode out_of_context -flatten_hierarchy rebuilt
create_clock -period $period -name clk [get_ports clk]
create_pblock pb_below_y219
resize_pblock [get_pblocks pb_below_y219] -add {SLICE_X0Y0:SLICE_X96Y218 RAMB36_X0Y0:RAMB36_X12Y42 RAMB18_X0Y0:RAMB18_X12Y85 DSP48E2_X0Y0:DSP48E2_X17Y85}
add_cells_to_pblock [get_pblocks pb_below_y219] [get_cells -hierarchical -filter {IS_PRIMITIVE}]
opt_design
place_design -directive Explore
phys_opt_design -directive AggressiveExplore
route_design -directive Explore
phys_opt_design -directive AggressiveExplore
report_timing_summary -max_paths 20 -file $out.timing.rpt
report_utilization -file $out.util.rpt
report_timing -max_paths 200 -nworst 1 -path_type summary -file $out.paths.rpt
write_checkpoint -force $out.dcp
