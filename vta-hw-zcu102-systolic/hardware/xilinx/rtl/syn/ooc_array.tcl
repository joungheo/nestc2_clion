# #### JO added (2026-11-28, DSP packing experiment): out-of-context place and route of the systolic array alone,
# sa_array (MODE 0), sa_array_pk (1: two output channels per DSP) or sa_array_px (2: two pixels per DSP), with registered ports (ooc_sa_top.v), in the same
# y219 fence region as ooc.tcl, to compare timing, congestion and resources.
# usage: vivado -mode batch -source ooc_array.tcl -tclargs <MODE> <period ns> <out prefix>
set pk [lindex $argv 0]; set period [lindex $argv 1]; set out [lindex $argv 2]
set R [file normalize [file join [file dirname [info script]] ..]]
read_verilog [list $R/sa_pe.v $R/sa_array.v $R/sa_pe_pk.v $R/sa_array_pk.v $R/sa_pe_px.v $R/sa_array_px.v $R/syn/ooc_sa_top.v]
synth_design -top ooc_sa_top -part xczu9eg-ffvb1156-2-e -mode out_of_context -flatten_hierarchy rebuilt -generic MODE=$pk
create_clock -period $period -name clk [get_ports clk]
create_pblock pb_below_y219
resize_pblock [get_pblocks pb_below_y219] -add {SLICE_X0Y0:SLICE_X96Y218 RAMB36_X0Y0:RAMB36_X12Y42 RAMB18_X0Y0:RAMB18_X12Y85 DSP48E2_X0Y0:DSP48E2_X17Y85}
add_cells_to_pblock [get_pblocks pb_below_y219] [get_cells -hierarchical -filter {IS_PRIMITIVE}]
report_utilization -file $out.synth_util.rpt
opt_design
place_design -directive Explore
phys_opt_design -directive AggressiveExplore
report_timing_summary -max_paths 10 -file $out.place_timing.rpt
route_design -directive Explore
report_timing_summary -max_paths 20 -file $out.route_timing.rpt
phys_opt_design -directive AggressiveExplore
report_timing_summary -max_paths 20 -file $out.timing.rpt
report_utilization -file $out.util.rpt
report_utilization -hierarchical -file $out.util_hier.rpt
report_timing -max_paths 200 -nworst 1 -path_type summary -file $out.paths.rpt
report_design_analysis -congestion -complexity -timing -max_paths 20 -file $out.analysis.rpt
report_route_status -file $out.route_status.rpt
write_checkpoint -force $out.dcp
