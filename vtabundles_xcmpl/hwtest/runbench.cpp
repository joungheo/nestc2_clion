// #### JO added (2026-11-02): fixed cost of an EVTA run and per-instruction costs (docs/evta_runtime_compare.md 7.14).
// Every case is: [LOADs] -> compute: GEMM reset (1 uop) [+ GEMM / ALU instructions] -> STORE 1 block [+ STOREs], one
// VTASynchronize. The time of VTASynchronize (flush of the instructions and uops, start, done poll) is taken per rep, the
// median printed.
//   base               the smallest run (reset, store)
//   gemm   N K         N GEMM instructions, each 1 uop x K iterations (same uop kernel: loaded once)
//   gemmu  N K         the same with N different uop kernels (one uop LOAD per instruction, as in the bundles)
//   aluimm N K         N ALU SHR-by-0 instructions with an immediate, K iterations each
//   alureg N K         N ALU ADD instructions acc[dst] += acc[src], K iterations each
//   load   N K         N inp LOADs of K blocks before the compute
//   store  N K         N STOREs of K blocks after the compute
// usage: sudo ./runbench <case> <N> <K> [<case> <N> <K> ...]   (REPS=<n>, default 200; all cases in one process: the
//        runtime's CMA is only given back by an xlnk_reset)
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "vta/runtime.h"
#include "vta/data_buf.h"

#define EVTA_ID 0

using namespace vta;

struct Arg {
  uint32_t k, mode, op, imm, salt;
};

static int uop_reset(void *, VTACommandHandle h) {
  VTAUopLoopBegin(h, 1, 0, 0, 0);
  VTAUopPush(h, 0, 1, 0, 0, 0, 0, 0, 0);
  VTAUopLoopEnd(h);
  return 0;
}

// GEMM (mode 0): acc[i] += inp[i] x wgt[0]; ALU (mode 1): acc[i] = op(acc[i], imm or acc[2048 + i]). salt only makes
// the uop kernels differ (wgt index)
static int uop_body(void *sig, VTACommandHandle h) {
  Arg *a = static_cast<Arg *>(sig);
  VTAUopLoopBegin(h, a->k, 1, 1, 0);
  if (a->mode == 0)
    VTAUopPush(h, 0, 0, 0, 0, a->salt, 0, 0, 0);
  else
    VTAUopPush(h, 1, 0, 0, 2048, 0, a->op, a->imm, 0);
  VTAUopLoopEnd(h);
  return 0;
}

static double now_us() {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec * 1e6 + t.tv_nsec * 1e-3;
}

int main(int argc, char **argv) {
  int reps = getenv("REPS") ? atoi(getenv("REPS")) : 200;
  DataBuffer *inp = (DataBuffer *)VTABufferAlloc(4096 * 16);
  DataBuffer *out = (DataBuffer *)VTABufferAlloc(4096 * 16);
  std::vector<int8_t> z(4096 * 16, 1);
  VTABufferCopy(z.data(), 0, inp, 0, z.size(), 1);
  VTACommandHandle h = VTATLSCommandHandle(EVTA_ID);
  for (int ai = 1; ai < argc; ai += 3) {
  std::string c = argv[ai];
  uint32_t N = ai + 1 < argc ? atoi(argv[ai + 1]) : 0, K = ai + 2 < argc ? atoi(argv[ai + 2]) : 1;
  std::vector<void *> uh(2 * (N + 2), nullptr);
  std::vector<Arg> args(N + 1);
  std::vector<double> t;
  for (int r = 0; r < reps; r++) {
    bool loads = c == "load" && N;
    if (loads) {
      for (uint32_t i = 0; i < N; i++) VTALoadBuffer2D(h, inp, 0, K, 1, K, 0, 0, 0, 0, 0, 2);
      VTADepPush(h, 1, 2);
    }
    if (loads) VTADepPop(h, 1, 2);
    VTAPushGEMMOp(h, &uh[0], &uop_reset, nullptr, 0);
    for (uint32_t i = 0; i < N && (c == "gemm" || c == "gemmu" || c == "aluimm" || c == "alureg"); i++) {
      Arg &a = args[c == "gemmu" ? i : 0];
      a = {K, c[0] == 'a' ? 1u : 0u, c == "aluimm" ? 3u : 2u, c == "aluimm" ? 1u : 0u, c == "gemmu" ? i % 64 : 0u};
      if (a.mode == 0)
        VTAPushGEMMOp(h, &uh[2 + (c == "gemmu" ? i : 0)], &uop_body, &a, sizeof(a));
      else
        VTAPushALUOp(h, &uh[2 + 0], &uop_body, &a, sizeof(a));
    }
    VTADepPush(h, 2, 3);
    VTADepPop(h, 2, 3);
    VTAStoreBuffer2D(h, 0, 4, out, 0, 1, 1, 1);
    for (uint32_t i = 0; c == "store" && i < N; i++) VTAStoreBuffer2D(h, 0, 4, out, 0, K, 1, K);
    double t0 = now_us();
    VTASynchronize(h, 1u << 31);
    t.push_back(now_us() - t0);
  }
  std::sort(t.begin() + 1, t.end());
  printf("RUNBENCH %s N %u K %u median %.2f us min %.2f us (first %.1f)\n", c.c_str(), N, K, t[1 + (t.size() - 1) / 2],
         t[1], t[0]);
  fflush(stdout);
  }
  VTARuntimeShutdown(EVTA_ID);
  VTABufferFree(inp);
  VTABufferFree(out);
  return 0;
}
