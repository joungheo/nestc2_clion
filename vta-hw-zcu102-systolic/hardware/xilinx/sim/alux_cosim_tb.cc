/*
 * #### JO added (2026-11-13, ALU extension, docs/evta_isa_ext.md 8.11): C/RTL co-simulation of compute() alone with the
 * random ALU programs of sim/alux_test.cc (built with -DALUX_COSIM). compute() takes one instruction per call; this
 * testbench feeds them one by one, gives the dependence tokens the instructions pop, takes the ones they push and does
 * each STORE itself (out_mem rows -> the output buffer, as store() does). The SRAMs are testbench arrays, so their
 * contents carry over from call to call; uop_mem and the ALU tables are static inside compute and carry over too.
 * Run: sim/run_cosim.sh [seeds] [ninsn]  (vivado_hls in the docker container vivado2020-build).
 */
#if defined(VTA_ALUX) && defined(ALUX_COSIM)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/vta.h"

int alux_test(unsigned seed, int ninsn);

// m_axi depths of the cosim (sim/cosim_compute.tcl sets the same)
const int kUopDepth = 1024;
const int kBiasDepth = 4096;

static bus_T inp_mem[VTA_INP_BUFF_DEPTH][INP_MAT_AXI_RATIO];
static bus_T wgt_mem[VTA_WGT_BUFF_DEPTH][WGT_MAT_AXI_RATIO];
static bus_T out_mem[VTA_ACC_BUFF_DEPTH][OUT_MAT_AXI_RATIO];
static bus_T acc_mem[VTA_ACC_BUFF_DEPTH][ACC_MAT_AXI_RATIO];
static uop_T uop_buf[kUopDepth];
static bus_T bias_buf[kBiasDepth];

void alux_cosim_run(int ninsn, const VTAGenericInsn *insns, const VTAUop *uops, const uint32_t *acc, int8_t *out) {
  // the program's uops and acc values in the DRAM arrays of the m_axi ports
  int nuop = 0, nacc = 0;
  for (int i = 0; i < ninsn; i++) {
    const VTAMemInsn &m = *(const VTAMemInsn *) &insns[i];
    if (m.opcode == VTA_OPCODE_LOAD && m.memory_type == VTA_MEM_ID_UOP) nuop = m.dram_base + m.x_size;
    if (m.opcode == VTA_OPCODE_LOAD && m.memory_type == VTA_MEM_ID_ACC) nacc = (m.dram_base + m.x_size) * m.y_size;
  }
  if (nuop > kUopDepth || nacc * VTA_ACC_ELEM_BYTES > kBiasDepth * (int) sizeof(bus_T)) {
    printf("ERROR - program too large for the cosim depths (%d uops, %d acc entries)\n", nuop, nacc);
    exit(1);
  }
  memset(uop_buf, 0, sizeof uop_buf);
  memcpy(uop_buf, uops, (size_t) nuop * sizeof(VTAUop));
  memset(bias_buf, 0, sizeof bias_buf);
  memcpy(bias_buf, acc, (size_t) nacc * VTA_ACC_ELEM_BYTES);

  for (int i = 0; i < ninsn; i++) {
    const VTAMemInsn &m = *(const VTAMemInsn *) &insns[i];
    if (m.opcode == VTA_OPCODE_STORE) {
      // store(): y_size rows of x_size out entries; the compute instruction before it pushed a token we consumed
      for (int y = 0; y < (int) m.y_size; y++)
        memcpy(out + (size_t) (m.dram_base + y * m.x_stride) * VTA_OUT_ELEM_BYTES, &out_mem[m.sram_base + y * m.x_size][0],
               (size_t) m.x_size * VTA_OUT_ELEM_BYTES);
      continue;
    }
    hls::stream<insn_T> gemm_queue;
    hls::stream<bool> l2g, s2g, g2l, g2s;
    hls::stream<sa_uop_T> gemm_uop;
    hls::stream<bool> gemm_done;
    insn_T raw;
    memcpy(&raw, &insns[i], sizeof(VTAGenericInsn));
    gemm_queue.write(raw);
    if (m.pop_prev_dep) l2g.write(1);
    if (m.pop_next_dep) s2g.write(1);   // the STORE before it has finished
    uint32_t done = 0;
    compute(done, uop_buf, bias_buf, gemm_queue, l2g, s2g, g2l, g2s, inp_mem, wgt_mem, out_mem, acc_mem, gemm_uop,
            gemm_done);
    while (!g2l.empty()) g2l.read();
    while (!g2s.empty()) g2s.read();
  }
}

int main(int argc, char **argv) {
  int seeds = 3, ninsn = 48;
  if (getenv("VTA_ALUX_SEEDS")) seeds = atoi(getenv("VTA_ALUX_SEEDS"));
  if (getenv("VTA_ALUX_NINSN")) ninsn = atoi(getenv("VTA_ALUX_NINSN"));
  int status = 0;
  for (int s = 1; s <= seeds; s++) status |= alux_test(s, ninsn);
  printf("INFO - cosim ALU extension: %d seeds, %s\n", seeds, status ? "FAILED" : "all passed");
  return status ? 1 : 0;
}

#endif
