# #### JO added (2026-10-27): copy of build/y219/place_f.tcl (git-ignored there). Run from vta-hw-zcu102-multi4/build/y219.
# step F: as step D, without the DONT_TOUCH on the HLS blocks and queues (added against the per-run differences,
# whose cause was the row-Y219 defect), so opt/place/phys_opt may optimize across the IP boundaries again.
set impl ../hardware/xilinx/vivado/zcu102_1x16_i8w8a32_16_15_18_17_4evta/vta.runs/impl_1
open_checkpoint $impl/vta_wrapper_opt.dcp
foreach re {{SLICE_X\d+Y21[89]} {SLICE_X\d+Y220} {RAMB36_X\d+Y4[34]} {RAMB18_X\d+Y8[6-9]} {DSP48E2_X\d+Y8[6-9]}} {
  set_property PROHIBIT true [get_sites -quiet -regexp $re]
}
foreach pb [get_pblocks] { set_property CONTAIN_ROUTING false $pb }
set dt [get_cells -quiet -hier -filter {DONT_TOUCH == true && NAME =~ vta_i/*}]
puts "CHK DONT_TOUCH cleared on [llength $dt] cells"
set_property DONT_TOUCH false $dt
opt_design -directive Explore
place_design -directive Explore
phys_opt_design -directive AggressiveExplore
write_checkpoint -force placed_f.dcp
route_design -directive AggressiveExplore
phys_opt_design -directive AggressiveExplore
write_checkpoint -force routed_f.dcp
write_edif -force routed_f.edf
report_timing_summary -file routed_f.timing.rpt
report_route_status -file routed_f.route.rpt
report_utilization -file routed_f.util.rpt
#### JO changed (2026-10-27): chkbody.tcl next to this script (was build/y219/chkbody.tcl)
set argv {x timing}; source [file join [file dirname [info script]] chkbody.tcl] -notrace
