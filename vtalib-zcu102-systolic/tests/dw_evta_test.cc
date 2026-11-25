// #### JO added (2026-11-24, docs/evta_dw_unit.md 5): dw_convolution_evta (depthwise 3 x 3 on the EVTA) against a CPU loop of
// the same arithmetic, on the MobileNetV2 shapes and a few others, in the mode the bitstream / simulator gives (window mode
// with the depthwise window unit, EVTA_DWU=1 on a "_dw" bitstream or in the simulator; the diagonal GEMM with EVTA_DWU=0).
// usage: dw_evta_test [case]    (tests/run_dw_test.sh builds it for the simulator; on the board: run_alux_board_tests.sh)
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

#include "Bundle/VTABundle.h"
#if NESTC_USE_VTASIM
#include "simulator/vta/runtime.h"
#else
#include "vta/runtime.h"
#endif

struct Shape {
  int H, W, C, s, pad, padEnd, nvt, th, tw;   // padEnd < 0: same as pad
};

static int run(int idx, const Shape &sh, unsigned seed) {
  std::mt19937 g(seed);
  std::uniform_int_distribution<int> u8(-128, 127), uw(-64, 63), ub(-2000, 2000), us(4, 9);
  const int H = sh.H, W = sh.W, C = sh.C, K = 3, s = sh.s, p = sh.pad, pe = sh.padEnd < 0 ? sh.pad : sh.padEnd;
  const int OH = (H + p + pe - K) / s + 1, OW = (W + p + pe - K) / s + 1, lo = 0, hi = 48;
  const size_t ni = (size_t)C * H * W, no = (size_t)C * OH * OW;
  void *h = VTATLSCommandHandle(0);
  void *vin = VTABufferAlloc(ni), *vk = VTABufferAlloc((size_t)(C / 16) * 9 * 256), *vb = VTABufferAlloc(C * 4),
       *vs = VTABufferAlloc(C * 4), *vo = VTABufferAlloc(no);
  int8_t *in = (int8_t *)VTABufferGetVirtAddr(vin), *k = (int8_t *)VTABufferGetVirtAddr(vk);
  int32_t *b = (int32_t *)VTABufferGetVirtAddr(vb), *sf = (int32_t *)VTABufferGetVirtAddr(vs);
  for (size_t i = 0; i < ni; i++) in[i] = (int8_t)u8(g);
  memset(k, 0, (size_t)(C / 16) * 9 * 256);
  std::vector<int8_t> w((size_t)C * 9);
  for (int c = 0; c < C; c++)
    for (int t = 0; t < 9; t++) {
      w[c * 9 + t] = (int8_t)uw(g);
      k[((size_t)(c / 16) * 9 + t) * 256 + (c % 16) * 17] = w[c * 9 + t];   // the bundle's diagonal layout
    }
  for (int c = 0; c < C; c++) {
    sf[c] = us(g);
    b[c] = ub(g) + (1 << (sf[c] - 1));   // the round half up is in the bias (VTASave)
  }
  memset(VTABufferGetVirtAddr(vo), 0x5a, no);
  VTABufferFlush(vin, 0, ni);
  VTABufferFlush(vk, 0, (size_t)(C / 16) * 9 * 256);
  VTABufferFlush(vb, 0, C * 4);
  VTABufferFlush(vs, 0, C * 4);
  VTABufferFlush(vo, 0, no);
  // TF SAME asymmetric pads: the bundle passes pad = the start pad and out_h / out_w (vtalib padEnd)
  int rc = dw_convolution_evta((int8_t *)vin, vk, vb, vs, (int8_t *)vo, 1, H, W, C, K, K, p, s, OH, OW, lo, hi, h, sh.nvt,
                               sh.th, sh.tw, 0);
  VTABufferInvalidate(vo, 0, no);
  const int8_t *o = (const int8_t *)VTABufferGetVirtAddr(vo);
  size_t bad = 0;
  for (int c = 0; c < C; c++)
    for (int y = 0; y < OH; y++)
      for (int x = 0; x < OW; x++) {
        int32_t a = b[c];
        for (int ky = 0; ky < K; ky++)
          for (int kx = 0; kx < K; kx++) {
            int iy = y * s - p + ky, ix = x * s - p + kx;
            if (iy < 0 || iy >= H || ix < 0 || ix >= W) continue;
            a += (int32_t)w[c * 9 + ky * 3 + kx] * in[(((size_t)(c / 16) * H + iy) * W + ix) * 16 + c % 16];
          }
        int32_t v = a >> sf[c];
        v = v < lo ? lo : v > hi ? hi : v;
        size_t i = (((size_t)(c / 16) * OH + y) * OW + x) * 16 + c % 16;
        if (o[i] != (int8_t)v && bad++ < 3)
          printf("  [%d] c %d y %d x %d: evta %d cpu %d\n", idx, c, y, x, o[i], v);
      }
  printf("[%2d] %3dx%3dx%3d s%d pad %d/%d vt %d tile %dx%d: rc %d, %zu of %zu differ%s\n", idx, H, W, C, s, p, pe, sh.nvt,
         sh.th, sh.tw, rc, bad, no, bad || rc ? "  FAIL" : "");
  VTABufferFree(vin);
  VTABufferFree(vk);
  VTABufferFree(vb);
  VTABufferFree(vs);
  VTABufferFree(vo);
  return bad || rc;
}

int main(int argc, char **argv) {
  const int only = argc > 1 ? atoi(argv[1]) : -1;
  // MobileNetV2Tf's depthwise shapes and schedules (VTASchedules.h, 11-24), then odd ones
  const Shape cs[] = {
      {112, 112, 32, 1, 1, -1, 2, 4, 112}, {112, 112, 96, 2, 0, 1, 2, 4, 56}, {56, 56, 144, 1, 1, -1, 1, 28, 56},
      {56, 56, 144, 2, 0, 1, 1, 14, 28},   {28, 28, 192, 1, 1, -1, 2, 28, 28}, {28, 28, 192, 2, 0, 1, 2, 14, 14},
      {14, 14, 384, 1, 1, -1, 2, 14, 14},  {14, 14, 576, 1, 1, -1, 2, 14, 14}, {14, 14, 576, 2, 0, 1, 2, 7, 7},
      {7, 7, 960, 1, 1, -1, 2, 7, 7},      {112, 112, 32, 1, 1, -1, 2, 14, 56}, {13, 11, 48, 1, 1, -1, 1, 5, 4},
      {9, 17, 32, 2, 1, -1, 2, 3, 5},      {20, 20, 16, 1, 0, -1, 1, 6, 18},   {6, 6, 32, 2, 1, -1, 2, 1, 3},
  };
  int bad = 0;
  for (int i = 0; i < (int)(sizeof cs / sizeof cs[0]); i++)
    if (only < 0 || only == i) bad |= run(i, cs[i], 100 + i);
  printf("dw_evta_test (window mode %s): %s\n", VTAHasDwUnit() ? "on" : "off", bad ? "FAILED" : "all passed");
  return bad;
}
