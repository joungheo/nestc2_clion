# step D: place again with rows Y218 and Y220 empty too, so no site pin sits next to row Y219 (whose PIPs the
# RapidWright pass bans: the IMUX inputs of rows 218/220 are half fed from row-219 tiles, and the reroute of
# rr_b/rr_c kept fighting over the other half), then route as in step A2.   usage: [-tclargs <out>=routed_d]
set out [expr {[llength $argv] ? [lindex $argv 0] : "routed_d"}]
#### JO changed (2026-10-27): VTA_IMPL_DIR picks another make bin (e.g. the FREQ=360 build dir _360mhz)
#set impl ../hardware/xilinx/vivado/zcu102_1x16_i8w8a32_16_15_18_17_4evta/vta.runs/impl_1
set impl [expr {[info exists ::env(VTA_IMPL_DIR)] && $::env(VTA_IMPL_DIR) ne "" ? $::env(VTA_IMPL_DIR) : "../hardware/xilinx/vivado/zcu102_1x16_i8w8a32_16_15_18_17_4evta/vta.runs/impl_1"}]
puts "impl dir $impl"
open_checkpoint $impl/vta_wrapper_opt.dcp
# the HP register slices go below the row, next to the PS (builds before 2026-10-25 missed them: regexp in the fence xdc)
if {[llength [get_pblocks -quiet pb_hpslice]]} { add_cells_to_pblock [get_pblocks pb_hpslice] [get_cells vta_i/hpslice*] }
foreach re {{SLICE_X\d+Y21[89]} {SLICE_X\d+Y220} {RAMB36_X\d+Y4[34]} {RAMB18_X\d+Y8[6-9]} {DSP48E2_X\d+Y8[6-9]}} {
  set_property PROHIBIT true [get_sites -quiet -regexp $re]
}
#### JO added (2026-11-23): VTA_FENCE_ROWS="<lo> <hi>" leaves slice rows lo..hi empty too (the crossbar multi4 alux3 had 1,922
# nets through row Y219 with only 218-220 empty: the logic right below the row was too dense for the RapidWright reroute)
if {[info exists ::env(VTA_FENCE_ROWS)] && $::env(VTA_FENCE_ROWS) ne ""} {
  lassign $::env(VTA_FENCE_ROWS) lo hi
  set n 0
  for {set y $lo} {$y <= $hi} {incr y} {
    set s [get_sites -quiet -regexp "SLICE_X\\d+Y$y"]
    set_property PROHIBIT true $s; incr n [llength $s]
  }
  puts "CHK slice rows $lo..$hi prohibited: $n sites"
}
#### JO added (2026-11-23): VTA_MMCM_MULT sets the pll_clk MMCM before placing (400 MHz = 100 x 12 / 3; 11.375 = 379.2 MHz),
# so placement and routing aim at that clock (mmcm_set.tcl does it after routing)
if {[info exists ::env(VTA_MMCM_MULT)] && $::env(VTA_MMCM_MULT) ne ""} {
  set_property CLKFBOUT_MULT_F $::env(VTA_MMCM_MULT) [get_cells vta_i/pll_clk/inst/mmcme4_adv_inst]
  puts "CHK pll period [get_property PERIOD [get_clocks clk_out1_vta_pll_clk_0]]"
}
foreach pb [get_pblocks] { set_property CONTAIN_ROUTING false $pb }
#### JO changed (2026-11-04): VTA_PLACE_DIRECTIVE picks another placer directive (multi4 rq missed 400 MHz by 7 ps with Explore)
#place_design -directive Explore
place_design -directive [expr {[info exists ::env(VTA_PLACE_DIRECTIVE)] && $::env(VTA_PLACE_DIRECTIVE) ne "" ? $::env(VTA_PLACE_DIRECTIVE) : "Explore"}]
phys_opt_design -directive AggressiveExplore
write_checkpoint -force [string map {routed placed} $out].dcp
route_design -directive AggressiveExplore
phys_opt_design -directive AggressiveExplore
write_checkpoint -force $out.dcp
write_edif -force $out.edf
report_timing_summary -file $out.timing.rpt
report_route_status -file $out.route.rpt
report_utilization -file $out.util.rpt
set argv {x timing}; source [file join [file dirname [info script]] chkbody.tcl] -notrace
