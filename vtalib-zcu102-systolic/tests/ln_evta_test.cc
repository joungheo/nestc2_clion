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
#include "simulator/vta/runtime.h"
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
  return !done || bad;
}

int main() {
  setenv("NESTC_LN_INT", "1", 1);
  int bad = 0;
  bad |= run(198, 192, 1.f / 37.3f, 1.f / 21.9f, 1);   // DeiT shape
  bad |= run(198, 192, 1.f / 9.07f, 1.f / 6.37f, 2);
  bad |= run(50, 384, 1.f / 12.f, 1.f / 30.f, 3);       // DeiT-small width, rows not a multiple of the run
  bad |= run(17, 64, 1.f / 50.f, 1.f / 8.f, 4);
  printf(bad ? "FAILED\n" : "all equal\n");
  return bad;
}
