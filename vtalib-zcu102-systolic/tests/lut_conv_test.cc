// #### JO added (2026-11-11, ALU extension, docs/evta_isa_ext.md 8.9): the output LUT of an EVTA conv as an ALU pass
// (VTASetOutputLut: LUTSET once, then per tile an ALU CPY with lut_en before the STORE) against the way it is done now
// (the conv, then the table on the CPU over its int8 output), in the vtalib simulator (the ALU extension runs there through
// include/zcu102/vta/alu_ext_ref.h). Every output byte must be the same. Shapes: the DeiT fc1 as VTA.cpp matMulToConv makes
// it (197 tokens as a 197 x 1 image, 192 -> 768, 1x1), and a 3x3 conv with relu over several tiles.
// Build and run: tests/run_lut_conv_test.sh
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

#include "Bundle/VTABundle.h"
#include "simulator/vta/runtime.h"

static int run(const char *name, int H, int W, int C, int KN, int K, int pad, bool relu, int shift, int nvt, int tile,
               const int8_t *T, unsigned seed) {
  std::mt19937 g(seed);
  std::uniform_int_distribution<int> u8(-128, 127), ub(-3000, 3000);
  const int OH = H + 2 * pad - K + 1, OW = W + 2 * pad - K + 1;
  std::vector<int8_t> in((size_t)H * W * C), wgt((size_t)KN * K * K * C), a((size_t)OH * OW * KN), b(a.size());
  std::vector<int32_t> bias(KN);
  for (auto &v : in) v = (int8_t)u8(g);
  for (auto &v : wgt) v = (int8_t)(u8(g) / 4);
  for (auto &v : bias) v = ub(g);
  void *h = VTATLSCommandHandle(0);
  // the conv takes VTA buffers in the EVTA layouts (input N{C/16}HW16, filter {KN/16}{C/16}KhKw16x16, bias int32 [KN],
  // output N{KN/16}HW16); random contents, so the layouts do not matter here: both runs read the same buffers
  auto vbuf = [](const void *src, size_t n) {
    void *b = VTABufferAlloc(n);
    memcpy(VTABufferGetVirtAddr(b), src, n);
    VTABufferFlush(b, 0, n);
    return b;
  };
  void *vin = vbuf(in.data(), in.size()), *vw = vbuf(wgt.data(), wgt.size()), *vb = vbuf(bias.data(), bias.size() * 4);
  void *vo = VTABufferAlloc(a.size());
  auto conv = [&](std::vector<int8_t> &out) {
    memset(VTABufferGetVirtAddr(vo), 0x5a, out.size());
    VTABufferFlush(vo, 0, out.size());
    convolution_wo_tr((int8_t *)vin, (int8_t *)vw, (int32_t *)vb, (int8_t *)vo, 1, H, W, C, KN, K, K, pad, 1, relu, true,
                      shift, OH, OW, h, nvt, tile, tile, 0);
    VTABufferInvalidate(vo, 0, out.size());
    memcpy(out.data(), VTABufferGetVirtAddr(vo), out.size());
  };
  // now: the conv, then T on the CPU
  VTASetOutputLut(nullptr);
  conv(a);
  for (auto &v : a) v = T[(uint8_t)v];
  // the ALU pass
  VTASetOutputLut(T);
  conv(b);
  VTASetOutputLut(nullptr);
  VTABufferFree(vin), VTABufferFree(vw), VTABufferFree(vb), VTABufferFree(vo);
  long diff = 0, first = -1;
  for (size_t i = 0; i < a.size(); i++)
    if (a[i] != b[i]) {
      if (first < 0) first = (long)i;
      diff++;
    }
  std::vector<int> hist(256, 0);
  for (auto v : b) hist[(uint8_t)v]++;
  int used = 0;
  for (int k = 0; k < 256; k++) used += hist[k] > 0;
  printf("%-28s %dx%dx%d -> %d, %dx%d, nvt %d tile %d: %zu outputs, %d distinct values, %ld differ", name, H, W, C, KN, K, K,
         nvt, tile, a.size(), used, diff);
  if (first >= 0) printf(" (first at %ld: cpu table %d, alu pass %d)", first, a[first], b[first]);
  printf("\n");
  return diff != 0;
}

int main(int argc, char **argv) {
  const int only = argc > 1 ? atoi(argv[1]) : -1;   // run one case (0..)
  int idx = 0;
#define CASE(...) if (only < 0 || only == idx) bad |= run(__VA_ARGS__); idx++;
  // GELU on int8 (input scale 1/16, output scale 1/32) and a random table
  int8_t gelu[256], rnd[256];
  for (int k = 0; k < 256; k++) {
    const double x = (int8_t)k / 16.0, y = 0.5 * x * (1 + std::erf(x / std::sqrt(2.0)));
    const long q = std::lround(y * 32);
    gelu[k] = (int8_t)(q > 127 ? 127 : q < -128 ? -128 : q);
  }
  std::mt19937 g(7);
  for (int k = 0; k < 256; k++) rnd[k] = (int8_t)(g() & 255);
  int bad = 0;
  CASE("DeiT fc1 + GELU table", 197, 1, 192, 768, 1, 0, false, 8, 2, 14, gelu, 1);
  CASE("DeiT fc1 + random table", 197, 1, 192, 768, 1, 0, false, 8, 2, 14, rnd, 2);
  CASE("3x3 relu + random table", 28, 28, 32, 64, 3, 1, true, 9, 2, 14, rnd, 3);
  CASE("3x3 relu nvt 1 + GELU", 14, 14, 64, 32, 3, 1, true, 10, 1, 7, gelu, 4);
  printf(bad ? "FAILED\n" : "all equal\n");
  return bad;
}
