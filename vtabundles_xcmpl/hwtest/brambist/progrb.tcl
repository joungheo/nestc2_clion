# Program the PL over JTAG with <bit>, then read the configuration back into <out>.bin.
set bit [file normalize [lindex $argv 0]]
set out [lindex $argv 1]
open_hw_manager
connect_hw_server -url 172.17.0.1:3121
open_hw_target
set dev [lindex [get_hw_devices xczu9*] 0]
current_hw_device $dev
refresh_hw_device -update_hw_probes false $dev
set_property PROGRAM.FILE $bit $dev
set_property PROBES.FILE {} $dev
program_hw_devices $dev
refresh_hw_device -update_hw_probes false $dev
puts "DONE pin: [get_property REGISTER.CONFIG_STATUS.BIT14_DONE_PIN $dev]"
readback_hw_device $dev -bin_file $out.bin
close_hw_target
disconnect_hw_server
