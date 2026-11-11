/*
 * #### JO added (2026-11-11, ALU extension, docs/evta_isa_ext.md 8.8): HLS C simulation of the extended ALU against the
 * reference model (vtalib-zcu102-systolic/include/zcu102/vta/alu_ext_ref.h). One vta() call per program:
 *   LOAD UOP, LOAD ACC (2N random entries), CPY of every entry onto itself (defines out), LUTSET of a random table,
 *   ninsn random ALU instructions (operations 0-11, imm / vector, loop factors, rq_*, lut_en, more LUTSETs), each
 *   followed by a STORE of out[0, N), then CPY [0, N) -> [N, 2N) and three SHR 8, each stored, so all 32 bits of acc
 *   are compared too. The same instructions run on the reference model; every stored byte must match.
 * C simulation runs the steps one after another, so it says nothing about steps overlapping in the pipeline (same dst
 * written by close steps); the programs here may do that, the hardware programs must not (docs/evta_isa_ext.md 8.7).
 */
#ifdef VTA_ALUX

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

#include "../src/vta.h"
#include "../../../tests/hardware/common/test_lib.h"
#include "../../../../vtalib-zcu102-systolic/include/zcu102/vta/alu_ext_ref.h"

using vta_alu_ref::kLanes;

namespace {

const int N = 128;   // acc entries the random instructions use; [N, 2N) is the scratch of the final acc check

struct Prog {
  std::vector<VTAGenericInsn> insns;
  std::vector<VTAUop> uops;
  std::vector<VTAAluInsn> alus;   // the ALU instructions in order, for the reference model
  std::vector<int> storeOff;      // sram offset of each STORE (out entries [off, off + N))
  unsigned seed;
};

int rnd(Prog &p, int n) { return (int) (rand_r(&p.seed) % (unsigned) n); }

void addStore(Prog &p, int off) {
  p.insns.push_back(get2DLoadStoreInsn(VTA_OPCODE_STORE, VTA_MEM_ID_OUT, off, (int) p.storeOff.size() * N, 1, N, N, 0, 0,
                                       1, 0, 1, 0));
  p.storeOff.push_back(off);
}

// an ALU instruction with one new uop per entry of us ({dst, src}); every ALU instruction is followed by a STORE
void addAlu(Prog &p, VTAAluInsn a, const std::vector<std::pair<int, int>> &us, int storeOff = 0) {
  a.opcode = VTA_OPCODE_ALU;
  a.pop_prev_dep = 0;
  a.pop_next_dep = p.storeOff.empty() ? 0 : 1;
  a.push_prev_dep = 0;
  a.push_next_dep = 1;
  a.uop_bgn = p.uops.size();
  for (auto &u : us) {
    VTAUop o;
    memset(&o, 0, sizeof o);
    o.dst_idx = u.first;
    o.src_idx = u.second;
    p.uops.push_back(o);
  }
  a.uop_end = p.uops.size();
  union VTAInsn c;
  memset(&c, 0, sizeof c);
  c.alu = a;
  p.insns.push_back(c.generic);
  p.alus.push_back(a);
  addStore(p, storeOff);
}

VTAAluInsn blank(int op) {
  VTAAluInsn a;
  memset(&a, 0, sizeof a);
  a.alu_opcode = op & 3;
  a.alu_op_ext = op >> 2;
  a.iter_out = 1;
  a.iter_in = 1;
  return a;
}

// a random instruction over [0, N)
void addRandom(Prog &p) {
  int op = rnd(p, 12);
  VTAAluInsn a = blank(op);
  a.iter_out = 1 + rnd(p, 4);
  a.iter_in = 1 + rnd(p, 4);
  a.dst_factor_out = rnd(p, 5);
  a.dst_factor_in = rnd(p, 5);
  a.src_factor_out = rnd(p, 5);
  a.src_factor_in = rnd(p, 5);
  a.use_imm = rnd(p, 2);
  a.imm = (int16_t) rand_r(&p.seed);
  if (rnd(p, 2)) {
    a.rq_en = 1;
    a.rq_round = rnd(p, 4);
    a.rq_shift = rnd(p, 32);
    a.rq_lo = (int8_t) (rnd(p, 256) - 128);
    a.rq_hi = (int8_t) (rnd(p, 256) - 128);
  }
  a.lut_en = rnd(p, 4) == 0;
  int dmax = (a.iter_out - 1) * a.dst_factor_out + (a.iter_in - 1) * a.dst_factor_in;
  int smax = (a.iter_out - 1) * a.src_factor_out + (a.iter_in - 1) * a.src_factor_in;
  int nu = 1 + rnd(p, 3);
  std::vector<std::pair<int, int>> us;
  for (int u = 0; u < nu; u++)
    us.push_back({rnd(p, N - dmax), rnd(p, N - smax)});
  addAlu(p, a, us);
}

void ppack(const int32_t (*acc)[kLanes], int n, uint32_t *buf) {
  for (int j = 0; j < n; j++)
    for (int b = 0; b < kLanes; b++)
      buf[j * kLanes + b] = (uint32_t) acc[j][b];
}

}  // namespace

int alux_test(unsigned seed, int ninsn) {
  printf("=====================================================================================\n");
  printf("INFO - ALU extension test: seed %u, %d random instructions\n", seed, ninsn);
  Prog p;
  p.seed = seed;

  // reference state; the initial acc mixes full-range, small, zero and power-of-two values
  static int32_t acc[VTA_ACC_BUFF_DEPTH][kLanes];
  static int8_t out[VTA_ACC_BUFF_DEPTH][kLanes];
  uint8_t T[256];
  memset(acc, 0, sizeof acc);
  memset(out, 0, sizeof out);
  memset(T, 0, sizeof T);
  for (int j = 0; j < 2 * N; j++)
    for (int b = 0; b < kLanes; b++) {
      int k = rnd(p, 4);
      acc[j][b] = k == 0 ? (int32_t) (rand_r(&p.seed) ^ (rand_r(&p.seed) << 16))
                : k == 1 ? rnd(p, 601) - 300
                : k == 2 ? (rnd(p, 2) ? 0 : 1 << rnd(p, 31))
                : rnd(p, 256);
    }

  p.insns.push_back(get2DLoadStoreInsn(VTA_OPCODE_LOAD, VTA_MEM_ID_ACC, 0, 0, 1, 2 * N, 2 * N, 0, 0, 0, 0, 0, 0));
  uint32_t *accBuf = static_cast<uint32_t *>(allocBuffer(VTA_ACC_ELEM_BYTES * 2 * N));
  ppack(acc, 2 * N, accBuf);

  // out of every entry defined: CPY onto itself over [0, 2N)
  {
    VTAAluInsn a = blank(VTA_ALUX_CPY);
    a.iter_in = 2 * N;
    a.dst_factor_in = 1;
    a.src_factor_in = 1;
    addAlu(p, a, {{0, 0}});
  }
  // a whole table, then the random instructions with a new partial table now and then (imm and vector LUTSETs)
  {
    VTAAluInsn a = blank(VTA_ALUX_LUTSET);
    a.iter_in = 16;
    a.src_factor_in = 1;
    a.use_imm = 1;
    a.imm = (int16_t) (16 * rnd(p, 100));   // multiples of 16: the start entry wraps to 0
    addAlu(p, a, {{0, rnd(p, N - 16)}});
  }
  for (int k = 0; k < ninsn; k++) {
    if (rnd(p, 8) == 0) {
      VTAAluInsn a = blank(VTA_ALUX_LUTSET);
      a.iter_in = 1 + rnd(p, 16);
      a.src_factor_in = rnd(p, 3);
      a.use_imm = rnd(p, 2);
      a.imm = (int16_t) rand_r(&p.seed);
      addAlu(p, a, {{rnd(p, N), rnd(p, N - 32)}});
    } else {
      addRandom(p);
    }
  }
  // all 32 bits of acc[0, N): CPY to [N, 2N), then the bytes by SHR 8 (out = low 8 bits)
  {
    VTAAluInsn a = blank(VTA_ALUX_CPY);
    a.iter_in = N;
    a.dst_factor_in = 1;
    a.src_factor_in = 1;
    addAlu(p, a, {{N, 0}}, N);
    for (int s = 0; s < 3; s++) {
      VTAAluInsn h = blank(VTA_ALU_OPCODE_SHR);
      h.iter_in = N;
      h.dst_factor_in = 1;
      h.use_imm = 1;
      h.imm = 8;
      addAlu(p, h, {{N, 0}}, N);
    }
  }
  p.insns.push_back(getFinishInsn(0, 1));
  p.insns.insert(p.insns.begin(), get1DLoadStoreInsn(VTA_OPCODE_LOAD, VTA_MEM_ID_UOP, 0, 0, p.uops.size(), 0, 0, 0, 0));

  // reference: the ALU instructions in order, out snapshots at each STORE
  std::vector<int8_t> ref((size_t) p.storeOff.size() * N * kLanes);
  for (size_t k = 0; k < p.alus.size(); k++) {
    vta_alu_ref::run(p.alus[k], p.uops.data(), acc, out, T);
    memcpy(&ref[k * N * kLanes], out[p.storeOff[k]], (size_t) N * kLanes);
  }

  // HLS
  VTAGenericInsn *insnBuf = static_cast<VTAGenericInsn *>(allocBuffer(sizeof(VTAGenericInsn) * p.insns.size()));
  memcpy(insnBuf, p.insns.data(), sizeof(VTAGenericInsn) * p.insns.size());
  VTAUop *uopBuf = static_cast<VTAUop *>(allocBuffer(sizeof(VTAUop) * p.uops.size()));
  memcpy(uopBuf, p.uops.data(), sizeof(VTAUop) * p.uops.size());
  int8_t *outBuf = static_cast<int8_t *>(allocBuffer(ref.size()));
  memset(outBuf, 0x5a, ref.size());
  vta(p.insns.size(), (volatile insn_T *) insnBuf, (volatile uop_T *) uopBuf, (volatile bus_T *) NULL,
      (volatile bus_T *) NULL, (volatile bus_T *) accBuf, (volatile bus_T *) outBuf);

  int err = 0;
  for (size_t k = 0; k < p.storeOff.size(); k++) {
    int e = 0;
    for (int i = 0; i < N * kLanes; i++)
      if (outBuf[k * N * kLanes + i] != ref[k * N * kLanes + i]) {
        if (err + e < 10) {
          const VTAAluInsn &a = p.alus[k];
          printf("ERROR - store %zu (op %d imm %d use_imm %d rq %d lut_en %d) entry %d lane %d: hls %d ref %d\n", k,
                 VTA_ALU_FULL_OP(a), (int) a.imm, (int) a.use_imm, (int) a.rq_en, (int) a.lut_en,
                 p.storeOff[k] + i / kLanes, i % kLanes, outBuf[k * N * kLanes + i], ref[k * N * kLanes + i]);
        }
        e++;
      }
    err += e;
  }
  int hist[16] = {0};
  for (auto &a : p.alus)
    hist[VTA_ALU_FULL_OP(a)]++;
  printf("INFO - %zu ALU instructions (by op:", p.alus.size());
  for (int o = 0; o < 12; o++)
    printf(" %d", hist[o]);
  printf("), %zu uops, %zu bytes compared\n", p.uops.size(), ref.size());

  freeBuffer(insnBuf);
  freeBuffer(uopBuf);
  freeBuffer(accBuf);
  freeBuffer(outBuf);
  if (err == 0) {
    printf("INFO - ALU extension test successful!\n");
    return 0;
  }
  printf("INFO - ALU extension test failed, %d bytes differ!\n", err);
  return -1;
}

#endif  // VTA_ALUX
