// #### JO added (2026-11-14, docs/evta_alux_epilogue.md): the conv epilogue (VTASetConvEpilogue) on the EVTA against the plain
// conv followed by the same program on the CPU (VTAEpilogueRef), in the vtalib simulator. Every output byte must be the same,
// whether the program ran on the EVTA (lowerEpilogue) or fell back to the CPU. Cases: the patterns of the current models
// (Resnet18 / 50 residual + relu, MobileNetV2 residual without relu, DeiT GELU table), new orders (a table before / after the
// add, ReLU6 after the add, a shift before the add, in place), programs the EVTA cannot do (two tables, two adds), and random
// programs. tests/run_epi_test.sh runs it with the requantize off / on and without the extended ALU.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "Bundle/VTABundle.h"
#if NESTC_USE_VTASIM
#include "simulator/vta/runtime.h"
#else
#include "vta/runtime.h"   // board: include/zcu102 is on the path
#endif

struct Shape {
  int H, W, C, KN, K, pad, nvt, tile;
};

static void *vbufRand(size_t n, std::mt19937 &g, int lo = -128, int hi = 127) {
  std::uniform_int_distribution<int> u(lo, hi);
  void *b = VTABufferAlloc(n);
  int8_t *p = (int8_t *)VTABufferGetVirtAddr(b);
  for (size_t i = 0; i < n; i++) p[i] = (int8_t)u(g);
  VTABufferFlush(b, 0, n);
  return b;
}

// E's tensors: t[0] (t[1]) are made here unless inPlace (t[0] = the output, filled with random values first)
static int run(const char *name, const Shape &s, bool relu, int clipHi, int shift, VTAEpilogue E, bool inPlace,
               unsigned seed) {
  std::mt19937 g(seed);
  const int OH = s.H + 2 * s.pad - s.K + 1, OW = s.W + 2 * s.pad - s.K + 1;
  const size_t nIn = (size_t)s.H * s.W * s.C, nW = (size_t)s.KN * s.K * s.K * s.C, n = (size_t)OH * OW * s.KN;
  void *h = VTATLSCommandHandle(0);
  void *vin = vbufRand(nIn, g), *vw = vbufRand(nW, g, -32, 31);
  std::vector<int32_t> bias(s.KN);
  std::uniform_int_distribution<int> ub(-3000, 3000);
  for (auto &v : bias) v = ub(g);
  void *vb = VTABufferAlloc(bias.size() * 4);
  memcpy(VTABufferGetVirtAddr(vb), bias.data(), bias.size() * 4);
  VTABufferFlush(vb, 0, bias.size() * 4);
  void *vo = vbufRand(n, g);
  void *vt[2] = {nullptr, nullptr};
  int nt = 0;
  for (int i = 0; i < E.n; i++)
    if (E.s[i].op == VTA_EPI_ADD) nt = std::max(nt, E.s[i].a + 1);
  for (int k = 0; k < nt; k++) vt[k] = (inPlace && k == 0) ? vo : vbufRand(n, g);
  for (int k = 0; k < nt; k++) E.t[k] = vt[k];
  // the tensors' values before the call (the in-place one is overwritten by it)
  std::vector<int8_t> tv[2];
  for (int k = 0; k < nt; k++) {
    VTABufferInvalidate(vt[k], 0, n);
    tv[k].assign((int8_t *)VTABufferGetVirtAddr(vt[k]), (int8_t *)VTABufferGetVirtAddr(vt[k]) + n);
  }
  VTASetActivationClip(clipHi);
  // the reference: the plain conv into a scratch buffer, then the program on the CPU
  void *vc = VTABufferAlloc(n);
  VTASetConvEpilogue(nullptr);
  convolution_wo_tr((int8_t *)vin, (int8_t *)vw, (int32_t *)vb, (int8_t *)vc, 1, s.H, s.W, s.C, s.KN, s.K, s.K, s.pad, 1,
                    relu, true, shift, OH, OW, h, s.nvt, s.tile, s.tile, 0);
  VTABufferInvalidate(vc, 0, n);
  const int8_t *c = (const int8_t *)VTABufferGetVirtAddr(vc);
  std::vector<int8_t> ref(n);
  for (size_t i = 0; i < n; i++) {
    int8_t t2[2] = {nt > 0 ? tv[0][i] : (int8_t)0, nt > 1 ? tv[1][i] : (int8_t)0};
    ref[i] = VTAEpilogueRef(&E, c[i], t2);
  }
  // the fused call
  VTASetConvEpilogue(&E);
  convolution_wo_tr((int8_t *)vin, (int8_t *)vw, (int32_t *)vb, (int8_t *)vo, 1, s.H, s.W, s.C, s.KN, s.K, s.K, s.pad, 1,
                    relu, true, shift, OH, OW, h, s.nvt, s.tile, s.tile, 0);
  VTASetConvEpilogue(nullptr);
  VTASetActivationClip(127);
  VTABufferInvalidate(vo, 0, n);
  const int8_t *o = (const int8_t *)VTABufferGetVirtAddr(vo);
  long diff = 0, first = -1;
  for (size_t i = 0; i < n; i++)
    if (o[i] != ref[i]) {
      if (first < 0) first = (long)i;
      diff++;
    }
  std::vector<int> hist(256, 0);
  for (size_t i = 0; i < n; i++) hist[(uint8_t)o[i]]++;
  int used = 0;
  for (int k = 0; k < 256; k++) used += hist[k] > 0;
  printf("%-34s %2dx%2dx%2d -> %2d %dx%d nvt %d tile %2d relu %d/%3d: %zu outputs, %3d values, %ld differ", name, s.H, s.W,
         s.C, s.KN, s.K, s.K, s.nvt, s.tile, (int)relu, clipHi, n, used, diff);
  if (first >= 0) printf(" (first at %ld: evta %d, conv %d, cpu %d)", first, o[first], c[first], ref[first]);
  printf("\n");
  VTABufferFree(vin), VTABufferFree(vw), VTABufferFree(vb), VTABufferFree(vc);
  for (int k = 0; k < nt; k++)
    if (vt[k] != vo) VTABufferFree(vt[k]);
  VTABufferFree(vo);
  return diff != 0;
}

static VTAEpilogue prog(std::initializer_list<VTAEpiStep> steps, const int8_t *l0 = nullptr, const int8_t *l1 = nullptr) {
  VTAEpilogue E = {};
  for (auto &s : steps) E.s[E.n++] = s;
  E.lut[0] = l0;
  E.lut[1] = l1;
  return E;
}

int main(int argc, char **argv) {
  const int only = argc > 1 ? atoi(argv[1]) : -1;
  int bad = 0, idx = 0;
#define CASE(...) do { if (only < 0 || only == idx) bad |= run(__VA_ARGS__); idx++; } while (0)
  int8_t gelu[256], rnd[256], sig[256];
  for (int k = 0; k < 256; k++) {
    const double x = (int8_t)k / 16.0, y = 0.5 * x * (1 + std::erf(x / std::sqrt(2.0)));
    const long q = std::lround(y * 32);
    gelu[k] = (int8_t)(q > 127 ? 127 : q < -128 ? -128 : q);
    sig[k] = (int8_t)std::lround(127 / (1 + std::exp(-(int8_t)k / 16.0)));
  }
  std::mt19937 g(7);
  for (int k = 0; k < 256; k++) rnd[k] = (int8_t)(g() & 255);
  const Shape a = {14, 14, 32, 32, 3, 1, 2, 7}, b = {28, 28, 16, 64, 1, 0, 1, 14}, c = {13, 13, 32, 32, 3, 1, 1, 7};
  const int ADD = VTA_EPI_ADD, RQ = VTA_EPI_RQ, CLIP = VTA_EPI_CLIP, SHL = VTA_EPI_SHL, LUT = VTA_EPI_LUT;
  // the current models
  CASE("residual, no relu (MobileNetV2)", a, false, 127, 9, prog({{ADD, 0, 1, 0}, {RQ, 2, 1, -128, 127}}), false, 1);
  CASE("residual + relu (Resnet18)", a, true, 127, 9, prog({{ADD, 0, 0, 1}, {RQ, 2, 1, -128, 127}, {CLIP, 0, 127}}),
       false, 2);
  CASE("relu to half scale (Resnet50)", b, false, 127, 8,
       prog({{ADD, 0, 1, 0}, {RQ, 2, 1, -128, 127}, {CLIP, 0, 127}, {SHL, 1}, {CLIP, -128, 127}}), false, 3);
  CASE("GELU table (DeiT fc1)", b, false, 127, 8, prog({{LUT, 0}}, gelu), false, 4);
  CASE("relu6 conv + table", c, true, 48, 9, prog({{LUT, 0}}, rnd), false, 5);
  // new orders
  CASE("add, then a table", a, false, 127, 9, prog({{ADD, 0, 0, 0}, {RQ, 2, 1, -128, 127}, {LUT, 0}}, sig), false, 6);
  CASE("a table, then add + relu", a, false, 127, 9, prog({{LUT, 0}, {ADD, 0, 0, 1}, {RQ, 2, 1, -128, 127},
                                                          {CLIP, 0, 127}}, sig), false, 7);
  CASE("add + ReLU6", c, false, 127, 9, prog({{ADD, 0, 0, 0}, {RQ, 2, 1, -128, 127}, {CLIP, 0, 48}}), false, 8);
  CASE("shift + clip, then add", a, true, 127, 10, prog({{SHL, 1}, {CLIP, 0, 127}, {ADD, 0, 0, 0}, {RQ, 1, 1, -128, 127}}),
       false, 9);
  CASE("a clip only", b, false, 127, 8, prog({{CLIP, -20, 50}}), false, 10);
  CASE("a shift only", b, false, 127, 10, prog({{SHL, 2}, {CLIP, -128, 127}}), false, 11);
  CASE("add + relu in place", a, true, 127, 9, prog({{ADD, 0, 0, 1}, {RQ, 2, 1, -128, 127}, {CLIP, 0, 127}}), true, 12);
  CASE("add (shifts 2 / 3) + relu6", c, false, 127, 9, prog({{ADD, 0, 2, 3}, {RQ, 2, 4, -128, 127}, {CLIP, 0, 96}}), false,
       13);
  // not on the EVTA: the CPU does them
  CASE("two tables (cpu)", a, false, 127, 9, prog({{LUT, 0}, {ADD, 0, 0, 0}, {RQ, 2, 1, -128, 127}, {LUT, 1}}, sig, rnd),
       false, 14);
  CASE("two adds (cpu)", a, false, 127, 9, prog({{ADD, 0, 0, 0}, {RQ, 2, 1, -128, 127}, {ADD, 1, 0, 0},
                                                  {RQ, 2, 1, -128, 127}}), false, 15);
  // random programs: [unary]* [ADD RQ] [unary]*
  std::mt19937 r(11);
  auto ri = [&](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(r); };
  for (int k = 0; k < 24; k++) {
    VTAEpilogue E = {};
    E.lut[0] = (k & 1) ? sig : rnd;
    bool usedLut = false;
    auto unary = [&]() {
      const int u = ri(0, usedLut ? 1 : 2);
      if (u == 0) {
        int lo = ri(-128, 20), hi = ri(lo, 127);
        E.s[E.n++] = {CLIP, lo, hi, 0, 0};
      } else if (u == 1) {
        E.s[E.n++] = {SHL, ri(1, 3), 0, 0, 0};
        E.s[E.n++] = {CLIP, -128, 127, 0, 0};
      } else {
        E.s[E.n++] = {LUT, 0, 0, 0, 0};
        usedLut = true;
      }
    };
    for (int u = ri(0, 1); u > 0; u--) unary();
    if (ri(0, 3)) {
      int lo = ri(-128, 0), hi = ri(std::max(lo, 0), 127);
      E.s[E.n++] = {ADD, 0, ri(0, 3), ri(0, 3), 0};
      E.s[E.n++] = {RQ, ri(0, 2), ri(0, 5), lo, hi};
    }
    for (int u = ri(0, 2); u > 0 && E.n < VTA_EPI_MAX_STEPS - 1; u--) unary();
    const Shape &s = (k % 3 == 0) ? a : (k % 3 == 1) ? b : c;
    const std::string name = "random " + std::to_string(k) + " (" + std::to_string(E.n) + " steps)";
    CASE(name.c_str(), s, (k & 2) != 0, (k & 4) ? 60 : 127, 8 + (k % 3), E, (k % 5) == 4, 100 + k);
  }
  printf(bad ? "FAILED\n" : "all equal\n");
  return bad;
}
