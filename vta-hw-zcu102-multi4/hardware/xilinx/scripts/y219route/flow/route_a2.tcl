# #### JO added (2026-10-27): copy of build/y219/route_a2.tcl (git-ignored there). Run from vta-hw-zcu102-multi4/build/y219.
# step A2: route the placed multi4 design without any CONTAIN_ROUTING (RapidWright clears row Y219 afterwards)
set impl ../hardware/xilinx/vivado/zcu102_1x16_i8w8a32_16_15_18_17_4evta/vta.runs/impl_1
open_checkpoint $impl/vta_wrapper_physopt.dcp
foreach pb [get_pblocks] { set_property CONTAIN_ROUTING false $pb }
route_design -directive AggressiveExplore
phys_opt_design -directive AggressiveExplore
write_checkpoint -force routed_a2.dcp
write_edif -force routed_a2.edf
report_timing_summary -file routed_a2.timing.rpt
report_route_status -file routed_a2.route.rpt
#### JO changed (2026-10-27): chkbody.tcl next to this script (was build/y219/chkbody.tcl)
set argv {x timing}; source [file join [file dirname [info script]] chkbody.tcl] -notrace
