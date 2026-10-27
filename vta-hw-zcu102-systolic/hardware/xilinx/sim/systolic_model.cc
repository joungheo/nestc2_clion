// #### JO added (2026-10-27, systolic Phase C, docs/vta_synth_notes.md §8.5): register-level cycle model of the
// weight-stationary 16x16 GEMM array, checked bit for bit against the sequential GEMM of vta.cc gemm().
//
// Array (PE(i, j): i = input channel row, j = output channel column, one DSP48E2 column per j):
//   A: inp row -> row i delayed i cycles (skew) -> one FF per column (fanout 1), so PE(i, j) sees an item at a+i+j
//   B: per column a 16-byte PISO feeds a B1 chain (BCIN); B2 holds the weight in use (BREG=2, BCASCREG=1)
//      CEB2(i, j) latches B1 -> B2 on the wavefront T+i+j, CEB1(i, j) shifts during [F+i+j, F+15+j] (F >= T)
//   P: P(i, j) = A(i, j) * B2(i, j) + P(i-1, j) (PCIN cascade), column j leaves the array at a+16+j
//   deskew: column j delayed 15-j, so a whole row arrives at the RMW stage at a+31 with its tag (dst, reset)
//   RMW: acc[dst] read, + row, write after RMW_LAT cycles; a read of a dst still in flight takes the forwarded value
// Controller: issues one GEMM per cycle; a new weight block needs its fill started (PISO load) >= 16 cycles before
// the first GEMM that uses it, and a fill can only start after the latch of the block before it. The next distinct
// weight is looked up in a LOOKAHEAD-deep uop FIFO (HLS pushes one uop per cycle).
//
// build: g++ -O2 -std=c++14 -o systolic_model systolic_model.cc
// usage: ./systolic_model [random_tests] [seed]
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <random>
#include <string>
#include <vector>

static const int N = 16;          // VTA_BLOCK_IN = VTA_BLOCK_OUT
static const int L_INP = 2;       // issue -> inp row at the skew input (BRAM with output register)
static const int L_WGT = 3;       // weight address known -> PISO loadable (BRAM read + register)
static const int RMW_LAT = 3;     // acc read -> acc write (BRAM 2 + add 1)
static const int A_OFS = L_INP + 1;
static const int OUT_OFS = A_OFS + 31;   // issue -> row at the RMW stage
static const int FILL = 16;              // weight period
static int LOOKAHEAD = 32;               // uop FIFO between HLS and the array
static int HANDSHAKE = 4;                // done -> next GEMM instruction (HLS)
static bool DRAIN = true;                // wait for done after each GEMM instruction

struct Item { int dst, src, wgt; bool reset; int insn; };
struct Insn { std::vector<Item> items; };

struct Mem {
  std::vector<int8_t> inp, wgt, out; std::vector<int32_t> acc;
  int ninp, nwgt, nacc;
  Mem(int ni, int nw, int na) : inp(ni * N), wgt(nw * N * N), out(na * N), acc(na * N), ninp(ni), nwgt(nw), nacc(na) {}
};

// sequential reference = vta.cc gemm(): acc = reset ? 0 : acc + dot, out = low 8 bits of (acc + dot) even on reset
//### JO changed (2026-10-27, Phase D1): out on reset as in vta.cc (was out = 0), so the RTL can be compared bit for bit
static void reference(const std::vector<Insn> &insns, Mem &m, std::vector<int> &last_writer_reset) {
  for (auto &in : insns)
    for (auto &it : in.items) {
      for (int oc = 0; oc < N; oc++) {
        int32_t tmp = 0;
        for (int ic = 0; ic < N; ic++) tmp += int32_t(m.inp[it.src * N + ic]) * int32_t(m.wgt[(it.wgt * N + oc) * N + ic]);
        int32_t accum = m.acc[it.dst * N + oc] + tmp;
        m.acc[it.dst * N + oc] = it.reset ? 0 : accum;
        m.out[it.dst * N + oc] = int8_t(accum & 0xff);
      }
      last_writer_reset[it.dst] = it.reset;
    }
}

struct Stats { long cycles = 0, items = 0, wstall = 0, fills = 0, forwards = 0, drain = 0; };

struct Tag { bool valid = false; int dst = 0; bool reset = false; };

static Stats simulate(const std::vector<Insn> &insns, Mem &m) {
  Stats st;
  // flattened stream with visibility times
  std::vector<Item> s; std::vector<int> insn_first;
  for (auto &in : insns) { insn_first.push_back(s.size()); for (auto &it : in.items) s.push_back(it); }
  const int ns = s.size();
  std::vector<long> issue_at(ns, -1);

  // array state
  int8_t A[N][N] = {}, B1[N][N] = {}, B2[N][N] = {};
  int32_t P[N][N] = {};
  std::vector<std::deque<int8_t>> skew(N);           // row i: i stages
  for (int i = 0; i < N; i++) skew[i].assign(i, 0);
  std::vector<std::deque<int32_t>> desk(N);          // column j: 15-j stages
  for (int j = 0; j < N; j++) desk[j].assign(N - 1 - j, 0);
  std::deque<Tag> tagq(OUT_OFS, Tag());              // issue -> RMW
  std::deque<std::pair<long, int>> inp_q;            // (cycle at skew input, src)
  struct Wr { long at; int dst; int32_t v[N]; bool reset; };
  std::deque<Wr> wq;

  // weight schedule in issue time: blocks (wgt addr, F fill start, T latch)
  struct Blk { int wgt; long F, T; };
  std::vector<Blk> blks;
  int bdone = 0;          // blocks before this one are latched everywhere
  int cur = -1;           // index into blks of the block in B2 for new issues
  int nxt = -1;           // block being filled
  long last_done = -1;    // cycle the previous instruction's last write lands
  int head = 0;
  int cur_insn = -1;
  long insn_open = 0;     // cycle from which the head instruction's uops are pushed
  bool insn_wait = false; // waiting for done before opening the next instruction

  auto visible_at = [&](int k) -> long {   // uop k enters the FIFO (HLS pushes one per cycle while there is room)
    long t = DRAIN ? insn_open + (k - insn_first[s[k].insn]) : long(k);
    if (k - LOOKAHEAD >= 0) {
      if (issue_at[k - LOOKAHEAD] < 0) return 1L << 60;   // FIFO full
      t = std::max(t, issue_at[k - LOOKAHEAD]);
    }
    return t;
  };
  // next distinct non-reset weight after position k within the same instruction (drain) or stream
  auto next_wgt = [&](int k, int w) -> int {
    for (int q = k; q < ns && q < head + LOOKAHEAD; q++) {
      if (DRAIN && s[q].insn != s[head].insn) break;
      if (s[q].wgt != w) return q;   //### JO changed (Phase D1): was !s[q].reset && ...
    }
    return -1;
  };

  long c = 0;
  int insn_left_writes = 0;
  for (;; c++) {
    // ---------------- controller (issue decision at cycle c) ----------------
    int issue = -1;
    if (head < ns) {
      if (s[head].insn != cur_insn) {             // instruction boundary
        if (!insn_wait) { insn_wait = DRAIN && cur_insn >= 0; }
        if (insn_wait && (insn_left_writes > 0 || c < last_done + HANDSHAKE)) { /* draining */ st.drain++; }
        else { insn_wait = false; cur_insn = s[head].insn; insn_open = c; if (DRAIN) { cur = -1; nxt = -1; } }
      }
      if (s[head].insn == cur_insn && visible_at(head) <= c) {
        const Item &h = s[head];
        //### JO changed (Phase D1): a reset uop uses its own weight like any other (vta.cc computes the dot product and
        //### writes it to out), so no reset bypass: if (h.reset) issue = head; else {
        {
          if (cur >= 0 && blks[cur].wgt == h.wgt) issue = head;
          else {
            if (nxt >= 0 && blks[nxt].wgt != h.wgt) { fprintf(stderr, "prefetched the wrong block\n"); exit(3); }
            if (nxt < 0) {                               // not prefetched: fill now
              long F = std::max(c + L_WGT, cur >= 0 ? blks[cur].T : 0L);
              blks.push_back({h.wgt, F, -1}); nxt = blks.size() - 1; st.fills++;
            }
            if (c >= blks[nxt].F + FILL) { blks[nxt].T = c; cur = nxt; nxt = -1; issue = head; }
            else st.wstall++;
          }
          // prefetch the next distinct weight once the current block is latched
          if (issue >= 0 && cur >= 0 && nxt < 0) {
            int q = next_wgt(head + 1, blks[cur].wgt);
            if (q >= 0) {
              long F = std::max(blks[cur].T, visible_at(q) + L_WGT);
              F = std::max(F, c);
              blks.push_back({s[q].wgt, F, -1}); nxt = blks.size() - 1; st.fills++;
            }
          }
        }
      }
    }
    if (issue >= 0) { issue_at[issue] = c; head++; st.items++; insn_left_writes++; }

    // ---------------- datapath: compute next state from the state of cycle c ----------------
    // inp read (address at issue) -> skew input at c + L_INP
    if (issue >= 0) inp_q.push_back({c + L_INP, s[issue].src});
    int8_t xin[N] = {};
    if (!inp_q.empty() && inp_q.front().first == c) { std::memcpy(xin, &m.inp[inp_q.front().second * N], N); inp_q.pop_front(); }
    // B control in physical time: block b latched for A at T+A_OFS -> CEB2(i, j) at step T+A_OFS+i+j-1
    int8_t nB1[N][N], nB2[N][N];
    std::memcpy(nB1, B1, sizeof B1); std::memcpy(nB2, B2, sizeof B2);
    while (bdone < (int)blks.size() && blks[bdone].T >= 0 && c > blks[bdone].T + A_OFS + 2 * N) bdone++;
    for (int bi = bdone; bi < (int)blks.size(); bi++) {
      const Blk &b = blks[bi];
      long e0 = b.T + A_OFS - 1;
      if (b.T >= 0 && c >= e0 && c <= e0 + 2 * N) {
        for (int i = 0; i < N; i++) for (int j = 0; j < N; j++) if (c == e0 + i + j) nB2[i][j] = B1[i][j];
      }
      long s0 = b.F + A_OFS - 1;   // column 0 window start
      if (c >= s0 && c <= s0 + 2 * N) {
        for (int j = 0; j < N; j++) {
          long sj = s0 + j;
          for (int i = 0; i < N; i++)
            if (c >= sj + i && c <= sj + N - 1) {
              if (i == 0) { int k = c - sj; nB1[0][j] = m.wgt[(b.wgt * N + j) * N + (N - 1 - k)]; }
              else nB1[i][j] = B1[i - 1][j];
            }
        }
      }
    }
    // P cascade and A chain
    int32_t nP[N][N]; int8_t nA[N][N];
    for (int i = 0; i < N; i++)
      for (int j = 0; j < N; j++) {
        nP[i][j] = int32_t(A[i][j]) * int32_t(B2[i][j]) + (i ? P[i - 1][j] : 0);
        nA[i][j] = j ? A[i][j - 1] : (i ? skew[i].front() : xin[0]);
      }
    for (int i = 1; i < N; i++) { skew[i].pop_front(); skew[i].push_back(xin[i]); }
    // deskew: column j output P(15, j) (state of cycle c) -> 15-j stages -> row
    int32_t row[N];
    for (int j = 0; j < N; j++) {
      if (desk[j].empty()) row[j] = P[N - 1][j];
      else { row[j] = desk[j].front(); desk[j].pop_front(); desk[j].push_back(P[N - 1][j]); }
    }
    // tag pipe
    Tag tg = tagq.front(); tagq.pop_front();
    Tag ti; if (issue >= 0) { ti.valid = true; ti.dst = s[issue].dst; ti.reset = s[issue].reset; }
    tagq.push_back(ti);
    // RMW: commit due writes, then read (forwarding) for this row
    while (!wq.empty() && wq.front().at <= c) {
      Wr &w = wq.front();
      for (int j = 0; j < N; j++) { m.acc[w.dst * N + j] = w.reset ? 0 : w.v[j]; m.out[w.dst * N + j] = int8_t(w.v[j] & 0xff); }
      wq.pop_front(); insn_left_writes--; last_done = c;
    }
    if (tg.valid) {
      Wr w; w.at = c + RMW_LAT; w.dst = tg.dst; w.reset = tg.reset;
      const Wr *fw = nullptr;
      for (auto it = wq.rbegin(); it != wq.rend(); ++it) if (it->dst == tg.dst) { fw = &*it; break; }
      if (fw) st.forwards++;
      for (int j = 0; j < N; j++) {
        int32_t old = fw ? (fw->reset ? 0 : fw->v[j]) : m.acc[tg.dst * N + j];
        w.v[j] = old + row[j];
      }
      wq.push_back(w);
    }
    std::memcpy(A, nA, sizeof A); std::memcpy(P, nP, sizeof P);
    std::memcpy(B1, nB1, sizeof B1); std::memcpy(B2, nB2, sizeof B2);

    if (head >= ns && insn_left_writes == 0 && wq.empty()) break;
    if (c > 100000000) { fprintf(stderr, "no progress\n"); exit(2); }
  }
  st.cycles = c + 1;
  return st;
}

// ---------------- instruction generators ----------------
struct Gen {   // VTAUopLoopBegin x2 + uops, as vtalib builds a GEMM instruction
  int eo, do_, so, wo, ei, di, si, wi; std::vector<Item> uops;
  Insn expand(int insn, bool reset) const {
    Insn in;
    for (int a = 0; a < eo; a++) for (int b = 0; b < ei; b++) for (auto u : uops)
      in.items.push_back({u.dst + a * do_ + b * di, u.src + a * so + b * si, u.wgt + a * wo + b * wi, reset, insn});
    return in;
  }
};
// gemmop_mxn of vtalib (old: it_out = x, new: it_out = kernelIdx)
static Gen conv_gen(bool neworder, int kh, int th, int tw, int nk, int stride, int ob, int ib, int wb) {
  Gen g; int kw = kh, itw = (tw - 1) * stride + kw;
  if (!neworder) {
    g = {tw, 1, stride, 0, kw, 0, 1, 1, {}};
    for (int dx = 0; dx < kh; dx++) for (int k = 0; k < nk; k++) for (int y = 0; y < th; y++)
      g.uops.push_back({ob + (y + k * th) * tw, ib + (y * stride + dx) * itw, wb + dx * kw + k * kw * kh, false, 0});
  } else {
    g = {nk, th * tw, 0, kw * kh, kw, 0, 1, 1, {}};
    for (int dx = 0; dx < kh; dx++) for (int y = 0; y < th; y++) for (int x = 0; x < tw; x++)
      g.uops.push_back({ob + y * tw + x, ib + (y * stride + dx) * itw + x * stride, wb + dx * kw, false, 0});
  }
  return g;
}

static int check(const Mem &a, const Mem &b, const std::vector<int> &lastreset) {
  int bad = 0;
  for (size_t i = 0; i < a.acc.size(); i++) bad += a.acc[i] != b.acc[i];
  (void)lastreset;   //### JO changed (Phase D1): out compared for every row (was skipped after a reset)
  for (int d = 0; d < a.nacc; d++) for (int j = 0; j < N; j++) bad += a.out[d * N + j] != b.out[d * N + j];
  return bad;
}

//### JO added (2026-10-27, Phase D1): test vectors for the RTL testbench (rtl/tb/tb_gemm_sa.v).
// ./systolic_model dump <dir> <seed> [hazard]   writes, in $readmemh format (one word per line, hex, lane 0 in the low bits):
//   meta.txt   n_uops n_inp n_wgt n_acc n_last
//   uops.hex   {first, last, reset, wgt[11], src[12], dst[12]} = 38 bits, issue order
//   inp.hex    n_inp x 128 bits (x[ic] in bits 8ic+7:8ic)
//   wgt.hex    n_wgt*16 x 128 bits, line blk*16+j = bank j of block blk = w[j][0..15] (w[j][ic] in bits 8ic+7:8ic)
//   acc0.hex   n_acc x 512 bits (lane oc in bits 32oc+31:32oc), initial accumulator
//   acc.hex, out.hex  expected acc (512 bits) and out (128 bits, lane oc in bits 8oc+7:8oc) after all uops (reference = vta.cc gemm())
// hazard: few dst rows (8), so the same dst comes back within the RMW window often (forwarding). same: dst 0 or 1 only.
// conv: 16 ResConv10-like GEMM instructions (3x3, 7x7 tile, new uop order), for cycles per GEMM.
static void hexline(FILE *f, const uint8_t *bytes, int n) {   // little endian bytes -> big endian hex text
  for (int k = n - 1; k >= 0; k--) fprintf(f, "%02x", bytes[k]);
  fprintf(f, "\n");
}
static int dump_vectors(const char *dir, unsigned seed, bool hazard, bool same, bool conv) {
  std::mt19937 rng(seed);
  auto rnd = [&](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); };
  DRAIN = false; LOOKAHEAD = 32;   // the RTL does not drain between GEMM instructions
  const int NI = 256, NW = 64, NA = 256;
  Mem m(NI, NW, NA);
  for (auto &v : m.inp) v = rnd(-128, 127);
  for (auto &v : m.wgt) v = rnd(-128, 127);
  for (auto &v : m.acc) v = rnd(-100000, 100000);
  std::vector<Insn> insns;
  if (conv) {   // ResConv10-like: 3x3 kernel, 7x7 output tile, new uop order, 16 instructions (one per input channel block)
    for (int q = 0; q < 16; q++) {
      Gen g = conv_gen(true, 3, 7, 7, 1, 1, 0, (q % 2) * 81, (q * 9) % 54);
      insns.push_back(g.expand(q, false));
    }
  }
  int ni = conv ? 0 : rnd(1, 6);
  int dmax = same ? 1 : hazard ? 7 : NA - 1;
  for (int q = 0; q < ni; q++) {
    Gen g{rnd(1, 3), rnd(0, 9), rnd(0, 9), rnd(0, 3), rnd(1, 3), rnd(0, 9), rnd(0, 9), rnd(0, 3), {}};
    if (hazard || same) { g.do_ = rnd(0, 2); g.di = 0; g.wi = 0; g.wo = rnd(0, 1); }   // same dst back to back with one weight
    int nu = rnd(1, 40);
    for (int u = 0; u < nu; u++) g.uops.push_back({rnd(0, dmax), rnd(0, 150), rnd(0, 40), false, 0});
    Insn in = g.expand(q, rnd(0, 7) == 0);
    for (auto &it : in.items) { it.dst %= NA; it.src %= NI; it.wgt %= NW; }
    insns.push_back(in);
  }
  Mem r = m; std::vector<int> lr(r.nacc, 0);
  reference(insns, r, lr);
  auto open = [&](const char *n) { std::string p = std::string(dir) + "/" + n; FILE *f = fopen(p.c_str(), "w"); if (!f) { perror(p.c_str()); exit(1); } return f; };
  int nu = 0, nl = 0;
  FILE *f = open("uops.hex");
  for (auto &in : insns)
    for (size_t k = 0; k < in.items.size(); k++) {
      const Item &it = in.items[k]; bool last = k + 1 == in.items.size();
      uint64_t w = uint64_t(it.dst) | uint64_t(it.src) << 12 | uint64_t(it.wgt) << 24 | uint64_t(it.reset) << 35 | uint64_t(last) << 36 |
                   uint64_t(k == 0) << 37;   // first step of the instruction (gemm_sa refills the weight)
      fprintf(f, "%010llx\n", (unsigned long long)w); nu++; nl += last;
    }
  fclose(f);
  f = open("inp.hex"); for (int i = 0; i < NI; i++) hexline(f, (const uint8_t *)&m.inp[i * N], N); fclose(f);
  f = open("wgt.hex"); for (int b = 0; b < NW; b++) for (int j = 0; j < N; j++) hexline(f, (const uint8_t *)&m.wgt[(b * N + j) * N], N); fclose(f);
  f = open("acc0.hex"); for (int a = 0; a < NA; a++) hexline(f, (const uint8_t *)&m.acc[a * N], 4 * N); fclose(f);
  f = open("acc.hex"); for (int a = 0; a < NA; a++) hexline(f, (const uint8_t *)&r.acc[a * N], 4 * N); fclose(f);
  f = open("out.hex"); for (int a = 0; a < NA; a++) hexline(f, (const uint8_t *)&r.out[a * N], N); fclose(f);
  f = open("meta.txt"); fprintf(f, "%d %d %d %d %d\n", nu, NI, NW, NA, nl); fclose(f);
  // the cycle model on the same vectors (must agree before the RTL is compared)
  Stats st = simulate(insns, m);
  int bad = check(r, m, lr);
  printf("dump %s seed %u: %d uops, %d instructions, model %s (forwarded %ld)\n", dir, seed, nu, nl, bad ? "MISMATCH" : "bit exact", st.forwards);
  return bad ? 1 : 0;
}

int main(int argc, char **argv) {
  if (argc > 1 && !strcmp(argv[1], "dump")) return dump_vectors(argv[2], argc > 3 ? atoi(argv[3]) : 1, argc > 4 && !strcmp(argv[4], "hazard"), argc > 4 && !strcmp(argv[4], "same"), argc > 4 && !strcmp(argv[4], "conv"));
  int ntests = argc > 1 ? atoi(argv[1]) : 300;
  unsigned seed = argc > 2 ? atoi(argv[2]) : 1;
  std::mt19937 rng(seed);
  auto rnd = [&](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); };

  // 1) random instructions, bit exact against the reference
  int fails = 0; long fw = 0;
  for (int t = 0; t < ntests; t++) {
    DRAIN = rnd(0, 1); LOOKAHEAD = rnd(1, 64);
    Mem m(256, 64, 256);
    for (auto &v : m.inp) v = rnd(-128, 127);
    for (auto &v : m.wgt) v = rnd(-128, 127);
    for (auto &v : m.acc) v = rnd(-100000, 100000);
    std::vector<Insn> insns; int ni = rnd(1, 4);
    for (int q = 0; q < ni; q++) {
      Gen g{rnd(1, 3), rnd(0, 9), rnd(0, 9), rnd(0, 3), rnd(1, 3), rnd(0, 9), rnd(0, 9), rnd(0, 3), {}};
      int nu = rnd(1, 20);
      for (int u = 0; u < nu; u++) g.uops.push_back({rnd(0, 150), rnd(0, 150), rnd(0, 40), false, 0});
      insns.push_back(g.expand(q, rnd(0, 7) == 0));
    }
    Mem r = m; std::vector<int> lr(r.nacc, 0);
    reference(insns, r, lr);
    Stats st = simulate(insns, m);
    int bad = check(r, m, lr); fw += st.forwards;
    if (bad) { fails++; if (fails < 5) printf("test %d: %d values wrong (drain %d, lookahead %d)\n", t, bad, DRAIN, LOOKAHEAD); }
  }
  printf("random: %d / %d tests bit exact (forwarded reads %ld)\n", ntests - fails, ntests, fw);

  // 2) conv tiles, old vs new uop order: cycles per GEMM instruction
  struct Case { const char *name; int kh, th, tw, nk, ninsn; };
  Case cases[] = {{"ResConv10-like 3x3 7x7 nk1", 3, 7, 7, 1, 64}, {"3x3 7x7 nk4", 3, 7, 7, 4, 64},
                  {"Conv4-like 3x3 8x56 nk1", 3, 8, 56, 1, 16}, {"1x1 7x7 nk4 (gemmop_1x1 new)", 1, 7, 7, 4, 64}};
  printf("\n%-32s %-5s %8s %8s %8s %8s %8s %7s\n", "case", "order", "GEMMs", "cycles", "wstall", "drain", "fills", "util");
  for (auto &cs : cases)
    for (int neworder = 0; neworder < 2; neworder++)
      for (int drain = 1; drain >= 0; drain--) {
        DRAIN = drain; LOOKAHEAD = 32;
        Mem m(2048, 1024, 2048);
        for (auto &v : m.inp) v = rnd(-128, 127);
        for (auto &v : m.wgt) v = rnd(-128, 127);
        std::vector<Insn> insns;
        for (int q = 0; q < cs.ninsn; q++) {
          Gen g;
          if (cs.kh == 1) {   // gemmop_1x1: old it_out = x, it_in = k; new it_out = k, it_in = x
            g = neworder ? Gen{cs.nk, cs.th * cs.tw, 0, 1, cs.tw, 1, 1, 0, {}} : Gen{cs.tw, 1, 1, 0, cs.nk, cs.th * cs.tw, 0, 1, {}};
            for (int y = 0; y < cs.th; y++) g.uops.push_back({y * cs.tw, y * cs.tw, (q % 8) * cs.nk, false, 0});
          } else g = conv_gen(neworder, cs.kh, cs.th, cs.tw, cs.nk, 1, 0, (q % 2) * 512, (q % 8) * 9 * cs.nk);
          insns.push_back(g.expand(q, false));
        }
        Mem r = m; std::vector<int> lr(r.nacc, 0);
        reference(insns, r, lr);
        Stats st = simulate(insns, m);
        int bad = check(r, m, lr);
        printf("%-32s %-5s %8ld %8ld %8ld %8ld %8ld %6.1f%%%s%s\n", cs.name, neworder ? "new" : "old", st.items, st.cycles,
               st.wstall, st.drain, st.fills, 100.0 * st.items / st.cycles, drain ? "" : "  (no drain)", bad ? "  WRONG" : "");
      }
  return fails != 0;
}
