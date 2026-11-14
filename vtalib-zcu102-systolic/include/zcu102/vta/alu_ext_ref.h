/*
 * #### JO added (2026-11-11, ALU extension, docs/evta_isa_ext.md 8): the reference semantics of one ALU instruction with the
 * extended operations (VTA_ALUX_* in hw_spec.h), requantize (rq_*) and the output LUT (lut_en). The vtalib simulator
 * runs the extended instructions with it, the instruction-program tests (tests/alu_ext_test.cc) check programs against
 * the CPU functions with it, and the HLS ALU (vta-hw-zcu102-systolic vta.cc alu()) has to give the same acc / out.
 *
 * The loop is VTA's ALU loop: for it_out, it_in, uop in [uop_bgn, uop_end): dst = uop.dst + offsets, src = uop.src +
 * offsets (both acc entries), every lane b of the BATCH x BLOCK_OUT entry:
 *   0 MIN / 1 MAX / 2 ADD (wraps)  dst = op(dst, s)     s = imm or acc[src][b]
 *   3 SHR                          dst = dst >> k        k = the low 5 bits of s, signed (-16..15; negative = left)
 *   4 MUL                          dst = dst * (int16) s (low 32 bits)
 *   5 SUB                          dst = dst - s (wraps)
 *   6 CPY                          dst = s
 *   7 LUT                          dst = T[dst & 255], sign-extended when imm bit 0 is 1, else zero-extended
 *   8 LUTSET                       T[16 * (imm + step) + b] = acc[src][b] & 255 (step = 0, 1, ... in loop order);
 *                                  writes neither acc nor out
 *   9 RSUM / 10 RMAX               dst[every lane] = sum (wraps) / max of the lanes of acc[src] (of imm: imm)
 *   11 CLZ                         dst = leading zeros of (uint32) dst, 32 for 0
 * then out[dst][b] = the low 8 bits of the new dst, or rq: clip((dst + round) >> rq_shift, rq_lo, rq_hi) as vta.cc
 * requant(); with lut_en, out = (int8) T[out & 255]. MIN / MAX / ADD / SHR are the existing HLS operations.
 */
#ifndef VTA_ALU_EXT_REF_H_
#define VTA_ALU_EXT_REF_H_

#include <cstdint>
#include <cstring>
#include <vector>

namespace vta_alu_ref {

static const int kLanes = (1 << VTA_LOG_BATCH) * (1 << VTA_LOG_BLOCK_OUT);

inline int32_t shr(int32_t v, int32_t s) {
  int k = (int)(s & 31);
  if (k & 16)
    k -= 32;
  return k >= 0 ? (int32_t)(v >> k) : (int32_t)((uint32_t)v << -k);
}

inline int8_t requant(int32_t v, int8_t o, bool en, int rnd, int shift, int lo, int hi) {
  if (!en)
    return o;
  uint32_t h = (rnd != 0 && shift != 0) ? (uint32_t)1 << (shift - 1) : 0;
  if (rnd == 2 && shift != 0 && v < 0)
    h -= 1;
  const int32_t t = (int32_t)((uint32_t)v + h);
  const int32_t q = t >> shift;
  return (int8_t)(q < lo ? lo : q > hi ? hi : q);
}

inline int clz32(uint32_t v) { return v ? __builtin_clz(v) : 32; }

// acc[depth][kLanes], out[depth][kLanes], uops as VTAUop, T the 256-entry LUT; Insn = VTAAluInsn
template <typename Insn, typename Uop>
void run(const Insn &insn, const Uop *uops, int32_t (*acc)[kLanes], int8_t (*out)[kLanes], uint8_t T[256]) {
  const int op = (int)(insn.alu_op_ext << VTA_ALU_OPCODE_BIT_WIDTH | insn.alu_opcode);
  const int32_t imm = (int32_t)insn.imm;
  int step = 0;
  uint32_t dst_out = 0, src_out = 0;
  for (uint32_t io = 0; io < insn.iter_out; io++) {
    uint32_t dst_in = dst_out, src_in = src_out;
    for (uint32_t ii = 0; ii < insn.iter_in; ii++) {
      for (uint32_t u = insn.uop_bgn; u < insn.uop_end; u++, step++) {
        const uint32_t d = uops[u].dst_idx + dst_in, s = uops[u].src_idx + src_in;
        int32_t *D = acc[d];
        const int32_t *S = acc[s];
        if (op == VTA_ALUX_LUTSET) {
          for (int b = 0; b < kLanes; b++)
            T[(16 * (imm + step) + b) & 255] = (uint8_t)(S[b] & 255);
          continue;
        }
        int32_t red = 0;
        if (op == VTA_ALUX_RSUM || op == VTA_ALUX_RMAX) {
          if (insn.use_imm) {
            red = imm;
          } else {
            red = op == VTA_ALUX_RSUM ? 0 : S[0];
            for (int b = 0; b < kLanes; b++)
              red = op == VTA_ALUX_RSUM ? (int32_t)((uint32_t)red + (uint32_t)S[b]) : (S[b] > red ? S[b] : red);
          }
        }
        for (int b = 0; b < kLanes; b++) {
          const int32_t x = D[b], y = insn.use_imm ? imm : S[b];
          int32_t r = x;
          switch (op) {
          case VTA_ALU_OPCODE_MIN: r = x < y ? x : y; break;
          case VTA_ALU_OPCODE_MAX: r = x > y ? x : y; break;
          case VTA_ALU_OPCODE_ADD: r = (int32_t)((uint32_t)x + (uint32_t)y); break;
          case VTA_ALU_OPCODE_SHR: r = shr(x, y); break;
          case VTA_ALUX_MUL: r = (int32_t)(uint32_t)((int64_t)x * (int16_t)(y & 0xffff)); break;
          case VTA_ALUX_SUB: r = (int32_t)((uint32_t)x - (uint32_t)y); break;
          case VTA_ALUX_CPY: r = y; break;
          case VTA_ALUX_LUT: r = (imm & 1) ? (int32_t)(int8_t)T[x & 255] : (int32_t)T[x & 255]; break;
          case VTA_ALUX_RSUM:
          case VTA_ALUX_RMAX: r = red; break;
          case VTA_ALUX_CLZ: r = clz32((uint32_t)x); break;
          default: break;
          }
          D[b] = r;
          int8_t o = requant(r, (int8_t)(r & 255), insn.rq_en, (int)insn.rq_round, (int)insn.rq_shift, (int)insn.rq_lo,
                             (int)insn.rq_hi);
          if (insn.lut_en)
            o = (int8_t)T[(uint8_t)o];
          out[d][b] = o;
        }
      }
      dst_in += insn.dst_factor_in;
      src_in += insn.src_factor_in;
    }
    dst_out += insn.dst_factor_out;
    src_out += insn.src_factor_out;
  }
}

// #### JO added (2026-11-11, docs/evta_isa_ext.md 8.8): steps of one instruction that read an acc entry (dst, or src unless
// use_imm) written by one of the `window` steps before it. The HLS ALU loop has no forwarding (DEPENDENCE inter false on
// acc_mem): with ALUX the vector loop is II 2, depth 33, so up to 17 steps are in flight and such a read gets the old value.
// #### JO (2026-11-14): with the 400 MHz registers the loop is depth 39; acc read at stage 5 (src) / 6 (dst), write at 38, so a
// step sees the write of the step d before it iff d >= 17 (2d + 5 > 38): the real window is 16 (directed C/RTL cosim, d = 14..16
// wrong, 17 / 18 right). The rule stays window 17 (one step of margin; the vtalib programs and checks keep using it).
// The imm loop (II 1, depth 12, MIN / MAX / ADD / SHR only) has up to 12. Returns the number of such steps.
template <typename Insn, typename Uop>
long hazards(const Insn &insn, const Uop *uops, int window) {
  const int op = (int)(insn.alu_op_ext << VTA_ALU_OPCODE_BIT_WIDTH | insn.alu_opcode);
  std::vector<long> wr;  // dst written by each step (-1: none)
  long bad = 0;
  uint32_t dst_out = 0, src_out = 0;
  for (uint32_t io = 0; io < insn.iter_out; io++) {
    uint32_t dst_in = dst_out, src_in = src_out;
    for (uint32_t ii = 0; ii < insn.iter_in; ii++) {
      for (uint32_t u = insn.uop_bgn; u < insn.uop_end; u++) {
        const long d = uops[u].dst_idx + dst_in, s = uops[u].src_idx + src_in;
        const bool rs = !insn.use_imm || op == VTA_ALUX_LUTSET;
        const bool rd = op != VTA_ALUX_LUTSET;
        for (int k = 1; k <= window && k <= (int)wr.size(); k++) {
          const long w = wr[wr.size() - k];
          if (w >= 0 && ((rd && w == d) || (rs && w == s))) {
            bad++;
            break;
          }
        }
        wr.push_back(op == VTA_ALUX_LUTSET ? -1 : d);
      }
      dst_in += insn.dst_factor_in;
      src_in += insn.src_factor_in;
    }
    dst_out += insn.dst_factor_out;
    src_out += insn.src_factor_out;
  }
  return bad;
}

} // namespace vta_alu_ref

#endif // VTA_ALU_EXT_REF_H_
