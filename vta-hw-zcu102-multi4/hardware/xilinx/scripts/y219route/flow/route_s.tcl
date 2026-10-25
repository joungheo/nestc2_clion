# step S: check a design rerouted by Y219RWRoute.java -Dpips=stuck_int_pips.txt (only the PIPs on a stuck cell banned),
# let Vivado route what is left unrouted (only those nets), list the nets with a PIP on a stuck cell and write the
# bitstream if fully routed; stuckchk.py on the bitstream is the final check.   usage: -tclargs <in.dcp> <out> [opt]
# With opt, post-route phys_opt_design runs first (it may reroute nets onto a stuck cell: those are listed, and
# Y219RWRoute.java on <out>.dcp moves them again).
set in [lindex $argv 0]; set out [lindex $argv 1]
set stuck [dict create]
set f [open [file join [file dirname [info script]] .. stuck_int_pips.txt]]
foreach l [split [read $f] \n] { if {$l ne ""} { dict set stuck $l 1 } }
close $f
proc stucknets {stuck} {
  set r {}
  foreach t [get_tiles -regexp {INT_X\d+Y219}] {
    foreach n [get_nets -quiet -of $t] {
      foreach p [get_pips -quiet -of $n -filter "TILE == $t"] {
        if {[regexp {/INT\.(\S+?)-+>+(\S+)$} $p - a b] && [dict exists $stuck "$a $b"]} { lappend r $n; break }
      }
    }
  }
  return [lsort -unique $r]
}
open_checkpoint $in
set err [get_nets -quiet -hier -filter {ROUTE_STATUS == CONFLICTS || ROUTE_STATUS == UNROUTED || ROUTE_STATUS == PARTIAL}]
puts "CHK nets not fully routed after RapidWright: [llength $err] $err"
if {[llength $err]} { route_design -nets $err }
if {[lindex $argv 2] eq "opt"} {
  phys_opt_design -directive AggressiveExplore
  phys_opt_design -directive AggressiveFanoutOpt
  phys_opt_design -directive AlternateFlowWithRetiming
} elseif {[lindex $argv 2] eq "opt2"} {
  phys_opt_design -placement_opt -critical_cell_opt -rewire -critical_pin_opt -clock_opt
  phys_opt_design -placement_opt -critical_cell_opt -rewire -critical_pin_opt -clock_opt
}
report_route_status -file $out.route.rpt
report_timing_summary -file $out.timing.rpt
set bad [stucknets $stuck]
puts "CHK nets with a PIP on a stuck cell: [llength $bad]"
# 2026-10-25: also PIPs in the row that set no stuck cell failed on the board (compute_0 RDATA[28] of rs_m), so count all
set rown {}
foreach t [get_tiles -regexp {INT_X\d+Y219}] { foreach n [get_nets -quiet -of $t] { if {[llength [get_pips -quiet -of $n -filter "TILE == $t"]]} { lappend rown $n } } }
puts "CHK nets with any PIP in row Y219: [llength [lsort -unique $rown]]"
foreach n $bad { puts "CHK   $n" }
set err [llength [get_nets -quiet -hier -filter {ROUTE_STATUS == CONFLICTS || ROUTE_STATUS == UNROUTED || ROUTE_STATUS == PARTIAL}]]
puts "CHK nets not fully routed: $err"
write_checkpoint -force $out.dcp
if {$err == 0} { write_bitstream -force $out.bit } else { puts "CHK no bitstream written" }
