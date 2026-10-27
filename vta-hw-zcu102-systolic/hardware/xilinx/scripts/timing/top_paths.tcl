# #### JO added (2026-10-27, systolic Phase A): worst paths of the fast clock as .rpt and .csv (one row per endpoint)
# usage: vivado -mode batch -source top_paths.tcl -tclargs <routed dcp> <out prefix>
set dcp [lindex $argv 0]
set out [lindex $argv 1]
open_checkpoint $dcp
report_clocks -file ${out}.clocks.txt
set clk [get_clocks -filter {PERIOD < 3.1 && NAME=~clk_out1*}]
puts "fast clocks: $clk"
report_timing -max_paths 1000 -nworst 1 -group $clk -sort_by slack -file ${out}.paths.rpt
set fh [open ${out}.paths.csv w]
puts $fh "slack,levels,datapath,route_pct,start,end,start_cell,end_cell"
foreach p [get_timing_paths -max_paths 1000 -nworst 1 -group $clk -sort_by slack] {
  set dp [get_property DATAPATH_DELAY $p]
  set nd [get_property DATAPATH_NET_DELAY $p]
  set pct [expr {$dp > 0 ? round(100.0*$nd/$dp) : 0}]
  set sp [get_property STARTPOINT_PIN $p]
  set ep [get_property ENDPOINT_PIN $p]
  set sc [get_property REF_NAME [get_cells -of $sp]]
  set ec [get_property REF_NAME [get_cells -of $ep]]
  puts $fh "[get_property SLACK $p],[get_property LOGIC_LEVELS $p],$dp,$pct,$sp,$ep,$sc,$ec"
}
close $fh
