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

### 2026-10-20 board: answers to the host-PC requests

Files: `log/board_20261020/` (plclk, register dumps, insn/uop dumps, 200 MHz log).
1. **PS ports are as assumed.** `plclk.py` with vtahw and with the etri bitstream loaded: identical — pl_clk0 99.999 MHz,
   pl_clk1-3 off, S_AXI_HPC0/1 and HP0-3 read/write **128 bit** (RDCTRL = WRCTRL = 0x3b0).
2. **200 MHz (`..._clk200.bin`, md5 5d417b7d…) is worse: everything hangs** — all 5 gemmprobe cases, `gemm_nc` and
   even `add_test_nc` (which passes on the 333 MHz no-prof bitstream). So it is not (only) timing slack; the same IPs
   behave differently per implementation run: 333 no-prof = GEMM output garbage, 333 prof = compute never done,
   200 = hang even for ALU.
3. **Insn/uop dumps** of the 333 MHz no-prof run: `vtahw333_gemmprobe_{zero,ident}.out`. The stream is the same one
   that passes on the etri bitstream (10 insns: LOAD UOP, GEMM reset, LOAD INP/WGT, LOAD UOP, GEMM, STORE,
   NOP/NOP, FINISH; uop acc=inp=wgt=8; DRAM uop 0x0efa0000 (8-byte units), inp 0x077c4a00, wgt 0x0077c4b0,
   out 0x077c4f00).
- The garbage is **deterministic**: `41 0 0 54 0 0 -65 63 0 0 -33 0 0 0 0 -1` (bytes 29 00 00 36 00 00 bf 3f 00 00 df
  00 00 00 00 ff, entries 7 and 10 vary 62/63 and -1/-33) in every case, every run, after every fresh bitstream
  load — not random/uninitialized BRAM.
- Prof bitstream: compute ctrl is already `0x0e` (done|idle|ready) **before** the first ap_start (no-prof: `0x04`),
  i.e. that compute FSM does not come out of reset in its idle state.

Next on the board (prepared, not yet run): `GP_IDX=<n>` makes gemmprobe use SRAM index n for inp/wgt/acc —
does the garbage move with the index? Plus rawtest + register dump of the 200 MHz bitstream before/after start.
Suggestion for the host PC: a debug build with an ILA on compute's `gemm_queue`/`out_mem` port and the
store's out_mem read port, captured over JTAG from the host PC (if the ZCU102 JTAG USB is connected to it);
programming over JTAG also rules out the fpga_manager/.bin path.

Ran (same day):
- `GP_IDX` = 0 / 1 / 100 (gemmprobe zero, 333 MHz no-prof): the output is **byte-identical** for every index
  (`41 0 0 54 0 0 -65 63 0 0 -1 0 0 0 0 -1`). It does not depend on the SRAM index, the input or the weights, so it
  is not out_mem content at that index; the STORE writes a constant 16-byte pattern (the 0x5a pre-fill is gone).
- 200 MHz rawtest 1/32/16: all four blocks idle (`0x04`) before start; after start fetch has issued its 1/2
  instructions, load/compute/store = `0x81` (started, auto-restart) but compute never sets done (0x18 = 0),
  even for a lone FINISH. Logs: `log/board_20261020/vtahw_clk200_rawtest.log`, `vtahw333_gemmprobe_zero_idx_0_1_100.log`.

### 2026-10-21 host PC: ILA build (`bit/evta_ila_261021/vtahw_zcu102_1x16_i8w8a32_16_16_19_18_ila.bin`)

333 MHz no-prof IPs + one ILA (gemm_queue, compute out_mem write port, store_queue, store out_mem read port,
g2s queue; probes in `vta-hw-zcu102-single/build/ila/ila_impl.tcl`), JTAG from the host PC. WNS +0.030 ns.
- With this implementation `gemmprobe zero` **hangs**. Trigger on the first gemm_queue handshake never fired:
  compute never received an instruction.
- Immediate capture while hung (`log/host_20261021/ila_gemmprobe_zero_hang.csv`): gemm_queue empty
  (TVALID 0), g2s empty, **store_queue TVALID 1 / TREADY 0** with head
  `0000020000000081 0280008818888200 4000000028003400 0408040004000401` = STORE dram 0x04020002 sram 2
  x=10240 y=13312, no dep bits — nothing like the runtime's STORE (dram 0x077c4f00 sram 8 x=y=1).
  So **fetch hands out garbage instructions** (opcodes wrong too: nothing went to compute); store is busy on a
  garbage STORE. The earlier symptoms (constant STORE pattern, prof/200 MHz hangs) fit "the instruction
  stream fetch sees is corrupt, differently per implementation run".

**Please run on the board** with the ILA bitstream, while `gemmprobe zero` hangs (second terminal):
`sudo python3 vtabundles_xcmpl/hwtest/dump_insns.py` — reads fetch's insn_count/insn_addr registers and
dumps that DRAM region decoded. DRAM = the runtime's 10 instructions -> the PL read path (fetch m_axi 256 bit
-> SmartConnect -> HP0) corrupts them; DRAM already garbage -> the CPU-side write of the (uncached)
instruction buffer. Commit the output.

Board answer (2026-10-22, JTAG from the host PC): `dump_insns.py` while hung shows the runtime's 10 instructions
intact in DRAM, so the earlier garbage STORE was not a DRAM problem (see the reset finding below).

### 2026-10-21/22 host PC + board: ILA builds `_ila3`..`_ila6`

All ILA builds are implementation-only re-runs of one synthesized design (`vta-hw-zcu102-single/build/ila3`,
probe lists in `build/ila{3,4,5,6}/ila_impl.tcl`, arm/save scripts in `build/ila/`), programmed over JTAG.
Captures kept in `log/host_20261022/`.

**Board hangs with JTAG attached = cpuidle, not the PL.** With hw_server connected, Linux died silently a few
minutes after programming (no oops in the kernel log read back over JTAG; A53 #2/#3 "No Power", #0 stuck at its
ATF warm entry 0xfffea120 = RVBAR). After `sudo ./nocpuidle.sh` (new, `vtabundles_xcmpl/hwtest/`: disables
cpuidle state1+, until reboot) the board stays up. Run it after every boot before using JTAG/ILA. Some earlier
"board died" observations (e.g. after `plreset.py` + register read) were probably this.

**Reset structure bug (fixed only in the `_ila3` copy so far).** `pl_resetn0` also reset the clk_wiz, so during a
reset pulse the 333 MHz clock stopped and the 333 MHz proc_sys_reset could not reset the HLS blocks/FIFOs. With
`pll_clk` `CONFIG.USE_RESET {false}`: right after programming all blocks idle and out_mem quiet (the `_ila2` build
had compute writing out_mem constantly), and a `plreset.py` pulse gives 333 MHz aresetn low 10 cycles after
pl_resetn0, the 100 MHz one later, `pll_clk_locked` stays 1 (`ila3_plreset_pulse.csv`); register reads after it
no longer hang. To do: same change in single/multi2/multi4 `vivado.tcl`, then `plreset.py` after every load.

**`_ila4` (load-side probes, WNS +0.008): the run completes, but load corrupts the weights.**
(`ila4_gemmprobe_zero.csv`, `gemmprobe zero` -> 9/16 wrong)
- fetch -> load/gemm/store queues: instructions bit-identical to the runtime dump; compute's uop reads and
  out_mem writes, store's out_mem read and DRAM write all correct (out_mem[8] = the printed output).
- LOAD INP: AR 0x77c4a000, RDATA 01..10, written to inp_mem[8] unchanged — correct.
- LOAD WGT: AR 0x77c4b000 len 16, **RDATA all 16 beats 0** (zero weights), but the data written to wgt_mem_0[8]
  is `20400000 00000000 00001006 00000010` (lane 0) and compute reads back another value
  (`20000010 00000800 00001004 00000000`). GEMM therefore uses garbage weights; the acc reset itself works
  (output of GEMM 2 = output of the reset GEMM >> 1 element-wise, i.e. 0 + inp*w vs. old acc + inp*w).
- How HLS writes wgt (`hls/<CONF>/vta_load/soln/impl/verilog/load.v`): a 128-bit beat goes into a 1024-bit BRAM
  word by **read-modify-write** (II=2: read in stage 0, `Din = (Dout & ~mask) | beat << lane` with WEN all ones
  in stage 1, bank chosen by beat parity). inp (128-bit word, plain write) is fine. The same RTL passes the PL
  simulation including the `ident` case, so something differs on the board (wgt_mem blk_mem_gen has
  `C_EN_SAFETY_CKT 1`, read latency 1, WRITE_FIRST, no URAM).
- Earlier "load never reads DRAM" (`_ila3`) was wrong: load signals were not in that capture condition.

**`_ila5` (1024-bit wgt probes) missed timing (WNS -0.901 ns, 4061 endpoints in compute, 645 in wgt_mem) —
not used.**

**`_ila6` (narrow wgt probes, WNS +0.055): every run deadlocks.** (`ila6_gemmprobe_zero_deadlock.csv`)
compute pops LOAD UOP and GEMM #1 (reset) and then nothing: no inp/wgt/out_mem access, no g2l push; load waits
on g2l with LOAD INP, fetch done. Registers while hung: fetch 0x06, load 0x81, compute 0x83 (0x18 done = 0),
store 0x81. So the same RTL behaves differently per implementation run (`_ila4` completes, `_ila6` deadlocks),
as seen earlier with prof / 200 MHz — timing reports are clean, so look at what timing does not cover.

**Next (host PC):** PL simulation (`build/simbd`) with the board's synthesized netlists of load and compute and
the blk_mem_gen netlists instead of RTL, to reproduce the wgt corruption / deadlock in simulation. Depending on
the result: remove the wgt read-modify-write in HLS (collect the 16 beats, write each 1024-bit word once) or
change the BRAM configuration.

ILA notes: probes are named `vta_i/<net>` or `u_ila_0_<net>` after opt (match by Tcl regexp, not
`get_hw_probes -regexp`, which treats `/` as hierarchy); with BASIC capture and pulse-only qualifiers use
`TRIGGER_POSITION 0` (otherwise it stays "pre-trigger"); qualify on queue TREADY, not TVALID (TVALID stays 1
while the consumer waits and fills the window). After a Ctrl-C'd EVTA run, `Xlnk().xlnk_reset()` before the
next one (CMA buffers leak: "Check failed: fpga_buff_ != nullptr").

### 2026-10-23 host PC + board: upper BRAMs fail on this board; single 333 MHz bitstream passes

**The cause of the wrong weights: BRAMs in the upper part of the device** (RAMB36 rows Y>=36 = SLICE Y>=180 =
clock-region rows 3..6). New EVTA-only test `hwtest/sramscan.cpp` (`./sramscan wgt|inp [invert] [dump]`)
writes patterns into every wgt/inp entry and reads them back through GEMM + STORE:
- wgt16 200 MHz build: wgt banks 0, 2, 3, 7, 8, 14 wrong in ~97% of their bytes, all others clean; the bad banks
  are exactly the ones placed at RAMB36 Y>=36. Same build with those sites PROHIBITed (`_lowbram`): 0 bytes wrong.
- `sramscan_analyse.py` on the dumps: the wrong bytes are not another address/byte/bank, not the previous
  (complemented) pattern, not constant, and not acc left over from earlier runs — essentially random.
- Clock and reset there are fine: routing complete, the EVTA clock (MMCM_X0Y3 -> BUFGCE_X0Y74, root X1Y2) has
  small skew, BRAM reset nets routed; `hwtest/regtest.py` writes/reads and resets the HLS AXI-lite registers that
  sit in row 3 (SLICE Y177-190) correctly.
- PL voltages idle (PS SYSMON): VCCINT 848 mV, VCCBRAM 847 mV (nominal 850).

**BRAM self-test** (`hwtest/brambist/`: `brambist.v` = all 912 RAMB36 with a common pattern controller,
`build.tcl` LOCs instance i to the i-th site -> `sites.txt`, `brambist.py` prints an X/Y error map,
`tb_brambist.v` testbench). v1 (all BRAMs at once, 100 MHz, WNS +0.489): **607 of 912 BRAMs with errors,
including most of X5..X12 below Y36** — which the `_lowbram` EVTA used without a single error, and many BRAMs
fail in the same bits (0x802). So v1 does not show a per-site defect; either the test itself misbehaves on the
board or 912 BRAMs switching every cycle draw more than the PL supply holds. v2 (`EN_LO`/`EN_HI` range and loop
mode; `brambist.py <sites.txt> all|columns|load`) tells these apart — built, not run yet.

**Single 333 MHz bitstream that passes everything** (`bit/evta_single_lowbram_261023/`, md5 `2fa610a9...`,
WNS +0.018 ns, no PROFILE). Changes in `vta-hw-zcu102-single` (all with `#### JO` comments):
1. `vivado.tcl`: `pll_clk` without reset input (the `_ila3` reset fix).
2. `vivado.tcl`: DONT_TOUCH on fetch/load/compute/store and the 7 queues (implementation only).
3. `pkg_config.py`: on zcu102 every SRAM bank is one AXI beat wide -> wgt 16 x 128 bit, no read-modify-write.
4. `vivado.tcl`: PROHIBIT RAMB36 Y36..Y89 / RAMB18 Y72..Y169 on zcu102 (`VTA_ALLOW_UPPER_BRAM=1` turns it off).
Board: `sramscan` wgt / wgt invert / inp / inp invert 0 bytes wrong; `gemmprobe` zero/ident/perm/ones OK;
`gemm_nc` Golden Matched; `add_test_nc` 16/16. Load sequence: fpga_manager, `xlnk_reset`, `plreset.py`.

**Next:** done race #1 in the driver (`pynq_driver.cc`), then real bundles (Conv3Test, ResNet18) with
`vtalib-zcu102-single`; brambist v2 on the board; the same four changes in multi2/multi4 and the `_prof` builds
(multi4 needs 556 BRAMs, only ~390 below Y36 -> depends on brambist v2).

**Done race in the driver fixed (2026-10-23).** `Run()` in `pynq_driver.cc` (vta/vtalib and the three
vtalib-zcu102-* copies) broke on `done_o == 1`, but done_o still holds the previous run's FINISH until compute's
first instruction has passed its dependence pops. It now clears `done_o_ap_vld` (compute 0x1c, clear on read)
before the start and accepts done only after vld has been seen. New `hwtest/donerace.cpp` (first compute
instruction waits for a 512 KB weight load): old driver 13 of 16 iterations wrong, returning after 0.022 ms with
the output not written; new driver 16/16 OK at 0.105 ms (512 KB at 5.0 GB/s). `dump_regs.py` no longer reads
compute 0x1c (reading it during a run would clear vld). Every earlier board run used the old driver: results of
runs whose first compute instruction waits on a long load (conv bundles) are suspect.

### 2026-10-23 host PC: configuration memory defect found (JTAG readback)

brambist v2 read every AXI-lite register with bits 6 and 15 set (ID 0xB1A5F042 instead of 0xB1A57002, same value
10000 times), so its results, and v1's, say nothing about the BRAMs. JTAG readback of the configuration
(`hwtest/brambist/readback.tcl`, compared with `cfgdiff.py` against the bitstream and its `write_bitstream
-mask_file` mask) shows the cause:
- brambist v2: 5000 compared bits differ, **all 1 -> 0**, all in clock-region row 3 (type-0 frames 20586..27350, the
  whole device width), all at **frame word 61 bit 18 and word 62 bits 0-3, 12-14**. Every frame with a 1 at one of
  these 8 positions reads 0 there (100 %). The logic location file puts these bits in the tile row **SLICE Y219**
  (= RAMB36 Y43, the row brambist v1 found bad in all 13 columns).
- Identical bits after a second readback, after reloading with fpga_manager, and after programming over JTAG
  (`progrb.tcl`): not the .bin conversion or the load path.
- The lowbram EVTA build: 194 bits differ, all word 62 bit 0 in the same frames — the same cells, every one where
  that design writes a 1. So 8 configuration cells per frame in that row are stuck at 0 on this device.
Everything configured in tile row Y219 (LUT, FF, routing PIP, BRAM settings) may lose bits; which ones depends on
the implementation, which explains why the same RTL failed differently per run while simulation and timing were
clean. The lowbram EVTA still has 194 such bits (in resources it apparently does not use) and passes.

**Next:** keep designs out of tile row Y219 (pblock / PROHIBIT below it, CONTAIN_ROUTING) and check whether that
alone fixes the upper-BRAM builds; that would give back RAMB36 Y44+ for multi2/multi4 if nothing has to cross the
row. Tell the board owner: the PL of this ZCU102 has a defective configuration row.

### 2026-10-24 board: fence below tile row Y219 fixes the upper-BRAM builds

`vta-hw-zcu102-single` with `VTA_BRAM_FENCE=y219` (now the zcu102 default in `vivado.tcl`; `upper` = the old
PROHIBIT from RAMB36 Y36 up, `none` = no fence): all PL cells but the PS in pblock `SLICE_X0Y0:SLICE_X96Y218` /
`RAMB36 ..Y42` / `RAMB18 ..Y85` / `DSP ..Y85` with CONTAIN_ROUTING, RAMB36 Y43 / RAMB18 Y86-87 / DSP Y86-87 and
MMCM rows 3..6 prohibited (a first try without the row PROHIBIT placed a cascaded wgt BRAM pair at Y42/Y43).
333 MHz, WNS +0.004 ns, 244 RAMB36 up to Y42 — **42 of them at Y36..42, 39 wgt** — and no net with a PIP in a
row-Y219 tile. `bit/evta_single_y219_333MHz_pass_261023/`, md5 `62c3f9e3...`.
Board (new driver): `sramscan` wgt / wgt invert / inp / inp invert 0 bytes wrong, `gemmprobe` 4/4, `donerace`
16/16, `gemm_nc` Golden Matched; `hw/run_all.sh` (BIN=vtahw_single_y219.bin): all 30 bundles with a golden PASS,
Resnet18 / Resnet18Partition / Resnet50 -> 281 on cat_285, Vgg16Cifar10 -> 0 on airplane, Resnetv10S1C0 runs;
Conv4Test 3.504 ms (65.98 GOPS), Resnet18 134.4 ms, Resnet50 1439 ms.
So the "upper BRAMs are bad" finding was row Y219 only; BRAMs at Y36..42 work.

**Next:** multi2 (278 BRAM tiles) fits below the row; multi4 (556) does not and has to cross it.

### 2026-10-24 host PC + board: multi2 fenced below Y219 works; multi4 does not fit

`hwtest/brambist/stuckchk.py <bit> -b <lowbram bit>` lists the stuck cells a bitstream sets beyond the 194 of
every design (0 extra = configured as written; old `_prof` builds: multi2 +2037, multi4 +5183).
`vta-hw-zcu102-multi{2,4}/.../vivado.tcl`: same Y219 fence as single (default `y219` for <= 2 EVTAs, `split` for
4 = EVTA 0-1 below and 2-3 above the row in child pblocks), pll_clk without reset, per-EVTA DONT_TOUCH, zcu102
beat-wide banks in `pkg_config.py`. Pitfalls found on the way:
- `get_sites -regexp {A|B}` with two site types returns only the first type, so one PROHIBIT line per site type.
- CONTAIN_ROUTING on a pblock made of two rectangles does not keep its nets out of the row between them, and
  PS-driven nets (S_AXI_HP2/3 rdata/rvalid, smartconnect exits) are in no pblock: 69 nets of the first multi4
  build had PIPs in row Y219. With CONTAIN_ROUTING on the child pblocks too, multi4 fails routing (DRC RTSTAT-6,
  18 partial route conflicts, no bitstream).
multi2 (`y219`): 333 MHz, WNS +0.049 ns, stuckchk 0 extra; `bit/evta_multi2_y219_333MHz_pass_261024/`, md5 `39b38dc2...`.
Board (`/lib/firmware/vtahw_multi2_y219.bin`), EVTA 0 and 1 each: `sramscan` wgt / wgt invert / inp / inp invert
0 bytes wrong (sramscan now runs ACC_DEPTH/16 weight entries per GEMM; with 256 the multi acc buffer of 2048
rows overflowed and every byte read back wrong), `gemmprobe` 4/4, `donerace` 16/16; `multi_gemm_gemm_2evta_m`
Golden Matched on both EVTAs; `hw_multievtax2/run_all.sh`: MultiEVTAx2ConvTest PASS (2 x 10000 runs 4405 ms),
multievtax2Resnet18Bundle 2 x 100 runs all 281 (15179 ms), multievtax2SingleResnet18Bundle 100 runs 281 (13624 ms).

**Next:** multi4 needs 572 RAMB36, only 379 are below the row, and the PS nets to EVTA 2/3 must cross it. Vivado
cannot block routing in one row: RapidWright (block the Y219 nodes, reroute), smaller buffers, or another board.

### 2026-10-25 host PC + board: nets that cross row Y219 fail even without a stuck cell; multi4 with small buffers

RapidWright route of multi4 (`vta-hw-zcu102-multi4/hardware/xilinx/scripts/y219route/`, work dir `build/y219/`):
the stuck cells hit only the INT PIPs that set a bit at one of the 8 stuck offsets (606 of 3778 PIP types, from
Project U-Ray `segbits_int.db`: `stuckpips.py` -> `stuck_int_pips.txt`; it predicted all 85 extra cells of blk_f).
Y219RWRoute reroutes the nets that use them (timing-driven with `-Dcrit=0.95`), Vivado finishes; rs_m = rs_h with
the MMCM at 321.7 MHz (`mmcm_set.tcl`): WNS +0.009, 0 stuck PIPs, stuckchk 0 extra. Board (`bit/evta_multi4_y219_261025/`):
- EVTA 1 `gemmprobe ident` OK; EVTA 0 hangs; `gemmprobe` on EVTA 2, EVTA 3, and a register write/read on EVTA 3
  freeze the whole board (no ping; power cycle; three times today). rs_h (333 MHz) the same, so not the clock.
- JTAG readback of the loaded PL (board hung) = the bitstream except the 193 base cells: configured as written.
- Register write/read patterns (`regpat.py`, no ap_start): 8 wrong bits, every one on a net that crosses row Y219:
  compute_0 RDATA[28] reads 1; store_1 RDATA[2,4,9,22,26,30] read 0; fetch_3 bit 14 (its WDATA[14] branch that
  crosses the row; the same net's branches to EVTA 0/1 below are fine). These nets use Vivado routes (same as the
  pure-Vivado blk_f), no stuck PIP, setup slack > 1.3 ns, hold > 0.27 ns. Other crossing nets on the same wire
  types work, so the bad wires cannot be told from their names; 0 errors on nets that stay below the row.
So the row's defect is more than the stuck configuration cells: nothing may cross it on this board. PS ports
(S_AXI_HP0-3, M_AXI_HPM0) all sit below it (INT rows <= 138).

multi4 with smaller buffers below the row (uop/inp/wgt/acc 15/15/17/16, fence `y219`) was started and dropped
when the pass-over wires proved good (below); hwtest `build.sh` keeps a `UOP=` option for such builds.

### 2026-10-25 later: the bad part of row Y219 is its PIPs; multi4 with no PIP in the row works (280 MHz)

Wire test designs (`y219route/WireTest.java`, `WireTestBit.tcl`, `wiretest_run.sh`, `wiretest.py`; at-speed:
`-Dspeed=1`, `wirespeed_run.sh`, `wirespeed.py`, `hwtest/plclkset.py`; work dir `build/wiretest/`): one net per INT
node, source and sink SRLs read back over JTAG (no capture on this device; SRL stage k = .ll Ram bit 2k+1). Loaded
with fpga_manager; run `hwtest/nocpuidle.sh` after boot before any JTAG access (forgetting it froze the board twice).
- Nodes passing over the row (begin below, end above or the reverse; no PIP in the row): 20207 of 20328 tested,
  static 0/1 and at 100/300/500/750 MHz: all good (one miss at 750 MHz).
- Nets that cross on PIPs inside the row that set no stuck cell (`-Dinrow=1`, 300 nets): 28 fail at every
  frequency. The configuration reads back as written, so some routing muxes of the row are broken although their
  cells hold the right value. Bits set by the failing PIPs: 22_03, 21_00, 33_15 (every net), 50_03, 59_01, 58_27,
  57_31, ... (offsets next to the stuck ones). rs_m's compute_0 RDATA[28] (always wrong) used such PIPs.
multi4 rs_zm (`build/y219/`): blk_f -> Y219RWRoute.java -Dcrit=0.95 without -Dpips (every PIP in the row banned:
168 nets rerouted) -> route_s.tcl (0 nets with any PIP in the row) -> mmcm_set.tcl MULT 40.25 (280.0 MHz; at 333 MHz
WNS -0.488): WNS +0.057, WHS -0.002 (1 path), stuckchk 0 extra. Board, EVTA 0-3: regpat all bits right (3 loads),
gemmprobe 4/4, sramscan wgt/inp (+invert) 0 bytes wrong, donerace 16/16, multi_gemm_gemm_4evta_m Golden Matched.
Then 309.6 MHz: rs_z's 333 MHz failures were mostly smc_2/3 (Y221-225) -> PS SAXIGP4/5 WDATA, so `vivado.tcl` puts
an axi_register_slice (`hpslice<i>`, all channels REG=1) before each `S_AXI_HP<i>_FPD`, in pblock `pb_hpslice`
below the row (the fence xdc's `-regexp {vta_i/hpslice\d}` matched nothing: now a glob; place_d.tcl re-adds them).
`flow/run_flow.sh hp` -> rs_hp: 0 nets with a PIP in the row, WNS -0.199 at 333 MHz (left: proc_sys_reset ->
compute_3, 97 % route). mmcm_set.tcl MULT 44.5 -> rs_hpm 309.6 MHz: WNS +0.019, WHS +0.008, stuckchk 0 extra over
multi2. Board (`bit/evta_multi4_y219_261025/rs_hpm.bin`), twice: regpat all bits right (3 loads), gemmprobe 4/4,
sramscan 0 wrong, donerace 16/16 on EVTA 0-3, multi_gemm_gemm_4evta_m Golden Matched (3 runs).
Bundles on rs_hpm (2026-10-26, `vtabundles_xcmpl` set `multievtax4`, vtalib-zcu102-multi4, README): MultiEVTAConvTest
(4 EVTAs x 10000 runs) outputs 0-3 = golden; multivtaMxnetResnet18Bundle (4 copies, 100 inferences shared) and
singlevtaMxnetResnet18Bundle all 281. Time for 100 inferences, 4 threads / single: OMP_NUM_THREADS=1 11.0 / 43.3 s,
2: 12.1 / 23.5 s, 4: 14.1 / 13.5 s (CPU-bound; the 4 threads' OpenMP teams oversubscribe the 4 cores).
Conv4Test (3x3, 56x56x64 -> 64, 231.2 Mops) on 1-4 EVTAs at once (`vtabundles_xcmpl/multi4conv4/`, rs_hpm 309.6 MHz,
all outputs = golden):
- EVTA run time (VTA_SYNC, _prof vtalib): 1.909 ms per conv = 121.1 GOPS per EVTA, the same alone and with 4 at
  once (no HP/DDR contention) -> 484 GOPS for 4. Peak 512 ops/cycle = 158.5 GOPS per EVTA (634 for 4): 76 %.
  Pure GEMM time = 451584 GEMM / 309.6 MHz = 1.459 ms; the rest is load/store not hidden behind GEMM.
- conv only (convolution_wo_tr in a loop incl. instruction generation, OMP_NUM_THREADS=1): 114.6 GOPS on 1, 229.0 on
  2, **457.3 GOPS on 4** (2.02 ms per conv each).
- whole bundle (CPU transposes + buffer allocation every run): 64 GOPS on 1, 108 on 4 (OMP=4) / 117 (OMP=1): CPU-bound.

2026-10-26: **multi4 at 333 MHz** (rs_rst, `bit/evta_multi4_y219_333MHz_pass_261026/vtahw_zcu102_1x16_i8w8a32_16_15_18_17_4evta.bin`): one proc_sys_reset per EVTA
in its pblock (`vivado.tcl` add_evta; the rs_hp failure was the global reset -> compute_3), then `run_flow.sh rst`:
0 nets with a PIP in row Y219, WNS +0.008, WHS +0.010; stuckchk 194, 0 beyond multi2. Board: gemmprobe 4/4,
sramscan, donerace 16/16 (EVTA 0-3), `multi_gemm_gemm_4evta_m` 3 runs all Golden Matched. Conv4 (all = golden):
EVTA time 1.864 ms per conv = 124.0 GOPS per EVTA (309.6 MHz: 1.909 ms; only 2.4 % faster for 7.7 % more clock:
load/store bound), 496 GOPS for 4; conv only OMP=1 122.6 / 245.1 / **489.6 GOPS** on 1 / 2 / 4 EVTAs.
