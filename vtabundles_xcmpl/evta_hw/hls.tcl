# #### JO added (2026-10-17): HLS synthesis of the four EVTA stages for the ZCU102.
# Copy of tvm/3rdparty/vta-hw/hardware/xilinx/scripts/hls.tcl with
#   - the vta-hw tree derived from this script's location (nest-compiler/tvm/3rdparty/vta-hw),
#   - the config tcl from gen_config.py (its CFLAGS put vtalib's EVTA hw_spec.h first),
#   - the C simulation optional (vta-hw's testbench is written for its own 128-bit ISA).
# usage (Vivado HLS 2020.1, in the IP build dir):
#   vivado_hls -f hls.tcl -tclargs <config.tcl> [csim 0|1]

if { [llength $argv] < 3 } {
    puts "usage: vivado_hls -f hls.tcl -tclargs <config.tcl> \[csim 0|1\]"
    exit
}
set vta_config [lindex $argv 2]
set run_csim   [expr {[llength $argv] > 3 ? [lindex $argv 3] : 0}]

set script_dir [file dirname [file normalize [info script]]]
set root_dir   [file normalize "$script_dir/../../tvm/3rdparty/vta-hw"]
set src_dir    "$root_dir/hardware/xilinx/src"
set sim_dir    "$root_dir/hardware/xilinx/sim"
set test_dir   "$root_dir/tests/hardware/common"

source $vta_config
set cflags $CFLAGS
set ::device $FPGA_DEVICE
set ::period $FPGA_PERIOD
set ::inp_reshape_factor    $INP_MEM_AXI_RATIO
set ::inp_partition_factor  $INP_MEM_BANKS
set ::wgt_reshape_factor    $WGT_MEM_AXI_RATIO
set ::wgt_partition_factor  $WGT_MEM_BANKS
set ::out_reshape_factor    $OUT_MEM_AXI_RATIO
set ::out_partition_factor  $OUT_MEM_BANKS

# Same memory reshape/partition directives as vta-hw (buses wider than 1024 bits are not allowed).
proc init_design {} {
    set_part $::device
    create_clock -period $::period -name default
    set_directive_array_reshape -type block -factor $::inp_reshape_factor -dim 2 "load" inp_mem
    set_directive_array_reshape -type block -factor $::inp_reshape_factor -dim 2 "compute" inp_mem
    if {$::inp_partition_factor > 1} {
        set_directive_array_partition -type block -factor $::inp_partition_factor -dim 2 "load" inp_mem
        set_directive_array_partition -type block -factor $::inp_partition_factor -dim 2 "compute" inp_mem
    }
    set_directive_array_reshape -type block -factor $::wgt_reshape_factor -dim 2 "load" wgt_mem
    set_directive_array_reshape -type block -factor $::wgt_reshape_factor -dim 2 "compute" wgt_mem
    if {$::wgt_partition_factor > 1} {
        set_directive_array_partition -type block -factor $::wgt_partition_factor -dim 2 "load" wgt_mem
        set_directive_array_partition -type block -factor $::wgt_partition_factor -dim 2 "compute" wgt_mem
    }
    set_directive_array_reshape -type block -factor $::out_reshape_factor -dim 2 "compute" out_mem
    set_directive_array_reshape -type block -factor $::out_reshape_factor -dim 2 "store" out_mem
    if {$::out_partition_factor > 1} {
        set_directive_array_partition -type block -factor $::out_partition_factor -dim 2 "compute" out_mem
        set_directive_array_partition -type block -factor $::out_partition_factor -dim 2 "store" out_mem
    }
}

if {$run_csim} {
    open_project vta_sim
    set_top vta
    add_files $src_dir/vta.cc -cflags $cflags
    add_files -tb $sim_dir/vta_test.cc -cflags $cflags
    add_files -tb $test_dir/test_lib.cc -cflags $cflags
    open_solution "soln"
    init_design
    csim_design -clean
    close_project
}

foreach stage {fetch load compute store} {
    open_project vta_$stage
    set_top $stage
    add_files $src_dir/vta.cc -cflags $cflags
    open_solution "soln"
    init_design
    csynth_design
    export_design -format ip_catalog
    close_project
}
exit
