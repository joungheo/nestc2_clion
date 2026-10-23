// #### JO added (2026-10-23): reproduce the done race of VTASynchronize (pynq_driver.cc Run()).
// compute's done_o register keeps the 1 written by the previous run's FINISH until compute writes done = 0,
// which it does only after its first instruction's dependence pops. Here the first compute instruction pops
// the load -> compute token behind a LOAD of the whole weight buffer (512 KB), so for that whole load done_o
// reads 1. A driver that trusts done_o right away returns before the run has finished: the output is still
// the 0x5a fill (or an older value). Every iteration uses another inp one-hot, so a stale output is caught.
// The first run after loading the bitstream starts with done_o = 0, so the race shows from iteration 1 on.
// usage: ./donerace [iterations]   (expect "RESULT donerace: OK")
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <sys/time.h>

#include "vta/runtime.h"
#include "vta/data_buf.h"

#define EVTA_ID 0

using namespace vta;

static const int WGT_DEPTH = 2048;
static int g_reset = 0;

// one GEMM: acc[0] = W[WGT_DEPTH - 1] * inp[0] (the last weight entry, i.e. after the whole load)
static int kernel(void *, VTACommandHandle h) {
  VTAUopLoopBegin(h, 1, 0, 0, 0);
  VTAUopPush(h, 0, g_reset, 0, 0, WGT_DEPTH - 1, 0, 0, 0);
  VTAUopLoopEnd(h);
  return 0;
}

static int8_t pat(int w, int oc, int ic) { return (int8_t)(w * 37 + oc * 11 + ic * 5 + 3); }

int main(int argc, char **argv) {
  int iters = argc > 1 ? atoi(argv[1]) : 16;
  VTACommandHandle h = VTATLSCommandHandle(EVTA_ID);
  std::vector<int8_t> inp(16), wgt((size_t)WGT_DEPTH * 256), out(16);
  for (int w = 0; w < WGT_DEPTH; w++)
    for (int oc = 0; oc < 16; oc++)
      for (int ic = 0; ic < 16; ic++) wgt[(size_t)w * 256 + oc * 16 + ic] = pat(w, oc, ic);
  DataBuffer *ib = (DataBuffer *)VTABufferAlloc(inp.size());
  DataBuffer *wb = (DataBuffer *)VTABufferAlloc(wgt.size());
  DataBuffer *ob = (DataBuffer *)VTABufferAlloc(out.size());
  VTABufferCopy(wgt.data(), 0, wb, 0, wgt.size(), 1);
  int bad_iters = 0;
  for (int it = 0; it < iters; it++) {
    int k = it % 16;
    memset(inp.data(), 0, inp.size());
    inp[k] = 1;
    VTABufferCopy(inp.data(), 0, ib, 0, inp.size(), 1);
    memset(out.data(), 0x5a, out.size());
    VTABufferCopy(out.data(), 0, ob, 0, out.size(), 1);
    void *uh_r[2] = {nullptr}, *uh[2] = {nullptr};
    struct timeval t0, t1;
    gettimeofday(&t0, nullptr);
    // load first; compute's first instruction (LOAD UOP of the reset GEMM) pops the load -> compute token
    VTALoadBuffer2D(h, ib, 0, 1, 1, 1, 0, 0, 0, 0, 0, 2);
    VTALoadBuffer2D(h, wb, 0, WGT_DEPTH, 1, WGT_DEPTH, 0, 0, 0, 0, 0, 1);
    VTADepPush(h, 1, 2);
    VTADepPop(h, 1, 2);
    g_reset = 1;
    VTAPushGEMMOp(h, uh_r, &kernel, nullptr, 0);
    g_reset = 0;
    VTAPushGEMMOp(h, uh, &kernel, nullptr, 0);
    VTADepPush(h, 2, 3);
    VTADepPop(h, 2, 3);
    VTAStoreBuffer2D(h, 0, 4, ob, 0, 1, 1, 1);
    VTASynchronize(h, 1 << 31);
    gettimeofday(&t1, nullptr);
    VTABufferCopy(ob, 0, out.data(), 0, out.size(), 2);
    int bad = 0;
    for (int oc = 0; oc < 16; oc++) bad += out[oc] != pat(WGT_DEPTH - 1, oc, k);
    bad_iters += bad != 0;
    printf("iter %2d (inp e_%-2d): %2d/16 wrong, %7.3f ms%s\n", it, k, bad,
           (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_usec - t0.tv_usec) / 1e3,
           bad && out[0] == 0x5a ? "  <- output not written yet (returned before the run finished)" : "");
  }
  printf("RESULT donerace: %s (%d of %d iterations wrong)\n", bad_iters ? "ERRORS" : "OK", bad_iters, iters);
  VTARuntimeShutdown(EVTA_ID);
  return bad_iters ? 1 : 0;
}
