/*
 * #### JO added (2026-11-11, ALU extension, docs/evta_isa_ext.md 8.8): runs an ALU instruction program of
 * vtalib-zcu102-systolic/tests/alu_ext_test.cc (integer layer norm / softmax) on the HLS ALU in C simulation, in place of
 * the reference model (alu_ext_test.cc -DALU_EXT_HLS; sim/run_csim.sh PROG=1). One vta() call:
 *   LOAD UOP, LOAD ACC [0, depth), the program, STORE out [0, depth), then CPY [0, depth) -> [depth, 2 depth) and three
 *   SHR 8, each stored, from which acc [0, depth) is put back together (the tests read acc and out).
 */
#ifdef VTA_ALUX

#include <stdint.h>
#include <string.h>
#include <vector>

#include "../src/vta.h"
#include "../../../tests/hardware/common/test_lib.h"

namespace {
const int kL = VTA_BATCH * VTA_BLOCK_OUT;

VTAGenericInsn toGeneric(const VTAAluInsn &a) {
  union VTAInsn c;
  memset(&c, 0, sizeof c);
  c.alu = a;
  return c.generic;
}
}  // namespace

void hlsRunProgram(const VTAAluInsn *p, int np, const VTAUop *u, int nu, int32_t *acc, int8_t *out, int depth) {
  std::vector<VTAUop> uops(u, u + nu);
  std::vector<VTAGenericInsn> insns;
  int slots = 0;
  auto store = [&](int off) {
    insns.push_back(get2DLoadStoreInsn(VTA_OPCODE_STORE, VTA_MEM_ID_OUT, off, slots * depth, 1, depth, depth, 0, 0, 1, 0, 1,
                                       0));
    slots++;
  };
  auto alu = [&](VTAAluInsn a, bool popNext, bool pushNext) {
    a.pop_prev_dep = 0;
    a.pop_next_dep = popNext;
    a.push_prev_dep = 0;
    a.push_next_dep = pushNext;
    insns.push_back(toGeneric(a));
  };
  insns.push_back(get2DLoadStoreInsn(VTA_OPCODE_LOAD, VTA_MEM_ID_ACC, 0, 0, 1, depth, depth, 0, 0, 0, 0, 0, 0));
  for (int k = 0; k < np; k++)
    alu(p[k], false, k == np - 1);
  store(0);
  // acc [0, depth) bytes: CPY to [depth, 2 depth), out = low 8 bits; SHR 8 three times
  for (int s = 0; s < 4; s++) {
    VTAAluInsn a;
    memset(&a, 0, sizeof a);
    a.opcode = VTA_OPCODE_ALU;
    const int op = s == 0 ? VTA_ALUX_CPY : VTA_ALU_OPCODE_SHR;
    a.alu_opcode = op & 3;
    a.alu_op_ext = op >> 2;
    a.iter_out = 1;
    a.iter_in = depth;
    a.dst_factor_in = 1;
    a.src_factor_in = s == 0 ? 1 : 0;
    a.use_imm = s != 0;
    a.imm = 8;
    VTAUop o;
    memset(&o, 0, sizeof o);
    o.dst_idx = depth;
    o.src_idx = 0;
    a.uop_bgn = uops.size();
    uops.push_back(o);
    a.uop_end = uops.size();
    alu(a, true, true);
    store(depth);
  }
  insns.push_back(getFinishInsn(0, 1));
  insns.insert(insns.begin(), get1DLoadStoreInsn(VTA_OPCODE_LOAD, VTA_MEM_ID_UOP, 0, 0, uops.size(), 0, 0, 0, 0));

  VTAGenericInsn *insnBuf = static_cast<VTAGenericInsn *>(allocBuffer(sizeof(VTAGenericInsn) * insns.size()));
  memcpy(insnBuf, insns.data(), sizeof(VTAGenericInsn) * insns.size());
  VTAUop *uopBuf = static_cast<VTAUop *>(allocBuffer(sizeof(VTAUop) * uops.size()));
  memcpy(uopBuf, uops.data(), sizeof(VTAUop) * uops.size());
  uint32_t *accBuf = static_cast<uint32_t *>(allocBuffer(sizeof(int32_t) * depth * kL));
  memcpy(accBuf, acc, sizeof(int32_t) * depth * kL);
  int8_t *outBuf = static_cast<int8_t *>(allocBuffer((size_t)slots * depth * kL));
  vta(insns.size(), (volatile insn_T *)insnBuf, (volatile uop_T *)uopBuf, (volatile bus_T *)NULL, (volatile bus_T *)NULL,
      (volatile bus_T *)accBuf, (volatile bus_T *)outBuf);

  memcpy(out, outBuf, (size_t)depth * kL);
  for (int i = 0; i < depth * kL; i++) {
    uint32_t v = 0;
    for (int s = 0; s < 4; s++)
      v |= (uint32_t)(uint8_t)outBuf[(size_t)(1 + s) * depth * kL + i] << (8 * s);
    acc[i] = (int32_t)v;
  }
  freeBuffer(insnBuf);
  freeBuffer(uopBuf);
  freeBuffer(accBuf);
  freeBuffer(outBuf);
}

#endif  // VTA_ALUX
