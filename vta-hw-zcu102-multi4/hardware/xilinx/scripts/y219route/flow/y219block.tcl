# step E: route with the row-Y219 blockage of Y219Block.java (dummy LUTs vta_y219blk_* whose fixed antenna routing holds
# every node a PIP in the row drives), timing-driven by Vivado, then remove the blockage and write the bitstream
# (only if fully routed and no net has a PIP in the row).   usage: -tclargs <in.dcp> <out>
set in [lindex $argv 0]; set out [lindex $argv 1]
open_checkpoint $in
set blk [get_nets -quiet vta_y219blk_*]
puts "CHK blockage nets [llength $blk] cells [llength [get_cells -quiet vta_y219blk_*]] pips [llength [get_pips -quiet -of $blk]]"
set_property IS_ROUTE_FIXED true $blk
route_design -directive AggressiveExplore
phys_opt_design -directive AggressiveExplore
puts "CHK blockage pips after routing [llength [get_pips -quiet -of $blk]]"
set_property IS_ROUTE_FIXED false $blk
remove_cell [get_cells vta_y219blk_*]
remove_net -quiet [get_nets -quiet vta_y219blk_*]
puts "CHK blockage left: [llength [get_cells -quiet vta_y219blk_*]] cells, [llength [get_nets -quiet vta_y219blk_*]] nets"
write_checkpoint -force $out.dcp
report_route_status -file $out.route.rpt
report_timing_summary -file $out.timing.rpt
set argv {x timing}; source [file join [file dirname [info script]] chkbody.tcl] -notrace
set err [llength [get_nets -quiet -hier -filter {ROUTE_STATUS == CONFLICTS || ROUTE_STATUS == UNROUTED || ROUTE_STATUS == PARTIAL || ROUTE_STATUS == ANTENNAS}]]
puts "CHK nets not fully routed: $err"
if {$err == 0 && [llength $pipnets] == 0} {
  write_bitstream -force $out.bit
} else { puts "CHK no bitstream written" }
