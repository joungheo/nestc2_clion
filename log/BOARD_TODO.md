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
   If EVTA 1 returns zeros, check the HP1 port width: AFIFM `0xFD370000` (HP1), `0xFD380000` (HP2),
   `0xFD390000` (HP3) — set by the boot image, not the bitstream; compare with HP0 `0xFD360000`.
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

(fill in: rawtest / bundle PASS, profile numbers, AFIFM values)
