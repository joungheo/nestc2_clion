#!/usr/bin/env python3
# #### JO added (2026-10-26): Conv4Test (3x3, 56x56x64 -> 64, 231 Mops) on 1-4 EVTAs at once, for the 4-EVTA
# bitstream (vta-hw-zcu102-multi4). VTASaverTest emits Conv4Test for EVTA 0 only, and rebuilding it means
# rebuilding Glow; the generated bundle differs per EVTA only in its names, the command handle (vtaCmdH<i>) and
# the last argument of convolution_wo_tr (the EVTA index), so the copies for EVTA 1-3 are made here by renaming.
# usage: make_multi4conv4.py <hw/Conv4Test dir> <out dir>   (then build with BUNDLE_SET=multievtax4, see README)
import os, re, shutil, sys

src, out = sys.argv[1], sys.argv[2]
os.makedirs(out, exist_ok=True)
cpp = open(f'{src}/vtaConv4TestBundle.cpp').read()
hdr = open(f'{src}/vtaConv4TestMainEntry.h').read()
for i in range(4):
    c = cpp.replace('vtaConv4TestBundle', f'vtaConv4TestBundle{i}').replace('vtaConv4TestMainEntry', f'vtaConv4TestMainEntry{i}')
    if i:
        c = c.replace('extern VTACommandHandle vtaCmdH;', f'extern VTACommandHandle vtaCmdH{i};')
        c, n = re.subn(r'(convolution_wo_tr\(.*), vtaCmdH, (.*), 0\);', rf'\1, vtaCmdH{i}, \2, {i});', c)
        assert n == 1, n
    # <entry>_convonly(n): the input transposed once, then n x convolution_wo_tr only (no CPU transposes)
    conv = re.search(r'^  convolution_wo_tr\(.*\);$', c, re.M).group(0)
    c += f'''
extern "C" void vtaConv4TestMainEntry{i}_convonly(uint8_t *mutableWeight, int n) {{
  int8_t* inputP = (int8_t*)mutableWeight + 0;
  int8_t* outP = (int8_t*)mutableWeight + 200704;
  int8_t* conv_input_transpose = (int8_t *)VTABufferAlloc(200704);
  transpose_nhwc2vtaio(inputP, (int8_t* )VTABufferGetVirtAddr(conv_input_transpose), 1, 56, 56, 64);
  VTABufferFlush(conv_input_transpose, 0, 200704);
  int8_t* conv_output_bef_transpose = (int8_t *)VTABufferAlloc(200704);
  for (int k = 0; k < n; k++) {{
{conv}
  }}
  VTABufferInvalidate(conv_output_bef_transpose, 0, 200704);
  transpose_vtaio2nhwc((int8_t* )VTABufferGetVirtAddr(conv_output_bef_transpose), outP, 1, 56, 56, 64 );
  VTABufferFree(conv_input_transpose);
  VTABufferFree(conv_output_bef_transpose);
}}
'''
    open(f'{out}/vtaConv4TestBundle{i}.cpp', 'w').write(c)
    h = hdr.replace('VTA_BUNDLE_VTACONV4TESTBUNDLE', f'VTA_BUNDLE_VTACONV4TESTBUNDLE{i}')
    h = h.replace('vtaConv4TestBundle', f'vtaConv4TestBundle{i}').replace('vtaConv4TestMainEntry', f'vtaConv4TestMainEntry{i}')
    h = h.replace('#ifdef __cplusplus\n}\n#endif', f'void vtaConv4TestMainEntry{i}_convonly(uint8_t *mutableWeight, int n);\n#ifdef __cplusplus\n}}\n#endif')
    open(f'{out}/vtaConv4TestMainEntry{i}.h', 'w').write(h)
for f in ('vtaConv4TestBundle.weights.bin', 'vtaConv4TestInput.bin', 'vtaConv4TestGolden.bin', 'VTARuntime.h'):
    shutil.copy(f'{src}/{f}', out)
shutil.copy(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'multi4conv4Main.cpp'), out)
print('written', out)
