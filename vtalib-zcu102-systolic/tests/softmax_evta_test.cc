// #### JO added (2026-11-12, ALU extension step 7, docs/evta_isa_ext.md 8.10): softmax_evta (the integer softmax and the
// quantize after it as an EVTA ALU program, VTABundle.cpp) in the vtalib simulator against the CPU function
// nestc_cpu::softmaxIntQ (VTASave -vta-alu-softmax; cpuops.cpp extracted from VTASave.cpp by run_lut_conv_test.sh).
// The DeiT attention (3 heads x 198 rows of 198 keys, s = 0.1055, out scale 1 / 127.1) and other shapes. Every output byte
// must be the same.
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

static int run(int R, int C, float s, float outInv, unsigned seed, bool expectRun = true) {
  std::mt19937 g(seed);
  std::normal_distribution<float> nd(0, 1);
  std::vector<int8_t> x((size_t)R * C), ref((size_t)R * C), o((size_t)R * C, 0x5a);
  for (int r = 0; r < R; r++) {
    const float sd = 2 + 30 * (r % 7) / 6.f;
    for (int c = 0; c < C; c++) {
      float v = sd * nd(g) + (r % 3) * 10;
      x[(size_t)r * C + c] = (int8_t)lrintf(v > 127 ? 127 : v < -128 ? -128 : v);
    }
  }
  nestc_cpu::softmaxIntQ(ref.data(), x.data(), R, C, s, outInv);
  const int done = softmax_evta(o.data(), x.data(), R, C, s, outInv, VTATLSCommandHandle(0));
  long bad = 0, first = -1;
  for (size_t i = 0; i < o.size(); i++)
    if (o[i] != ref[i] && bad++ == 0)
      first = (long)i;
  int nz = 0;
  for (auto v : ref) nz += v != 0;
  printf("softmax %d x %d (s %.4f, out 1/%.2f): %s, %ld of %zu differ, %d nonzero", R, C, s, outInv,
         done ? "on the EVTA" : "not run", done ? bad : 0, o.size(), nz);
  if (done && first >= 0)
    printf(" (first at row %ld col %ld: cpu %d, evta %d)", first / C, first % C, ref[first], o[first]);
  printf("\n");
  return done != expectRun || (done && bad);
}

int main() {
  int bad = 0;
  bad |= run(594, 198, 0.1055316f, 127.115196f, 1);   // DeiT attention
  bad |= run(594, 198, 0.06f, 200.f, 2);
  bad |= run(100, 64, 0.01f, 127.f, 3);               // E[255] != 0, but no padded keys
  bad |= run(40, 37, 0.2f, 255.f, 4);
  bad |= run(10, 197, 0.01f, 127.f, 5, false);        // padded keys with E[255] != 0: not run
  printf(bad ? "FAILED\n" : "all equal\n");
  return bad;
}
