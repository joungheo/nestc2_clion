# #### JO added (2026-10-23): build the BRAM self-test bitstream (brambist.v) for the ZCU102.
# Same PS configuration as the EVTA designs (M_AXI_HPM0_FPD registers at 0xA0000000, pl_clk0 100 MHz,
# S_AXI_HP0 128 bit enabled but unused), so it loads with fpga_manager like the vtahw .bin files.
# After synthesis instance gen_bram[i] is LOC'ed to the i-th RAMB36 site (sorted by X, then Y): sites.txt.
#   vivado -mode batch -source build.tcl -tclargs <out dir> [jobs]
set out  [file normalize [lindex $argv 0]]
set jobs [expr {[llength $argv] > 1 ? [lindex $argv 1] : 8}]
set src  [file dirname [file normalize [info script]]]
file mkdir $out
cd $out
create_project -force brambist . -part xczu9eg-ffvb1156-2-e
set_property board_part xilinx.com:zcu102:part0:3.3 [current_project]
add_files -norecurse $src/brambist.v
update_compile_order -fileset sources_1

create_bd_design bist
set ps [create_bd_cell -type ip -vlnv xilinx.com:ip:zynq_ultra_ps_e:3.3 processing_system]
apply_bd_automation -rule xilinx.com:bd_rule:zynq_ultra_ps_e -config {apply_board_preset "1"} $ps
set_property -dict [list \
  CONFIG.PSU__FPGA_PL0_ENABLE {1} CONFIG.PSU__CRL_APB__PL0_REF_CTRL__FREQMHZ {100} \
  CONFIG.PSU__USE__M_AXI_GP0 {1} CONFIG.PSU__USE__M_AXI_GP1 {0} CONFIG.PSU__USE__M_AXI_GP2 {0} \
  CONFIG.PSU__USE__S_AXI_GP0 {0} CONFIG.PSU__USE__S_AXI_GP2 {1} CONFIG.PSU__SAXIGP2__DATA_WIDTH {128}] $ps
set bist [create_bd_cell -type module -reference brambist_axi bist]
set rst [create_bd_cell -type ip -vlnv xilinx.com:ip:proc_sys_reset:5.0 proc_sys_reset]
set ic [create_bd_cell -type ip -vlnv xilinx.com:ip:axi_interconnect:2.1 axi_ic]
set_property CONFIG.NUM_MI {1} $ic
connect_bd_intf_net [get_bd_intf_pins processing_system/M_AXI_HPM0_FPD] [get_bd_intf_pins axi_ic/S00_AXI]
connect_bd_intf_net [get_bd_intf_pins axi_ic/M00_AXI] [get_bd_intf_pins bist/s_axi]
set clk [get_bd_pins processing_system/pl_clk0]
connect_bd_net $clk [get_bd_pins bist/aclk] [get_bd_pins proc_sys_reset/slowest_sync_clk] \
  [get_bd_pins axi_ic/ACLK] [get_bd_pins axi_ic/S00_ACLK] [get_bd_pins axi_ic/M00_ACLK] \
  [get_bd_pins processing_system/maxihpm0_fpd_aclk] [get_bd_pins processing_system/saxihp0_fpd_aclk]
connect_bd_net [get_bd_pins processing_system/pl_resetn0] [get_bd_pins proc_sys_reset/ext_reset_in]
connect_bd_net [get_bd_pins proc_sys_reset/peripheral_aresetn] [get_bd_pins bist/aresetn] \
  [get_bd_pins axi_ic/S00_ARESETN] [get_bd_pins axi_ic/M00_ARESETN]
connect_bd_net [get_bd_pins proc_sys_reset/interconnect_aresetn] [get_bd_pins axi_ic/ARESETN]
assign_bd_address
set seg [get_bd_addr_segs -of [get_bd_addr_spaces processing_system/Data] -filter {NAME =~ *bist*}]
puts "SEG $seg"
set_property offset 0xA0000000 $seg
set_property range 64K $seg
validate_bd_design
save_bd_design
#### one global synthesis of the BD (the interconnect adds converter IPs at validate time)
set_property synth_checkpoint_mode None [get_files bist.bd]
generate_target all [get_files bist.bd]
make_wrapper -files [get_files bist.bd] -top
add_files -norecurse [glob ./brambist.srcs/sources_1/bd/bist/hdl/bist_wrapper.v]
update_compile_order -fileset sources_1

launch_runs synth_1 -jobs $jobs
wait_on_run synth_1
open_run synth_1
set sites [lsort -command {apply {{a b} {
  regexp {X(\d+)Y(\d+)} $a - ax ay; regexp {X(\d+)Y(\d+)} $b - bx by
  expr {$ax != $bx ? $ax - $bx : $ay - $by}}}} [get_sites RAMB36_*]]
set rams [get_cells -hier -filter {REF_NAME == RAMB36E2}]
puts "BRAMS [llength $rams] SITES [llength $sites]"
set xdc [file normalize ./loc.xdc]
set f [open $xdc w]
set g [open ./sites.txt w]
foreach c $rams {
  regexp {gen_bram\[(\d+)\]} $c - i
  set s [lindex $sites $i]
  puts $f "set_property LOC $s \[get_cells {$c}\]"
  puts $g "$i $s"
}
close $f
close $g
close_design
add_files -fileset constrs_1 $xdc
set_property used_in_synthesis false [get_files $xdc]
launch_runs impl_1 -to_step write_bitstream -jobs $jobs
wait_on_run impl_1
open_run impl_1
report_timing_summary -file brambist.timing.rpt
report_utilization -file brambist.utilization.rpt
file copy -force ./brambist.runs/impl_1/bist_wrapper.bit ./brambist.bit
puts "bitstream: [file normalize ./brambist.bit]"
exit
