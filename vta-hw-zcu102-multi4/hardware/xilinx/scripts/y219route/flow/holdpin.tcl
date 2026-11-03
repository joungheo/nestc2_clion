# #### JO added (2026-11-04): step H2, when holdnets.tcl does not help (the plain reroute takes the same wires): unroute the
# nets of the failing hold endpoints and route each endpoint with a minimum route delay (route_design -pins -delay -min_delay, ps),
# trying the delays in the order given until hold is met with no PIP in row Y219; the bitstream only if clean.
# Run from vta-hw-zcu102-multi4/build/y219.   usage: -tclargs <in.dcp> <out> <min delay ps> ...   e.g. rs_malu1r.dcp rs_malu1rp 320 340 360
set in [lindex $argv 0]; set out [lindex $argv 1]; set delays [lrange $argv 2 end]
open_checkpoint $in
set eps {}
foreach p [get_timing_paths -hold -max_paths 50 -nworst 1 -slack_lesser_than 0] {
  puts "CHK hold [get_property SLACK $p] [get_property ENDPOINT_PIN $p]"
  lappend eps [get_property ENDPOINT_PIN $p]
}
proc rownets {} {
  set rown {}
  foreach t [get_tiles -regexp {INT_X\d+Y219}] { foreach n [get_nets -quiet -of $t] { if {[llength [get_pips -quiet -of $n -filter "TILE == $t"]]} { lappend rown $n } } }
  return [lsort -unique $rown]
}
set ok 0
foreach d $delays {
  foreach ep $eps {
    set n [get_nets -of [get_pins $ep]]
    route_design -unroute -nets $n
    route_design -pins [get_pins $ep] -delay -min_delay $d
    route_design -nets $n
  }
  set whs [get_property SLACK [get_timing_paths -hold]]
  set rn [llength [rownets]]
  puts "CHK min_delay $d ps: WHS $whs, nets with any PIP in row Y219 $rn"
  if {$whs >= 0 && $rn == 0} { set ok 1; break }
}
report_timing_summary -file $out.timing.rpt
set err [llength [get_nets -quiet -filter {ROUTE_STATUS != ROUTED && ROUTE_STATUS != NOLOADS && ROUTE_STATUS != INTRASITE && TYPE != POWER && TYPE != GROUND}]]
puts "CHK nets not fully routed: $err"
write_checkpoint -force $out.dcp
if {$ok && $err == 0} { write_bitstream -force $out.bit } else { puts "CHK no bitstream written" }
