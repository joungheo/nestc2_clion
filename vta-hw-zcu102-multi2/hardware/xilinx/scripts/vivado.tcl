# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.


#### JO rewritten for the ZCU102 multi-EVTA bitstream (one HLS IP set, NUM_EVTA copies), from the
#### vta-hw-zcu102-single vivado.tcl and vtabundles_xcmpl/evta_hw/vivado.tcl. EVTA i:
####   - control registers at FETCH/LOAD/COMPUTE/STORE_BASE_ADDR + i * 0x10000 (vtalib VTA_*_ADDR_<i>)
####   - its 5 AXI masters -> smc_<i> -> S_AXI_HP<i>_FPD (= S_AXI_GP<2+i>), 128 bit, non-coherent
####   - with <profile> = 1: AXI perf monitor apm_<i> at 0xA0050000 + i * 0x10000 (profile mode,
####     slot 0 = smc_<i> -> HP<i>, slot 1 = load_<i> data port, slot 2 = store_<i> data port)
#### usage (in the Vivado build dir):
####   vivado -mode batch -source vivado.tcl -tclargs <hls ip dir> <config.tcl> <num evta> [export name] [jobs] [profile]

# Check if script is running in correct Vivado version.
set scripts_vivado_version 2020.1
set current_vivado_version [version -short]

if { [string first $scripts_vivado_version $current_vivado_version] == -1 } {
   puts ""
   catch {common::send_msg_id "BD_TCL-109" "ERROR" "This script was generated using Vivado \
    <$scripts_vivado_version> and is being run in <$current_vivado_version> of Vivado."}
   return 1
}

if { [llength $argv] >= 3 && [llength $argv] <= 6 } {
  set ip_path     [lindex $argv 0]
  set vta_config  [lindex $argv 1]
  set num_evta    [lindex $argv 2]
  set export_name [expr {[llength $argv] > 3 ? [lindex $argv 3] : "vta"}]
  set num_threads [expr {[llength $argv] > 4 ? [lindex $argv 4] : 8}]
  set profile     [expr {[llength $argv] > 5 ? [lindex $argv 5] : 0}]
} else {
  puts "Arg list incomplete: <path to ip dir> <path to vta_config.tcl> <num evta> \[export name\] \[jobs\] \[profile\]"
  return 1
}
if { $num_evta < 1 || $num_evta > 4 } {
  puts "num evta must be 1-4 (one PS HP port each)"
  return 1
}

# Source vta config variables
source $vta_config
if { $TARGET ne "zcu102" } {
  puts "this script supports TARGET zcu102 only"
  return 1
}

set evta_addr_stride 0x10000
set apm_base_addr    0xA0050000

# Paths to IP library of VTA modules
set proj_name vta
set design_name $proj_name
set proj_path "."
set ip_lib "ip_lib"

# Create custom project
create_project -force $proj_name $proj_path -part $FPGA_DEVICE

# Apply board preset if exists
if {$FPGA_BOARD != "None" && $FPGA_BOARD_REV != "None"} {
  set_property board_part $FPGA_BOARD:$FPGA_BOARD_REV [current_project]
}

# Update IP repository with generated IP
file mkdir $ip_lib
set_property ip_repo_paths $ip_lib [current_project]
update_ip_catalog
foreach stage {fetch load compute store} {
  update_ip_catalog -add_ip "$ip_path/vta_$stage/soln/impl/ip/xilinx_com_hls_${stage}_1_0.zip" -repo_path $ip_lib
}

##################################################################
# CONFIGURE BLOCK DIAGRAM DESIGN
##################################################################

create_bd_design $design_name
current_bd_design $design_name

# Procedure to initialize FIFO
proc init_fifo_property {fifo width_bytes depth} {
  set_property -dict [ list \
    CONFIG.FIFO_Implementation_rach {Common_Clock_Distributed_RAM} \
    CONFIG.FIFO_Implementation_wach {Common_Clock_Distributed_RAM} \
    CONFIG.FIFO_Implementation_wrch {Common_Clock_Distributed_RAM} \
    CONFIG.Full_Flags_Reset_Value {1} \
    CONFIG.INTERFACE_TYPE {AXI_STREAM} \
    CONFIG.Input_Depth_axis $depth \
    CONFIG.Reset_Type {Asynchronous_Reset} \
    CONFIG.TDATA_NUM_BYTES $width_bytes \
  ] $fifo
}

# Procedure to initialize BRAM
proc init_bram_property {bram width depth} {
  set_property -dict [ list \
    CONFIG.use_bram_block {Stand_Alone} \
    CONFIG.Assume_Synchronous_Clk {true} \
    CONFIG.Byte_Size {8} \
    CONFIG.Enable_32bit_Address {true} \
    CONFIG.Enable_B {Use_ENB_Pin} \
    CONFIG.Memory_Type {True_Dual_Port_RAM} \
    CONFIG.Read_Width_A $width \
    CONFIG.Read_Width_B $width \
    CONFIG.Register_PortA_Output_of_Memory_Primitives {false} \
    CONFIG.Register_PortB_Output_of_Memory_Primitives {false} \
    CONFIG.Use_Byte_Write_Enable {true} \
    CONFIG.Use_RSTA_Pin {true} \
    CONFIG.Use_RSTB_Pin {true} \
    CONFIG.Write_Depth_A $depth \
    CONFIG.Write_Width_A $width \
    CONFIG.Write_Width_B $width \
  ] $bram
}

# ---- shared: PS, clock, reset, control crossbar
set processing_system [ create_bd_cell -type ip -vlnv xilinx.com:ip:zynq_ultra_ps_e:3.3 processing_system ]
apply_bd_automation -rule xilinx.com:bd_rule:zynq_ultra_ps_e -config {apply_board_preset "1" }  [get_bd_cells processing_system]
set ps_cfg [ list \
  CONFIG.PSU__FPGA_PL0_ENABLE {1} \
  CONFIG.PSU__CRL_APB__PL0_REF_CTRL__FREQMHZ {100} \
  CONFIG.PSU__USE__M_AXI_GP0 {1} \
  CONFIG.PSU__USE__M_AXI_GP1 {0} \
  CONFIG.PSU__USE__M_AXI_GP2 {0} \
  CONFIG.PSU__USE__S_AXI_GP0 {0} ]
for {set i 0} {$i < $num_evta} {incr i} {
  # S_AXI_GP2..5 = S_AXI_HP0..3_FPD
  lappend ps_cfg CONFIG.PSU__USE__S_AXI_GP[expr {2 + $i}] {1} CONFIG.PSU__SAXIGP[expr {2 + $i}]__DATA_WIDTH {128}
}
set_property -dict $ps_cfg $processing_system

set proc_sys_reset [ create_bd_cell -type ip -vlnv xilinx.com:ip:proc_sys_reset:5.0 proc_sys_reset ]
# keep the design in reset until the PLL locks
set pll_clk [ create_bd_cell -type ip -vlnv xilinx.com:ip:clk_wiz:6.0 pll_clk ]
set_property -dict [ list \
  CONFIG.CLKOUT1_REQUESTED_OUT_FREQ $FPGA_FREQ \
  CONFIG.USE_RESET {false} \
] $pll_clk
#### JO changed (2026-10-24, from vta-hw-zcu102-single): no MMCM reset (was CONFIG.RESET_PORT resetn / RESET_TYPE
#### ACTIVE_LOW on pl_resetn0): a pl_resetn0 pulse stopped the EVTA clock, so proc_sys_reset never reset the HLS
#### blocks and FIFOs (single, ILA _ila2/_ila3).

# control crossbar: 4 register slaves per EVTA (+1 perf monitor per EVTA with profile)
set n_ctrl [expr {($profile ? 5 : 4) * $num_evta}]
set axi_xbar [ create_bd_cell -type ip -vlnv xilinx.com:ip:axi_interconnect:2.1 axi_xbar ]
set_property -dict [ list CONFIG.NUM_MI $n_ctrl CONFIG.NUM_SI {1} ] $axi_xbar

connect_bd_intf_net -intf_net processing_system_m_axi [get_bd_intf_pins axi_xbar/S00_AXI] \
  [get_bd_intf_pins processing_system/M_AXI_HPM0_FPD]
connect_bd_net -net processing_system_reset [get_bd_pins processing_system/pl_resetn0] \
  [get_bd_pins proc_sys_reset/ext_reset_in]
connect_bd_net -net ps_clk_net [get_bd_pins processing_system/pl_clk0] [get_bd_pins pll_clk/clk_in1]
connect_bd_net -net pll_clk_locked [get_bd_pins pll_clk/locked] [get_bd_pins proc_sys_reset/dcm_locked]
connect_bd_net -net evta_clk [get_bd_pins pll_clk/clk_out1] [get_bd_pins proc_sys_reset/slowest_sync_clk] \
  [get_bd_pins axi_xbar/ACLK]
connect_bd_net -net evta_ic_rstn [get_bd_pins proc_sys_reset/interconnect_aresetn] [get_bd_pins axi_xbar/ARESETN]
connect_bd_net -net evta_rstn [get_bd_pins proc_sys_reset/peripheral_aresetn]
# the PS register port (M_AXI_HPM0) and the crossbar's slave side run on pl_clk0 (100 MHz): at 333 MHz the
# paths into PS8 MAXIGP0* miss timing (single EVTA: -0.039 ns); the crossbar inserts the clock converter
set proc_sys_reset_ps [ create_bd_cell -type ip -vlnv xilinx.com:ip:proc_sys_reset:5.0 proc_sys_reset_ps ]
connect_bd_net -net ps_clk_net [get_bd_pins proc_sys_reset_ps/slowest_sync_clk] \
  [get_bd_pins axi_xbar/S00_ACLK] [get_bd_pins processing_system/maxihpm0_fpd_aclk]
connect_bd_net -net processing_system_reset [get_bd_pins proc_sys_reset_ps/ext_reset_in]
connect_bd_net -net ps_rstn_100 [get_bd_pins proc_sys_reset_ps/peripheral_aresetn] [get_bd_pins axi_xbar/S00_ARESETN]

# control crossbar master port k -> slave pin s (and its clock / reset)
proc connect_ctrl {k s} {
  set m [format "M%02d" $k]
  connect_bd_intf_net [get_bd_intf_pins axi_xbar/${m}_AXI] [get_bd_intf_pins $s]
  connect_bd_net -net evta_clk [get_bd_pins axi_xbar/${m}_ACLK]
  connect_bd_net -net evta_rstn [get_bd_pins axi_xbar/${m}_ARESETN]
}

# ---- one EVTA core: instance suffix i, crossbar ports 4i..4i+3, PS HP port i
proc add_evta {i} {
  global AXI_CACHE_BITS AXI_PROT_BITS INSN_BYTES INP_MEM_BANKS INP_MEM_WIDTH INP_MEM_DEPTH \
         WGT_MEM_BANKS WGT_MEM_WIDTH WGT_MEM_DEPTH OUT_MEM_BANKS OUT_MEM_WIDTH OUT_MEM_DEPTH \
         IP_REG_MAP_RANGE FETCH_BASE_ADDR LOAD_BASE_ADDR COMPUTE_BASE_ADDR STORE_BASE_ADDR evta_addr_stride
  set c $AXI_CACHE_BITS
  set p $AXI_PROT_BITS
  set fetch   [ create_bd_cell -type ip -vlnv xilinx.com:hls:fetch:1.0 fetch_$i ]
  set_property -dict [ list CONFIG.C_M_AXI_INS_PORT_CACHE_VALUE $c CONFIG.C_M_AXI_INS_PORT_PROT_VALUE $p ] $fetch
  set load    [ create_bd_cell -type ip -vlnv xilinx.com:hls:load:1.0 load_$i ]
  set_property -dict [ list CONFIG.C_M_AXI_DATA_PORT_CACHE_VALUE $c CONFIG.C_M_AXI_DATA_PORT_PROT_VALUE $p ] $load
  set compute [ create_bd_cell -type ip -vlnv xilinx.com:hls:compute:1.0 compute_$i ]
  set_property -dict [ list CONFIG.C_M_AXI_DATA_PORT_CACHE_VALUE $c CONFIG.C_M_AXI_DATA_PORT_PROT_VALUE $p \
                            CONFIG.C_M_AXI_UOP_PORT_CACHE_VALUE $c CONFIG.C_M_AXI_UOP_PORT_PROT_VALUE $p ] $compute
  set store   [ create_bd_cell -type ip -vlnv xilinx.com:hls:store:1.0 store_$i ]
  set_property -dict [ list CONFIG.C_M_AXI_DATA_PORT_CACHE_VALUE $c CONFIG.C_M_AXI_DATA_PORT_PROT_VALUE $p ] $store

  # command queues carry whole instructions (INSN_BYTES, 32 B for the 256-bit EVTA ISA); dependence queues 1 B
  foreach q {load_queue gemm_queue store_queue} {
    init_fifo_property [ create_bd_cell -type ip -vlnv xilinx.com:ip:fifo_generator:13.2 ${q}_$i ] $INSN_BYTES 512
  }
  foreach q {l2g_queue g2l_queue g2s_queue s2g_queue} {
    init_fifo_property [ create_bd_cell -type ip -vlnv xilinx.com:ip:fifo_generator:13.2 ${q}_$i ] 1 1024
  }
  foreach {q src_pin dst_pin} [list \
      load_queue  fetch_$i/load_queue_V_V      load_$i/load_queue_V_V \
      gemm_queue  fetch_$i/gemm_queue_V_V      compute_$i/gemm_queue_V_V \
      store_queue fetch_$i/store_queue_V_V     store_$i/store_queue_V_V \
      l2g_queue   load_$i/l2g_dep_queue_V      compute_$i/l2g_dep_queue_V \
      g2l_queue   compute_$i/g2l_dep_queue_V   load_$i/g2l_dep_queue_V \
      g2s_queue   compute_$i/g2s_dep_queue_V   store_$i/g2s_dep_queue_V \
      s2g_queue   store_$i/s2g_dep_queue_V     compute_$i/s2g_dep_queue_V ] {
    connect_bd_intf_net [get_bd_intf_pins $src_pin] [get_bd_intf_pins ${q}_$i/S_AXIS]
    connect_bd_intf_net [get_bd_intf_pins ${q}_$i/M_AXIS] [get_bd_intf_pins $dst_pin]
  }

  # SRAMs between the stages (pin names: <mem>_V_PORTA, or <mem>_<k>_V_PORTA when partitioned)
  foreach {mem banks width depth a b} [list \
      inp_mem $INP_MEM_BANKS $INP_MEM_WIDTH $INP_MEM_DEPTH load_$i    compute_$i \
      wgt_mem $WGT_MEM_BANKS $WGT_MEM_WIDTH $WGT_MEM_DEPTH load_$i    compute_$i \
      out_mem $OUT_MEM_BANKS $OUT_MEM_WIDTH $OUT_MEM_DEPTH compute_$i store_$i ] {
    for {set k 0} {$k < $banks} {incr k} {
      set bram [ create_bd_cell -type ip -vlnv xilinx.com:ip:blk_mem_gen:8.4 ${mem}_${i}_$k ]
      init_bram_property $bram $width $depth
      set pin [expr {$banks > 1 ? "${mem}_${k}_V_PORTA" : "${mem}_V_PORTA"}]
      connect_bd_intf_net [get_bd_intf_pins $bram/BRAM_PORTA] [get_bd_intf_pins $a/$pin]
      connect_bd_intf_net [get_bd_intf_pins $bram/BRAM_PORTB] [get_bd_intf_pins $b/$pin]
    }
  }

  # data: 5 masters -> smc_i -> S_AXI_HP<i>_FPD
  set smc [ create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 smc_$i ]
  set_property -dict [ list CONFIG.NUM_MI {1} CONFIG.NUM_SI {5} ] $smc
  set k 0
  foreach m [list fetch_$i/m_axi_ins_port load_$i/m_axi_data_port compute_$i/m_axi_uop_port \
                  compute_$i/m_axi_data_port store_$i/m_axi_data_port] {
    connect_bd_intf_net -intf_net [string map {/ _} $m] [get_bd_intf_pins $m] [get_bd_intf_pins smc_$i/S0${k}_AXI]
    incr k
  }
  connect_bd_intf_net -intf_net smc_${i}_M00_AXI [get_bd_intf_pins smc_$i/M00_AXI] \
    [get_bd_intf_pins processing_system/S_AXI_HP${i}_FPD]

  # control: crossbar M(4i+0..3) -> fetch/load/compute/store
  set k 0
  foreach s [list fetch_$i load_$i compute_$i store_$i] {
    connect_ctrl [expr {4 * $i + $k}] $s/s_axi_CONTROL_BUS
    incr k
  }

  # clocks / resets of this core
  foreach s [list fetch_$i load_$i compute_$i store_$i] {
    connect_bd_net -net evta_clk [get_bd_pins $s/ap_clk]
    connect_bd_net -net evta_rstn [get_bd_pins $s/ap_rst_n]
  }
  foreach q {load_queue gemm_queue store_queue l2g_queue g2l_queue g2s_queue s2g_queue} {
    connect_bd_net -net evta_clk [get_bd_pins ${q}_$i/s_aclk]
    connect_bd_net -net evta_rstn [get_bd_pins ${q}_$i/s_aresetn]
  }
  connect_bd_net -net evta_clk [get_bd_pins smc_$i/aclk] [get_bd_pins processing_system/saxihp${i}_fpd_aclk]
  connect_bd_net -net evta_rstn [get_bd_pins smc_$i/aresetn]

  # address map: control registers of EVTA i, and the low 2 GB of DDR through HP<i>
  foreach {s base} [list fetch_$i $FETCH_BASE_ADDR load_$i $LOAD_BASE_ADDR \
                         compute_$i $COMPUTE_BASE_ADDR store_$i $STORE_BASE_ADDR] {
    set addr [format "0x%08X" [expr {$base + $i * $evta_addr_stride}]]
    create_bd_addr_seg -range $IP_REG_MAP_RANGE -offset $addr [get_bd_addr_spaces processing_system/Data] \
      [get_bd_addr_segs $s/s_axi_CONTROL_BUS/Reg] SEG_${s}_Reg
  }
  set gp [expr {2 + $i}]
  foreach sp [list fetch_$i/Data_m_axi_ins_port load_$i/Data_m_axi_data_port compute_$i/Data_m_axi_uop_port \
                   compute_$i/Data_m_axi_data_port store_$i/Data_m_axi_data_port] {
    create_bd_addr_seg -range 0x80000000 -offset 0x00000000 [get_bd_addr_spaces $sp] \
      [get_bd_addr_segs processing_system/SAXIGP${gp}/HP${i}_DDR_LOW] SEG_HP${i}_DDR_LOW
  }
}

# ---- AXI perf monitor of EVTA i (profile mode), crossbar port 4 * num_evta + i.
# Profile-mode counters per slot: +0x00 wr bytes, +0x10 wr transactions, +0x20 wr latency,
# +0x30 rd bytes, +0x40 rd transactions, +0x50 rd latency; slots at 0x100 / 0x160 / 0x500, control 0x300
# (vtalib pynq_driver.cc AXI_PERF_MONITOR + i * 0x10000).
proc add_apm {i num_evta} {
  global apm_base_addr evta_addr_stride
  set apm [ create_bd_cell -type ip -vlnv xilinx.com:ip:axi_perf_mon:5.0 apm_$i ]
  set_property -dict [ list \
    CONFIG.C_ENABLE_PROFILE {1} \
    CONFIG.C_ENABLE_ADVANCED {0} \
    CONFIG.C_ENABLE_TRACE {0} \
    CONFIG.C_NUM_MONITOR_SLOTS {3} \
  ] $apm
  connect_ctrl [expr {4 * $num_evta + $i}] apm_$i/S_AXI
  connect_bd_intf_net -intf_net [get_bd_intf_nets smc_${i}_M00_AXI] [get_bd_intf_pins apm_$i/SLOT_0_AXI] [get_bd_intf_pins smc_$i/M00_AXI]
  connect_bd_intf_net -intf_net [get_bd_intf_nets load_${i}_m_axi_data_port] [get_bd_intf_pins apm_$i/SLOT_1_AXI] [get_bd_intf_pins load_$i/m_axi_data_port]
  connect_bd_intf_net -intf_net [get_bd_intf_nets store_${i}_m_axi_data_port] [get_bd_intf_pins apm_$i/SLOT_2_AXI] [get_bd_intf_pins store_$i/m_axi_data_port]
  foreach pin {s_axi core slot_0_axi slot_1_axi slot_2_axi} {
    set clk [expr {$pin eq "core" ? "core_aclk" : "${pin}_aclk"}]
    set rst [expr {$pin eq "core" ? "core_aresetn" : "${pin}_aresetn"}]
    connect_bd_net -net evta_clk [get_bd_pins apm_$i/$clk]
    connect_bd_net -net evta_rstn [get_bd_pins apm_$i/$rst]
  }
  set addr [format "0x%08X" [expr {$apm_base_addr + $i * $evta_addr_stride}]]
  create_bd_addr_seg -range 0x10000 -offset $addr [get_bd_addr_spaces processing_system/Data] \
    [get_bd_addr_segs apm_$i/S_AXI/Reg] SEG_apm_${i}_Reg
}

for {set i 0} {$i < $num_evta} {incr i} {
  add_evta $i
}
if {$profile} {
  for {set i 0} {$i < $num_evta} {incr i} {
    add_apm $i $num_evta
  }
}

# fail early on connection / address errors
validate_bd_design
save_bd_design


##################################################################
# COMPILATION FLOW
##################################################################

# Create top-level wrapper file
make_wrapper -files \
  [get_files $proj_path/$proj_name.srcs/sources_1/bd/$proj_name/$proj_name.bd] -top
add_files -norecurse $proj_path/$proj_name.srcs/sources_1/bd/$proj_name/hdl/${proj_name}_wrapper.v
update_compile_order -fileset sources_1
update_compile_order -fileset sim_1

#### JO added (2026-10-24, from vta-hw-zcu102-single): keep the HLS blocks and the queues as they are in
#### implementation. Without this opt_design/phys_opt pulled FIFO EMPTY logic and reset replicas across the IP
#### boundaries, and the result differed per run (single, board: compute stuck at the first GEMM, _ila6).
set dt_xdc $proj_path/$proj_name.srcs/constrs_1/new/dont_touch.xdc
file mkdir [file dirname $dt_xdc]
set f [open $dt_xdc w]
for {set i 0} {$i < $num_evta} {incr i} {
  set cells {}
  foreach c {fetch load compute store load_queue gemm_queue store_queue l2g_queue g2l_queue g2s_queue s2g_queue} {
    lappend cells vta_i/${c}_$i
  }
  puts $f "set_property DONT_TOUCH true \[get_cells {$cells}\]"
}
close $f
add_files -fileset constrs_1 $dt_xdc
set_property used_in_synthesis false [get_files $dt_xdc]
#### JO added (2026-10-24): fence for the lab ZCU102 (192.168.20.2), environment VTA_BRAM_FENCE. Its PL has one
#### defective configuration row (vta-hw-zcu102-single, JTAG readback, hwtest/brambist/cfgdiff.py): clock-region
#### row 3, tile row SLICE Y219 / RAMB36 Y43 / RAMB18 Y86-87 / DSP Y86-87, 8 cells per frame stuck at 0, so whatever
#### is configured in that row (LUT, FF, routing, BRAM settings) may lose bits.
####   y219 (default for 1-2 EVTAs): all PL cells but the PS and the MMCM in a pblock below the row (Y0..218) with
####          CONTAIN_ROUTING, as in vta-hw-zcu102-single (379 RAMB36 below the row; multi2 needs 286)
####   split (default for 3-4 EVTAs): pblock pb_fence = the device below (Y0..218) and above (Y220..419) the row;
####          EVTA i in child pblock pb_evta_<i> (0, 1 below, 2, 3 above; multi4 needs 572 RAMB36), each with
####          CONTAIN_ROUTING. 2026-10-24 multi2/multi4 builds: CONTAIN_ROUTING of a pblock made of two rectangles
####          does not keep its nets out of the row between them (nets of child pblocks used row-Y219 PIPs), and
####          nets that cross the row (PS HP2/HP3 <-> smc_2/3) are not contained by any pblock: multi4 still sets 26
####          stuck cells (stuckchk.py), not usable on the lab board yet.
####   The row's sites and the MMCMs of rows 3..6 are prohibited.
####   upper: PROHIBIT every BRAM from RAMB36 Y36 up (the first workaround)
####   none:  no fence (other boards)
#### Check the bitstream with hwtest/brambist/stuckchk.py -b <a working bitstream>: 0 extra cells set.
set bram_fence [expr {[info exists ::env(VTA_BRAM_FENCE)] ? $::env(VTA_BRAM_FENCE) : ($num_evta <= 2 ? "y219" : "split")}]
set fence_lo {SLICE_X0Y0:SLICE_X96Y218 RAMB36_X0Y0:RAMB36_X12Y42 RAMB18_X0Y0:RAMB18_X12Y85 DSP48E2_X0Y0:DSP48E2_X17Y85}
set fence_hi {SLICE_X0Y220:SLICE_X96Y419 RAMB36_X0Y44:RAMB36_X12Y83 RAMB18_X0Y88:RAMB18_X12Y167 DSP48E2_X0Y88:DSP48E2_X17Y167}
if {$bram_fence eq "split" || $bram_fence eq "y219"} {
  set pb_xdc $proj_path/$proj_name.srcs/constrs_1/new/fence_y219.xdc
  set f [open $pb_xdc w]
  puts $f "create_pblock pb_fence"
  puts $f "resize_pblock \[get_pblocks pb_fence\] -add {$fence_lo[expr {$bram_fence eq "split" ? " $fence_hi" : ""}]}"
  puts $f "set_property CONTAIN_ROUTING true \[get_pblocks pb_fence\]"
  if {$bram_fence eq "split"} {
    for {set i 0} {$i < $num_evta} {incr i} {
      puts $f "create_pblock pb_evta_$i"
      puts $f "resize_pblock \[get_pblocks pb_evta_$i\] -add {[expr {$i < 2 ? $fence_lo : $fence_hi}]}"
      puts $f "set_property PARENT pb_fence \[get_pblocks pb_evta_$i\]"
      puts $f "set_property CONTAIN_ROUTING true \[get_pblocks pb_evta_$i\]"
      puts $f "add_cells_to_pblock \[get_pblocks pb_evta_$i\] \[get_cells -regexp {vta_i/\[a-z_\]+_${i}(_\\d+)?}\]"
    }
  }
  puts $f {add_cells_to_pblock [get_pblocks pb_fence] [get_cells -filter {NAME !~ vta_i/processing_system && NAME !~ vta_i/pll_clk && PBLOCK == ""} vta_i/*]}
  puts $f {set_property PROHIBIT true [get_sites -regexp {MMCM_X\d+Y[3-6]}]}
  #### one line per site type: with SLICE first in one -regexp alternation get_sites returned only the SLICEs
  #### (multi2 build 2026-10-24: 2 RAMB36 placed at Y43)
  foreach re {{SLICE_X\d+Y219} {RAMB36_X\d+Y43} {RAMB18_X\d+Y8[67]} {DSP48E2_X\d+Y8[67]}} {
    puts $f "set_property PROHIBIT true \[get_sites -regexp {$re}\]"
  }
  close $f
  add_files -fileset constrs_1 $pb_xdc
  set_property used_in_synthesis false [get_files $pb_xdc]
} elseif {$bram_fence eq "upper"} {
  set pb_xdc $proj_path/$proj_name.srcs/constrs_1/new/prohibit_upper_bram.xdc
  set f [open $pb_xdc w]
  puts $f {set_property PROHIBIT true [get_sites -regexp {RAMB36_X\d+Y(3[6-9]|[4-8]\d)}]}
  puts $f {set_property PROHIBIT true [get_sites -regexp {RAMB18_X\d+Y(7[2-9]|[89]\d|1[0-6]\d)}]}
  close $f
  add_files -fileset constrs_1 $pb_xdc
  set_property used_in_synthesis false [get_files $pb_xdc]
} elseif {$bram_fence ne "none"} {
  puts "VTA_BRAM_FENCE must be split, y219, upper or none"
  return 1
}
launch_runs impl_1 -to_step write_bitstream -jobs $num_threads
wait_on_run impl_1

# Export export/<export name>.bit plus .hwh, .xsa and utilization / timing reports
# (check WNS >= 0 in <export name>.timing.rpt before using the bitstream)
if {[file exist $proj_path/$proj_name.runs/impl_1/${proj_name}_wrapper.bit]} {
  file mkdir $proj_path/export
  file copy -force $proj_path/$proj_name.runs/impl_1/${proj_name}_wrapper.bit \
    $proj_path/export/$export_name.bit
  open_run impl_1
  report_utilization -file $proj_path/export/$export_name.utilization.rpt
  report_timing_summary -file $proj_path/export/$export_name.timing.rpt
  write_hw_platform -fixed -include_bit -force -file $proj_path/export/$export_name.xsa
  foreach hwh [glob -nocomplain $proj_path/$proj_name.srcs/sources_1/bd/$proj_name/hw_handoff/$proj_name.hwh] {
    file copy -force $hwh $proj_path/export/$export_name.hwh
  }
  puts "bitstream: [file normalize $proj_path/export/$export_name.bit]"
} else {
  puts "ERROR: no bitstream (see $proj_name.runs/impl_1/runme.log)"
  exit 1
}

exit
