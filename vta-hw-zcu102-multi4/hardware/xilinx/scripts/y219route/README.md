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
(`bit/evta_multi4_y219_333MHz_pass_261026/vtahw_zcu102_1x16_i8w8a32_16_15_18_17_4evta.bin` = `/lib/firmware/vtahw_multi4_y219.bin`).

Since 2026-11-04 `flow/m4_build.sh <tag> [make vars]` (on the host) does the whole thing: `make bin IMPL_TO_STEP=opt_design`
(run_flow.sh only reads `impl_1/vta_wrapper_opt.dcp`; impl_1 also stopped if it reaches place_design), then `run_flow.sh m<tag>`
in `build/y219`. Logs `build/m4_<tag>.log`, `build/m4_<tag>_flow.log`; ALL_DONE at the end. The full impl_1 place and route
cost hours for nothing (alu1r 2026-11-03: route still not done after 4 h).
If hold still fails after `route_s.tcl` and `holdnets.tcl` does not move it (the reroute takes the same wires), `flow/holdpin.tcl
-tclargs <in.dcp> <out> <min delay ps> ...` reroutes each failing endpoint with a minimum route delay, the first delay that meets hold
with no PIP in row Y219 wins (alu1r 2026-11-04: WHS -0.032 -> +0.007 at 310 ps; 400 ps took a detour through the row).
If setup is a few ps short after `route_s.tcl`: `route_s.tcl <rs>.dcp <out> opt` (post-route phys_opt), then, if it put a net back into
row Y219, `write_edif <out>.edf` and `Y219RWRoute.java` / `route_s.tcl ... opt2` again; what is left can go to the MMCM (`mmcm_set.tcl <dcp> <out> 11.875`
= 395.8 MHz instead of 12 = 400). rq 2026-11-04: -0.127 -> -0.007 ns (store_queue FIFO36 RDEN), 395.8 MHz +0.004 ns, board pass.
Another placer directive may do better: rq with `VTA_PLACE_DIRECTIVE=ExtraNetDelay_high` +0.002 ns before RapidWright, -0.205 after,
`opt` +0.002 ns with one net back in the row; that net (no timed path) rerouted with `Y219RWRoute.java -Dtd=0` (wirelength-driven: timing-driven
RWRoute 2025.2 throws in printRoutingStatistics when no routed connection is timed), then `route_s.tcl` with no opt: 400 MHz +0.002 ns, board pass.
`run_flow.sh` takes `VTA_PLACE_DIRECTIVE` (placer directive in `place_d.tcl`, default Explore) and `RW_XMX` (RapidWright heap, default 64g).

## Tools
- `Y219RWRoute.java` reroute the nets with a PIP in the row (`-Dpips=stuck_int_pips.txt`: only stuck-cell PIPs,
  not enough on this board); `stuckpips.py` makes that list from Project U-Ray `segbits_int.db`.
- `WireTest.java`, `WireTestBit.tcl`, `wiretest_run.sh`, `wiretest.py`: test designs for the wires over the row
  (static); `-Dspeed=1` with `wirespeed_run.sh` / `wirespeed.py` (and `hwtest/plclkset.py`) at speed;
  `-Dinrow=1 -Dpips=stuck_int_pips.txt` for nets crossing on PIPs inside the row. Loaded with fpga_manager, read back
  over JTAG: run `hwtest/nocpuidle.sh` on the board first.
- `Y219Block.java`, `Y219Route.java`, `FloorDump.java`, `floorplan_svg.py`: earlier attempts / floorplan pictures.
