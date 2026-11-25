/*
 * #### JO added (2026-11-24, docs/evta_dw_unit.md 3, 6): HLS C simulation of the depthwise window mode (VTA_DWU, Makefile
 * DWU=1). One vta() call per case: a 16-channel block of an input tile (zero border in DRAM) and its 3 x 3 filters, a reset
 * GEMM, one window-mode GEMM over ky (iter_out: one weight block per ky) and the output rows (iter_in), uops = the input pixels of one output row
 * ((wo - 1) * s + 3 steps, emit from the third, every s-th), rq on, then a STORE; compared with the depthwise conv in C.
 */
#ifdef VTA_DWU

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

#include "../src/vta.h"
#include "../../../tests/hardware/common/test_lib.h"

namespace {

int rq_ref(int32_t v, int rnd, int shift, int lo, int hi) {
  uint32_t h = (rnd != 0 && shift != 0) ? (1u << (shift - 1)) : 0;
  if (rnd == 2 && shift != 0 && v < 0) h -= 1;
  int32_t q = (int32_t) ((uint32_t) v + h) >> shift;
  return q < lo ? lo : q > hi ? hi : q;
}

VTAGenericInsn gemm(int bgn, int end, int iout, int iin, int dfo, int dfi, int sfo, int sfi, int wfo, int wfi, bool reset,
                    int pop_prev, int push_next) {
  VTAGemInsn g;
  memset(&g, 0, sizeof g);
  g.opcode = VTA_OPCODE_GEMM;
  g.pop_prev_dep = pop_prev;
  g.push_next_dep = push_next;
  g.reset_reg = reset;
  g.uop_bgn = bgn;
  g.uop_end = end;
  g.iter_out = iout;
  g.iter_in = iin;
  g.dst_factor_out = dfo;
  g.dst_factor_in = dfi;
  g.src_factor_out = sfo;
  g.src_factor_in = sfi;
  g.wgt_factor_out = wfo;
  g.wgt_factor_in = wfi;
  union VTAInsn c;
  memset(&c, 0, sizeof c);
  c.gemm = g;
  return c.generic;
}

VTAUop uop(int dst, int src, int wgt, bool emit) {
  VTAUop u;
  memset(&u, 0, sizeof u);
  u.dst_idx = dst;
  u.src_idx = src;
  u.wgt_idx = wgt;
  uint64_t b;
  memcpy(&b, &u, sizeof b);
  if (emit) b |= 1ull << VTA_UOP_DW_EMIT;
  memcpy(&u, &b, sizeof b);
  return u;
}

}  // namespace

int dwu_test(unsigned seed, int ho, int wo, int s) {
  const int C = VTA_BLOCK_IN, wi = (wo - 1) * s + 3, hi = (ho - 1) * s + 3;
  unsigned r = seed;
  std::vector<int8_t> in((size_t) hi * wi * C), w(3 * 3 * C);
  for (auto &x : in) x = (int8_t) (rand_r(&r) % 256 - 128);
  for (auto &x : w) x = (int8_t) (rand_r(&r) % 256 - 128);
  // a zero border as the LOAD padding would give
  for (int y = 0; y < hi; y++)
    for (int x = 0; x < wi; x++)
      if (y == 0 || x == 0 || ((rand_r(&r) & 15) == 0))
        for (int c = 0; c < C; c++) in[((size_t) y * wi + x) * C + c] = 0;
  const int rnd = 1 + rand_r(&r) % 2, shift = rand_r(&r) % 12, lo = -(int) (rand_r(&r) % 129), hiq = rand_r(&r) % 128;

  // weights: entry ky, row j = channel j, column k = kx
  int8_t *wgtBuf = static_cast<int8_t *>(allocBuffer(3 * VTA_BLOCK_OUT * VTA_BLOCK_IN));
  memset(wgtBuf, 0, 3 * VTA_BLOCK_OUT * VTA_BLOCK_IN);
  for (int ky = 0; ky < 3; ky++)
    for (int j = 0; j < C; j++)
      for (int k = 0; k < 3; k++) wgtBuf[ky * 256 + j * 16 + k] = w[(ky * 3 + k) * C + j];
  int8_t *inpBuf = static_cast<int8_t *>(allocBuffer(in.size()));
  memcpy(inpBuf, in.data(), in.size());
  const int nOut = ho * wo;
  int8_t *outBuf = static_cast<int8_t *>(allocBuffer((size_t) nOut * C));
  memset(outBuf, 0x5a, (size_t) nOut * C);

  std::vector<VTAUop> uops;
  uops.push_back(uop(0, 0, 0, false));   // the reset GEMM
  const int rowB = uops.size();
  for (int x = 0; x < wi; x++) {
    const bool e = x >= 2 && (x - 2) % s == 0;
    uops.push_back(uop(e ? (x - 2) / s : 0, x, 0, e));
  }
  const int rowE = uops.size();
  VTAUop *uopBuf = static_cast<VTAUop *>(allocBuffer(sizeof(VTAUop) * uops.size()));
  memcpy(uopBuf, uops.data(), sizeof(VTAUop) * uops.size());

  std::vector<VTAGenericInsn> insns;
  insns.push_back(get1DLoadStoreInsn(VTA_OPCODE_LOAD, VTA_MEM_ID_UOP, 0, 0, uops.size(), 0, 0, 0, 0));
  insns.push_back(get2DLoadStoreInsn(VTA_OPCODE_LOAD, VTA_MEM_ID_INP, 0, 0, 1, hi * wi, hi * wi, 0, 0, 0, 0, 0, 0));
  insns.push_back(get2DLoadStoreInsn(VTA_OPCODE_LOAD, VTA_MEM_ID_WGT, 0, 0, 1, 3, 3, 0, 0, 0, 0, 0, 1));
  insns.push_back(gemm(0, 1, 1, nOut, 0, 1, 0, 0, 0, 0, true, 1, 0));
  {
    VTAGenericInsn g = gemm(rowB, rowE, 3, ho, 0, wo, wi, s * wi, 1, 0, false, 0, 1);   // ky outside, rows inside
    union VTAInsn c;
    c.generic = g;
    c.gemm.dw_win = 1;
    c.gemm.rq_en = 1;
    c.gemm.rq_round = rnd;
    c.gemm.rq_shift = shift;
    c.gemm.rq_lo = lo;
    c.gemm.rq_hi = hiq;
    insns.push_back(c.generic);
  }
  insns.push_back(get2DLoadStoreInsn(VTA_OPCODE_STORE, VTA_MEM_ID_OUT, 0, 0, 1, nOut, nOut, 0, 0, 1, 0, 0, 0));
  insns.push_back(getFinishInsn(0, 0));
  VTAGenericInsn *insnBuf = static_cast<VTAGenericInsn *>(allocBuffer(sizeof(VTAGenericInsn) * insns.size()));
  memcpy(insnBuf, insns.data(), sizeof(VTAGenericInsn) * insns.size());

  vta(insns.size(), (volatile insn_T *) insnBuf, (volatile uop_T *) uopBuf, (volatile bus_T *) inpBuf,
      (volatile bus_T *) wgtBuf, (volatile bus_T *) NULL, (volatile bus_T *) outBuf);

  int err = 0;
  for (int oy = 0; oy < ho; oy++)
    for (int ox = 0; ox < wo; ox++)
      for (int c = 0; c < C; c++) {
        int32_t a = 0;
        for (int ky = 0; ky < 3; ky++)
          for (int kx = 0; kx < 3; kx++)
            a += in[((size_t) (oy * s + ky) * wi + ox * s + kx) * C + c] * w[(ky * 3 + kx) * C + c];
        const int ref = rq_ref(a, rnd, shift, lo, hiq), got = outBuf[((size_t) oy * wo + ox) * C + c];
        if (ref != got && err++ < 10)
          printf("ERROR - dwu %dx%d s%d: out (%d, %d) ch %d: hls %d ref %d\n", ho, wo, s, oy, ox, c, got, ref);
      }
  printf("INFO - dwu %dx%d stride %d (rq round %d shift %d [%d, %d]): %s, %d bytes compared\n", ho, wo, s, rnd, shift, lo,
         hiq, err ? "FAILED" : "ok", nOut * C);
  freeBuffer(insnBuf);
  freeBuffer(uopBuf);
  freeBuffer(inpBuf);
  freeBuffer(wgtBuf);
  freeBuffer(outBuf);
  return err ? -1 : 0;
}

#endif  // VTA_DWU
