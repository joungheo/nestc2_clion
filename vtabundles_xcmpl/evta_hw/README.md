# EVTA bitstreams for the ZCU102 (Vivado / Vivado HLS 2020.1)

Builds 1-4 EVTA cores into one bitstream from the HLS sources in `tvm/3rdparty/vta-hw`, so that the
multi-EVTA bundles (`vta/multievtax2_bundles`, ...) have hardware that matches the current vtalib.
The etri/nest-data multi-EVTA bitstreams (`zcu102_2_*`, `zcu102_4_*`, 2020-08) do not work with it
(2026-10-17: they run, but add_test/gemm results are all zero with every field-width setting).

| | vta-hw as shipped | here |
|---|---|---|
| ISA header | `vta-hw/include/vta/hw_spec.h` (128-bit insn, 3-bit fields) | `vta/vtalib/include/zcu102/vta/hw_spec.h` (256-bit, 2-bit `memory_type`/`alu_opcode`) — what NEST-C emits |
| board | zcu104 / ultra96 | ZCU102 `xczu9eg-ffvb1156-2-e`, board `xilinx.com:zcu102:part0:<BOARD_REV>` |
| memory port | `S_AXI_HPC0_FPD`, coherent | `S_AXI_HP<i>_FPD`, non-coherent, 128 bit (vtalib flushes/invalidates) |
| cores | 1 | `NUM_EVTA` (1-4), EVTA i registers at `0xA0000000 + i*0x10000` (+0/0x1000/0x2000/0x3000 fetch/load/compute/store) |
| command FIFOs | 16 bytes | 32 bytes (one 256-bit instruction) |

Only the four stage IPs come from HLS; they do not depend on `NUM_EVTA`. Files: `gen_config.py` (config tcl, uses
vta-hw's `PkgConfig` formulas), `hls.tcl`, `vivado.tcl`, `Makefile`, `evta_zcu102.json` (buffers 16/16/19/18 =
the bitstream in use), `evta_zcu102_small.json` (16/15/18/17 = vtalib's `NESTC_EVTA_MULTI` sizes).

For a single EVTA, `vta-hw-zcu102-single/hardware/xilinx` (vta-hw's own Makefile/scripts, ZCU102 target) has
been built and timing-checked (2026-10-20, see CLAUDE.md); this directory is still the path for 2-4 cores.

## Build (in the Vivado 2020.1 docker, with the nest-compiler tree mounted)

```bash
docker exec -it -u ubuntu vivado2020-build bash         # host PC container with Vivado 2020.1
#source /tools/Xilinx/Vivado/2020.1/settings64.sh
source /tools/Xilinx/v2020.1/Vivado/2020.1/settings64.sh
cd <nest-compiler>/vtabundles_xcmpl/evta_hw
make ip                         # HLS (once per CFG); CSIM=1 also runs vta-hw's C simulation (may not compile: 128-bit testbench)
make bin NUM_EVTA=1             # step 1: one core, compare with the bitstream in use
make bin NUM_EVTA=2             # step 2
make bin CFG=evta_zcu102_small.json NUM_EVTA=4   # 4 cores need the smaller buffers (BRAM: ~200 BRAM36 per 16/19/18 core, xczu9eg has 912)
# BOARD_REV=<rev> if the installed ZCU102 board files are not 3.3; JOBS=<n> for parallel runs
```

Outputs in `build/<cfg>/vivado_<N>evta/export/`: `<cfg>_<N>evta.bit`, `.bin`, `.xsa` (address map), utilization and
timing reports. Check `*.timing.rpt` (WNS >= 0 at 333 MHz) before using a bitstream.

## Test on the board

```bash
scp .../export/evta_zcu102_1evta.bin xilinx@<board>:/home/xilinx/JO/npuprg/bit/
sudo cp ~/JO/npuprg/bit/evta_zcu102_1evta.bin /lib/firmware/
cd <nest-compiler>/vtabundles_xcmpl/hwtest
sudo BIN=evta_zcu102_1evta.bin ./run_rawtests.sh                      # 256-bit insns: "32" must pass, "16" time out
sudo BIN=evta_zcu102_1evta.bin ./run_rawtests2.sh add_test_nc gemm_nc  # add_test: both numbers equal; gemm: Golden Matched
sudo BIN=evta_zcu102_1evta.bin ../hw/script/run_all.sh Conv4Test Resnet18Test   # (2026-10-27: was hw/run_all.sh)
```

Notes:
- Loading a bitstream through `fpga_manager` does not change the PS configuration: the EVTA clock comes from
  `pl_clk0` (expected 100 MHz, multiplied to 333 MHz by `clk_wiz`), and the HP ports run with the width the boot
  image configured (128 bit expected). If results are wrong only on the new bitstreams, check those first.
- Never run the `hwtest/*_old*` binaries (prebuilt nest-data runtimes): they caused SError kernel oopses.
