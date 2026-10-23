// #### JO added (2026-10-23): scan the EVTA SRAMs with EVTA instructions only (no ILA, any bitstream).
// The vtahw bitstreams give wrong weights for some lanes, and the failing lanes map to BRAMs in the upper
// rows of the device (ILA _ila7, wgt16 200 MHz). This writes known patterns into every SRAM entry and reads
// them back through GEMM + STORE, so each byte can be checked:
//   wgt : inp[k] = e_k (k = 0..15); W[w] = pattern; GEMM acc[c*16+k] = W[base+c] * e_k = column k of W
//         -> out row c*16+k, element oc = W[base+c][oc][k]. Runs of 256 weight entries cover all of them.
//         On the 16-bank (wgt16) bitstreams, row oc of a weight entry is wgt bank oc.
//   inp : W[0] = I; inp[i] = pattern; GEMM acc[i] = I * inp[i] -> out row i = inp row i (all entries).
// #### JO changed (2026-10-24): depths from hw_spec.h (single 16_16_19_18: wgt 2048, inp/acc 4096; multi-EVTA
// 16_15_18_17: 1024 / 2048), so build.sh BUFF="15 18 17" builds it for the multi-EVTA bitstreams.
// usage: ./sramscan wgt|inp [invert] [dump]   (invert = complemented pattern, catches bits stuck at the other value;
//        dump = also write every value read back to sramscan_<mode>[_invert].bin, analyse with sramscan_analyse.py)
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "vta/runtime.h"
#include "vta/data_buf.h"
#include "vta/hw_spec.h"

#define EVTA_ID 0

using namespace vta;

//static const int WGT_DEPTH = 2048, INP_DEPTH = 4096, ACC_DEPTH = 4096, CHUNK = 256;
static const int WGT_DEPTH = VTA_WGT_BUFF_DEPTH, INP_DEPTH = VTA_INP_BUFF_DEPTH, ACC_DEPTH = VTA_ACC_BUFF_DEPTH;
// one wgt run writes CHUNK * 16 acc rows (multi-EVTA acc depth 2048 -> 128 entries per run)
static const int CHUNK = ACC_DEPTH / 16;
static int g_base = 0, g_reset = 0;

// wgt scan: iter_out over the CHUNK weight entries (dst += 16, wgt += 1), iter_in over k (dst += 1, src += 1)
static int wgt_kernel(void *, VTACommandHandle h) {
  VTAUopLoopBegin(h, CHUNK, 16, 0, 1);
  VTAUopLoopBegin(h, 16, 1, 1, 0);
  VTAUopPush(h, 0, g_reset, 0, 0, g_base, 0, 0, 0);
  VTAUopLoopEnd(h);
  VTAUopLoopEnd(h);
  return 0;
}

// inp scan: acc[i] = W[0] * inp[i] for all i (INP_DEPTH / 64 x 64)
static int inp_kernel(void *, VTACommandHandle h) {
  VTAUopLoopBegin(h, INP_DEPTH / 64, 64, 64, 0);
  VTAUopLoopBegin(h, 64, 1, 1, 0);
  VTAUopPush(h, 0, g_reset, 0, 0, 0, 0, 0, 0);
  VTAUopLoopEnd(h);
  VTAUopLoopEnd(h);
  return 0;
}

static int8_t pat(int a, int b, int c, bool inv) {
  uint8_t v = (uint8_t)(a * 37 + b * 11 + c * 5 + 3) ^ (uint8_t)((a >> 3) * 0x55);
  return (int8_t)(inv ? ~v : v);
}

// one run: reset GEMM, load, GEMM, store (same token order as gemmprobe)
static void run(VTACommandHandle h, void **uh_r, void **uh, int (*k)(void *, VTACommandHandle),
                DataBuffer *inp, int inp_off, int inp_n, int inp_sram,
                DataBuffer *wgt, int wgt_off, int wgt_n, int wgt_sram, DataBuffer *out, int out_n) {
  g_reset = 1;
  VTAPushGEMMOp(h, uh_r, k, nullptr, 0);
  VTADepPush(h, 2, 1);
  VTADepPop(h, 2, 1);
  VTALoadBuffer2D(h, inp, inp_off, inp_n, 1, inp_n, 0, 0, 0, 0, inp_sram, 2);
  VTALoadBuffer2D(h, wgt, wgt_off, wgt_n, 1, wgt_n, 0, 0, 0, 0, wgt_sram, 1);
  VTADepPush(h, 1, 2);
  VTADepPop(h, 1, 2);
  g_reset = 0;
  VTAPushGEMMOp(h, uh, k, nullptr, 0);
  VTADepPush(h, 2, 3);
  VTADepPop(h, 2, 3);
  VTAStoreBuffer2D(h, 0, 4, out, 0, out_n, 1, out_n);
  VTASynchronize(h, 1 << 31);
}

int main(int argc, char **argv) {
  std::string mode = argc > 1 ? argv[1] : "wgt";
  bool inv = false, dump = false;
  for (int a = 2; a < argc; a++) { inv |= std::string(argv[a]) == "invert"; dump |= std::string(argv[a]) == "dump"; }
  std::string dump_name = "sramscan_" + mode + (inv ? "_invert" : "") + ".bin";
  FILE *df = dump ? fopen(dump_name.c_str(), "wb") : nullptr;
  VTACommandHandle h = VTATLSCommandHandle(EVTA_ID);
  long bad_total = 0;

  if (mode == "wgt") {
    std::vector<int8_t> inp(16 * 16, 0), wgt((size_t)WGT_DEPTH * 256), out((size_t)CHUNK * 16 * 16);
    for (int k = 0; k < 16; k++) inp[k * 16 + k] = 1;
    for (int w = 0; w < WGT_DEPTH; w++)
      for (int oc = 0; oc < 16; oc++)
        for (int ic = 0; ic < 16; ic++) wgt[(size_t)w * 256 + oc * 16 + ic] = pat(w, oc, ic, inv);
    DataBuffer *ib = (DataBuffer *)VTABufferAlloc(inp.size());
    DataBuffer *wb = (DataBuffer *)VTABufferAlloc(wgt.size());
    DataBuffer *ob = (DataBuffer *)VTABufferAlloc(out.size());
    VTABufferCopy(inp.data(), 0, ib, 0, inp.size(), 1);
    VTABufferCopy(wgt.data(), 0, wb, 0, wgt.size(), 1);
    long bad_bank[16] = {0};
    uint8_t xor_bank[16] = {0};
    int shown = 0;
    for (int base = 0; base < WGT_DEPTH; base += CHUNK) {
      void *uh_r[2] = {nullptr}, *uh[2] = {nullptr};
      memset(out.data(), 0x5a, out.size());
      VTABufferCopy(out.data(), 0, ob, 0, out.size(), 1);
      g_base = base;
      run(h, uh_r, uh, &wgt_kernel, ib, 0, 16, 0, wb, base, CHUNK, base, ob, CHUNK * 16);
      VTABufferCopy(ob, 0, out.data(), 0, out.size(), 2);
      if (df) fwrite(out.data(), 1, out.size(), df);
      long bad = 0;
      for (int c = 0; c < CHUNK; c++)
        for (int k = 0; k < 16; k++)
          for (int oc = 0; oc < 16; oc++) {
            int8_t got = out[(size_t)(c * 16 + k) * 16 + oc], exp = wgt[(size_t)(base + c) * 256 + oc * 16 + k];
            if (got != exp) {
              bad++; bad_bank[oc]++; xor_bank[oc] |= (uint8_t)(got ^ exp);
              if (shown < 12) { printf("  w=%4d oc(bank)=%2d ic=%2d exp=%4d got=%4d xor=0x%02x\n", base + c, oc, k, exp, got, (uint8_t)(got ^ exp)); shown++; }
            }
          }
      printf("wgt entries %4d..%4d: %ld / %d bytes wrong\n", base, base + CHUNK - 1, bad, CHUNK * 256);
      bad_total += bad;
    }
    printf("per row oc (= wgt bank on 16-bank bitstreams): wrong bytes / OR of the flipped bits\n");
    for (int oc = 0; oc < 16; oc++) printf("  oc %2d: %6ld  0x%02x\n", oc, bad_bank[oc], xor_bank[oc]);
  } else if (mode == "inp") {
    std::vector<int8_t> inp((size_t)INP_DEPTH * 16), wgt(256, 0), out((size_t)ACC_DEPTH * 16);
    for (int i = 0; i < 16; i++) wgt[i * 16 + i] = 1;
    for (int r = 0; r < INP_DEPTH; r++)
      for (int e = 0; e < 16; e++) inp[(size_t)r * 16 + e] = pat(r, e, 7, inv);
    DataBuffer *ib = (DataBuffer *)VTABufferAlloc(inp.size());
    DataBuffer *wb = (DataBuffer *)VTABufferAlloc(wgt.size());
    DataBuffer *ob = (DataBuffer *)VTABufferAlloc(out.size());
    VTABufferCopy(inp.data(), 0, ib, 0, inp.size(), 1);
    VTABufferCopy(wgt.data(), 0, wb, 0, wgt.size(), 1);
    memset(out.data(), 0x5a, out.size());
    VTABufferCopy(out.data(), 0, ob, 0, out.size(), 1);
    void *uh_r[2] = {nullptr}, *uh[2] = {nullptr};
    run(h, uh_r, uh, &inp_kernel, ib, 0, INP_DEPTH, 0, wb, 0, 1, 0, ob, ACC_DEPTH);
    VTABufferCopy(ob, 0, out.data(), 0, out.size(), 2);
    if (df) fwrite(out.data(), 1, out.size(), df);
    long bad_lane[16] = {0}, bad_rows = 0;
    int shown = 0;
    for (int r = 0; r < INP_DEPTH; r++) {
      bool rb = false;
      for (int e = 0; e < 16; e++) {
        int8_t got = out[(size_t)r * 16 + e], exp = inp[(size_t)r * 16 + e];
        if (got != exp) {
          bad_total++; bad_lane[e]++; rb = true;
          if (shown < 12) { printf("  row=%4d lane=%2d exp=%4d got=%4d\n", r, e, exp, got); shown++; }
        }
      }
      bad_rows += rb;
    }
    printf("inp/acc/out rows with errors: %ld / %d\n", bad_rows, INP_DEPTH);
    for (int e = 0; e < 16; e++) printf("  lane %2d: %ld\n", e, bad_lane[e]);
  } else {
    fprintf(stderr, "usage: %s wgt|inp [invert]\n", argv[0]);
    return 2;
  }
  if (df) { fclose(df); printf("dumped %s\n", dump_name.c_str()); }
  printf("RESULT sramscan %s%s: %s (%ld bytes wrong)\n", mode.c_str(), inv ? " invert" : "", bad_total ? "ERRORS" : "OK", bad_total);
  VTARuntimeShutdown(EVTA_ID);
  return bad_total ? 1 : 0;
}
