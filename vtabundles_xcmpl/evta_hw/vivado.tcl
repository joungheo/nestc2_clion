# #### JO added (2026-10-17): block design + bitstream with NUM_EVTA EVTA cores on the ZCU102 (Vivado 2020.1).
# Based on tvm/3rdparty/vta-hw/hardware/xilinx/scripts/vivado.tcl (single VTA, coherent HPC0, 128-bit insn FIFOs).
# Changes:
#   - NUM_EVTA copies of fetch/load/compute/store + queues + SRAMs (proc add_evta), EVTA i:
#       control registers at FETCH/LOAD/COMPUTE/STORE_BASE_ADDR + i * EVTA_ADDR_STRIDE (vtalib VTA_*_ADDR_<i>),
#       its 5 AXI masters -> SmartConnect smc_<i> -> PS S_AXI_HP<i>_FPD (non-coherent, 128 bit; vtalib flushes)
#   - command-queue FIFOs INSN_BYTES wide (32 bytes = the 256-bit EVTA instructions; vta-hw had 16)
#   - also writes export/<name>.xsa (address map / hardware handoff) next to the .bit
# usage (in the Vivado build dir):
#   vivado -mode batch -source vivado.tcl -tclargs <hls ip dir> <config.tcl> [jobs]

set scripts_vivado_version 2020.1
if { [string first $scripts_vivado_version [version -short]] == -1 } {
  puts "WARNING: written for Vivado $scripts_vivado_version, running [version -short]"
}
if { [llength $argv] < 2 } {
  puts "usage: vivado -mode batch -source vivado.tcl -tclargs <hls ip dir> <config.tcl> \[jobs\]"
  return 1
}
set ip_path    [lindex $argv 0]
set vta_config [lindex $argv 1]
set num_jobs   [expr {[llength $argv] > 2 ? [lindex $argv 2] : 8}]
source $vta_config

set name "${CFG_NAME}_${NUM_EVTA}evta"
set proj_name vta
set proj_path "."
create_project -force $proj_name $proj_path -part $FPGA_DEVICE
if {$FPGA_BOARD != "None" && $FPGA_BOARD_REV != "None"} {
  set_property board_part $FPGA_BOARD:$FPGA_BOARD_REV [current_project]
}
file mkdir ip_lib
set_property ip_repo_paths ip_lib [current_project]
update_ip_catalog
foreach stage {fetch load compute store} {
  update_ip_catalog -add_ip "$ip_path/vta_$stage/soln/impl/ip/xilinx_com_hls_${stage}_1_0.zip" -repo_path ip_lib
}
create_bd_design $proj_name
current_bd_design $proj_name

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
set ps [ create_bd_cell -type ip -vlnv xilinx.com:ip:zynq_ultra_ps_e:3.3 processing_system ]
apply_bd_automation -rule xilinx.com:bd_rule:zynq_ultra_ps_e -config {apply_board_preset "1" } $ps
set ps_cfg [ list \
  CONFIG.PSU__FPGA_PL0_ENABLE {1} \
  CONFIG.PSU__CRL_APB__PL0_REF_CTRL__FREQMHZ {100} \
  CONFIG.PSU__USE__M_AXI_GP0 {1} \
  CONFIG.PSU__USE__M_AXI_GP1 {0} \
  CONFIG.PSU__USE__M_AXI_GP2 {0} \
  CONFIG.PSU__USE__S_AXI_GP0 {0} ]
for {set i 0} {$i < $NUM_EVTA} {incr i} {
  # S_AXI_GP2..5 = S_AXI_HP0..3_FPD
  lappend ps_cfg CONFIG.PSU__USE__S_AXI_GP[expr {2 + $i}] {1} CONFIG.PSU__SAXIGP[expr {2 + $i}]__DATA_WIDTH {128}
}
set_property -dict $ps_cfg $ps

set proc_sys_reset [ create_bd_cell -type ip -vlnv xilinx.com:ip:proc_sys_reset:5.0 proc_sys_reset ]
set pll_clk [ create_bd_cell -type ip -vlnv xilinx.com:ip:clk_wiz:6.0 pll_clk ]
set_property -dict [ list \
  CONFIG.CLKOUT1_REQUESTED_OUT_FREQ $FPGA_FREQ \
  CONFIG.RESET_PORT {resetn} \
  CONFIG.RESET_TYPE {ACTIVE_LOW} \
  CONFIG.USE_LOCKED {false} \
] $pll_clk
set n_ctrl [expr {4 * $NUM_EVTA}]
set axi_xbar [ create_bd_cell -type ip -vlnv xilinx.com:ip:axi_interconnect:2.1 axi_xbar ]
set_property -dict [ list CONFIG.NUM_MI $n_ctrl CONFIG.NUM_SI {1} ] $axi_xbar

connect_bd_intf_net [get_bd_intf_pins axi_xbar/S00_AXI] [get_bd_intf_pins processing_system/M_AXI_HPM0_FPD]
connect_bd_net -net ps_rstn [get_bd_pins processing_system/pl_resetn0] [get_bd_pins pll_clk/resetn] \
  [get_bd_pins proc_sys_reset/ext_reset_in]
connect_bd_net -net ps_clk [get_bd_pins processing_system/pl_clk0] [get_bd_pins pll_clk/clk_in1]
connect_bd_net -net evta_clk [get_bd_pins pll_clk/clk_out1] [get_bd_pins proc_sys_reset/slowest_sync_clk] \
  [get_bd_pins axi_xbar/ACLK] [get_bd_pins axi_xbar/S00_ACLK] [get_bd_pins processing_system/maxihpm0_fpd_aclk]
connect_bd_net -net evta_ic_rstn [get_bd_pins proc_sys_reset/interconnect_aresetn] [get_bd_pins axi_xbar/ARESETN]
connect_bd_net -net evta_rstn [get_bd_pins proc_sys_reset/peripheral_aresetn] [get_bd_pins axi_xbar/S00_ARESETN]

# ---- one EVTA core: instance suffix i, crossbar ports 4i..4i+3, PS HP port i
proc add_evta {i} {
  global AXI_CACHE_BITS AXI_PROT_BITS INSN_BYTES INP_MEM_BANKS INP_MEM_WIDTH INP_MEM_DEPTH \
         WGT_MEM_BANKS WGT_MEM_WIDTH WGT_MEM_DEPTH OUT_MEM_BANKS OUT_MEM_WIDTH OUT_MEM_DEPTH \
         IP_REG_MAP_RANGE FETCH_BASE_ADDR LOAD_BASE_ADDR COMPUTE_BASE_ADDR STORE_BASE_ADDR EVTA_ADDR_STRIDE
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

  # command queues carry whole instructions; dependence queues carry 1 bit
  foreach q {load_queue gemm_queue store_queue} {
    init_fifo_property [ create_bd_cell -type ip -vlnv xilinx.com:ip:fifo_generator:13.2 ${q}_$i ] $INSN_BYTES 512
  }
  foreach q {l2g_queue g2l_queue g2s_queue s2g_queue} {
    init_fifo_property [ create_bd_cell -type ip -vlnv xilinx.com:ip:fifo_generator:13.2 ${q}_$i ] 1 1024
  }
  foreach {fifo_in src_pin fifo_out dst_pin} [list \
      load_queue  fetch_$i/load_queue_V_V      load_queue  load_$i/load_queue_V_V \
      gemm_queue  fetch_$i/gemm_queue_V_V      gemm_queue  compute_$i/gemm_queue_V_V \
      store_queue fetch_$i/store_queue_V_V     store_queue store_$i/store_queue_V_V \
      l2g_queue   load_$i/l2g_dep_queue_V      l2g_queue   compute_$i/l2g_dep_queue_V \
      g2l_queue   compute_$i/g2l_dep_queue_V   g2l_queue   load_$i/g2l_dep_queue_V \
      g2s_queue   compute_$i/g2s_dep_queue_V   g2s_queue   store_$i/g2s_dep_queue_V \
      s2g_queue   store_$i/s2g_dep_queue_V     s2g_queue   compute_$i/s2g_dep_queue_V ] {
    connect_bd_intf_net [get_bd_intf_pins $src_pin] [get_bd_intf_pins ${fifo_in}_$i/S_AXIS]
    connect_bd_intf_net [get_bd_intf_pins ${fifo_out}_$i/M_AXIS] [get_bd_intf_pins $dst_pin]
  }

  # SRAMs between the stages (pin names as vta-hw: <mem>_V_PORTA, or <mem>_<k>_V_PORTA when partitioned)
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
    connect_bd_intf_net [get_bd_intf_pins $m] [get_bd_intf_pins smc_$i/S0${k}_AXI]
    incr k
  }
  connect_bd_intf_net [get_bd_intf_pins smc_$i/M00_AXI] [get_bd_intf_pins processing_system/S_AXI_HP${i}_FPD]

  # control: crossbar M(4i+0..3) -> fetch/load/compute/store
  set k 0
  foreach s [list fetch_$i load_$i compute_$i store_$i] {
    set m [format "M%02d" [expr {4 * $i + $k}]]
    connect_bd_intf_net [get_bd_intf_pins axi_xbar/${m}_AXI] [get_bd_intf_pins $s/s_axi_CONTROL_BUS]
    connect_bd_net -net evta_clk [get_bd_pins axi_xbar/${m}_ACLK]
    connect_bd_net -net evta_rstn [get_bd_pins axi_xbar/${m}_ARESETN]
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
  set k 0
  foreach {s base} [list fetch_$i $FETCH_BASE_ADDR load_$i $LOAD_BASE_ADDR \
                         compute_$i $COMPUTE_BASE_ADDR store_$i $STORE_BASE_ADDR] {
    set addr [format "0x%08X" [expr {$base + $i * $EVTA_ADDR_STRIDE}]]
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

for {set i 0} {$i < $NUM_EVTA} {incr i} {
  add_evta $i
}

validate_bd_design
save_bd_design
make_wrapper -files [get_files $proj_path/$proj_name.srcs/sources_1/bd/$proj_name/$proj_name.bd] -top
add_files -norecurse $proj_path/$proj_name.srcs/sources_1/bd/$proj_name/hdl/${proj_name}_wrapper.v
update_compile_order -fileset sources_1

launch_runs impl_1 -to_step write_bitstream -jobs $num_jobs
wait_on_run impl_1
open_run impl_1
report_utilization -file export_utilization.rpt
report_timing_summary -file export_timing.rpt
file mkdir $proj_path/export
if {[file exist $proj_path/$proj_name.runs/impl_1/${proj_name}_wrapper.bit]} {
  file copy -force $proj_path/$proj_name.runs/impl_1/${proj_name}_wrapper.bit $proj_path/export/$name.bit
  write_hw_platform -fixed -include_bit -force -file $proj_path/export/$name.xsa
  file copy -force export_utilization.rpt $proj_path/export/$name.utilization.rpt
  file copy -force export_timing.rpt $proj_path/export/$name.timing.rpt
  puts "bitstream: [file normalize $proj_path/export/$name.bit]"
} else {
  puts "ERROR: no bitstream (see $proj_name.runs/impl_1/runme.log)"
}
exit
