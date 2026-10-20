# BOARD_TODO — ZCU102 board tests for the new EVTA bitstreams (2026-10-20)

Hand-off from the host-PC session that synthesized the bitstreams (commits `44b33307`, `0acdc20a`,
`1bd29b78`, `97d2146e`). Background and build flow: `CLAUDE.md` section
"EVTA bitstreams (host PC, Vivado 2020.1 in docker)". Nothing below has been run on the board yet.

## Bitstreams

All built at 333 MHz with `PROFILE=1` (compute counters + AXI perf monitor). The `.bin` files are not
in git (`build/` is ignored); they are copied from the host PC to `/home/xilinx/JO/npuprg/bit/`.

| file | EVTA | buffers (inp/wgt/acc/out log2) | WNS | source / vtalib |
|---|---|---|---|---|
| `vtahw_zcu102_1x16_i8w8a32_16_16_19_18_prof.bin` | 1 | 16/16/19/18 | +0.054 ns | `vta-hw-zcu102-single` / `vtalib-zcu102-single` |
| `vtahw_zcu102_1x16_i8w8a32_16_15_18_17_2evta_prof.bin` | 2 | 16/15/18/17 | +0.004 ns | `vta-hw-zcu102-multi2` / `vtalib-zcu102-multi2` |
| `vtahw_zcu102_1x16_i8w8a32_16_15_18_17_4evta_prof.bin` | 4 | 16/15/18/17 | +0.003 ns | `vta-hw-zcu102-multi4` / `vtalib-zcu102-multi4` |

Load: copy the `.bin` to `/lib/firmware/` (the `vtahw_` prefix keeps it apart from the etri
`zcu102_1x16_i8w8a32_16_16_19_18.bin`), then as root
`echo 0 > /sys/class/fpga_manager/fpga0/flags; echo <bin> > /sys/class/fpga_manager/fpga0/firmware`
and `python3 -c "from pynq import Xlnk; Xlnk().xlnk_reset()"`. Reload before every run (CLAUDE.md).
`vtabundles_xcmpl/hw/run_all.sh` has `BIN=` hard-coded (`run_prof.sh` reads it from there);
`hwtest/run_rawtests.sh` takes `BIN=...` from the environment.

## Register map

- EVTA i (fetch/load/compute/store block registers): `0xA0000000 + i*0x10000`; data through `S_AXI_HP<i>_FPD`.
- Compute profiling counters (output-only s_axilite, compute block): `0x50` load, `0x60` gemm, `0x70` alu
  (load = UOP `x_size` / ACC `x_size*y_size`; gemm/alu = `iter_out*iter_in*(uop_end-uop_bgn)`;
  reset after FINISH).
- AXI perf monitor i: `0xA0050000 + i*0x10000` (profile mode, 3 slots).
  Control `0x300` (bit0 enable, bit1 reset; counters are disabled after reset — the driver writes 2 then 1
  before each run). Slots `0x100` = all EVTA traffic to HP, `0x160` = load reads, `0x500` = store writes;
  per slot `+0x00` wr bytes, `+0x10` wr trans, `+0x20` wr latency, `+0x30` rd bytes, `+0x40` rd trans,
  `+0x50` rd latency.

## Steps

1. **single `_prof` sanity**
   - `hwtest/run_rawtests.sh` with `BIN=vtahw_zcu102_1x16_i8w8a32_16_16_19_18_prof.bin`: must match the
     etri bitstream results (same ISA: 256-bit insn, 2-bit `memory_type`/`alu_opcode`).
   - Read `0xA0050300` and the compute `0x50/0x60/0x70` with `hwtest/dump_regs.py` (or `devmem`) after a run.
2. **vtabundles with profiling** (`vtabundles_xcmpl/README.md`):
   `mkdir build_prof_single && cd build_prof_single && cmake -DVTALIB=$PWD/../../vtalib-zcu102-single -DNESTC_EVTA_PROFILE=ON -DNESTC_EVTA_PROFILE_AXI_MONITOR=ON -DBUNDLE_EXE_SUFFIX=_prof .. && make -j2 hw_bundles`,
   set `BIN=` in `hw/run_all.sh` to the `_prof` bin, `sudo hw/run_prof.sh`. Check the golden results still
   PASS and the profile table (read_bytes / read_latency / gemm_count / alu_count / load_count) is non-zero.
   Without `NESTC_EVTA_PROFILE_AXI_MONITOR=ON` the APM is not read; with it on a non-`_prof` bitstream the
   reads SIGBUS.
3. **Is load bandwidth-bound?** From slot1 (load reads): bytes / exe_time vs. ~5.3 GB/s per 128-bit HP port
   at 333 MHz, and read_latency / read_transactions. Widening the PL AXI to 1024 bit does not help (PS HP
   ports are max 128 bit); if load is bound, the next option is splitting load/compute/store over separate HP ports.
4. **multi2**: `vtalib-zcu102-multi2`, `BUNDLE_SET=multievtax2` (README "multievtax2"), `hw_multievtax2/`.
   If EVTA 1 returns zeros, check the HP1 port width: `sudo python3 vtabundles_xcmpl/hwtest/plclk.py`
   (AFIFM i = S_AXI_GP i: HPC0 `0xFD360000`, HPC1 `0xFD370000`, HP0 `0xFD380000`, HP1 `0xFD390000`,
   HP2 `0xFD3A0000`, HP3 `0xFD3B0000`) — set by the boot image, not the bitstream.
5. **multi4**: vtalib tests / rawtest only for now — `vtabundles_xcmpl` has no 4-EVTA `BUNDLE_SET` yet
   (needs one, plus `VTA_MAX_XFER` small enough for 4 command handles in the 128 MB CMA pool;
   the runtime pre-allocates 64 MB per handle).

## Known risks

- multi2/multi4 timing slack is only 0.003–0.004 ns. Flaky or wrong results only on multi → rebuild at
  300 MHz on the host PC (zcu102 `fpga_freq`/`fpga_per` = 333/2 in `config/pkg_config.py` of that tree)
  before debugging further.
- The PS register port `M_AXI_HPM0` runs on `pl_clk0` 100 MHz (EVTA on 333 MHz); register access is
  through a clock converter in the `axi_xbar`.

## Results

### 2026-10-20 board session: single bitstreams — both FAIL, back to the host PC

Logs/dumps on the board in `vtabundles_xcmpl/hwtest/` (`rt_vtahw*.log`, `rt2_vtahw*.log`, `gp_*.log`,
`regs_*_after.txt`; not in git). Probe: `vtabundles_xcmpl/hwtest/gemmprobe.cpp` (`SUFFIX=_nc ./build.sh gemmprobe`,
`run_rawtests2.sh gemmprobe_<reset|zero|ident|perm|ones>`), all 5 cases OK on the etri bitstream.

**`vtahw_zcu102_1x16_i8w8a32_16_16_19_18.bin` (no prof): GEMM broken, everything else OK**
- rawtest 1/32 DONE, 16 TIMEOUT (= etri). `add_test_nc` OK (LOAD ACC / ALU / STORE work).
- `gemm_nc` and every gemmprobe case MISMATCH. The output is the same garbage in every case, independent of
  inp/wgt (`41 0 0 54 0 0 -65 63 0 0 -33 0 0 0 0 -1`, a few entries vary by 1-32 between runs). STORE does write
  (the 0x5a pre-fill is overwritten), so STORE reads out_mem[8] but the GEMM never updates it (or writes elsewhere);
  the garbage looks like uninitialized BRAM.
- Bundles: Conv4Test MISMATCH (5.1 ms vs 3.6), Resnet18Test wrong class 473 / 0.064 (330 ms vs 136).
- Note: vta-hw's GEMM with `reset_reg` writes out = old accum (acc = 0); the etri HW writes out = 0
  (gemmprobe `reset` = all 0 on etri). Not the cause here (zero/ident fail too), but a semantic difference.

**`vtahw_zcu102_1x16_i8w8a32_16_16_19_18_prof.bin`: compute never reports done → every run hangs**
- Even rawtest 1 (a single FINISH) times out; all rawtest2 cases and add_test_nc time out.
- compute ctrl before start = `0x0e` (done|idle|ready already set; no-prof: `0x04`); after = `0x8b`.
- Compute register dump after rawtest 1 (`dump_regs.py`):

  | offset | no-prof | prof |
  |---|---|---|
  | 0x18 done_o | **1** | **0** |
  | 0x40 | 0x81 (alias of ctrl: map is 0x40 wide) | 0 (map 0x80 wide) |
  | 0x50 load_count | — | `0x68153800` (garbage for a lone FINISH) |
  | 0x54 / 0x64 / 0x74 | — | 1 (ap_vld of the three counters) |
  | 0x60 gemm / 0x70 alu | — | 0 |

  The counters are being written (ap_vld = 1) but load_count holds garbage and done stays 0: compute looks like it
  decodes the FINISH as something else (e.g. a LOAD with garbage x/y size) and never reaches FINISH.

**For the host-PC session**
1. Compare the compute register maps (`.../vta_compute/soln/impl/ip/drivers/compute_v1_0/src/xcompute_hw.h`)
   of the prof and no-prof IPs, and the axis width of the compute insn port (`gemm_queue`) vs the 32-byte FIFO.
2. C-simulate the gemmprobe sequence (GEMM reset → LOAD INP/WGT → GEMM → STORE, 10 insns, dump in
   `gemmprobe_zero.out` on the board) on `vta.cc`; if C is right, check the inp/wgt/out BRAM wiring
   between load/compute/store in `vivado.tcl` (wgt_mem is 2048 bit → 2 banks) and the HLS `INTERFACE bram` ports.
3. Possibly relevant: GCC packs `VTAGemInsn` with `wgt_factor_in` moved to bit 128 (the 64-bit word would
   overflow); check HLS's struct cast from `ap_uint<256>` packs it the same way.
4. Multi2/multi4 not tested (single must pass first).

Ruled out: `pl_clk0` = 99.999 MHz with the no-prof bitstream loaded (`PL0_REF_CTRL=0x01010f00`: IOPLL
33.333*90/2 = 1500 MHz / 15 / 1; pynq `Clocks.fclk0_mhz` agrees). `pl_clk1..3` are disabled (9.375 MHz,
enable=0), so the design must not use them.

### 2026-10-20 host PC: the design is functionally correct in simulation — next board tests

What the host PC checked (all on the bitstreams' own HLS IPs and block-design wiring; scratch files in the
git-ignored `vta-hw-zcu102-single/build/hardware/xilinx/cosim/`):
- Item 1 (register map / axis width): compute map is identical except 0x50-0x74 in prof; every queue port is
  256 bit = the 32-byte FIFOs. Item 3: HLS decodes `VTAGemInsn`/`VTAAluInsn` exactly as GCC packs them
  (`wgt_factor_in` = bits 138:128, ALU `imm` = 143:128; checked in the generated `compute.v`).
- C simulation of the gemmprobe sequence on `vta.cc`: all 5 cases correct.
- C/RTL co-simulation of `compute` (GEMM reset, GEMM, ALU, FINISH) and of `load` (INP/WGT -> BRAM), prof and
  no-prof: PASS (prof counters 1/2/1, done = 1).
- **PL simulation of the whole block design** (same HLS IPs, FIFOs, 1024-bit wgt BRAM banks, SmartConnect,
  AXI interconnect; PS replaced by AXI VIPs; registers written in `pynq_driver.cc` order): alu / zero / ident /
  perm / ones all **OK, prof and no-prof**; prof finishes (done = 1, no hang). BRAM byte-address bits, AXI
  cache/prot, address map and FIFO wiring in the routed no-prof netlist are as intended; IPs are synthesized
  out of context (no clash of the two HLS `reset_mem` modules in hardware).
- vta-hw semantics: a reset GEMM writes out = old accumulator (not 0). Right after a bitstream load every
  BRAM is 0, so `gemmprobe reset` should still print 0 on vtahw — the board printed garbage, i.e. compute
  produces garbage even when every memory is 0. That points at the board, not at the design:
  silicon timing / implementation, PS-side port settings, or the real instruction stream.

**Please run on the board** (single, no-prof first; reload the bitstream before each run as before):
1. `sudo python3 vtabundles_xcmpl/hwtest/plclk.py` with `vtahw_zcu102_1x16_i8w8a32_16_16_19_18.bin` loaded:
   the S_AXI_HP0 read/write width must be 128 (vtahw uses HP0 = S_AXI_GP2 at 128 bit). Note the values for
   the etri bitstream too.
2. Same HLS IPs, EVTA clock **200 MHz** instead of 333 (WNS +0.265 ns), copy it from the host PC:
   `vta-hw-zcu102-single/build/clk200/build/hardware/xilinx/vivado/zcu102_1x16_i8w8a32_16_16_19_18/export/vtahw_zcu102_1x16_i8w8a32_16_16_19_18_clk200.bin`
   -> run `gemmprobe` (all 5 cases), `gemm_nc`, `add_test_nc`. Works at 200 MHz = timing/silicon at 333;
   fails the same way = not timing.
3. Commit the full `gemmprobe_zero.out` and `gemmprobe_ident.out` (VTA_DEBUG_DUMP_INSN / UOP output) of the
   vtahw run, so the host PC can replay the exact instruction stream of the runtime in the PL simulation.
