# step M: the fenced multi4 closes at about 322 MHz, not 333 (every placement left -0.08..-0.09 ns somewhere): set the
# MMCM of pll_clk in the routed design to VCO 925 MHz (100 MHz x 46.25 / 5), CLKOUT0 925 / 2.875 = 321.7 MHz, and
# write the bitstream (placement and routing unchanged).   usage: -tclargs <in.dcp> <out> [mult=46.25]
set in [lindex $argv 0]; set out [lindex $argv 1]
set mult [expr {[llength $argv] > 2 ? [lindex $argv 2] : 46.25}]
open_checkpoint $in
set_property CLKFBOUT_MULT_F $mult [get_cells vta_i/pll_clk/inst/mmcme4_adv_inst]
puts "CHK pll period [get_property PERIOD [get_clocks clk_out1_vta_pll_clk_0]]"
report_timing_summary -file $out.timing.rpt
report_route_status -file $out.route.rpt
report_utilization -file $out.util.rpt
write_checkpoint -force $out.dcp
write_bitstream -force $out.bit
