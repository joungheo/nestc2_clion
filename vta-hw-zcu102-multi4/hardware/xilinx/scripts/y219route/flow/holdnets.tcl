# #### JO added (2026-10-27): step H, after route_s: unroute and reroute the nets of the failing hold paths (the Vivado router
# fixes hold with detours; phys_opt_design -hold_fix did not), then the row-Y219 check; the bitstream only if clean.
# Run from vta-hw-zcu102-multi4/build/y219.   usage: -tclargs <in.dcp> <out>   e.g. rs_m380o.dcp rs_m380n
set in [lindex $argv 0]; set out [lindex $argv 1]
open_checkpoint $in
set paths [get_timing_paths -hold -max_paths 50 -nworst 1 -slack_lesser_than 0]
puts "CHK hold paths [llength $paths]"
set nets {}
foreach p $paths { puts "CHK   [get_property SLACK $p] [get_property STARTPOINT_PIN $p] -> [get_property ENDPOINT_PIN $p]"; foreach n [get_nets -quiet -of $p] { lappend nets $n } }
set nets [lsort -unique $nets]
puts "CHK nets to reroute [llength $nets]"
route_design -unroute -nets [get_nets $nets]
route_design -nets [get_nets $nets]
report_timing_summary -file $out.timing.rpt
set rown {}
foreach t [get_tiles -regexp {INT_X\d+Y219}] { foreach n [get_nets -quiet -of $t] { if {[llength [get_pips -quiet -of $n -filter "TILE == $t"]]} { lappend rown $n } } }
puts "CHK nets with any PIP in row Y219: [llength [lsort -unique $rown]]"
set err [llength [get_nets -quiet -filter {ROUTE_STATUS != ROUTED && ROUTE_STATUS != NOLOADS && ROUTE_STATUS != INTRASITE && TYPE != POWER && TYPE != GROUND}]]
puts "CHK nets not fully routed: $err"
write_checkpoint -force $out.dcp
if {$err == 0 && [llength $rown] == 0} { write_bitstream -force $out.bit } else { puts "CHK no bitstream written" }
