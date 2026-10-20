// #### JO added (2026-10-20): GEMM probe for new EVTA bitstreams (vtalib tests/zcu102/tests/gemm.cpp
// passes on the etri bitstream but gives garbage on vtahw_*_16_16_19_18). Same runtime calls as gemm.cpp,
// but deterministic data, input flushed (gemm.cpp memcpy's it without a flush), and one case per run:
//   reset : GEMM reset only, then STORE                      -> expect all 0  (reset + store path)
//   zero  : weights all 0                                    -> expect all 0  (gemm with zero weights)
//   ident : W = 2*I, inp[j] = j+1                            -> expect 2*(i+1)
//   perm  : W[i][(i+1)%16] = 1, inp[j] = j+1                 -> expect (i+1)%16+1 (shows index mapping)
//   ones  : W[i][j] = 1 for j <= i, inp[j] = 1               -> expect i+1 (shows which weights arrive)
// usage: ./gemmprobe <case>   (built by build.sh gemmprobe; run via run_rawtests2.sh gemmprobe_<case>)
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "vta/runtime.h"
#include "vta/data_buf.h"

#define EVTA_ID 0
//#### JO changed: SRAM index of inp/wgt/acc from GP_IDX (default 8), to see whether the garbage depends on it
//#define INP_IDX 8
//#define WEIGHT_IDX 8
//#define ACC_IDX 8
static int INP_IDX = 8, WEIGHT_IDX = 8, ACC_IDX = 8;

using namespace vta;

static int gemm_reset(void *, VTACommandHandle cmdh) {
  VTAUopLoopBegin(cmdh, 1, 0, 0, 0);
  VTAUopPush(cmdh, 0, 1, ACC_IDX, INP_IDX, WEIGHT_IDX, 0, 0, 0);
  VTAUopLoopEnd(cmdh);
  return 0;
}

static int gemm_run(void *, VTACommandHandle cmdh) {
  VTAUopLoopBegin(cmdh, 1, 0, 0, 0);
  VTAUopPush(cmdh, 0, 0, ACC_IDX, INP_IDX, WEIGHT_IDX, 0, 0, 0);
  VTAUopLoopEnd(cmdh);
  return 0;
}

int main(int argc, char **argv) {
  std::string c = argc > 1 ? argv[1] : "ident";
  if (const char *e = getenv("GP_IDX")) INP_IDX = WEIGHT_IDX = ACC_IDX = atoi(e);
  int8_t input[16], weight[16][16], output[16];
  int expect[16];
  memset(weight, 0, sizeof(weight));
  for (int j = 0; j < 16; j++) input[j] = (c == "ones") ? 1 : j + 1;
  for (int i = 0; i < 16; i++) {
    if (c == "ident") { weight[i][i] = 2; expect[i] = 2 * (i + 1); }
    else if (c == "perm") { weight[i][(i + 1) % 16] = 1; expect[i] = (i + 1) % 16 + 1; }
    else if (c == "ones") { for (int j = 0; j <= i; j++) weight[i][j] = 1; expect[i] = i + 1; }
    else expect[i] = 0;  // reset, zero
  }
  memset(output, 0x5a, sizeof(output));

  DataBuffer *inp_buf = (DataBuffer *)VTABufferAlloc(16);
  DataBuffer *wgt_buf = (DataBuffer *)VTABufferAlloc(16 * 16);
  DataBuffer *out_buf = (DataBuffer *)VTABufferAlloc(16);
  VTABufferCopy(input, 0, inp_buf, 0, sizeof(input), 1);    // host -> device, flushes
  VTABufferCopy(weight, 0, wgt_buf, 0, sizeof(weight), 1);
  VTABufferCopy(output, 0, out_buf, 0, sizeof(output), 1);  // 0x5a marks "not written"

  void *uh1[2] = {nullptr}, *uh2[2] = {nullptr};
  VTACommandHandle h = VTATLSCommandHandle(EVTA_ID);
  VTASetDebugMode(h, VTA_DEBUG_DUMP_INSN | VTA_DEBUG_DUMP_UOP);

  VTAPushGEMMOp(h, uh1, &gemm_reset, nullptr, 0);
  if (c != "reset") {
    VTADepPush(h, 2, 1);
    VTADepPop(h, 2, 1);
    VTALoadBuffer2D(h, inp_buf, 0, 1, 1, 1, 0, 0, 0, 0, INP_IDX, 2);
    VTALoadBuffer2D(h, wgt_buf, 0, 1, 1, 1, 0, 0, 0, 0, WEIGHT_IDX, 1);
    VTADepPush(h, 1, 2);
    VTADepPop(h, 1, 2);
    VTAPushGEMMOp(h, uh2, &gemm_run, nullptr, 0);
  }
  VTADepPush(h, 2, 3);
  VTADepPop(h, 2, 3);
  VTAStoreBuffer2D(h, ACC_IDX, 4, out_buf, 0, 1, 1, 1);
  VTASynchronize(h, 1 << 31);
  VTABufferCopy(out_buf, 0, output, 0, sizeof(output), 2);  // device -> host, invalidates

  int bad = 0;
  printf("case=%s idx=%d\n  i  : ", c.c_str(), ACC_IDX);
  for (int i = 0; i < 16; i++) printf("%4d", i);
  printf("\n  inp: ");
  for (int i = 0; i < 16; i++) printf("%4d", input[i]);
  printf("\n  exp: ");
  for (int i = 0; i < 16; i++) printf("%4d", expect[i]);
  printf("\n  out: ");
  for (int i = 0; i < 16; i++) { printf("%4d", output[i]); bad += output[i] != (int8_t)expect[i]; }
  printf("\nRESULT gemmprobe %s: %s (%d/16 wrong)\n", c.c_str(), bad ? "MISMATCH" : "OK", bad);
  VTARuntimeShutdown(EVTA_ID);
  VTABufferFree(inp_buf);
  VTABufferFree(wgt_buf);
  VTABufferFree(out_buf);
  return bad ? 1 : 0;
}
