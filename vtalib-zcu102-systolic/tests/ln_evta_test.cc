// #### JO added (2026-11-12, ALU extension step 7, docs/evta_isa_ext.md 8.10): layernorm_evta (the integer layer norm as an
// EVTA ALU program, VTABundle.cpp) in the vtalib simulator against the bundles' CPU function nestc_cpu::layerNormI8
// (NESTC_LN_INT=1; cpuops.cpp is extracted from VTASave.cpp by run_lut_conv_test.sh). DeiT shapes (198 x 192) and others,
// rows of varied mean / spread, one gamma / beta for all rows, free scales. Every output byte must be the same.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

#include "Bundle/VTABundle.h"
#if NESTC_USE_VTASIM
#include "simulator/vta/runtime.h"
#else
#include "vta/runtime.h"   // board (run_alux_board_tests.sh): include/zcu102 is on the path
#endif
#include "cpuops.cpp"

static int run(int R, int C, float a2b, float a2d, unsigned seed) {
  std::mt19937 g(seed);
  std::normal_distribution<float> nd(0, 1);
  std::vector<int8_t> x((size_t)R * C), b((size_t)R * C), ref((size_t)R * C), o((size_t)R * C, 0x5a), z(R, 0);
  std::vector<float> w((size_t)R * C);
  for (int r = 0; r < R; r++) {
    const float sd = 1 + 40 * (r % 9) / 8.f, mu = (r % 5) * 6.f - 12;
    for (int l = 0; l < C; l++) {
      float v = mu + sd * nd(g);
      x[(size_t)r * C + l] = (int8_t)lrintf(v > 127 ? 127 : v < -128 ? -128 : v);
    }
  }
  for (int l = 0; l < C; l++) {
    const float gm = 0.2f + 2.f * std::fabs(nd(g));
    const int8_t bb = (int8_t)(nd(g) * 30);
    for (int r = 0; r < R; r++)
      w[(size_t)r * C + l] = gm, b[(size_t)r * C + l] = bb;
  }
  nestc_cpu::layerNormI8(ref.data(), x.data(), R, C, z.data(), z.data(), z.data(), (int8_t *)w.data(), (int8_t *)w.data(),
                         b.data(), 1.f / 16, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, a2b, a2d,
                         1);
  const int done = layernorm_evta(o.data(), x.data(), R, C, w.data(), b.data(), a2b, a2d, VTATLSCommandHandle(0));
  long bad = 0, first = -1;
  for (size_t i = 0; i < o.size(); i++)
    if (o[i] != ref[i] && bad++ == 0)
      first = (long)i;
  printf("layer norm %d x %d (a2b %.4f, a2d %.4f): %s, %ld of %zu differ", R, C, a2b, a2d,
         done ? "on the EVTA" : "NOT RUN", bad, o.size());
  if (first >= 0)
    printf(" (first at row %ld col %ld: cpu %d, evta %d)", first / C, first % C, ref[first], o[first]);
  printf("\n");
  //#### JO added (2026-11-15, docs/evta_isa_ext.md 8.16): layernorm_evta2 with x and / or out packed in VTA buffers
  int fail = !done || bad;
  const size_t n = (size_t)R * C;
  auto pk = [&](size_t r, size_t c) { return (c / 16) * (size_t)R * 16 + r * 16 + c % 16; };
  void *xb = VTABufferAlloc(n), *ob = VTABufferAlloc(n);
  int8_t *xp = (int8_t *)VTABufferGetVirtAddr(xb), *op = (int8_t *)VTABufferGetVirtAddr(ob);
  for (size_t r = 0; r < (size_t)R; r++)
    for (size_t c = 0; c < (size_t)C; c++) xp[pk(r, c)] = x[r * C + c];
  VTABufferFlush(xb, 0, n);
  for (int m = 1; m < 4; m++) {
    const bool xP = m & 1, oP = m & 2;
    std::vector<int8_t> orow(n, 0x5a);
    memset(op, 0x5a, n);
    VTABufferFlush(ob, 0, n);
    const int d = layernorm_evta2(oP ? op : orow.data(), oP ? ob : nullptr, xP ? xp : x.data(), xP ? xb : nullptr, R, C,
                                  w.data(), b.data(), a2b, a2d, VTATLSCommandHandle(0));
    long e = 0;
    for (size_t r = 0; r < (size_t)R; r++)
      for (size_t c = 0; c < (size_t)C; c++)
        e += (oP ? op[pk(r, c)] : orow[r * C + c]) != ref[r * C + c];
    printf("  layernorm_evta2 x %s out %s: %s, %ld differ\n", xP ? "packed" : "row", oP ? "packed" : "row",
           d ? "on the EVTA" : "NOT RUN", e);
    fail |= !d || e;
  }
  VTABufferFree(xb), VTABufferFree(ob);
  return fail;
}

// an EVTA conv before (the layer norms of a bundle run between convs)
static void convBefore(int H, int W, int C, int KN, int vt, int tile) {
  std::vector<int8_t> v((size_t)H * W * C + (size_t)KN * C);
  for (size_t i = 0; i < v.size(); i++) v[i] = (int8_t)(i * 37 % 251 - 120);
  void *in = VTABufferAlloc((size_t)H * W * C), *wt = VTABufferAlloc((size_t)KN * C), *bs = VTABufferAlloc(KN * 4),
       *o = VTABufferAlloc((size_t)H * W * KN);
  memcpy(VTABufferGetVirtAddr(in), v.data(), (size_t)H * W * C);
  memcpy(VTABufferGetVirtAddr(wt), v.data() + (size_t)H * W * C, (size_t)KN * C);
  memset(VTABufferGetVirtAddr(bs), 0, KN * 4);
  convolution_wo_tr((int8_t *)in, (int8_t *)wt, (int32_t *)bs, (int8_t *)o, 1, H, W, C, KN, 1, 1, 0, 1, false, true, 8, H,
                    W, VTATLSCommandHandle(0), vt, tile, W, 0);
  VTABufferFree(in), VTABufferFree(wt), VTABufferFree(bs), VTABufferFree(o);
}

int main(int argc, char **argv) {
  setenv("NESTC_LN_INT", "1", 1);
  int bad = 0;
  if (argc > 1) {
    printf("after an EVTA conv (18 x 11 x 192 -> 768, as the DeiT fc1)\n");
    convBefore(18, 11, 192, 768, 2, 9);
  }
  bad |= run(198, 192, 1.f / 37.3f, 1.f / 21.9f, 1);   // DeiT shape
  bad |= run(198, 192, 1.f / 9.07f, 1.f / 6.37f, 2);
  bad |= run(50, 384, 1.f / 12.f, 1.f / 30.f, 3);       // DeiT-small width, rows not a multiple of the run
  bad |= run(17, 64, 1.f / 50.f, 1.f / 8.f, 4);
  printf(bad ? "FAILED\n" : "all equal\n");
  return bad;
}
