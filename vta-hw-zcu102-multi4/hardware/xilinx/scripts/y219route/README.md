# Routing around tile row Y219 of the lab ZCU102 (multi4)

The lab ZCU102 (192.168.20.2) has a defective PL row at tile row Y219 (log/BOARD_TODO.md 2026-10-23..25):
- 8 configuration cells per frame of clock-region row 3 are stuck at 0 (`hwtest/brambist/stuckchk.py` finds them);
- some routing PIPs inside the row do not work although their cells read back right (wire test, 2026-10-25);
- the wires that only pass over the row (NN/SS 2/4/12 beginning on one side, ending on the other) all work.

**Rule: no PIP in the INT tiles of row Y219.** Designs that fit below the row use the `y219` fence of `vivado.tcl`.
multi4 does not fit and crosses the row on pass-over wires only.

## multi4 flow (work dir `vta-hw-zcu102-multi4/build/y219`, Vivado 2020.1 in docker, RapidWright 2025.2)
1. `make ip bin` in `hardware/xilinx` (fence `split`; its routing is not used), then
   `vivado -source ../../hardware/xilinx/scripts/y219route/flow/place_d.tcl` -> `routed_d.dcp`
   (placement with rows Y218-220 empty, Vivado routing).
2. `Y219Block.java` + `flow/y219block.tcl` -> `blk_f.dcp`, then `write_edif blk_f.edf` (Vivado routing that mostly
   avoids the row; the dummy blockage itself does not hold, Vivado ignores antenna nets).
3. `Y219RWRoute.java -Dcrit=0.95 blk_f.dcp rws_z.dcp` (no `-Dpips`: every PIP in the row banned; the nets that used
   one are rerouted timing-driven, the rest keeps Vivado's routing).
4. `flow/route_s.tcl -tclargs rws_z.dcp rs_z` (Vivado routes what is left; must report 0 nets with any PIP in row
   Y219), then `flow/mmcm_set.tcl -tclargs rs_z.dcp rs_zm 40.25` (MMCM to 280 MHz: rs_z has WNS -0.488 at 333 MHz).
5. `stuckchk.py rs_zm.bit -b <single lowbram bit>`: 0 beyond the base. Board 2026-10-25: all hwtest tests pass on
   EVTA 0-3 (`bit/evta_multi4_y219_261025/rs_zm.bin`).

Since the HP register slices (`hpslice<i>` in `vivado.tcl`, 2026-10-25) steps 1-4 are `flow/run_flow.sh <tag>` (run in
`build/y219`; step 2 is skipped, Y219RWRoute starts from routed_<tag>): rs_hp, WNS -0.199 at 333 MHz (left: the
reset fan-out to compute_3), then `flow/mmcm_set.tcl -tclargs rs_hp.dcp rs_hpm 44.5` (309.6 MHz, WNS +0.019).
stuckchk `-b` the multi2 bit: 0 extra. Board: all hwtest tests and multi_gemm_gemm_4evta_m pass (`rs_hpm.bin`).
With one `proc_sys_reset_<i>` per EVTA inside its pblock (2026-10-26), `run_flow.sh rst` meets 333 MHz with no
MMCM change: rs_rst, WNS +0.008, WHS +0.010, stuckchk 0 extra, all board tests pass
(`bit/evta_multi4_y219_261026/rs_rst.bin` = `/lib/firmware/vtahw_multi4_y219.bin`).

## Tools
- `Y219RWRoute.java` reroute the nets with a PIP in the row (`-Dpips=stuck_int_pips.txt`: only stuck-cell PIPs,
  not enough on this board); `stuckpips.py` makes that list from Project U-Ray `segbits_int.db`.
- `WireTest.java`, `WireTestBit.tcl`, `wiretest_run.sh`, `wiretest.py`: test designs for the wires over the row
  (static); `-Dspeed=1` with `wirespeed_run.sh` / `wirespeed.py` (and `hwtest/plclkset.py`) at speed;
  `-Dinrow=1 -Dpips=stuck_int_pips.txt` for nets crossing on PIPs inside the row. Loaded with fpga_manager, read back
  over JTAG: run `hwtest/nocpuidle.sh` on the board first.
- `Y219Block.java`, `Y219Route.java`, `FloorDump.java`, `floorplan_svg.py`: earlier attempts / floorplan pictures.
