// Minimal EVTA probe that bypasses the vtalib runtime: writes a tiny instruction stream into a
// CMA buffer, starts fetch/load/compute/store like pynq_driver.cc VTADevice::Run(), and polls
// compute's done register (0x18). Used to find the instruction width the bitstream expects.
//
//   ./rawtest 1    : [FINISH]                          -> checks the done path itself
//   ./rawtest 32   : [empty insn][FINISH @ byte 32]     -> succeeds only if HW insn = 256 bit
//   ./rawtest 16   : [empty insn][FINISH @ byte 16]     -> succeeds only if HW insn = 128 bit
//
// The empty insn (all zero) decodes as LOAD, memory_type 0 (UOP), x_size 0 -> a no-op for the
// compute stage in both layouts. FINISH is opcode 3 in bits [2:0] with no dependence flags,
// identical in both layouts. Run each case on a freshly loaded bitstream (see run_rawtests.sh).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <time.h>

extern "C" {
void *cma_alloc(uint32_t len, uint32_t cacheable);
unsigned long cma_get_phy_addr(void *buf);
void cma_free(void *buf);
}

static const uint32_t kBase[4] = {0xA0000000, 0xA0001000, 0xA0002000, 0xA0003000};
static const char *kName[4] = {"fetch", "load", "compute", "store"};
enum { FETCH, LOAD, COMPUTE, STORE };

static volatile uint32_t *regs[4];

static uint32_t rd(int b, uint32_t off) { return regs[b][off / 4]; }
static void wr(int b, uint32_t off, uint32_t v) { regs[b][off / 4] = v; }

static void dump() {
  for (int b = 0; b < 4; b++)
    printf("  %-7s ctrl=%#06x 0x10=%#010x 0x18=%#010x 0x1c=%#010x\n", kName[b], rd(b, 0),
           rd(b, 0x10), rd(b, 0x18), rd(b, 0x1c));
}

int main(int argc, char **argv) {
  int mode = argc > 1 ? atoi(argv[1]) : 1;
  if (mode != 1 && mode != 16 && mode != 32) {
    fprintf(stderr, "usage: %s 1|16|32\n", argv[0]);
    return 2;
  }

  int fd = open("/dev/mem", O_RDWR | O_SYNC);
  if (fd < 0) { perror("open /dev/mem (run with sudo)"); return 1; }
  for (int b = 0; b < 4; b++) {
    void *p = mmap(nullptr, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, kBase[b]);
    if (p == MAP_FAILED) { perror("mmap"); return 1; }
    regs[b] = static_cast<volatile uint32_t *>(p);
  }
  close(fd);

  // Non-cacheable CMA buffer: no cache flush needed. It is mapped as device memory on aarch64,
  // so only aligned 32-bit volatile stores are used (memset/byte stores raise SIGBUS there).
  void *buf = cma_alloc(4096, 0);
  if (!buf) { fprintf(stderr, "cma_alloc failed (try xlnk_reset)\n"); return 1; }
  volatile uint32_t *insn = static_cast<volatile uint32_t *>(buf);
  for (int i = 0; i < 4096 / 4; i++) insn[i] = 0;
  unsigned long phy = cma_get_phy_addr(buf);

  uint32_t count;
  if (mode == 1) {
    insn[0] = 3;          // FINISH (opcode in bits [2:0])
    count = 1;
  } else {
    insn[mode / 4] = 3;   // FINISH at byte 16 or 32; insn 0 stays all-zero
    count = 2;
  }
  __sync_synchronize();

  printf("mode=%d insn_count=%u insn_phy=%#lx\n", mode, count, phy);
  printf("before:\n");
  dump();

  // Same sequence as pynq_driver.cc VTADevice::Run()
  wr(FETCH, 0x10, count);
  wr(FETCH, 0x18, (uint32_t)phy);
  wr(LOAD, 0x10, 0);
  wr(LOAD, 0x18, 0);
  wr(STORE, 0x10, 0);
  wr(FETCH, 0x0, 0x1);    // VTA_START
  wr(LOAD, 0x0, 0x81);    // VTA_AUTORESTART
  wr(COMPUTE, 0x0, 0x81);
  wr(STORE, 0x0, 0x81);

  struct timespec t0, t;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  uint32_t done = 0;
  long polls = 0;
  double sec = 0;
  do {
    done = rd(COMPUTE, 0x18);
    polls++;
    clock_gettime(CLOCK_MONOTONIC, &t);
    sec = (t.tv_sec - t0.tv_sec) + (t.tv_nsec - t0.tv_nsec) * 1e-9;
  } while (done != 1 && sec < 2.0);

  printf("after %.3f s, %ld polls:\n", sec, polls);
  dump();
  printf("RESULT mode=%d: %s\n", mode, done == 1 ? "DONE (compute reached FINISH)" : "TIMEOUT");
  cma_free(buf);
  return done == 1 ? 0 : 1;
}
