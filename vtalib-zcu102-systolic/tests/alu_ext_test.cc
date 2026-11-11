// #### JO added (2026-11-11, ALU extension, docs/evta_isa_ext.md 8.7): the integer layer norm (docs/evta_isa_ext.md 8.6) as
// a program of ALU instructions (include/zcu102/vta/alu_ext_ref.h), run on random rows and compared byte for byte with
// the CPU function the bundles use (VTASave nestc_cpu::layerNormI8, NESTC_LN_INT=1; cpuops.cpp is extracted from
// VTASave.cpp by run_alu_ext_test.sh). Also counts the ALU steps (one step = one acc entry of 16 lanes) per token.
//
// Layout (acc entries, one token = NB = C / 16 blocks, n tokens per program run):
//   X[t][b] the int8 input as int32, D[t][b] the element work entries, E[t][b] the shifted bias, then per token one entry
//   each of S, Q, W, A, SH, TV, TT, A2, H, K (all 16 lanes the same after RSUM / broadcast), and per block G[b], B[b]
//   (the gamma / out scale and the bias at 2^16 per output step, as layerNormI8), and the 256-entry rsqrt LUT in 16 entries.
// Loops: a broadcast of a per-token entry over the blocks is src_factor_in = 0; per-block constants are
// src_factor_out = 0. Sums over the blocks run blocks outer / tokens inner, so two steps in a row never write the same
// dst (the HLS ALU loop has no forwarding between steps).
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

#include "vta/hw_spec.h"
#include "vta/alu_ext_ref.h"
#include "cpuops.cpp"

using vta_alu_ref::kLanes;

static std::vector<VTAAluInsn> prog;
static std::vector<VTAUop> uops;
static long steps = 0;

// one instruction: iter_out x iter_in steps of one uop, dst = d + io * dfo + ii * dfi, src = s + io * sfo + ii * sfi
static VTAAluInsn &I(int op, int io, int ii, int d, int dfo, int dfi, int s, int sfo, int sfi, bool useImm = false,
                     int imm = 0) {
  VTAAluInsn a;
  memset(&a, 0, sizeof a);
  a.opcode = VTA_OPCODE_ALU;
  a.alu_opcode = op & 3;
  a.alu_op_ext = op >> 2;
  a.use_imm = useImm;
  a.imm = imm;
  a.iter_out = io;
  a.iter_in = ii;
  a.dst_factor_out = dfo;
  a.dst_factor_in = dfi;
  a.src_factor_out = sfo;
  a.src_factor_in = sfi;
  VTAUop u;
  memset(&u, 0, sizeof u);
  u.dst_idx = d;
  u.src_idx = s;
  a.uop_bgn = uops.size();
  uops.push_back(u);
  a.uop_end = uops.size();
  prog.push_back(a);
  steps += (long)io * ii;
  return prog.back();
}

// the integer softmax (VTASave softmaxInt, NESTC_SM_INT=1: 8-bit exp LUT, CLZ + 8-bit reciprocal LUT, a probability with
// 16 fraction bits) as an ALU program over rows of COLS int8 scores (DeiT attention: 197 keys, padded to 13 blocks with a
// very negative score so that their exp is E[255] = 0), compared with the CPU function (its float output * 2^16)
static int softmaxTest(int seed) {
  const int COLS = 197, NB = (COLS + 15) / 16, R = 197 * 3, n = 32;
  const float s = 1.f / 8;
  std::mt19937 g(seed);
  std::normal_distribution<float> nd(0, 1);
  std::vector<int8_t> x(R * COLS);
  for (int r = 0; r < R; r++) {
    const float sd = 2 + 30 * (r % 7) / 6.f;
    for (int c = 0; c < COLS; c++) {
      float v = sd * nd(g) + (r % 3) * 10;
      x[r * COLS + c] = (int8_t)lrintf(v > 127 ? 127 : v < -128 ? -128 : v);
    }
  }
  std::vector<float> ref(R * COLS);
  nestc_cpu::softmaxInt(ref.data(), x.data(), R, COLS, s, 8);
  uint8_t E[256], RT[256];
  for (int k = 0; k < 256; k++) {
    const long e = std::lround(255.0 * std::exp(-(double)k * s));
    E[k] = (uint8_t)(e > 255 ? 255 : e);
    RT[k] = (uint8_t)(k < 128 ? 0 : std::lround(32768.0 / (k + 0.5)));
  }
  prog.clear(), uops.clear(), steps = 0;
  const int X = 0, D = X + n * NB, M = D + n * NB, S = M + n, A = S + n, SH = A + n, RV = SH + n, TT = RV + n, H = TT + n,
            K = H + n, TE = K + n, TR = TE + 16, DEPTH = TR + 16;
  // 1. m = max x (blocks outer), d = min(m - x, 255), e = E[d] (unsigned)
  I(VTA_ALUX_LUTSET, 1, 16, 0, 0, 0, TE, 0, 1, true, 0);
  I(VTA_ALUX_CPY, n, 1, M, 1, 0, X, NB, 0);
  I(VTA_ALU_OPCODE_MAX, NB - 1, n, M, 0, 1, X + 1, 1, NB);
  I(VTA_ALUX_RMAX, n, 1, M, 1, 0, M, 1, 0);
  I(VTA_ALUX_CPY, n, NB, D, NB, 1, M, 1, 0);
  I(VTA_ALUX_SUB, n, NB, D, NB, 1, X, NB, 1);
  I(VTA_ALU_OPCODE_MIN, n, NB, D, NB, 1, 0, 0, 0, true, 255);
  I(VTA_ALUX_LUT, n, NB, D, NB, 1, 0, 0, 0, true, 0);
  // 2. S = sum e; sh = 16 - clz(S); R = RT[(S >> sh) >> 8]
  I(VTA_ALUX_CPY, n, 1, S, 1, 0, D, NB, 0);
  I(VTA_ALU_OPCODE_ADD, NB - 1, n, S, 0, 1, D + 1, 1, NB);
  I(VTA_ALUX_RSUM, n, 1, S, 1, 0, S, 1, 0);
  I(VTA_ALUX_CPY, n, 1, A, 1, 0, S, 1, 0);
  I(VTA_ALUX_CLZ, n, 1, A, 1, 0, 0, 0, 0);
  I(VTA_ALUX_CPY, n, 1, SH, 1, 0, 0, 0, 0, true, 16);
  I(VTA_ALUX_SUB, n, 1, SH, 1, 0, A, 1, 0);
  I(VTA_ALUX_LUTSET, 1, 16, 0, 0, 0, TR, 0, 1, true, 0);
  I(VTA_ALUX_CPY, n, 1, RV, 1, 0, S, 1, 0);
  I(VTA_ALU_OPCODE_SHR, n, 1, RV, 1, 0, SH, 1, 0);
  I(VTA_ALU_OPCODE_SHR, n, 1, RV, 1, 0, 0, 0, 0, true, 8);
  I(VTA_ALUX_LUT, n, 1, RV, 1, 0, 0, 0, 0, true, 0);
  // 3. t = 7 + sh (-1..7: one SHR); H = 1 >> (1 - t); p = (e * R + H) >> t
  I(VTA_ALUX_CPY, n, 1, TT, 1, 0, SH, 1, 0);
  I(VTA_ALU_OPCODE_ADD, n, 1, TT, 1, 0, 0, 0, 0, true, 7);
  I(VTA_ALUX_CPY, n, 1, K, 1, 0, 0, 0, 0, true, 1);
  I(VTA_ALUX_SUB, n, 1, K, 1, 0, TT, 1, 0);
  I(VTA_ALUX_CPY, n, 1, H, 1, 0, 0, 0, 0, true, 1);
  I(VTA_ALU_OPCODE_SHR, n, 1, H, 1, 0, K, 1, 0);
  I(VTA_ALUX_MUL, n, NB, D, NB, 1, RV, 1, 0);
  I(VTA_ALU_OPCODE_ADD, n, NB, D, NB, 1, H, 1, 0);
  I(VTA_ALU_OPCODE_SHR, n, NB, D, NB, 1, TT, 1, 0);
  const long progSteps = steps;
  std::vector<int32_t> accv(DEPTH * kLanes);
  std::vector<int8_t> outv(DEPTH * kLanes);
  int32_t(*acc)[kLanes] = (int32_t(*)[kLanes])accv.data();
  int8_t(*out)[kLanes] = (int8_t(*)[kLanes])outv.data();
  uint8_t T[256] = {0};
  long bad = 0;
  for (int r0 = 0; r0 < R; r0 += n) {
    std::fill(accv.begin(), accv.end(), 0);
    for (int t = 0; t < n; t++)
      for (int c = 0; c < NB * 16; c++)
        acc[X + t * NB + c / 16][c % 16] = r0 + t < R && c < COLS ? x[(r0 + t) * COLS + c] : -(1 << 20);
    for (int k = 0; k < 256; k++)
      acc[TE + k / 16][k % 16] = E[k], acc[TR + k / 16][k % 16] = RT[k];
    for (const VTAAluInsn &a : prog)
      vta_alu_ref::run(a, uops.data(), acc, out, T);
    for (int t = 0; t < n && r0 + t < R; t++)
      for (int c = 0; c < COLS; c++) {
        const long e = std::lround(ref[(r0 + t) * COLS + c] * 65536.0), o = acc[D + t * NB + c / 16][c % 16];
        if (e != o && bad++ == 0)
          printf("softmax first difference: row %d col %d: cpu %ld, alu program %ld\n", r0 + t, c, e, o);
      }
  }
  printf("softmax %d keys, %d rows, %d rows per run: %zu ALU instructions, %ld steps per run = %.1f steps per row; "
         "%ld of %d outputs differ\n",
         COLS, R, n, prog.size(), progSteps, (double)progSteps / n, bad, R * COLS);
  return bad != 0;
}

int main(int argc, char **argv) {
  const int C = 192, NB = C / 16, R = argc > 1 ? atoi(argv[1]) : 197, n = 32;
  std::mt19937 g(argc > 2 ? atoi(argv[2]) : 1);
  std::normal_distribution<float> nd(0, 1);
  // rows of varied mean / spread, one gamma / beta for all rows (as DeiT), scales as a DeiT layer norm
  std::vector<int8_t> x(R * C), b(R * C), ref(R * C), z(R, 0);
  std::vector<float> w(R * C);
  for (int r = 0; r < R; r++) {
    const float sd = 1 + 40 * (r % 9) / 8.f, mu = (r % 5) * 6.f - 12;
    for (int l = 0; l < C; l++) {
      float v = mu + sd * nd(g);
      v = v > 127 ? 127 : v < -128 ? -128 : v;
      x[r * C + l] = (int8_t)lrintf(v);
    }
  }
  for (int l = 0; l < C; l++) {
    const float gm = 0.2f + 2.f * std::fabs(nd(g));
    const int8_t bb = (int8_t)(nd(g) * 30);
    for (int r = 0; r < R; r++)
      w[r * C + l] = gm, b[r * C + l] = bb;
  }
  const float a2b = 1.f / 37.3f, a2d = 1.f / 21.9f;  // free (non-power-of-two) scales
  setenv("NESTC_LN_INT", "1", 1);
  nestc_cpu::layerNormI8(ref.data(), x.data(), R, C, z.data(), z.data(), z.data(), (int8_t *)w.data(), (int8_t *)w.data(),
                         b.data(), 1.f / 16, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, a2b, a2d, 1);

  // the constants as layerNormI8 makes them (8-bit rsqrt LUT, k = 8)
  uint8_t T8[256];
  for (int k = 0; k < 256; k++)
    T8[k] = (uint8_t)(k < 64 ? 0 : std::min(255L, std::lround(255.0 / std::sqrt((k + 0.5) / 64.0))));
  const double gk = 256.0 / ((double)a2d * 255.0 * 128.0);
  double gmax = 0;
  for (int l = 0; l < C; l++) gmax = std::max(gmax, std::fabs((double)w[l]));
  int gs = 0;
  while (gmax * gk * std::ldexp(1.0, gs + 1) < 16383.0 && gs < 40) gs++;
  std::vector<int32_t> G(C), B(C);
  for (int l = 0; l < C; l++) {
    G[l] = (int32_t)std::lround(w[l] * gk * std::ldexp(1.0, gs));
    B[l] = (int32_t)std::lround(b[l] * (double)a2b / a2d * 65536.0);
  }

  // acc layout
  const int X = 0, D = X + n * NB, E = D + n * NB, S = E + n * NB, Q = S + n, W = Q + n, A = W + n, SH = A + n, TV = SH + n,
            TT = TV + n, A2 = TT + n, H = A2 + n, K = H + n, K1 = K + n, GB = K1 + n, BB = GB + NB, TB = BB + NB,
            DEPTH = TB + 16;
  // a variable shift of R entries (per-token amounts in amt) in two SHRs: k1 = amt >> 1, k2 = amt - k1 (-30..30 total)
  auto vshr = [&](int dst, int dfo, int dfi, int io, int ii, int amt, bool last) {
    I(VTA_ALUX_CPY, n, 1, K1, 1, 0, amt, 1, 0);
    I(VTA_ALU_OPCODE_SHR, n, 1, K1, 1, 0, 0, 0, 0, true, 1);            // k1 = amt >> 1
    I(VTA_ALUX_CPY, n, 1, K, 1, 0, amt, 1, 0);
    I(VTA_ALUX_SUB, n, 1, K, 1, 0, K1, 1, 0);                           // k2 = amt - k1
    I(VTA_ALU_OPCODE_SHR, io, ii, dst, dfo, dfi, K1, 1, 0);
    VTAAluInsn &s2 = I(VTA_ALU_OPCODE_SHR, io, ii, dst, dfo, dfi, K, 1, 0);
    if (last) {
      s2.rq_en = 1, s2.rq_round = 0, s2.rq_shift = 0, s2.rq_lo = -128, s2.rq_hi = 127;
    }
  };
  // 0. the LUT from 16 acc entries
  I(VTA_ALUX_LUTSET, 1, 16, 0, 0, 0, TB, 0, 1, true, 0);
  // 1. S = sum x: S[t] = X[t][0], += X[t][b] (blocks outer), then the lanes
  I(VTA_ALUX_CPY, n, 1, S, 1, 0, X, NB, 0);
  I(VTA_ALU_OPCODE_ADD, NB - 1, n, S, 0, 1, X + 1, 1, NB);
  I(VTA_ALUX_RSUM, n, 1, S, 1, 0, S, 1, 0);
  // 2. Q = sum x^2
  I(VTA_ALUX_CPY, n, NB, D, NB, 1, X, NB, 1);
  I(VTA_ALUX_MUL, n, NB, D, NB, 1, X, NB, 1);
  I(VTA_ALUX_CPY, n, 1, Q, 1, 0, D, NB, 0);
  I(VTA_ALU_OPCODE_ADD, NB - 1, n, Q, 0, 1, D + 1, 1, NB);
  I(VTA_ALUX_RSUM, n, 1, Q, 1, 0, Q, 1, 0);
  // 3. W = max(C Q - S^2, 1)
  I(VTA_ALUX_CPY, n, 1, W, 1, 0, Q, 1, 0);
  I(VTA_ALUX_MUL, n, 1, W, 1, 0, 0, 0, 0, true, C);
  I(VTA_ALUX_CPY, n, 1, A, 1, 0, S, 1, 0);
  I(VTA_ALUX_MUL, n, 1, A, 1, 0, S, 1, 0);
  I(VTA_ALUX_SUB, n, 1, W, 1, 0, A, 1, 0);
  I(VTA_ALU_OPCODE_MAX, n, 1, W, 1, 0, 0, 0, 0, true, 1);
  // 4. sh = 16 - clz(W), made even: ((sh + 1) >> 1) << 1
  I(VTA_ALUX_CPY, n, 1, A, 1, 0, W, 1, 0);
  I(VTA_ALUX_CLZ, n, 1, A, 1, 0, 0, 0, 0);
  I(VTA_ALUX_CPY, n, 1, SH, 1, 0, 0, 0, 0, true, 16);
  I(VTA_ALUX_SUB, n, 1, SH, 1, 0, A, 1, 0);
  I(VTA_ALU_OPCODE_ADD, n, 1, SH, 1, 0, 0, 0, 0, true, 1);
  I(VTA_ALU_OPCODE_SHR, n, 1, SH, 1, 0, 0, 0, 0, true, 1);
  I(VTA_ALU_OPCODE_SHR, n, 1, SH, 1, 0, 0, 0, 0, true, -1);
  // 5. TV = T[(W >> sh) >> 8]
  I(VTA_ALUX_CPY, n, 1, TV, 1, 0, W, 1, 0);
  I(VTA_ALU_OPCODE_SHR, n, 1, TV, 1, 0, SH, 1, 0);
  I(VTA_ALU_OPCODE_SHR, n, 1, TV, 1, 0, 0, 0, 0, true, 8);
  I(VTA_ALUX_LUT, n, 1, TV, 1, 0, 0, 0, 0, true, 0);
  // 6. t = gs + sh / 2; A2 = 16 - t (the bias shift right); H = 1 << (t - 1) (0 for t <= 0): 1 shifted right by 1 - t
  I(VTA_ALUX_CPY, n, 1, TT, 1, 0, SH, 1, 0);
  I(VTA_ALU_OPCODE_SHR, n, 1, TT, 1, 0, 0, 0, 0, true, 1);
  I(VTA_ALU_OPCODE_ADD, n, 1, TT, 1, 0, 0, 0, 0, true, gs);
  I(VTA_ALUX_CPY, n, 1, A2, 1, 0, 0, 0, 0, true, 16);
  I(VTA_ALUX_SUB, n, 1, A2, 1, 0, TT, 1, 0);
  I(VTA_ALUX_CPY, n, 1, H, 1, 0, 0, 0, 0, true, 1);
  I(VTA_ALUX_CPY, n, 1, A, 1, 0, 0, 0, 0, true, 1);
  I(VTA_ALUX_SUB, n, 1, A, 1, 0, TT, 1, 0);
  vshr(H, 1, 0, n, 1, A, false);
  // 7. per element: D = ((C x - S) * TV >> 8) * G[b]; E = B[b] >> (16 - t); D = (D + E + H) >> t, clipped to int8 by rq
  I(VTA_ALUX_CPY, n, NB, D, NB, 1, X, NB, 1);
  I(VTA_ALUX_MUL, n, NB, D, NB, 1, 0, 0, 0, true, C);
  I(VTA_ALUX_SUB, n, NB, D, NB, 1, S, 1, 0);
  I(VTA_ALUX_MUL, n, NB, D, NB, 1, TV, 1, 0);
  I(VTA_ALU_OPCODE_SHR, n, NB, D, NB, 1, 0, 0, 0, true, 8);
  I(VTA_ALUX_MUL, n, NB, D, NB, 1, GB, 0, 1);
  I(VTA_ALUX_CPY, n, NB, E, NB, 1, BB, 0, 1);
  vshr(E, NB, 1, n, NB, A2, false);
  I(VTA_ALU_OPCODE_ADD, n, NB, D, NB, 1, E, NB, 1);
  I(VTA_ALU_OPCODE_ADD, n, NB, D, NB, 1, H, 1, 0);
  vshr(D, NB, 1, n, NB, TT, true);
  const long progSteps = steps;

  // run it over the rows, n tokens at a time
  std::vector<int32_t> accv(DEPTH * kLanes);
  std::vector<int8_t> outv(DEPTH * kLanes);
  int32_t(*acc)[kLanes] = (int32_t(*)[kLanes])accv.data();
  int8_t(*out)[kLanes] = (int8_t(*)[kLanes])outv.data();
  uint8_t T[256] = {0};
  long bad = 0, first = -1;
  for (int r0 = 0; r0 < R; r0 += n) {
    std::fill(accv.begin(), accv.end(), 0);
    for (int t = 0; t < n; t++)
      for (int l = 0; l < C; l++)
        acc[X + t * NB + l / 16][l % 16] = r0 + t < R ? x[(r0 + t) * C + l] : 0;
    for (int l = 0; l < C; l++)
      acc[GB + l / 16][l % 16] = G[l], acc[BB + l / 16][l % 16] = B[l];
    for (int k = 0; k < 256; k++)
      acc[TB + k / 16][k % 16] = T8[k];
    for (const VTAAluInsn &a : prog)
      vta_alu_ref::run(a, uops.data(), acc, out, T);
    for (int t = 0; t < n && r0 + t < R; t++)
      for (int l = 0; l < C; l++) {
        const int8_t e = ref[(r0 + t) * C + l], o = out[D + t * NB + l / 16][l % 16];
        if (e != o) {
          if (first < 0) {
            first = (r0 + t) * C + l;
            printf("first difference: row %d col %d: cpu %d, alu program %d\n", r0 + t, l, e, o);
          }
          bad++;
        }
      }
  }
  printf("layer norm C %d, %d rows, %d tokens per run: %zu ALU instructions, %ld steps per run = %.1f steps per token; "
         "%ld of %d outputs differ\n",
         C, R, n, prog.size(), progSteps, (double)progSteps / n, bad, R * C);
  return (bad != 0) | softmaxTest(argc > 2 ? atoi(argv[2]) : 1);
}
