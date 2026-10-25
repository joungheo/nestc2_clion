# #### JO added (2026-10-25): bitstreams of the WireTest.java design (Y219 wire test). Clocks the sink SRLs from
# the PS pl_clk0 (100 MHz) through a BUFG_PS, keeps the RapidWright routes of the test nets, routes the rest, and writes
# <out>_0.bit (sources 0, sinks INIT 1) and <out>_1.bit (sources 1, sinks INIT 0) with logic location files
# (they list the SRL bits as Ram=<LUT>:<bit>).
# A design from WireTest.java -Dspeed=1 (source SRLs) gets the clock on the sources too, and <out>_p.bit (sources
# circulate 32'h1F35AC93, sinks INIT 0) plus <out>_n.bit (every INIT inverted; only for wirespeed.py's bit alignment).
# usage: vivado -mode batch -source WireTestBit.tcl -tclargs <wt.dcp> <out>
lassign $argv in out
open_checkpoint $in
set tnets [get_nets -regexp {wt_\d+}]
set_property IS_ROUTE_FIXED 1 $tnets
set_property IS_LOC_FIXED 1 [get_cells wt_*]
set_property IS_BEL_FIXED 1 [get_cells wt_*]
#### JO changed: PS8 pl_clk0 -> BUFG_PS instead of STARTUPE3 CFGMCLK -> BUFGCE. The first version (no PS8 cell,
#### STARTUPE3 with open inputs) froze the board's Linux both when programmed over JTAG and when loaded with
#### fpga_manager; the EVTA designs, which work, all have the PS8 cell. Nothing is connected to its AXI ports.
create_cell -reference PS8 wt_ps8
create_cell -reference BUFG_PS wt_bufg
create_net wt_plclk
connect_net -net wt_plclk -objects [list [get_pins {wt_ps8/PLCLK[0]}] [get_pins wt_bufg/I]]
create_net wt_clk
set speed [expr {[get_property REF_NAME [lindex [get_cells wt_*_s] 0]] eq "SRLC32E"}]
connect_net -net wt_clk -objects [concat [get_pins wt_bufg/O] [get_pins -of [get_cells wt_*_d] -filter {REF_PIN_NAME == CLK}]]
if {$speed} { connect_net -net wt_clk -objects [get_pins -of [get_cells wt_*_s] -filter {REF_PIN_NAME == CLK}] }
puts "CHK clock loads: [llength [get_pins -leaf -of [get_nets wt_clk] -filter {DIRECTION == IN}]]"
place_design
route_design
report_route_status -file $out.route.rpt
report_drc -file $out.drc.rpt
puts "CHK test nets: [llength $tnets], not fully routed: [llength [filter $tnets {ROUTE_STATUS != ROUTED}]], clock: [get_property ROUTE_STATUS [get_nets wt_clk]]"
# nothing may have a PIP in the row (the test nets only pass over it)
set n 0
foreach t [get_tiles -regexp {INT_X\d+Y219}] { incr n [llength [get_pips -quiet -of $t -filter {IS_USED}]] }
puts "CHK used PIPs in INT tiles of row Y219: $n"
write_checkpoint -force $out.dcp
set_property SEVERITY Warning [get_drc_checks {NSTD-1 UCIO-1}]
if {$speed} {
  foreach {v ps pd} {p 32'h1F35AC93 32'h00000000 n 32'hE0CA536C 32'hFFFFFFFF} {
    set_property INIT $ps [get_cells wt_*_s]
    set_property INIT $pd [get_cells wt_*_d]
    write_bitstream -force -logic_location_file ${out}_$v.bit
  }
  return
}
foreach v {0 1} {
  set_property INIT [expr {$v ? "1'b1" : "1'b0"}] [get_cells wt_*_s]
  set_property INIT [expr {$v ? "32'h00000000" : "32'hFFFFFFFF"}] [get_cells wt_*_d]
  write_bitstream -force -logic_location_file ${out}_$v.bit
}
