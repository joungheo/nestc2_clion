#!/usr/bin/env python3
"""#### JO added (2026-10-17): write the Vivado/HLS config tcl for an EVTA bitstream on the ZCU102.

Uses the VTA hardware sources of tvm/3rdparty/vta-hw (HLS vta.cc + its PkgConfig formulas), but
  - compiles them against vta/vtalib/include/zcu102/vta/hw_spec.h (the EVTA ISA the NEST-C runtime emits:
    256-bit instructions, 2-bit memory_type / alu_opcode) instead of vta-hw's 128-bit hw_spec,
  - targets the ZCU102 (xczu9eg) with non-coherent HP ports (vtalib flushes/invalidates; VTA_COHERENT_ACCESSES=false),
  - places EVTA i at 0xA0000000 + i * 0x10000 (fetch/load/compute/store +0x0000/0x1000/0x2000/0x3000), as
    vta/vtalib/CMakeLists.txt assumes (VTA_FETCH_ADDR_<i> ...).
Paths are derived from this file's location, so it can run inside the Vivado docker with the tree mounted anywhere.

usage: gen_config.py <cfg.json> <out.tcl> [--num-evta N] [--board-rev 3.3] [--freq 333]
"""
import argparse, json, os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
NESTC = os.path.dirname(os.path.dirname(HERE))
VTA_HW = os.path.join(NESTC, 'tvm', '3rdparty', 'vta-hw')
TVM = os.path.join(NESTC, 'tvm')
VTALIB_SPEC_DIR = os.path.join(NESTC, 'vta', 'vtalib', 'include', 'zcu102')
sys.path.insert(0, os.path.join(VTA_HW, 'config'))
from pkg_config import PkgConfig  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('cfg')
    ap.add_argument('out')
    ap.add_argument('--num-evta', type=int, default=1, choices=(1, 2, 3, 4))
    ap.add_argument('--board-rev', default='3.3', help='ZCU102 board file revision installed in Vivado')
    ap.add_argument('--freq', type=int, default=333, help='EVTA clock in MHz (clk_wiz output)')
    a = ap.parse_args()

    cfg = json.load(open(a.cfg))
    target = cfg['TARGET']
    cfg['TARGET'] = 'zcu104'          # closest built-in target (zynq-ultrascale+, 333 MHz, 128-bit AXI)
    pkg = PkgConfig(cfg)
    pkg.TARGET = target
    pkg.fpga_device = 'xczu9eg-ffvb1156-2-e'
    pkg.fpga_board = 'xilinx.com:zcu102:part0'
    pkg.fpga_board_rev = a.board_rev
    pkg.fpga_freq = a.freq
    pkg.axi_cache_bits = '0011'       # normal non-cacheable bufferable: HP ports are not coherent
    pkg.axi_prot_bits = '000'

    # instruction width of the EVTA ISA (vtalib hw_spec.h), for the command-queue FIFOs
    spec = open(os.path.join(VTALIB_SPEC_DIR, 'vta', 'hw_spec.h')).read()
    log_ins = int(re.search(r'#define VTA_LOG_INS_WIDTH (\d+)', spec).group(1))
    insn_bytes = (1 << log_ins) // 8

    defs = [d for d in pkg.macro_defs if not d.startswith(('-DVTA_COHERENT_ACCESSES', '-DVTA_TARGET='))]
    defs.append('-DVTA_TARGET=' + target)
    defs.append('-DVTA_COHERENT_ACCESSES=false')
    incs = ['-I' + VTALIB_SPEC_DIR,                     # vtalib's vta/hw_spec.h wins over vta-hw's
            '-I' + os.path.join(VTA_HW, 'include'),
            '-I' + os.path.join(TVM, 'include'),
            '-I' + os.path.join(TVM, '3rdparty', 'dlpack', 'include'),
            '-I' + os.path.join(TVM, '3rdparty', 'dmlc-core', 'include')]
    cflags = ' '.join(incs + defs + ['-DVTA_TARGET_ZCU102'])

    c = {
        'CFLAGS': '"%s"' % cflags, 'TARGET': target, 'FPGA_DEVICE': pkg.fpga_device,
        'FPGA_FAMILY': pkg.fpga_family, 'FPGA_BOARD': pkg.fpga_board, 'FPGA_BOARD_REV': pkg.fpga_board_rev,
        'FPGA_PERIOD': pkg.fpga_per, 'FPGA_FREQ': pkg.fpga_freq,
        'INP_MEM_AXI_RATIO': pkg.inp_mem_axi_ratio, 'WGT_MEM_AXI_RATIO': pkg.wgt_mem_axi_ratio,
        'OUT_MEM_AXI_RATIO': pkg.out_mem_axi_ratio, 'INP_MEM_BANKS': pkg.inp_mem_banks,
        'WGT_MEM_BANKS': pkg.wgt_mem_banks, 'OUT_MEM_BANKS': pkg.out_mem_banks,
        'INP_MEM_WIDTH': pkg.inp_mem_width, 'WGT_MEM_WIDTH': pkg.wgt_mem_width, 'OUT_MEM_WIDTH': pkg.out_mem_width,
        'INP_MEM_DEPTH': pkg.inp_mem_depth, 'WGT_MEM_DEPTH': pkg.wgt_mem_depth, 'OUT_MEM_DEPTH': pkg.out_mem_depth,
        'NUM_WGT_MEM_URAM': 0,        # the xczu9eg has no UltraRAM
        'AXI_CACHE_BITS': pkg.axi_cache_bits, 'AXI_PROT_BITS': pkg.axi_prot_bits,
        'IP_REG_MAP_RANGE': pkg.ip_reg_map_range,
        'FETCH_BASE_ADDR': '0xA0000000', 'LOAD_BASE_ADDR': '0xA0001000',
        'COMPUTE_BASE_ADDR': '0xA0002000', 'STORE_BASE_ADDR': '0xA0003000',
        'EVTA_ADDR_STRIDE': '0x10000', 'NUM_EVTA': a.num_evta, 'INSN_BYTES': insn_bytes,
        'CFG_NAME': os.path.splitext(os.path.basename(a.cfg))[0],
    }
    with open(a.out, 'w') as fo:
        fo.write('proc const {name value} {\n    uplevel 1 [list set $name $value]\n'
                 '    uplevel 1 [list trace var $name w {error constant ;#} ]\n}\n')
        for k, v in c.items():
            fo.write(f'const {k} {v}\n')
    print(f'{a.out}: {target} {a.num_evta} EVTA(s), insn {insn_bytes * 8} bit, '
          f'buffers uop/inp/wgt/acc = {cfg["LOG_UOP_BUFF_SIZE"]}/{cfg["LOG_INP_BUFF_SIZE"]}/'
          f'{cfg["LOG_WGT_BUFF_SIZE"]}/{cfg["LOG_ACC_BUFF_SIZE"]}, {a.freq} MHz')


if __name__ == '__main__':
    main()
