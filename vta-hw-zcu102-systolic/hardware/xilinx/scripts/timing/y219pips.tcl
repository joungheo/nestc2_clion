# #### JO added (2026-10-27): usage: vivado -mode batch -source y219pips.tcl -tclargs <routed dcp>
# nets with any PIP in an INT tile of row Y219 (same count as y219route/flow/route_s.tcl), must be 0
open_checkpoint [lindex $argv 0]
set rown {}
foreach t [get_tiles -regexp {INT_X\d+Y219}] { foreach n [get_nets -quiet -of $t] { if {[llength [get_pips -quiet -of $n -filter "TILE == $t"]]} { lappend rown $n } } }
puts "CHK nets with any PIP in row Y219: [llength [lsort -unique $rown]]"
