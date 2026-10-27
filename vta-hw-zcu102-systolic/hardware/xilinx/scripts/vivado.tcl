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

# Check if script is running in correct Vivado version.
set scripts_vivado_version 2020.1
set current_vivado_version [version -short]

if { [string first $scripts_vivado_version $current_vivado_version] == -1 } {
   puts ""
   catch {common::send_msg_id "BD_TCL-109" "ERROR" "This script was generated using Vivado \
    <$scripts_vivado_version> and is being run in <$current_vivado_version> of Vivado."}
   return 1
}

# Parse argument list, derive the clock to utilize
#### JO changed: optional <export name> (export/<name>.bit/.hwh/.xsa, default vta), <jobs> (default 8) and
#### <profile> (1 = AXI perf monitor at 0xA0050000, default 0)
#if { [llength $argv] eq 2 } {
if { [llength $argv] >= 2 && [llength $argv] <= 5 } {
  set ip_path     [lindex $argv 0]
  set vta_config  [lindex $argv 1]
  set export_name [expr {[llength $argv] > 2 ? [lindex $argv 2] : "vta"}]
  set num_threads [expr {[llength $argv] > 3 ? [lindex $argv 3] : 8}]
  set profile     [expr {[llength $argv] > 4 ? [lindex $argv 4] : 0}]
} else {
  puts "Arg list incomplete: <path to ip dir> <path to vta_config.py> \[export name\] \[jobs\] \[profile\]"
  return 1
}

# Source vta config variables
source $vta_config

# Get the VTA configuration paramters
set target            $TARGET
set device            $FPGA_DEVICE
set device_family     $FPGA_FAMILY
set clock_freq        $FPGA_FREQ
#### JO added (2026-10-27, systolic Phase A): Makefile FREQ=<MHz> overrides the PLL frequency,
#### MEM_OREG=1 registers port B (compute's read port) of inp_mem/wgt_mem
if {[info exists ::env(VTA_FREQ)] && $::env(VTA_FREQ) ne ""} {
  set clock_freq $::env(VTA_FREQ)
}
set mem_oreg [expr {[info exists ::env(VTA_MEM_OREG)] && $::env(VTA_MEM_OREG) eq "1"}]
#### JO added (2026-10-27, systolic Phase D, docs/evta_systolic_design.md §2): SYSTOLIC=1 (env VTA_SYSTOLIC) -> GEMM in the RTL
#### unit gemm_sa (rtl/gemm_sa_bd.v): acc_mem is a BD BRAM shared by compute and gemm_sa, inp/wgt port B and out port A
#### go through gemm_sa_bd, and inp/wgt port B have their output register (read latency 2 = gemm_sa L_RD).
set systolic [expr {[info exists ::env(VTA_SYSTOLIC)] && $::env(VTA_SYSTOLIC) eq "1"}]
if {$systolic} { set mem_oreg 1 }
puts "systolic GEMM (gemm_sa) $systolic"
puts "PLL $clock_freq MHz, inp/wgt port B output register $mem_oreg"
set board             $FPGA_BOARD
set board_rev         $FPGA_BOARD_REV

# SRAM dimensions
set inp_part          $INP_MEM_BANKS
set inp_mem_width     $INP_MEM_WIDTH
set inp_mem_depth     $INP_MEM_DEPTH
set wgt_part          $WGT_MEM_BANKS
set wgt_mem_width     $WGT_MEM_WIDTH
set wgt_mem_depth     $WGT_MEM_DEPTH
set out_part          $OUT_MEM_BANKS
set out_mem_width     $OUT_MEM_WIDTH
set out_mem_depth     $OUT_MEM_DEPTH
set num_wgt_mem_uram  $NUM_WGT_MEM_URAM

# AXI bus signals
set axi_cache         $AXI_CACHE_BITS
set axi_prot          $AXI_PROT_BITS

# Address map
set ip_reg_map_range  $IP_REG_MAP_RANGE
set fetch_base_addr   $FETCH_BASE_ADDR
set load_base_addr    $LOAD_BASE_ADDR
set compute_base_addr $COMPUTE_BASE_ADDR
set store_base_addr   $STORE_BASE_ADDR
#### JO added: vtalib pynq_driver.cc AXI_PERF_MONITOR
set apm_base_addr     0xA0050000

# Paths to IP library of VTA modules
set proj_name vta
set design_name $proj_name
set proj_path "."
set ip_lib "ip_lib"
set fetch_ip "${ip_path}/vta_fetch/soln/impl/ip/xilinx_com_hls_fetch_1_0.zip"
set load_ip "${ip_path}/vta_load/soln/impl/ip/xilinx_com_hls_load_1_0.zip"
set compute_ip "${ip_path}/vta_compute/soln/impl/ip/xilinx_com_hls_compute_1_0.zip"
set store_ip "${ip_path}/vta_store/soln/impl/ip/xilinx_com_hls_store_1_0.zip"

# Create custom project
create_project -force $proj_name $proj_path -part $device

# Apply board preset if exists
if {$board != "None" && $board_rev != "None"} {
  set_property board_part $board:$board_rev [current_project]
}

# Update IP repository with generated IP
file mkdir $ip_lib
set_property ip_repo_paths $ip_lib [current_project]
update_ip_catalog
update_ip_catalog -add_ip $fetch_ip -repo_path $ip_lib
update_ip_catalog -add_ip $load_ip -repo_path $ip_lib
update_ip_catalog -add_ip $compute_ip -repo_path $ip_lib
update_ip_catalog -add_ip $store_ip -repo_path $ip_lib
#### JO added (systolic Phase D): RTL sources of gemm_sa_bd
if {$systolic} {
  set rtl_dir [file normalize [file join [file dirname [info script]] .. rtl]]
  add_files -norecurse [list $rtl_dir/sa_pe.v $rtl_dir/sa_array.v $rtl_dir/sa_ctrl.v $rtl_dir/sa_rmw.v $rtl_dir/gemm_sa.v $rtl_dir/gemm_sa_bd.v]
  update_compile_order -fileset sources_1
}


##################################################################
# CONFIGURE BLOCK DIAGRAM DESIGN
##################################################################

# Create bd design
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
#### JO changed (2026-10-27): regb = Register_PortB_Output_of_Memory_Primitives
#proc init_bram_property {bram width depth} {
proc init_bram_property {bram width depth {regb false}} {
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
    CONFIG.Register_PortB_Output_of_Memory_Primitives $regb \
    CONFIG.Use_Byte_Write_Enable {true} \
    CONFIG.Use_RSTA_Pin {true} \
    CONFIG.Use_RSTB_Pin {true} \
    CONFIG.Write_Depth_A $depth \
    CONFIG.Write_Width_A $width \
    CONFIG.Write_Width_B $width \
  ] $bram
}

# Create instance: proc_sys_reset, and set properties
set proc_sys_reset \
  [ create_bd_cell -type ip -vlnv xilinx.com:ip:proc_sys_reset:5.0 proc_sys_reset ]

# Create instance: pll_clk, and set properties
set pll_clk [ create_bd_cell -type ip -vlnv xilinx.com:ip:clk_wiz:6.0 pll_clk ]
set_property -dict [ list \
  CONFIG.CLKOUT1_REQUESTED_OUT_FREQ $clock_freq \
  CONFIG.USE_RESET {false} \
] $pll_clk
#### JO changed (2026-10-23): no MMCM reset (was CONFIG.RESET_PORT resetn / RESET_TYPE ACTIVE_LOW on
#### pl_resetn0): a pl_resetn0 pulse stopped the EVTA clock, so the 333 MHz proc_sys_reset never reset the
#### HLS blocks and FIFOs (ILA _ila2/_ila3). Now both proc_sys_resets reset with their clocks running.
#### JO changed: keep the design in reset until the PLL locks (locked -> proc_sys_reset/dcm_locked below)
#  CONFIG.USE_LOCKED {false} \

# Create instance: axi_smc0, and set properties
set axi_smc0 [ create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 axi_smc0 ]
set_property -dict [ list \
  CONFIG.NUM_MI {1} \
  CONFIG.NUM_SI {5} \
] $axi_smc0

# Create instance: axi_xbar, and set properties
set axi_xbar \
  [ create_bd_cell -type ip -vlnv xilinx.com:ip:axi_interconnect:2.1 axi_xbar ]
set_property -dict [ list \
  CONFIG.NUM_MI {4} \
  CONFIG.NUM_SI {1} \
] $axi_xbar
#### JO added: one more register slave for the perf monitor
if {$profile} {
  set_property CONFIG.NUM_MI {5} $axi_xbar
}

# Create instance: fetch_0, and set properties
set fetch_0 [ create_bd_cell -type ip -vlnv xilinx.com:hls:fetch:1.0 fetch_0 ]
set_property -dict [ list \
  CONFIG.C_M_AXI_INS_PORT_CACHE_VALUE $axi_cache \
  CONFIG.C_M_AXI_INS_PORT_PROT_VALUE $axi_prot \
] $fetch_0

# Create instance: load_0, and set properties
set load_0 [ create_bd_cell -type ip -vlnv xilinx.com:hls:load:1.0 load_0 ]
set_property -dict [ list \
  CONFIG.C_M_AXI_DATA_PORT_CACHE_VALUE $axi_cache \
  CONFIG.C_M_AXI_DATA_PORT_PROT_VALUE $axi_prot \
] $load_0

# Create instance: compute_0, and set properties
set compute_0 [ create_bd_cell -type ip -vlnv xilinx.com:hls:compute:1.0 compute_0 ]
set_property -dict [ list \
  CONFIG.C_M_AXI_DATA_PORT_CACHE_VALUE $axi_cache \
  CONFIG.C_M_AXI_DATA_PORT_PROT_VALUE $axi_prot \
  CONFIG.C_M_AXI_UOP_PORT_CACHE_VALUE $axi_cache \
  CONFIG.C_M_AXI_UOP_PORT_PROT_VALUE $axi_prot \
] $compute_0

# Create instance: store_0, and set properties
set store_0 [ create_bd_cell -type ip -vlnv xilinx.com:hls:store:1.0 store_0 ]
set_property -dict [ list \
  CONFIG.C_M_AXI_DATA_PORT_CACHE_VALUE $axi_cache \
  CONFIG.C_M_AXI_DATA_PORT_PROT_VALUE $axi_prot \
] $store_0

#### JO added (systolic Phase D): gemm_sa_0 and acc_mem (4096 x 512 for 16/16/19/18, output registers on both ports = latency 2)
if {$systolic} {
  set sa_dw [expr {int(round(log($out_mem_depth) / log(2)))}]
  set sa_sw [expr {int(round(log($inp_mem_depth) / log(2)))}]
  set sa_ww [expr {int(round(log($wgt_mem_depth) / log(2)))}]
  set gemm_sa_0 [ create_bd_cell -type module -reference gemm_sa_bd gemm_sa_0 ]
  set_property -dict [ list CONFIG.DW $sa_dw CONFIG.SW $sa_sw CONFIG.WW $sa_ww \
    CONFIG.UB [expr {(($sa_dw + $sa_sw + $sa_ww + 2 + 7) / 8) * 8}] ] $gemm_sa_0
  set acc_mem [ create_bd_cell -type ip -vlnv xilinx.com:ip:blk_mem_gen:8.4 acc_mem ]
  [ init_bram_property $acc_mem 512 $out_mem_depth true ]
  set_property CONFIG.Register_PortA_Output_of_Memory_Primitives {true} $acc_mem
  connect_bd_intf_net [get_bd_intf_pins gemm_sa_0/mem_acc_a] [get_bd_intf_pins acc_mem/BRAM_PORTA]
  connect_bd_intf_net [get_bd_intf_pins gemm_sa_0/mem_acc_b] [get_bd_intf_pins acc_mem/BRAM_PORTB]
  connect_bd_intf_net [get_bd_intf_pins compute_0/acc_mem_V_PORTA] [get_bd_intf_pins gemm_sa_0/hls_acc_a]
  connect_bd_intf_net [get_bd_intf_pins compute_0/acc_mem_V_PORTB] [get_bd_intf_pins gemm_sa_0/hls_acc_b]
  connect_bd_intf_net [get_bd_intf_pins compute_0/out_mem_V_PORTA] [get_bd_intf_pins gemm_sa_0/hls_out]
  connect_bd_intf_net [get_bd_intf_pins compute_0/gemm_uop_V_V] [get_bd_intf_pins gemm_sa_0/s_uop]
  connect_bd_intf_net [get_bd_intf_pins gemm_sa_0/m_done] [get_bd_intf_pins compute_0/gemm_done_V]
}

# Create command queues and set properties
set cmd_queue_list {load_queue gemm_queue store_queue}
foreach cmd_queue $cmd_queue_list {
  set tmp_cmd_queue [ create_bd_cell -type ip -vlnv xilinx.com:ip:fifo_generator:13.2 $cmd_queue ]
  # Width is 16B (128b, as set in hw_spec.h), depth is 512 (depth of FIFO on Zynq 7000 and Zynq Ultrascale+)
  # TODO: derive it from vta_config.h
  #### JO changed: width = INSN_BYTES from hw_spec.h (32B for the 256-bit EVTA instructions)
  #[ init_fifo_property $tmp_cmd_queue 16 512 ]
  [ init_fifo_property $tmp_cmd_queue $INSN_BYTES 512 ]
}

# Create dependence queues and set properties
set dep_queue_list {l2g_queue g2l_queue g2s_queue s2g_queue}
foreach dep_queue $dep_queue_list {
  set tmp_dep_queue [ create_bd_cell -type ip -vlnv xilinx.com:ip:fifo_generator:13.2 $dep_queue ]
  # Width is 1B (min width), depth is 1024
  # TODO: derive it from vta_config.h
  [ init_fifo_property $tmp_dep_queue 1 1024 ]
}

# Create and connect inp_mem partitions
for {set i 0} {$i < $inp_part} {incr i} {
  # Create instance: inp_mem, and set properties
  set inp_mem [ create_bd_cell -type ip -vlnv xilinx.com:ip:blk_mem_gen:8.4 inp_mem_${i} ]
  [ init_bram_property $inp_mem $inp_mem_width $inp_mem_depth [expr {$mem_oreg ? "true" : "false"}] ]
  # If module has more than 1 mem port, the naming convention changes
  if {$inp_part > 1} {
    set porta [get_bd_intf_pins load_0/inp_mem_${i}_V_PORTA]
    set portb [get_bd_intf_pins compute_0/inp_mem_${i}_V_PORTA]
  } else {
    set porta [get_bd_intf_pins load_0/inp_mem_V_PORTA]
    set portb [get_bd_intf_pins compute_0/inp_mem_V_PORTA]
  }
  if {$systolic} { set portb [get_bd_intf_pins gemm_sa_0/mem_inp] }
  # Create interface connections
  connect_bd_intf_net -intf_net load_0_inp_mem_V_PORTA \
    [get_bd_intf_pins $inp_mem/BRAM_PORTA] \
    $porta
  connect_bd_intf_net -intf_net compute_0_inp_mem_V_PORTA \
    [get_bd_intf_pins $inp_mem/BRAM_PORTB] \
    $portb
}

# Create and connect wgt_mem partitions
for {set i 0} {$i < $wgt_part} {incr i} {
  # Create instance: wgt_mem, and set properties
  set wgt_mem [ create_bd_cell -type ip -vlnv xilinx.com:ip:blk_mem_gen:8.4 wgt_mem_${i} ]
  [ init_bram_property $wgt_mem $wgt_mem_width $wgt_mem_depth [expr {$mem_oreg ? "true" : "false"}] ]
  # If module has more than 1 mem port, the naming convention changes
  if {$wgt_part > 1} {
    set porta [get_bd_intf_pins load_0/wgt_mem_${i}_V_PORTA]
    set portb [get_bd_intf_pins compute_0/wgt_mem_${i}_V_PORTA]
  } else {
    set porta [get_bd_intf_pins load_0/wgt_mem_V_PORTA]
    set portb [get_bd_intf_pins compute_0/wgt_mem_V_PORTA]
  }
  if {$systolic} { set portb [get_bd_intf_pins gemm_sa_0/mem_wgt${i}] }
  # Create interface connections
  connect_bd_intf_net -intf_net load_0_wgt_mem_${i}_V_PORTA \
    [get_bd_intf_pins $wgt_mem/BRAM_PORTA] \
    $porta
  connect_bd_intf_net -intf_net compute_0_wgt_mem_${i}_V_PORTA \
    [get_bd_intf_pins $wgt_mem/BRAM_PORTB] \
    $portb
  if { $device_family eq "zynq-ultrascale+" && $i < $num_wgt_mem_uram } {
    set_property -dict [list CONFIG.PRIM_type_to_Implement {URAM}] $wgt_mem
  }
}

# Create and connect out_mem partitions
for {set i 0} {$i < $out_part} {incr i} {
  # Create instance: out_mem, and set properties
  set out_mem [ create_bd_cell -type ip -vlnv xilinx.com:ip:blk_mem_gen:8.4 out_mem_${i} ]
  [ init_bram_property $out_mem $out_mem_width $out_mem_depth ]
  # If module has more than 1 mem port, the naming convention changes
  if {$out_part > 1} {
    set porta [get_bd_intf_pins compute_0/out_mem_${i}_V_PORTA]
    set portb [get_bd_intf_pins store_0/out_mem_${i}_V_PORTA]
  } else {
    set porta [get_bd_intf_pins compute_0/out_mem_V_PORTA]
    set portb [get_bd_intf_pins store_0/out_mem_V_PORTA]
  }
  if {$systolic} { set porta [get_bd_intf_pins gemm_sa_0/mem_out] }
  # Create interface connections
  connect_bd_intf_net -intf_net compute_0_out_mem_${i}_V_PORTA \
    [get_bd_intf_pins $out_mem/BRAM_PORTA] \
    $porta
  connect_bd_intf_net -intf_net store_0_out_mem_${i}_V_PORTA \
    [get_bd_intf_pins $out_mem/BRAM_PORTB] \
    $portb
}

# Create instance: processing_system, and set properties
if { $device_family eq "zynq-7000" } {
  set processing_system [ create_bd_cell -type ip -vlnv xilinx.com:ip:processing_system7:5.5 processing_system ]
  set_property -dict [ list \
    CONFIG.PCW_EN_CLK0_PORT {1} \
    CONFIG.PCW_FPGA0_PERIPHERAL_FREQMHZ {100} \
    CONFIG.PCW_USE_DEFAULT_ACP_USER_VAL {1} \
    CONFIG.PCW_USE_S_AXI_ACP {1} \
    CONFIG.preset {ZC702} \
  ] $processing_system
  # Get ports that are specific to the Zynq 7000 processing system
  set ps_clk    [get_bd_pins processing_system/FCLK_CLK0]
  set ps_rstn   [get_bd_pins processing_system/FCLK_RESET0_N]
  set maxi_clk  [get_bd_pins processing_system/M_AXI_GP0_ACLK]
  set saxi_clk  [get_bd_pins processing_system/S_AXI_ACP_ACLK]
  set maxi      [get_bd_intf_pins processing_system/M_AXI_GP0]
  set saxi      [get_bd_intf_pins processing_system/S_AXI_ACP]
#### JO added: ZCU102 EVTA: non-coherent S_AXI_HP0_FPD (= S_AXI_GP2), 128 bit; vtalib flushes/invalidates
} elseif { $target eq "zcu102" } {
  set processing_system [ create_bd_cell -type ip -vlnv xilinx.com:ip:zynq_ultra_ps_e:3.3 processing_system ]
  apply_bd_automation -rule xilinx.com:bd_rule:zynq_ultra_ps_e -config {apply_board_preset "1" }  [get_bd_cells processing_system]
  set_property -dict [ list \
    CONFIG.PSU__FPGA_PL0_ENABLE {1} \
    CONFIG.PSU__CRL_APB__PL0_REF_CTRL__FREQMHZ {100} \
    CONFIG.PSU__USE__M_AXI_GP0 {1} \
    CONFIG.PSU__USE__M_AXI_GP1 {0} \
    CONFIG.PSU__USE__M_AXI_GP2 {0} \
    CONFIG.PSU__USE__S_AXI_GP0 {0} \
    CONFIG.PSU__USE__S_AXI_GP2 {1} \
    CONFIG.PSU__SAXIGP2__DATA_WIDTH {128}
  ] $processing_system
  set ps_clk    [get_bd_pins processing_system/pl_clk0]
  set ps_rstn   [get_bd_pins processing_system/pl_resetn0]
  set maxi_clk  [get_bd_pins processing_system/maxihpm0_fpd_aclk]
  set saxi_clk  [get_bd_pins processing_system/saxihp0_fpd_aclk]
  set maxi      [get_bd_intf_pins processing_system/M_AXI_HPM0_FPD]
  set saxi      [get_bd_intf_pins processing_system/S_AXI_HP0_FPD]
} elseif { $device_family eq "zynq-ultrascale+" } {
  set processing_system [ create_bd_cell -type ip -vlnv xilinx.com:ip:zynq_ultra_ps_e:3.3 processing_system ]
  apply_bd_automation -rule xilinx.com:bd_rule:zynq_ultra_ps_e -config {apply_board_preset "1" }  [get_bd_cells processing_system]
  set_property -dict [ list \
    CONFIG.PSU__FPGA_PL0_ENABLE {1} \
    CONFIG.PSU__CRL_APB__PL0_REF_CTRL__FREQMHZ {100} \
    CONFIG.PSU__USE__M_AXI_GP0 {1} \
    CONFIG.PSU__USE__M_AXI_GP1 {0} \
    CONFIG.PSU__USE__M_AXI_GP2 {0} \
    CONFIG.PSU__USE__S_AXI_GP0 {1}
  ] $processing_system
  # Get ports that are specific to the Zynq Ultrascale MPSoC processing system
  set ps_clk    [get_bd_pins processing_system/pl_clk0]
  set ps_rstn   [get_bd_pins processing_system/pl_resetn0]
  set maxi_clk  [get_bd_pins processing_system/maxihpm0_fpd_aclk]
  set saxi_clk  [get_bd_pins processing_system/saxihpc0_fpd_aclk]
  set maxi      [get_bd_intf_pins processing_system/M_AXI_HPM0_FPD]
  set saxi      [get_bd_intf_pins processing_system/S_AXI_HPC0_FPD]
}

# Create interface connections
connect_bd_intf_net -intf_net axi_xbar_M00_AXI [get_bd_intf_pins axi_xbar/M00_AXI] [get_bd_intf_pins fetch_0/s_axi_CONTROL_BUS]
connect_bd_intf_net -intf_net axi_xbar_M01_AXI [get_bd_intf_pins axi_xbar/M01_AXI] [get_bd_intf_pins load_0/s_axi_CONTROL_BUS]
connect_bd_intf_net -intf_net axi_xbar_M02_AXI [get_bd_intf_pins axi_xbar/M02_AXI] [get_bd_intf_pins compute_0/s_axi_CONTROL_BUS]
connect_bd_intf_net -intf_net axi_xbar_M03_AXI [get_bd_intf_pins axi_xbar/M03_AXI] [get_bd_intf_pins store_0/s_axi_CONTROL_BUS]
connect_bd_intf_net -intf_net fetch_0_l2g_dep_queue_V [get_bd_intf_pins l2g_queue/S_AXIS] [get_bd_intf_pins load_0/l2g_dep_queue_V]
connect_bd_intf_net -intf_net fetch_0_load_queue_V_V [get_bd_intf_pins fetch_0/load_queue_V_V] [get_bd_intf_pins load_queue/S_AXIS]
connect_bd_intf_net -intf_net fetch_0_gemm_queue_V_V [get_bd_intf_pins fetch_0/gemm_queue_V_V] [get_bd_intf_pins gemm_queue/S_AXIS]
connect_bd_intf_net -intf_net fetch_0_store_queue_V_V [get_bd_intf_pins fetch_0/store_queue_V_V] [get_bd_intf_pins store_queue/S_AXIS]
connect_bd_intf_net -intf_net compute_0_g2l_dep_queue_V [get_bd_intf_pins compute_0/g2l_dep_queue_V] [get_bd_intf_pins g2l_queue/S_AXIS]
connect_bd_intf_net -intf_net compute_0_g2s_dep_queue_V [get_bd_intf_pins compute_0/g2s_dep_queue_V] [get_bd_intf_pins g2s_queue/S_AXIS]
connect_bd_intf_net -intf_net store_0_s2g_dep_queue_V [get_bd_intf_pins s2g_queue/S_AXIS] [get_bd_intf_pins store_0/s2g_dep_queue_V]
connect_bd_intf_net -intf_net load_queue_M_AXIS [get_bd_intf_pins load_0/load_queue_V_V] [get_bd_intf_pins load_queue/M_AXIS]
connect_bd_intf_net -intf_net gemm_queue_M_AXIS [get_bd_intf_pins compute_0/gemm_queue_V_V] [get_bd_intf_pins gemm_queue/M_AXIS]
connect_bd_intf_net -intf_net store_queue_M_AXIS [get_bd_intf_pins store_0/store_queue_V_V] [get_bd_intf_pins store_queue/M_AXIS]
connect_bd_intf_net -intf_net l2g_queue_M_AXIS [get_bd_intf_pins compute_0/l2g_dep_queue_V] [get_bd_intf_pins l2g_queue/M_AXIS]
connect_bd_intf_net -intf_net g2l_queue_M_AXIS [get_bd_intf_pins g2l_queue/M_AXIS] [get_bd_intf_pins load_0/g2l_dep_queue_V]
connect_bd_intf_net -intf_net g2s_queue_M_AXIS [get_bd_intf_pins g2s_queue/M_AXIS] [get_bd_intf_pins store_0/g2s_dep_queue_V]
connect_bd_intf_net -intf_net s2g_queue_M_AXIS [get_bd_intf_pins compute_0/s2g_dep_queue_V] [get_bd_intf_pins s2g_queue/M_AXIS]
connect_bd_intf_net -intf_net fetch_0_m_axi_ins_port [get_bd_intf_pins axi_smc0/S00_AXI] [get_bd_intf_pins fetch_0/m_axi_ins_port]
connect_bd_intf_net -intf_net load_0_m_axi_data_port [get_bd_intf_pins axi_smc0/S01_AXI] [get_bd_intf_pins load_0/m_axi_data_port]
connect_bd_intf_net -intf_net compute_0_m_axi_uop_port [get_bd_intf_pins axi_smc0/S02_AXI] [get_bd_intf_pins compute_0/m_axi_uop_port]
connect_bd_intf_net -intf_net compute_0_m_axi_data_port [get_bd_intf_pins axi_smc0/S03_AXI] [get_bd_intf_pins compute_0/m_axi_data_port]
connect_bd_intf_net -intf_net store_0_m_axi_data_port [get_bd_intf_pins axi_smc0/S04_AXI] [get_bd_intf_pins store_0/m_axi_data_port]
connect_bd_intf_net -intf_net axi_smc0_M00_AXI [get_bd_intf_pins axi_smc0/M00_AXI] $saxi
connect_bd_intf_net -intf_net processing_system_m_axi [get_bd_intf_pins axi_xbar/S00_AXI] $maxi

# Create port connections
#### JO changed (2026-10-23): pll_clk/resetn removed from this net (see pll_clk above)
connect_bd_net -net processing_system_reset \
  [get_bd_pins proc_sys_reset/ext_reset_in] \
  $ps_rstn
connect_bd_net -net ps_clk_net \
  [get_bd_pins pll_clk/clk_in1] \
  $ps_clk
#### JO added
connect_bd_net -net pll_clk_locked \
  [get_bd_pins pll_clk/locked] \
  [get_bd_pins proc_sys_reset/dcm_locked]
connect_bd_net -net proc_sys_reset_interconnect_aresetn \
  [get_bd_pins axi_xbar/ARESETN] \
  [get_bd_pins proc_sys_reset/interconnect_aresetn]
connect_bd_net -net proc_sys_reset_peripheral_aresetn \
  [get_bd_pins proc_sys_reset/peripheral_aresetn] \
  [get_bd_pins axi_smc0/aresetn] \
  [get_bd_pins axi_xbar/M00_ARESETN] \
  [get_bd_pins axi_xbar/M01_ARESETN] \
  [get_bd_pins axi_xbar/M02_ARESETN] \
  [get_bd_pins axi_xbar/M03_ARESETN] \
  [get_bd_pins fetch_0/ap_rst_n] \
  [get_bd_pins load_0/ap_rst_n] \
  [get_bd_pins store_0/ap_rst_n] \
  [get_bd_pins compute_0/ap_rst_n] \
  [get_bd_pins load_queue/s_aresetn] \
  [get_bd_pins gemm_queue/s_aresetn] \
  [get_bd_pins store_queue/s_aresetn] \
  [get_bd_pins l2g_queue/s_aresetn] \
  [get_bd_pins g2l_queue/s_aresetn] \
  [get_bd_pins g2s_queue/s_aresetn] \
  [get_bd_pins s2g_queue/s_aresetn]
connect_bd_net -net processing_system_clk \
  [get_bd_pins pll_clk/clk_out1] \
  [get_bd_pins proc_sys_reset/slowest_sync_clk] \
  [get_bd_pins axi_smc0/aclk] \
  [get_bd_pins axi_xbar/ACLK] \
  [get_bd_pins axi_xbar/M00_ACLK] \
  [get_bd_pins axi_xbar/M01_ACLK] \
  [get_bd_pins axi_xbar/M02_ACLK] \
  [get_bd_pins axi_xbar/M03_ACLK] \
  [get_bd_pins fetch_0/ap_clk] \
  [get_bd_pins load_0/ap_clk] \
  [get_bd_pins compute_0/ap_clk] \
  [get_bd_pins store_0/ap_clk] \
  [get_bd_pins load_queue/s_aclk] \
  [get_bd_pins gemm_queue/s_aclk] \
  [get_bd_pins store_queue/s_aclk] \
  [get_bd_pins l2g_queue/s_aclk] \
  [get_bd_pins g2l_queue/s_aclk] \
  [get_bd_pins g2s_queue/s_aclk] \
  [get_bd_pins s2g_queue/s_aclk] \
  $saxi_clk
#### JO added (systolic Phase D): gemm_sa_0 on the EVTA clock and the peripheral reset, like compute_0
if {$systolic} {
  connect_bd_net -net processing_system_clk [get_bd_pins gemm_sa_0/clk]
  connect_bd_net -net proc_sys_reset_peripheral_aresetn [get_bd_pins gemm_sa_0/rstn]
}
#### JO changed: the PS register port (M_AXI_HPM0) and the crossbar's slave side run on the PS clock
#### (pl_clk0, 100 MHz); at 333 MHz the paths into PS8 MAXIGP0* missed timing (-0.039 ns). The crossbar
#### inserts the clock converter; register accesses are rare, so the slower clock costs nothing.
#  [get_bd_pins axi_xbar/S00_ACLK] \
#  $maxi_clk \
#  [get_bd_pins axi_xbar/S00_ARESETN] (was on proc_sys_reset/peripheral_aresetn)
set proc_sys_reset_ps \
  [ create_bd_cell -type ip -vlnv xilinx.com:ip:proc_sys_reset:5.0 proc_sys_reset_ps ]
connect_bd_net -net ps_clk_net [get_bd_pins proc_sys_reset_ps/slowest_sync_clk] \
  [get_bd_pins axi_xbar/S00_ACLK] $maxi_clk
connect_bd_net -net processing_system_reset [get_bd_pins proc_sys_reset_ps/ext_reset_in]
connect_bd_net -net proc_sys_reset_ps_peripheral_aresetn \
  [get_bd_pins proc_sys_reset_ps/peripheral_aresetn] [get_bd_pins axi_xbar/S00_ARESETN]

#### JO added: AXI perf monitor (profile mode) for vtalib's PROFILE_ENABLE + PROFILE_AXI_MONITOR build.
#### Profile-mode counters per slot: +0x00 wr bytes, +0x10 wr transactions, +0x20 wr latency,
#### +0x30 rd bytes, +0x40 rd transactions, +0x50 rd latency; slots at 0x100 / 0x160 / 0x500, control 0x300.
#### slot 0 = all EVTA DRAM traffic (smartconnect -> HP0), slot 1 = load data port (inp/wgt reads),
#### slot 2 = store data port (out writes) -- what pynq_driver.cc reports as total / read / write.
if {$profile} {
  set axi_perf_mon [ create_bd_cell -type ip -vlnv xilinx.com:ip:axi_perf_mon:5.0 axi_perf_mon ]
  set_property -dict [ list \
    CONFIG.C_ENABLE_PROFILE {1} \
    CONFIG.C_ENABLE_ADVANCED {0} \
    CONFIG.C_ENABLE_TRACE {0} \
    CONFIG.C_NUM_MONITOR_SLOTS {3} \
  ] $axi_perf_mon
  connect_bd_intf_net -intf_net axi_xbar_M04_AXI [get_bd_intf_pins axi_xbar/M04_AXI] [get_bd_intf_pins axi_perf_mon/S_AXI]
  connect_bd_intf_net -intf_net [get_bd_intf_nets axi_smc0_M00_AXI] [get_bd_intf_pins axi_perf_mon/SLOT_0_AXI] [get_bd_intf_pins axi_smc0/M00_AXI]
  connect_bd_intf_net -intf_net [get_bd_intf_nets load_0_m_axi_data_port] [get_bd_intf_pins axi_perf_mon/SLOT_1_AXI] [get_bd_intf_pins load_0/m_axi_data_port]
  connect_bd_intf_net -intf_net [get_bd_intf_nets store_0_m_axi_data_port] [get_bd_intf_pins axi_perf_mon/SLOT_2_AXI] [get_bd_intf_pins store_0/m_axi_data_port]
  connect_bd_net [get_bd_pins pll_clk/clk_out1] \
    [get_bd_pins axi_xbar/M04_ACLK] \
    [get_bd_pins axi_perf_mon/s_axi_aclk] \
    [get_bd_pins axi_perf_mon/core_aclk] \
    [get_bd_pins axi_perf_mon/slot_0_axi_aclk] \
    [get_bd_pins axi_perf_mon/slot_1_axi_aclk] \
    [get_bd_pins axi_perf_mon/slot_2_axi_aclk]
  connect_bd_net [get_bd_pins proc_sys_reset/peripheral_aresetn] \
    [get_bd_pins axi_xbar/M04_ARESETN] \
    [get_bd_pins axi_perf_mon/s_axi_aresetn] \
    [get_bd_pins axi_perf_mon/core_aresetn] \
    [get_bd_pins axi_perf_mon/slot_0_axi_aresetn] \
    [get_bd_pins axi_perf_mon/slot_1_axi_aresetn] \
    [get_bd_pins axi_perf_mon/slot_2_axi_aresetn]
}

# Create address segments
create_bd_addr_seg -range $ip_reg_map_range -offset $fetch_base_addr [get_bd_addr_spaces processing_system/Data] [get_bd_addr_segs fetch_0/s_axi_CONTROL_BUS/Reg] SEG_fetch_0_Reg
create_bd_addr_seg -range $ip_reg_map_range -offset $load_base_addr [get_bd_addr_spaces processing_system/Data] [get_bd_addr_segs load_0/s_axi_CONTROL_BUS/Reg] SEG_load_0_Reg
create_bd_addr_seg -range $ip_reg_map_range -offset $compute_base_addr [get_bd_addr_spaces processing_system/Data] [get_bd_addr_segs compute_0/s_axi_CONTROL_BUS/Reg] SEG_compute_0_Reg
create_bd_addr_seg -range $ip_reg_map_range -offset $store_base_addr [get_bd_addr_spaces processing_system/Data] [get_bd_addr_segs store_0/s_axi_CONTROL_BUS/Reg] SEG_store_0_Reg
#### JO added
if {$profile} {
  create_bd_addr_seg -range 0x10000 -offset $apm_base_addr [get_bd_addr_spaces processing_system/Data] [get_bd_addr_segs axi_perf_mon/S_AXI/Reg] SEG_axi_perf_mon_Reg
}
if { $device_family eq "zynq-7000" } {
  create_bd_addr_seg -range 0x40000000 -offset 0x00000000 [get_bd_addr_spaces compute_0/Data_m_axi_uop_port] [get_bd_addr_segs processing_system/S_AXI_ACP/ACP_DDR_LOWOCM] SEG_processing_system_ACP_DDR_LOWOCM
  create_bd_addr_seg -range 0x40000000 -offset 0x00000000 [get_bd_addr_spaces compute_0/Data_m_axi_data_port] [get_bd_addr_segs processing_system/S_AXI_ACP/ACP_DDR_LOWOCM] SEG_processing_system_ACP_DDR_LOWOCM
  create_bd_addr_seg -range 0x40000000 -offset 0x00000000 [get_bd_addr_spaces fetch_0/Data_m_axi_ins_port] [get_bd_addr_segs processing_system/S_AXI_ACP/ACP_DDR_LOWOCM] SEG_processing_system_ACP_DDR_LOWOCM
  create_bd_addr_seg -range 0x40000000 -offset 0x00000000 [get_bd_addr_spaces load_0/Data_m_axi_data_port] [get_bd_addr_segs processing_system/S_AXI_ACP/ACP_DDR_LOWOCM] SEG_processing_system_ACP_DDR_LOWOCM
  create_bd_addr_seg -range 0x40000000 -offset 0x00000000 [get_bd_addr_spaces store_0/Data_m_axi_data_port] [get_bd_addr_segs processing_system/S_AXI_ACP/ACP_DDR_LOWOCM] SEG_processing_system_ACP_DDR_LOWOCM
#### JO added: low 2 GB of DDR through HP0
} elseif { $target eq "zcu102" } {
  foreach sp {fetch_0/Data_m_axi_ins_port load_0/Data_m_axi_data_port compute_0/Data_m_axi_uop_port \
              compute_0/Data_m_axi_data_port store_0/Data_m_axi_data_port} {
    create_bd_addr_seg -range 0x80000000 -offset 0x00000000 [get_bd_addr_spaces $sp] [get_bd_addr_segs processing_system/SAXIGP2/HP0_DDR_LOW] SEG_processing_system_HP0_DDR_LOW
  }
} elseif { $device_family eq "zynq-ultrascale+"} {
  create_bd_addr_seg -range 0x80000000 -offset 0x00000000 [get_bd_addr_spaces fetch_0/Data_m_axi_ins_port] [get_bd_addr_segs processing_system/SAXIGP0/HPC0_DDR_LOW] SEG_processing_system_HPC0_DDR_LOW
  create_bd_addr_seg -range 0x80000000 -offset 0x00000000 [get_bd_addr_spaces load_0/Data_m_axi_data_port] [get_bd_addr_segs processing_system/SAXIGP0/HPC0_DDR_LOW] SEG_processing_system_HPC0_DDR_LOW
  create_bd_addr_seg -range 0x80000000 -offset 0x00000000 [get_bd_addr_spaces compute_0/Data_m_axi_uop_port] [get_bd_addr_segs processing_system/SAXIGP0/HPC0_DDR_LOW] SEG_processing_system_HPC0_DDR_LOW
  create_bd_addr_seg -range 0x80000000 -offset 0x00000000 [get_bd_addr_spaces compute_0/Data_m_axi_data_port] [get_bd_addr_segs processing_system/SAXIGP0/HPC0_DDR_LOW] SEG_processing_system_HPC0_DDR_LOW
  create_bd_addr_seg -range 0x80000000 -offset 0x00000000 [get_bd_addr_spaces store_0/Data_m_axi_data_port] [get_bd_addr_segs processing_system/SAXIGP0/HPC0_DDR_LOW] SEG_processing_system_HPC0_DDR_LOW
}

#### JO added: fail early on connection / address errors
validate_bd_design
save_bd_design


##################################################################
# COMPILATION FLOW
##################################################################

# Create top-level wrapper file
make_wrapper -files \
  [get_files $proj_path/$proj_name.srcs/sources_1/bd/$proj_name/$proj_name.bd] -top
add_files -norecurse $proj_path/$proj_name.srcs/sources_1/bd/$proj_name/hdl/${proj_name}_wrapper.v
#### JO added (systolic Phase D): with the gemm_sa RTL files in sources_1 the automatic top would be gemm_sa_bd
set_property top ${proj_name}_wrapper [current_fileset]
update_compile_order -fileset sources_1
update_compile_order -fileset sim_1

# Run bistream generation on 8 threads with performance oriented P&R strategy
#### JO changed: thread count from the argument list; the default implementation strategy is used
#### (the comment above never matched the code)
#set num_threads 8
#### JO added (2026-10-23): keep the HLS blocks and the queues as they are in implementation. Without this
#### opt_design/phys_opt pulled FIFO EMPTY logic and reset replicas across the IP boundaries, and the
#### result differed per run (board: compute stuck at the first GEMM, _ila6); with it the run completes (_ila7).
set dt_xdc $proj_path/$proj_name.srcs/constrs_1/new/dont_touch.xdc
file mkdir [file dirname $dt_xdc]
set f [open $dt_xdc w]
puts $f "set_property DONT_TOUCH true \[get_cells {vta_i/fetch_0 vta_i/load_0 vta_i/compute_0 vta_i/store_0 \
vta_i/load_queue vta_i/gemm_queue vta_i/store_queue vta_i/l2g_queue vta_i/g2l_queue vta_i/g2s_queue vta_i/s2g_queue}\]"
close $f
add_files -fileset constrs_1 $dt_xdc
set_property used_in_synthesis false [get_files $dt_xdc]
#### JO added (2026-10-23): fence for the lab ZCU102 (192.168.20.2), environment VTA_BRAM_FENCE (zcu102 only):
####   y219 (default): its PL has one defective configuration row (JTAG readback, hwtest/brambist/cfgdiff.py):
####          clock-region row 3, frame words 61/62 = tile row SLICE Y219 / RAMB36 Y43 / DSP Y86-87, 8 cells per
####          frame stuck at 0, so whatever is configured in that row (LUT, FF, routing, BRAM settings) may lose bits.
####          Keep the PL design (all but the PS) in a pblock below that row with CONTAIN_ROUTING and the MMCM out of
####          rows 3..6. Board 2026-10-24: wgt BRAMs at RAMB36 Y36..42, sramscan clean, all 35 bundles pass.
####   upper: the first workaround, PROHIBIT every BRAM from RAMB36 Y36 up (the "upper BRAMs return wrong data"
####          finding that the readback later traced to row Y219)
####   none:  no fence (other boards)
set bram_fence [expr {[info exists ::env(VTA_BRAM_FENCE)] ? $::env(VTA_BRAM_FENCE) : "y219"}]
if {$target eq "zcu102" && $bram_fence eq "y219"} {
  set pb_xdc $proj_path/$proj_name.srcs/constrs_1/new/fence_y219.xdc
  set f [open $pb_xdc w]
  puts $f {create_pblock pb_below_y219}
  puts $f {add_cells_to_pblock [get_pblocks pb_below_y219] [get_cells -filter {NAME !~ vta_i/processing_system && NAME !~ vta_i/pll_clk} vta_i/*]}
  puts $f {resize_pblock [get_pblocks pb_below_y219] -add {SLICE_X0Y0:SLICE_X96Y218 RAMB36_X0Y0:RAMB36_X12Y42 RAMB18_X0Y0:RAMB18_X12Y85 DSP48E2_X0Y0:DSP48E2_X17Y85}}
  puts $f {set_property CONTAIN_ROUTING true [get_pblocks pb_below_y219]}
  puts $f {set_property PROHIBIT true [get_sites -regexp {MMCM_X\d+Y[3-6]}]}
  #### the placer still put a cascaded BRAM pair across the pblock edge (ramloop[5] Y42, ramloop[6] Y43): prohibit the row
  puts $f {set_property PROHIBIT true [get_sites -regexp {RAMB36_X\d+Y43|RAMB18_X\d+Y8[67]|DSP48E2_X\d+Y8[67]}]}
  close $f
  add_files -fileset constrs_1 $pb_xdc
  set_property used_in_synthesis false [get_files $pb_xdc]
} elseif {$target eq "zcu102" && $bram_fence eq "upper"} {
  set pb_xdc $proj_path/$proj_name.srcs/constrs_1/new/prohibit_upper_bram.xdc
  set f [open $pb_xdc w]
  puts $f {set_property PROHIBIT true [get_sites -regexp {RAMB36_X\d+Y(3[6-9]|[4-8]\d)}]}
  puts $f {set_property PROHIBIT true [get_sites -regexp {RAMB18_X\d+Y(7[2-9]|[89]\d|1[0-6]\d)}]}
  close $f
  add_files -fileset constrs_1 $pb_xdc
  set_property used_in_synthesis false [get_files $pb_xdc]
} elseif {$target eq "zcu102" && $bram_fence ne "none"} {
  puts "VTA_BRAM_FENCE must be y219, upper or none"
  return 1
}
launch_runs impl_1 -to_step write_bitstream -jobs $num_threads
wait_on_run impl_1

# Export hardware description file and bitstream files to export/ dir
#### JO changed: export/<export name>.bit (the Makefile's target, was vta.bit) plus .hwh, .xsa and
#### utilization / timing reports (check WNS >= 0 in <export name>.timing.rpt before using the bitstream)
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
