// Step-by-step EVTA probe (follow-up to rawtest.cpp, which showed the HW uses 256-bit insns).
// Instructions are encoded with the vtalib ZCU102 structs (include/zcu102/vta/hw_spec.h) so the
// field layout is exactly what the runtime produces. Each case isolates one mechanism:
//
//   uop     : LOAD UOP (1 elem, compute reads DRAM)                     + FINISH
//   acc     : LOAD ACC (1 elem, compute reads DRAM)                     + FINISH
//   l2c     : load-stage NOP push_next -> compute NOP pop_prev          + FINISH
//   c2s     : compute NOP push_next -> STORE(x=0) pop_prev push_prev
//             -> compute NOP pop_next                                    + FINISH
//   store   : LOAD ACC -> compute NOP push_next -> STORE 1 elem (DRAM write, memory_type=OUT)
//             pop_prev push_prev -> compute NOP pop_next                 + FINISH
//   storeacc: same as store but STORE memory_type=ACC
//   inp     : LOAD INP (1 elem, load stage reads DRAM) push_next -> compute NOP pop_prev + FINISH
//   add     : same instruction sequence as tests/zcu102/tests/add_test.cpp
//
// usage: ./rawtest2 <case>   (run on a freshly loaded bitstream, see run_rawtests2.sh)
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <time.h>
#include "vta/hw_spec.h"

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

// A non-cacheable CMA buffer, written only with aligned 32-bit volatile stores.
struct Buf {
  void *p;
  unsigned long phy;
  explicit Buf(uint32_t bytes) {
    p = cma_alloc(bytes, 0);
    if (!p) { fprintf(stderr, "cma_alloc failed (try xlnk_reset)\n"); exit(1); }
    phy = cma_get_phy_addr(p);
    volatile uint32_t *w = static_cast<volatile uint32_t *>(p);
    for (uint32_t i = 0; i < bytes / 4; i++) w[i] = 0;
  }
  volatile uint32_t *w() { return static_cast<volatile uint32_t *>(p); }
};

static std::vector<VTAGenericInsn> prog;

static VTAMemInsn mem(int opcode, int mtype, uint32_t sram, uint32_t dram, uint32_t x) {
  VTAMemInsn m;
  memset(&m, 0, sizeof(m));
  m.opcode = opcode;
  m.memory_type = mtype;
  m.sram_base = sram;
  m.dram_base = dram;
  m.y_size = x ? 1 : 0;
  m.x_size = x;
  m.x_stride = x ? x : 0;
  return m;
}
static void push(const VTAMemInsn &m) { VTAGenericInsn g; memcpy(&g, &m, sizeof(m)); prog.push_back(g); }
// Load-stage NOP: LOAD INP with x_size 0 (as the runtime's NOP-MEMORY-STAGE)
static void load_nop(int pop_next, int push_next) {
  VTAMemInsn m = mem(VTA_OPCODE_LOAD, VTA_MEM_ID_INP, 0, 0, 0);
  m.pop_next_dep = pop_next; m.push_next_dep = push_next; push(m);
}
// Compute-stage NOP: LOAD UOP with x_size 0 (as the runtime's NOP-COMPUTE-STAGE)
static void compute_nop(int pop_prev, int pop_next, int push_prev, int push_next) {
  VTAMemInsn m = mem(VTA_OPCODE_LOAD, VTA_MEM_ID_UOP, 0, 0, 0);
  m.pop_prev_dep = pop_prev; m.pop_next_dep = pop_next;
  m.push_prev_dep = push_prev; m.push_next_dep = push_next; push(m);
}
// --- Hypothesis: the bitstream decodes memory_type / alu_opcode as 2 bits (as in the 21.12
// simulator header) while the ZCU102 header uses 3 bits. Re-pack the first 64-bit word of a
// mem insn with a 2-bit memory_type (all later fields of that word shift down by one bit).
static void push2b(const VTAMemInsn &m) {
  uint64_t q[4];
  memcpy(q, &m, sizeof(q));
  q[0] = (uint64_t)m.opcode | (uint64_t)m.pop_prev_dep << 3 | (uint64_t)m.pop_next_dep << 4 |
         (uint64_t)m.push_prev_dep << 5 | (uint64_t)m.push_next_dep << 6 |
         (uint64_t)(m.memory_type & 3) << 7 | (uint64_t)m.sram_base << 9 |
         (uint64_t)m.dram_base << 25;
  VTAGenericInsn g; memcpy(&g, q, sizeof(q)); prog.push_back(g);
}
// ALU insn with a 2-bit alu_opcode (same field order as the ZCU102 VTAAluInsn otherwise)
typedef struct {
  uint64_t opcode : VTA_OPCODE_BIT_WIDTH;
  uint64_t pop_prev_dep : 1; uint64_t pop_next_dep : 1;
  uint64_t push_prev_dep : 1; uint64_t push_next_dep : 1;
  uint64_t reset_reg : 1;
  uint64_t uop_bgn : VTA_LOG_UOP_BUFF_DEPTH;
  uint64_t uop_end : VTA_LOG_UOP_BUFF_DEPTH + 1;
  uint64_t iter_out : VTA_LOOP_ITER_WIDTH;
  uint64_t iter_in : VTA_LOOP_ITER_WIDTH;
  uint64_t dst_factor_out : VTA_LOG_ACC_BUFF_DEPTH;
  uint64_t dst_factor_in : VTA_LOG_ACC_BUFF_DEPTH;
  uint64_t src_factor_out : VTA_LOG_INP_BUFF_DEPTH;
  uint64_t src_factor_in : VTA_LOG_INP_BUFF_DEPTH;
  uint64_t alu_opcode : 2;
  uint64_t use_imm : 1;
  int64_t imm : VTA_ALUOP_IMM_BIT_WIDTH;
  uint64_t pad_0 : 32;
  uint64_t pad_1 : 64;
} AluInsn2b;

static void finish() {
  VTAGemInsn f; memset(&f, 0, sizeof(f)); f.opcode = VTA_OPCODE_FINISH;
  VTAGenericInsn g; memcpy(&g, &f, sizeof(f)); prog.push_back(g);
}

int main(int argc, char **argv) {
  std::string c = argc > 1 ? argv[1] : "";
  printf("sizeof(VTAGenericInsn)=%zu (expect 32)\n", sizeof(VTAGenericInsn));

  int fd = open("/dev/mem", O_RDWR | O_SYNC);
  if (fd < 0) { perror("open /dev/mem (run with sudo)"); return 1; }
  for (int b = 0; b < 4; b++) {
    void *p = mmap(nullptr, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, kBase[b]);
    if (p == MAP_FAILED) { perror("mmap"); return 1; }
    regs[b] = static_cast<volatile uint32_t *>(p);
  }
  close(fd);

  // Data buffers. dram_base is in units of the element size of each memory type.
  const uint32_t kUopBytes = VTA_UOP_ELEM_BYTES, kAccBytes = VTA_ACC_ELEM_BYTES,
                 kInpBytes = VTA_INP_ELEM_BYTES, kOutBytes = VTA_OUT_ELEM_BYTES;
  Buf uop(4096), acc(4096), inp(4096), out(4096);
  for (int i = 0; i < 16; i++) acc.w()[i] = 10 + i;   // acc elem 0 = {10..25}
  out.w()[0] = 0xdeadbeef;                              // must be overwritten by a STORE

  if (c == "uop") {
    push(mem(VTA_OPCODE_LOAD, VTA_MEM_ID_UOP, 0, uop.phy / kUopBytes, 1));
  } else if (c == "acc") {
    push(mem(VTA_OPCODE_LOAD, VTA_MEM_ID_ACC, 0, acc.phy / kAccBytes, 1));
  } else if (c == "l2c") {
    load_nop(0, 1);
    compute_nop(1, 0, 0, 0);
  } else if (c == "c2s") {
    compute_nop(0, 0, 0, 1);
    VTAMemInsn s = mem(VTA_OPCODE_STORE, VTA_MEM_ID_OUT, 0, 0, 0);
    s.pop_prev_dep = 1; s.push_prev_dep = 1; push(s);
    compute_nop(0, 1, 0, 0);
  } else if (c == "store" || c == "storeacc") {
    push(mem(VTA_OPCODE_LOAD, VTA_MEM_ID_ACC, 0, acc.phy / kAccBytes, 1));
    compute_nop(0, 0, 0, 1);
    VTAMemInsn s = mem(VTA_OPCODE_STORE, c == "store" ? VTA_MEM_ID_OUT : VTA_MEM_ID_ACC, 0,
                       out.phy / kOutBytes, 1);
    s.pop_prev_dep = 1; s.push_prev_dep = 1; push(s);
    compute_nop(0, 1, 0, 0);
  } else if (c == "inp") {
    VTAMemInsn l = mem(VTA_OPCODE_LOAD, VTA_MEM_ID_INP, 0, inp.phy / kInpBytes, 1);
    l.push_next_dep = 1; push(l);
    compute_nop(1, 0, 0, 0);
  } else if (c == "add") {
    // Mirrors the add_test dump: LOAD ACC, LOAD UOP, ALU add-imm (push_next), STORE
    // (pop_prev, push_prev), load NOP (push_next), compute NOP (pop_prev, pop_next), FINISH.
    uop.w()[0] = 0; uop.w()[1] = 0;   // uop {acc=0, inp=0, wgt=0}
    push(mem(VTA_OPCODE_LOAD, VTA_MEM_ID_ACC, 0, acc.phy / kAccBytes, 1));
    push(mem(VTA_OPCODE_LOAD, VTA_MEM_ID_UOP, 0, uop.phy / kUopBytes, 1));
    VTAAluInsn a; memset(&a, 0, sizeof(a));
    a.opcode = VTA_OPCODE_ALU; a.alu_opcode = VTA_ALU_OPCODE_ADD; a.use_imm = 1; a.imm = 1;
    a.uop_bgn = 0; a.uop_end = 1; a.iter_out = 1; a.iter_in = 1; a.push_next_dep = 1;
    VTAGenericInsn g; memcpy(&g, &a, sizeof(a)); prog.push_back(g);
    VTAMemInsn s = mem(VTA_OPCODE_STORE, VTA_MEM_ID_OUT, 0, out.phy / kOutBytes, 1);
    s.pop_prev_dep = 1; s.push_prev_dep = 1; push(s);
    load_nop(0, 1);
    compute_nop(1, 1, 0, 0);
  } else if (c == "store2b") {
    // STORE of (whatever is in out SRAM) with a 2-bit memory_type encoding
    compute_nop(0, 0, 0, 1);
    VTAMemInsn s = mem(VTA_OPCODE_STORE, VTA_MEM_ID_OUT, 0, out.phy / kOutBytes, 1);
    s.pop_prev_dep = 1; s.push_prev_dep = 1; push2b(s);
    compute_nop(0, 1, 0, 0);
  } else if (c == "add2b") {
    // add sequence with 2-bit memory_type in mem insns and 2-bit alu_opcode
    uop.w()[0] = 0; uop.w()[1] = 0;
    push2b(mem(VTA_OPCODE_LOAD, VTA_MEM_ID_ACC, 0, acc.phy / kAccBytes, 1));
    push2b(mem(VTA_OPCODE_LOAD, VTA_MEM_ID_UOP, 0, uop.phy / kUopBytes, 1));
    AluInsn2b a; memset(&a, 0, sizeof(a));
    a.opcode = VTA_OPCODE_ALU; a.alu_opcode = VTA_ALU_OPCODE_ADD; a.use_imm = 1; a.imm = 1;
    a.uop_bgn = 0; a.uop_end = 1; a.iter_out = 1; a.iter_in = 1; a.push_next_dep = 1;
    VTAGenericInsn g; memcpy(&g, &a, sizeof(a)); prog.push_back(g);
    VTAMemInsn s = mem(VTA_OPCODE_STORE, VTA_MEM_ID_OUT, 0, out.phy / kOutBytes, 1);
    s.pop_prev_dep = 1; s.push_prev_dep = 1; push2b(s);
    load_nop(0, 1);
    compute_nop(1, 1, 0, 0);
  } else {
    fprintf(stderr, "usage: %s uop|acc|l2c|c2s|store|storeacc|inp|add|store2b|add2b\n", argv[0]);
    return 2;
  }
  finish();

  Buf insn(4096);
  const uint32_t *src = reinterpret_cast<const uint32_t *>(prog.data());
  for (size_t i = 0; i < prog.size() * sizeof(VTAGenericInsn) / 4; i++) insn.w()[i] = src[i];
  __sync_synchronize();

  printf("case=%s insn_count=%zu insn_phy=%#lx\n", c.c_str(), prog.size(), insn.phy);
  for (size_t i = 0; i < prog.size(); i++) {
    const uint32_t *q = reinterpret_cast<const uint32_t *>(&prog[i]);
    printf("  insn%zu:", i);
    for (int k = 7; k >= 0; k--) printf(" %08x", q[k]);
    printf("\n");
  }

  wr(FETCH, 0x10, prog.size());
  wr(FETCH, 0x18, (uint32_t)insn.phy);
  wr(LOAD, 0x10, 0); wr(LOAD, 0x18, 0); wr(STORE, 0x10, 0);
  wr(FETCH, 0x0, 0x1);
  wr(LOAD, 0x0, 0x81); wr(COMPUTE, 0x0, 0x81); wr(STORE, 0x0, 0x81);

  struct timespec t0, t;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  uint32_t done = 0;
  double sec = 0;
  do {
    done = rd(COMPUTE, 0x18);
    clock_gettime(CLOCK_MONOTONIC, &t);
    sec = (t.tv_sec - t0.tv_sec) + (t.tv_nsec - t0.tv_nsec) * 1e-9;
  } while (done != 1 && sec < 2.0);

  printf("after %.3f s:\n", sec);
  dump();
  if (c == "store" || c == "storeacc" || c == "add" || c == "store2b" || c == "add2b") {
    printf("  out[0..3] = %08x %08x %08x %08x  (was deadbeef)\n", out.w()[0], out.w()[1],
           out.w()[2], out.w()[3]);
    usleep(200000);
    printf("  +200ms out[0..3] = %08x %08x %08x %08x\n", out.w()[0], out.w()[1], out.w()[2],
           out.w()[3]);
  }
  if (c == "add" || c == "add2b") {
    // Expected OUT element: int8(acc + 1) = bytes 0x0b..0x1a -> look for it in DRAM.
    const uint32_t pat[4] = {0x0e0d0c0b, 0x1211100f, 0x16151413, 0x1a191817};
    printf("  out.phy=%#lx  dram_base(OUT elems)=%#lx\n", out.phy, out.phy / kOutBytes);
    struct { const char *why; unsigned long lo, hi; } win[] = {
        {"CMA +-64MB around out", out.phy - (64ul << 20), out.phy + (64ul << 20)},
        {"dram_base as bytes", (out.phy / kOutBytes) & ~0xffffful, ((out.phy / kOutBytes) & ~0xffffful) + (2ul << 20)},
    };
    int mfd = open("/dev/mem", O_RDONLY | O_SYNC);
    for (auto &wv : win) {
      int hits = 0;
      for (unsigned long a = wv.lo; a < wv.hi && hits < 8; a += (1ul << 20)) {
        void *m = mmap(nullptr, 1ul << 20, PROT_READ, MAP_SHARED, mfd, a);
        if (m == MAP_FAILED) continue;
        const volatile uint32_t *p = static_cast<const volatile uint32_t *>(m);
        for (unsigned long i = 0; i + 4 <= (1ul << 18) && hits < 8; i += 4)
          if (p[i] == pat[0] && p[i + 1] == pat[1] && p[i + 2] == pat[2] && p[i + 3] == pat[3]) {
            printf("  FOUND out pattern at phys %#lx  (%s)\n", a + i * 4, wv.why);
            hits++;
          }
        munmap(m, 1ul << 20);
      }
      if (!hits) printf("  pattern not found in %s [%#lx, %#lx)\n", wv.why, wv.lo, wv.hi);
    }
    close(mfd);
  }
  printf("RESULT case=%s: %s\n", c.c_str(), done == 1 ? "DONE" : "TIMEOUT");
  return done == 1 ? 0 : 1;
}
