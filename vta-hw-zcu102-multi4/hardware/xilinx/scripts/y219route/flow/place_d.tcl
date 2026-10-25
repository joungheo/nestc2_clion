# step D: place again with rows Y218 and Y220 empty too, so no site pin sits next to row Y219 (whose PIPs the
# RapidWright pass bans: the IMUX inputs of rows 218/220 are half fed from row-219 tiles, and the reroute of
# rr_b/rr_c kept fighting over the other half), then route as in step A2.
set impl ../hardware/xilinx/vivado/zcu102_1x16_i8w8a32_16_15_18_17_4evta/vta.runs/impl_1
open_checkpoint $impl/vta_wrapper_opt.dcp
foreach re {{SLICE_X\d+Y21[89]} {SLICE_X\d+Y220} {RAMB36_X\d+Y4[34]} {RAMB18_X\d+Y8[6-9]} {DSP48E2_X\d+Y8[6-9]}} {
  set_property PROHIBIT true [get_sites -quiet -regexp $re]
}
foreach pb [get_pblocks] { set_property CONTAIN_ROUTING false $pb }
place_design -directive Explore
phys_opt_design -directive AggressiveExplore
write_checkpoint -force placed_d.dcp
route_design -directive AggressiveExplore
phys_opt_design -directive AggressiveExplore
write_checkpoint -force routed_d.dcp
write_edif -force routed_d.edf
report_timing_summary -file routed_d.timing.rpt
report_route_status -file routed_d.route.rpt
report_utilization -file routed_d.util.rpt
set argv {x timing}; source [file join [file dirname [info script]] chkbody.tcl] -notrace
