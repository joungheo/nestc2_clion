# step D: place again with rows Y218 and Y220 empty too, so no site pin sits next to row Y219 (whose PIPs the
# RapidWright pass bans: the IMUX inputs of rows 218/220 are half fed from row-219 tiles, and the reroute of
# rr_b/rr_c kept fighting over the other half), then route as in step A2.   usage: [-tclargs <out>=routed_d]
set out [expr {[llength $argv] ? [lindex $argv 0] : "routed_d"}]
set impl ../hardware/xilinx/vivado/zcu102_1x16_i8w8a32_16_15_18_17_4evta/vta.runs/impl_1
open_checkpoint $impl/vta_wrapper_opt.dcp
# the HP register slices go below the row, next to the PS (builds before 2026-10-25 missed them: regexp in the fence xdc)
if {[llength [get_pblocks -quiet pb_hpslice]]} { add_cells_to_pblock [get_pblocks pb_hpslice] [get_cells vta_i/hpslice*] }
foreach re {{SLICE_X\d+Y21[89]} {SLICE_X\d+Y220} {RAMB36_X\d+Y4[34]} {RAMB18_X\d+Y8[6-9]} {DSP48E2_X\d+Y8[6-9]}} {
  set_property PROHIBIT true [get_sites -quiet -regexp $re]
}
foreach pb [get_pblocks] { set_property CONTAIN_ROUTING false $pb }
place_design -directive Explore
phys_opt_design -directive AggressiveExplore
write_checkpoint -force [string map {routed placed} $out].dcp
route_design -directive AggressiveExplore
phys_opt_design -directive AggressiveExplore
write_checkpoint -force $out.dcp
write_edif -force $out.edf
report_timing_summary -file $out.timing.rpt
report_route_status -file $out.route.rpt
report_utilization -file $out.util.rpt
set argv {x timing}; source [file join [file dirname [info script]] chkbody.tcl] -notrace
