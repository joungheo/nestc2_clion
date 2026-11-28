# #### JO added (2026-11-29, pixel pair experiment): out-of-context place and route of the GEMM unit, original (PX 0) or
# pixel pairs (PX 1), in the y219 fence region of ooc.tcl.
# usage: vivado -mode batch -source ooc_gemm.tcl -tclargs <ports|mem> <PX> <period ns> <out prefix>
#   ports: gemm_sa / gemm_sa_px alone (as ooc.tcl: the memory ports are OOC ports, their paths are not timed)
#   mem:   ooc_gemm_mem.v (BRAMs and the HLS port muxes inside, every top port registered)
set kind [lindex $argv 0]; set px [lindex $argv 1]; set period [lindex $argv 2]; set out [lindex $argv 3]
set R [file normalize [file join [file dirname [info script]] ..]]
read_verilog [list $R/sa_pe.v $R/sa_array.v $R/sa_ctrl.v $R/sa_rmw.v $R/sa_dw.v $R/gemm_sa.v \
  $R/sa_pe_px.v $R/sa_array_px.v $R/sa_ctrl_px.v $R/gemm_sa_px.v $R/syn/ooc_gemm_mem.v]
if {$kind eq "mem"} {
  synth_design -top ooc_gemm_mem -part xczu9eg-ffvb1156-2-e -mode out_of_context -flatten_hierarchy rebuilt -generic PX=$px
} else {
  synth_design -top [expr {$px ? "gemm_sa_px" : "gemm_sa"}] -part xczu9eg-ffvb1156-2-e -mode out_of_context -flatten_hierarchy rebuilt
}
create_clock -period $period -name clk [get_ports clk]
create_pblock pb_below_y219
resize_pblock [get_pblocks pb_below_y219] -add {SLICE_X0Y0:SLICE_X96Y218 RAMB36_X0Y0:RAMB36_X12Y42 RAMB18_X0Y0:RAMB18_X12Y85 DSP48E2_X0Y0:DSP48E2_X17Y85}
add_cells_to_pblock [get_pblocks pb_below_y219] [get_cells -hierarchical -filter {IS_PRIMITIVE}]
opt_design
place_design -directive Explore
phys_opt_design -directive AggressiveExplore
report_timing_summary -max_paths 10 -file $out.place_timing.rpt
route_design -directive Explore
phys_opt_design -directive AggressiveExplore
report_timing_summary -max_paths 20 -file $out.timing.rpt
report_utilization -file $out.util.rpt
report_timing -max_paths 300 -nworst 1 -path_type summary -file $out.paths.rpt
report_design_analysis -congestion -complexity -timing -max_paths 20 -file $out.analysis.rpt
report_route_status -file $out.route_status.rpt
write_checkpoint -force $out.dcp
