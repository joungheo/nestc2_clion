# #### JO added (2026-11-23): mmcm_set.tcl for the HP port clock (hp_pll, VCO 100 / 5 x mult; 47.875 = 333.0 MHz, 46.25 = 321.7 MHz)
# when a multi4 HP read path crossing row Y219 is a few ps short and the EVTA clock (pll_clk) meets 400 MHz.
#   usage: -tclargs <in.dcp> <out> [mult=47.875]
set in [lindex $argv 0]; set out [lindex $argv 1]
set mult [expr {[llength $argv] > 2 ? [lindex $argv 2] : 47.875}]
open_checkpoint $in
set_property CLKFBOUT_MULT_F $mult [get_cells vta_i/hp_pll/inst/mmcme4_adv_inst]
puts "CHK pll period [get_property PERIOD [get_clocks clk_out1_vta_hp_pll_0_1]]"
report_timing_summary -file $out.timing.rpt
report_route_status -file $out.route.rpt
report_utilization -file $out.util.rpt
write_checkpoint -force $out.dcp
write_bitstream -force $out.bit
