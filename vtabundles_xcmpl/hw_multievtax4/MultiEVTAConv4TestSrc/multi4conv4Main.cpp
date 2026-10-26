// #### JO added (2026-10-26): Conv4Test (3x3 conv, 56x56x64 -> 64 channels, 231.2 Mops) on several EVTAs at
// once (4-EVTA bitstream). One thread per EVTA: load, one warm-up run, then all threads start together at a
// barrier and run <iters> inferences each; prints the wall time of the concurrent part, per-EVTA times and GOPS,
// and compares each EVTA's output with the golden. Built with NESTC_EVTA_PROFILE, vtalib also prints the VTA_SYNC
// events (EVTA run time from start to done) of each EVTA at shutdown.
// With "conv" as the third argument each thread runs only convolution_wo_tr <iters> times on an input transposed
// once (<entry>_convonly, added by make_multi4conv4.py): EVTA plus the runtime's instruction generation, no CPU
// transposes and buffer allocation per run.
// #### JO added: the output of the lowest EVTA in the mask is written to output.bin (run_all.sh compares it with
// vtaConv4TestGolden.bin; the exit code is 1 if any EVTA's output differs from the golden).
// usage: multi4conv4Bundle <evta mask, e.g. 15 or 1> [iters=100] [conv]   (run in its dir: reads the *.bin files here)
#include "vtaConv4TestMainEntry0.h"
#include "vtaConv4TestMainEntry1.h"
#include "vtaConv4TestMainEntry2.h"
#include "vtaConv4TestMainEntry3.h"
#include "VTARuntime.h"
#include "vta/runtime.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <vector>

static const double kConvOps = 2.0 * 56 * 56 * 64 * 64 * 3 * 3;
static const size_t kTensorBytes = 56 * 56 * 64;

typedef int (*EntryFn)(uint8_t *, uint8_t *, uint8_t *);
typedef void (*LoadFn)(uint8_t *);
typedef void (*DestroyFn)();
typedef void (*ConvOnlyFn)(uint8_t *, int);
struct Evta {
  BundleConfig *config;
  LoadFn load;
  EntryFn entry;
  DestroyFn destroy;
  ConvOnlyFn convonly;
  void (*init)();
  void (*shutdown)();
};
static Evta evtas[4] = {
    {&vtaConv4TestBundle0_config, vtaConv4TestMainEntry0_load_module, vtaConv4TestMainEntry0,
     vtaConv4TestMainEntry0_destroy_module, vtaConv4TestMainEntry0_convonly, initVTARuntime, destroyVTARuntime},
    {&vtaConv4TestBundle1_config, vtaConv4TestMainEntry1_load_module, vtaConv4TestMainEntry1,
     vtaConv4TestMainEntry1_destroy_module, vtaConv4TestMainEntry1_convonly, initVTARuntime1, destroyVTARuntime1},
    {&vtaConv4TestBundle2_config, vtaConv4TestMainEntry2_load_module, vtaConv4TestMainEntry2,
     vtaConv4TestMainEntry2_destroy_module, vtaConv4TestMainEntry2_convonly, initVTARuntime2, destroyVTARuntime2},
    {&vtaConv4TestBundle3_config, vtaConv4TestMainEntry3_load_module, vtaConv4TestMainEntry3,
     vtaConv4TestMainEntry3_destroy_module, vtaConv4TestMainEntry3_convonly, initVTARuntime3, destroyVTARuntime3},
};

static pthread_barrier_t start_barrier, end_barrier;
static int iters = 100;
static bool conv_only = false;
static timespec t_start, t_end;
static double evta_ms[4];
static int evta_ok[4];
static int first_id;

static double ms_between(const timespec &a, const timespec &b) {
  return (b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) * 1e-6;
}

static uint8_t *read_file(const char *name, size_t expect) {
  FILE *f = fopen(name, "rb");
  if (!f) { fprintf(stderr, "cannot open %s\n", name); exit(1); }
  fseek(f, 0, SEEK_END);
  size_t n = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (expect && n != expect) { fprintf(stderr, "%s: %zu bytes, expected %zu\n", name, n, expect); exit(1); }
  uint8_t *p = nullptr;
  if (posix_memalign((void **)&p, 64, n) || fread(p, n, 1, f) != 1) { fprintf(stderr, "cannot read %s\n", name); exit(1); }
  fclose(f);
  return p;
}

static uint8_t *zalloc(size_t n) {
  uint8_t *p = nullptr;
  if (posix_memalign((void **)&p, 64, n ? n : 64)) exit(1);
  memset(p, 0, n);
  return p;
}

static void *run_evta(void *arg) {
  int i = (int)(intptr_t)arg;
  Evta &e = evtas[i];
  e.init();
  uint8_t *cw = read_file("vtaConv4TestBundle.weights.bin", e.config->constantWeightVarsMemSize);
  uint8_t *mw = zalloc(e.config->mutableWeightVarsMemSize);
  uint8_t *act = zalloc(e.config->activationsMemSize);
  uint8_t *in = read_file("vtaConv4TestInput.bin", kTensorBytes);
  // symbol table: inputP at offset 0, outP after it (vtaConv4TestBundle.cpp)
  memcpy(mw + e.config->symbolTable[0].offset, in, kTensorBytes);
  e.load(cw);
  e.entry(cw, mw, act);  // warm-up

  pthread_barrier_wait(&start_barrier);
  timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  if (conv_only)
    e.convonly(mw, iters);
  else
    for (int k = 0; k < iters; k++)
      e.entry(cw, mw, act);
  clock_gettime(CLOCK_MONOTONIC, &t1);
  evta_ms[i] = ms_between(t0, t1);
  pthread_barrier_wait(&end_barrier);

  // output: vtaConv4TestGolden.bin is the NHWC int8 output (= outP)
  uint8_t *golden = read_file("vtaConv4TestGolden.bin", kTensorBytes);
  evta_ok[i] = !memcmp(mw + e.config->symbolTable[1].offset, golden, kTensorBytes);
  if (i == first_id) {
    FILE *f = fopen("output.bin", "wb");
    if (f) { fwrite(mw + e.config->symbolTable[1].offset, kTensorBytes, 1, f); fclose(f); }
  }
  e.destroy();
  e.shutdown();
  free(cw); free(mw); free(act); free(in); free(golden);
  return nullptr;
}

int main(int argc, char **argv) {
  unsigned mask = argc > 1 ? strtoul(argv[1], nullptr, 0) & 15 : 15;
  if (argc > 2) iters = atoi(argv[2]);
  conv_only = argc > 3 && !strcmp(argv[3], "conv");
  std::vector<int> ids;
  for (int i = 0; i < 4; i++) if (mask >> i & 1) ids.push_back(i);
  if (ids.empty()) { fprintf(stderr, "usage: %s <evta mask 1..15> [iters]\n", argv[0]); return 2; }
  first_id = ids[0];
  // +1: main takes the wall time of the concurrent part between the barriers
  pthread_barrier_init(&start_barrier, nullptr, ids.size() + 1);
  pthread_barrier_init(&end_barrier, nullptr, ids.size() + 1);
  std::vector<pthread_t> th(ids.size());
  for (size_t k = 0; k < ids.size(); k++)
    pthread_create(&th[k], nullptr, run_evta, (void *)(intptr_t)ids[k]);
  pthread_barrier_wait(&start_barrier);
  clock_gettime(CLOCK_MONOTONIC, &t_start);
  pthread_barrier_wait(&end_barrier);
  clock_gettime(CLOCK_MONOTONIC, &t_end);
  for (auto &t : th) pthread_join(t, nullptr);

  double wall = ms_between(t_start, t_end);
  int ok = 1;
  for (int i : ids) {
    printf("EVTA%d: %d x Conv4%s %.3f ms (%.3f ms each, %.2f GOPS) output %s\n", i, iters, conv_only ? " conv only" : "",
           evta_ms[i], evta_ms[i] / iters, kConvOps * iters / (evta_ms[i] * 1e6), evta_ok[i] ? "= golden" : "WRONG");
    ok &= evta_ok[i];
  }
  printf("RESULT mask 0x%x: %zu EVTAs x %d runs%s in %.3f ms wall, %.2f GOPS, %s\n", mask, ids.size(), iters,
         conv_only ? " (conv only)" : " (end-to-end incl. CPU transposes)", wall, kConvOps * iters * ids.size() / (wall * 1e6), ok ? "all = golden" : "WRONG");
  return ok ? 0 : 1;
}
