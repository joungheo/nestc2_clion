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
#### JO added (2026-11-25): VTA_CONTAIN=1 keeps the nets inside one half in that half: CONTAIN_ROUTING on the one-rectangle
# pblocks pb_evta_<i> / pb_hpslice and a new pb_lo (crossbar, width converter, resets: below the row, as the PS). multi4 amuldw had
# 549 nets with a PIP in row Y219, 156 + 47 + 30 of them compute_1 / gemm_sa_1 / acc_mem_1 nets between cells below the row
# (rowcls: driver -> load hierarchies); RapidWright's detours for them broke compute_1's timing (-5.4 ns). Two-rectangle
# pblocks (pb_fence) do not hold (vivado.tcl 2026-10-24); a one-rectangle one should.
if {[info exists ::env(VTA_CONTAIN)] && $::env(VTA_CONTAIN) ne "" && $::env(VTA_CONTAIN) ne "0"} {
  set lo {SLICE_X0Y0:SLICE_X96Y218 RAMB36_X0Y0:RAMB36_X12Y42 RAMB18_X0Y0:RAMB18_X12Y85 DSP48E2_X0Y0:DSP48E2_X17Y85}
  create_pblock pb_lo
  resize_pblock [get_pblocks pb_lo] -add $lo
  set_property PARENT pb_fence [get_pblocks pb_lo]
  set c [get_cells -quiet -filter {PBLOCK == pb_fence} {vta_i/axi_xbar vta_i/reg_dwc* vta_i/proc_sys_reset_ps*}]
  if {[llength $c]} { add_cells_to_pblock [get_pblocks pb_lo] $c }
  puts "CHK pb_lo: [llength $c] cells: $c"
  foreach pb [get_pblocks -quiet {pb_evta_0 pb_evta_1 pb_evta_2 pb_evta_3 pb_hpslice pb_lo}] { set_property CONTAIN_ROUTING true $pb; puts "CHK contain $pb [get_property GRID_RANGES $pb]" }
}
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
