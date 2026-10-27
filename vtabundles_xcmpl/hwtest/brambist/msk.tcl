# #### JO added (2026-10-27, copy of vta-hw-zcu102-multi4/build/y219/msk.tcl): write the readback mask of a routed checkpoint
# and a matching .bit (same configuration as a write_bitstream of the DCP), for cfgdiff.py <out>.bit <out>.msk <readback.bin>.
# usage: vivado -mode batch -source msk.tcl -tclargs <routed.dcp> <out>   -> <out>.bit, <out>.msk
open_checkpoint [lindex $argv 0]
write_bitstream -force -mask_file [lindex $argv 1].bit
