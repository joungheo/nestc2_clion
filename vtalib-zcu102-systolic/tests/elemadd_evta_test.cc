// #### JO added (2026-11-15, docs/evta_isa_ext.md 8.17): elemadd_evta (the integer elemadd a A + b B >> N on the EVTA ALU,
// VTABundle.cpp) in the vtalib simulator / on the board against the bundles' CPU function nestc_cpu::elemaddInt (cpuops.cpp
// from VTASave.cpp), every byte; also how many bytes the integer elemadd differs from the float elemadd (information).
// DeiT residual-add scales and sizes (38016 = 198 x 192), a short one (< 40 entries), several runs, in place (out = a).
#include <algorithm>
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
#include "vta/runtime.h"
#endif
#include "cpuops.cpp"

static bool params(float sa, float sb, float so, int *A, int *B, int *N) {   // = VTASave elemAddIntParams
  const double ka = (double)sa / so, kb = (double)sb / so, k = std::max(std::fabs(ka), std::fabs(kb));
  int n = 24;
  while (n > 0 && std::llround(k * std::ldexp(1.0, n)) > 32767) n--;
  if (n < 1) return false;
  n += 7;
  *A = (int)std::llround(ka * std::ldexp(1.0, n)), *B = (int)std::llround(kb * std::ldexp(1.0, n)), *N = n;
  return true;
}

static bool precise = true;
static int run(int n, float sa, float sb, float so, unsigned seed, bool inPlace) {
  std::mt19937 g(seed);
  std::normal_distribution<float> nd(0, 1);
  int A, B, N;
  if (!params(sa, sb, so, &A, &B, &N)) { printf("no params\n"); return 1; }
  if (!precise) A = (A + 64) >> 7 << 7, B = (B + 64) >> 7 << 7;   // ~ VTASave without -vta-elemadd-precise (Al = Bl = 0)
  void *ab = VTABufferAlloc(n), *bb = VTABufferAlloc(n), *ob = inPlace ? ab : VTABufferAlloc(n);
  int8_t *a = (int8_t *)VTABufferGetVirtAddr(ab), *b = (int8_t *)VTABufferGetVirtAddr(bb), *o = (int8_t *)VTABufferGetVirtAddr(ob);
  for (int i = 0; i < n; i++) {
    const float x = 30 * nd(g) * (1 + (i / 192) % 4), y = 40 * nd(g);
    a[i] = (int8_t)std::max(-128.f, std::min(127.f, std::round(x)));
    b[i] = (int8_t)std::max(-128.f, std::min(127.f, std::round(y)));
  }
  std::vector<int8_t> ref(n), fl(n), a0(a, a + n);
  nestc_cpu::elemaddInt(a, b, ref.data(), n, A, B, N);
  elemadd(a0.data(), sa, 0, b, sb, 0, fl.data(), so, 0, n);
  if (!inPlace) { memset(o, 0x5a, n); VTABufferFlush(ob, 0, n); }
  VTABufferFlush(ab, 0, n), VTABufferFlush(bb, 0, n);
  const int done = elemadd_evta(ob, ab, bb, n, A, B, N, VTATLSCommandHandle(0));
  long bad = 0, first = -1, fd = 0;
  for (int i = 0; i < n; i++) {
    if (o[i] != ref[i] && bad++ == 0) first = i;
    fd += fl[i] != ref[i];
  }
  printf("elemadd %d (A %d B %d N %d%s): %s, %ld differ", n, A, B, N, inPlace ? ", in place" : "", done ? "on the EVTA" : "NOT RUN", bad);
  if (first >= 0) printf(" (first at %ld: cpu %d, evta %d)", first, ref[first], o[first]);
  long xi = 0, xf = 0;   // against the exact value (double)
  for (int i = 0; i < n; i++) {
    const double x = a0[i] * ((double)sa / so) + b[i] * ((double)sb / so);
    const double q = std::max(-128.0, std::min(127.0, std::round(x)));
    xi += ref[i] != q, xf += fl[i] != q;
  }
  printf("; integer vs float elemadd: %ld of %d differ; not the exact rounding: integer %ld, float %ld\n", fd, n, xi, xf);
  // the CPU path's NEON vs scalar (n not a multiple of 16 -> the scalar tail too)
  std::vector<int8_t> r2(n);
  for (int i = 0; i < n; i++) {
    const int32_t v = a0[i] * A + b[i] * B, q = (v + (1 << (N - 1)) - (v < 0 ? 1 : 0)) >> N;
    r2[i] = (int8_t)(q > 127 ? 127 : q < -128 ? -128 : q);
  }
  const bool cpuOk = !memcmp(r2.data(), ref.data(), n);
  if (!cpuOk) printf("  elemaddInt differs from the scalar formula\n");
  VTABufferFree(ab), VTABufferFree(bb);
  if (!inPlace) VTABufferFree(ob);
  return !done || bad || !cpuOk;
}

int main() {
  int bad = 0;
  for (int pr = 1; pr >= 0; pr--) {
  precise = pr;
  printf(pr ? "22-bit A / B (-vta-elemadd-precise)\n" : "15-bit A / B (A, B multiples of 2^7: MUL + MADD)\n");
  bad |= run(38016, 1 / 31.836929f, 1 / 9.123440f, 1 / 11.196388f, 1, false);   // DeiT layer 0 attention add
  bad |= run(38016, 1 / 8.011114f, 1 / 11.196388f, 1 / 10.123664f, 2, false);   // DeiT layer 0 mlp add
  bad |= run(38016, 1 / 9.619959f, 1 / 10.705478f, 1 / 8.223184f, 3, true);
  bad |= run(16 * 30, 1 / 20.f, 1 / 7.f, 1 / 9.f, 4, false);                    // one short run (< 40 entries)
  bad |= run(16 * 1100, 1 / 3.f, 1 / 50.f, 1 / 2.5f, 5, false);                 // 3 runs, large ratio
  bad |= run(16 * 2049, 1 / 12.f, 1 / 12.f, 1 / 12.f, 6, false);                // equal scales, 5 runs (last 1 entry)
  }
  printf(bad ? "FAILED\n" : "all equal\n");
  return bad;
}
