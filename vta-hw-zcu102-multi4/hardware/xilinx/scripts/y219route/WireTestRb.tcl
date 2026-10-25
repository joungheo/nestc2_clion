# #### JO added (2026-10-25): program the PL over JTAG with each Y219 wire test bitstream and read it back with
# capture (FF states) into <bit>.rb.bin, for wiretest.py. No PS access, so a bad wire cannot hang the board.
# usage: vivado -mode batch -source WireTestRb.tcl -tclargs <a.bit> [<b.bit> ...]
open_hw_manager
connect_hw_server -url 172.17.0.1:3121
open_hw_target
set dev [lindex [get_hw_devices xczu9*] 0]
current_hw_device $dev
foreach bit $argv {
  refresh_hw_device -update_hw_probes false $dev
  set_property PROGRAM.FILE [file normalize $bit] $dev
  set_property PROBES.FILE {} $dev
  program_hw_devices $dev
  refresh_hw_device -update_hw_probes false $dev
  puts "CHK $bit DONE pin: [get_property REGISTER.CONFIG_STATUS.BIT14_DONE_PIN $dev]"
  after 500
  readback_hw_device -force -capture -bin_file [file rootname $bit].rb.bin $dev
}
close_hw_target
disconnect_hw_server
