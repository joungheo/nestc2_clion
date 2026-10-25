foreach pb [get_pblocks] { puts "CHK pblock $pb cells=[llength [get_cells -quiet -of $pb]] parent=[get_property PARENT $pb]" }
set ys {}; foreach c [get_cells -hier -filter {REF_NAME =~ RAMB36*}] { regexp {Y(\d+)$} [get_property LOC $c] -> y; lappend ys $y }
set lo 0; set hi 0; set at 0; foreach y $ys { if {$y < 43} {incr lo} elseif {$y > 43} {incr hi} else {incr at} }
puts "CHK RAMB36 below=$lo row43=$at above=$hi"
set pipnets {}
foreach t [get_tiles -regexp {.*_X\d+Y219}] {
  foreach n [get_nets -quiet -of $t] {
    set ps [get_pips -quiet -of $n -filter "TILE =~ *Y219"]
    if {[llength $ps]} { lappend pipnets $n }
  }
}
set pipnets [lsort -unique $pipnets]
puts "CHK nets with PIPs in Y219 tiles: [llength $pipnets]"
foreach n $pipnets {
  set d [get_cells -quiet -of [get_pins -quiet -of $n -filter {DIRECTION == OUT && IS_LEAF}]]
  set l [get_cells -quiet -of [get_pins -quiet -of $n -filter {DIRECTION == IN && IS_LEAF}]]
  set pbs {}; foreach c [concat $d $l] { lappend pbs [get_property PBLOCK $c] }
  puts "CHK   $n type=[get_property TYPE $n] fanout=[llength $l] drv=[get_property LOC $d] pblocks=[lsort -unique $pbs] pips=[lrange [get_pips -of $n -filter {TILE =~ *Y219}] 0 2]"
}
if {[lindex $argv 1] eq "timing"} {
  foreach p [get_timing_paths -max_paths 8 -nworst 1 -unique_pins] { puts "CHK path slack=[get_property SLACK $p] from=[get_property STARTPOINT_PIN $p] to=[get_property ENDPOINT_PIN $p]" }
}
