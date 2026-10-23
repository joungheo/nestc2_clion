# Read the PL configuration back over JTAG into <out>.bin (does not program).
set out [lindex $argv 0]
open_hw_manager
connect_hw_server -url 172.17.0.1:3121
open_hw_target
set dev [lindex [get_hw_devices xczu9*] 0]
current_hw_device $dev
refresh_hw_device -update_hw_probes false $dev
readback_hw_device $dev -bin_file $out.bin
close_hw_target
disconnect_hw_server
