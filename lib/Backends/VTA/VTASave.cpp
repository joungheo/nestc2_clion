/*****************************************************************************
 *
 * Copyright Next-Generation System Software Research Group, All rights
 *reserved. Future Computing Research Division, Artificial Intelligence Reserch
 *Laboratory Electronics and Telecommunications Research Institute (ETRI)
 *
 * THESE DOCUMENTS CONTAIN CONFIDENTIAL INFORMATION AND KNOWLEDGE
 * WHICH IS THE PROPERTY OF ETRI. NO PART OF THIS PUBLICATION IS
 * TO BE USED FOR ANY OTHER PURPOSE, AND THESE ARE NOT TO BE
 * REPRODUCED, COPIED, DISCLOSED, TRANSMITTED, STORED IN A RETRIEVAL
 * SYSTEM OR TRANSLATED INTO ANY OTHER HUMAN OR COMPUTER LANGUAGE,
 * IN ANY FORM, BY ANY MEANS, IN WHOLE OR IN PART, WITHOUT THE
 * COMPLETE PRIOR WRITTEN PERMISSION OF ETRI.
 *
 * LICENSE file : LICENSE_ETRI located in the top directory
 *
 *****************************************************************************/

#include "VTA.h"
#include "VTACodeGen/VTASaver.h"
#include "glow/Graph/Graph.h"
#include "glow/Graph/Nodes.h"
#include "glow/IR/IR.h"
#include "glow/IR/IRUtils.h"
#include "glow/IR/Instrs.h"
#include "glow/Optimizer/IROptimizer/IROptimizer.h"
#include "llvm/Support/CommandLine.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <cmath>
#include <cstring>
#include <fstream>
#include <list>
#include <map>
#include <set>
#include <vector>

//#define VTA_PROFILE
//#define VTA_ALLINONE_FUNCTION
//#define VTA_RESET_CMDH
//#define VTA_MEMOPT_DISABLE

using namespace glow;

//#### JO added (2026-10-31): keep the activations between EVTA convs in the EVTA layout (findPackedActivations)
static llvm::cl::opt<bool> vtaKeepLayout(
    "vta-keep-layout",
    llvm::cl::desc("Keep activations in the EVTA layout N{C/16}HW16 between "
                   "layers instead of transposing around every conv"),
    llvm::cl::init(true));

//#### JO added (2026-10-31): conv + elementadd + relu in one EVTA call (findResidualFusions)
static llvm::cl::opt<bool> vtaFuseResidual(
    "vta-fuse-residual",
    llvm::cl::desc("Run an elementadd + relu that follows an EVTA conv in the "
                   "conv (convolution_wo_tr_res); needs -vta-keep-layout"),
    //#### default on since the board check (2026-10-31: bit-exact, docs/evta_runtime_compare.md 7.2)
    llvm::cl::init(true));

//#### JO added (2026-10-31): max pool on the EVTA (maxpool_evta) when its input is >= 0 (findEvtaMaxPools)
static llvm::cl::opt<bool> vtaEvtaMaxPool(
    "vta-evta-maxpool",
    llvm::cl::desc("Run a max pool of an activation in the EVTA layout whose "
                   "producer ends with relu on the EVTA (maxpool_evta)"),
    //#### default on since the board check (2026-10-31: bit-exact, docs/evta_runtime_compare.md 7.3)
    llvm::cl::init(true));

//#### JO added (2026-11-02): fully connected layer on the EVTA (fullyconnected_evta_packed, weights and bias written in
//#### the EVTA layout here; saveEvtaFullyConnectedInst), when its shape and quantization fit, else the CPU fullyconnected
static llvm::cl::opt<bool> vtaEvtaFc(
    "vta-evta-fc",
    llvm::cl::desc("Run fully connected layers on the EVTA (fullyconnected_evta_packed)"),
    llvm::cl::init(true));

//#### JO added (2026-11-09): an int8 MatMul whose rhs is an activation (the DeiT attention products; the ones with a
//#### constant rhs are EVTA convs, VTA.cpp -vta-matmul-conv) as an EVTA 1x1 conv with both operands packed at run time
//#### (vtalib matmul_evta), when the scale ratio is 2^-shift with 1 <= shift <= 15, else the CPU matmulI8
static llvm::cl::opt<bool> vtaEvtaMatMul(
    "vta-evta-matmul",
    llvm::cl::desc("Run int8 MatMuls with an activation on the right on the EVTA (matmul_evta)"),
    llvm::cl::init(true));

//#### JO added (2026-11-10): faster CPU arithmetic that may change values (softmaxI8 / layerNormI8: times a reciprocal
//#### instead of a division per element); off = the same result as the replaced instructions
static llvm::cl::opt<bool> vtaCpuApprox(
    "vta-cpu-approx",
    llvm::cl::desc("softmax / layer norm on the CPU with reciprocal multiplies instead of divisions (values may change)"),
    llvm::cl::init(true));

//#### JO added (2026-11-02): the input preprocessing (quantize of the float input, then elemsub / elemdiv by per-channel
//#### constants and an NCHW -> NHWC transpose) done by the input copy of the zero-filled first conv
//#### (preprocess_nhwc2vtaio_zerofill; findInputPreprocess)
static llvm::cl::opt<bool> vtaFusePreprocess(
    "vta-fuse-preprocess",
    llvm::cl::desc("Do the quantize / per-channel elemsub, elemdiv / transpose before the "
                   "zero-filled first EVTA conv in its input copy (preprocess_nhwc2vtaio_zerofill)"),
    llvm::cl::init(true));

//#### JO added (2026-11-02): a first conv with few channels (the image conv0) as a ceil(KH/s) x 1 / stride 1 conv over its
//#### packed input (s input rows and the KW taps in the channels; vtalib conv0pack_nhwc2vtaio / preprocess_conv0pack)
//#### instead of KH x KW / stride s over the channels zero-filled to 16 (getConv0Pack); off: the zero-filled conv
static llvm::cl::opt<bool> vtaConv0Pack(
    "vta-conv0-pack",
    llvm::cl::desc("Run the first conv with few channels over a packed input (stride rows and the "
                   "filter columns in the channels) instead of the channels zero-filled to 16"),
    llvm::cl::init(true));

//#### JO added (2026-11-02): the EVTA max pool 3x3 / stride 2 / pad 1 right after the packed first conv (-vta-conv0-pack)
//#### with relu done by that conv's call (convolution_wo_tr_pool, findPoolFusions): no conv output in DRAM, no pool call
static llvm::cl::opt<bool> vtaFusePool(
    "vta-fuse-pool",
    llvm::cl::desc("Do the EVTA max pool after the packed first conv in that conv's call (convolution_wo_tr_pool)"),
    llvm::cl::init(true));

//#### JO added (2026-11-08): a depthwise channelwise conv whose input and output are kept in the EVTA layout on the EVTA
//#### (vtalib dw_convolution_evta: diagonal weights per tap, per-channel power-of-two scales as a per-channel shift, the
//#### shift rounding half up; saveEvtaDepthwise) instead of vtalib cw_convolution on the CPU. It rounds half up where
//#### cw_convolution rounds as std::round (negative halves differ). On by default since 2026-11-08: it only touches the
//#### channelwise depthwise convs (-enable-channelwise-depthwise), the layers that do not fit stay on the CPU, and with
//#### 4 cores (OMP 4) it is only a little slower than cw_convolution (MobileNetV2 24.7 vs 22.5 ms; OMP 1 28.6 vs 43.2)
static llvm::cl::opt<bool> vtaEvtaDepthwise(
    "vta-evta-depthwise",
    llvm::cl::desc("Run depthwise channelwise convs (power-of-two scales, EVTA layout) on the EVTA (dw_convolution_evta)"),
    llvm::cl::init(true));

namespace vta_save{

//#### JO added (2026-10-31): an EVTA conv that also does the elementadd and the relu after it
struct ResidualFusion {
  const Value *residual; // the other elementadd operand
  const Value *out;      // relu dest: written by the fused call
  int convShl, resShl, sumShr;
  int reluShl;           //#### JO added (2026-11-05): relu dest scale = elementadd dest scale / 2^reluShl
  bool hoistOut;         // out is allocated after the conv: its allocation is emitted before the conv
};

//#### JO added (2026-11-02): a max pool done by the first conv's call (findPoolFusions)
struct PoolFusion {
  const Instruction *pool;
  const Value *out; // pool dest: written by the fused call
  bool hoistOut;    // out is allocated after the conv: its allocation is emitted before the conv
};

//#### JO added (2026-11-02): the preprocessing done in the input copy of a zero-filled first conv (findInputPreprocess)
struct InputPreprocess {
  const QuantizeInst *quantize; // its float source is read by the copy
  bool nchw;                    // the source is NCHW (the transpose to the conv's NHWC is folded in)
  std::vector<int8_t> lut;      // [C][256] value after the elemsub / elemdiv chain, empty: none
};

//#### JO added (2026-11-09): a run of elementwise instructions from one int8 buffer (root) to an int8 value (out), every
//#### other operand a constant with one value in all elements (findLutRegions, -vta-lut-elementwise): emitted as a table
//#### of its 256 results, computed once at run time by the same code on a 256-element probe (so the same result)
//#### JO added (2026-11-10): findLayerNorms
struct LayerNormFusion {
  const Value *x, *out, *p1, *p2, *eps, *e, *w, *b;
  size_t rows, cols;
  std::vector<std::string> scales; // the scale arguments as the replaced calls wrote them
};

//#### JO added (2026-11-10): findMatMulGroups / findAttnKeepLayout
struct MatMulGroup {
  std::vector<const MatMulInst *> mm;
  int shift = 0;
  bool qk = false;
  const Value *qbuf = nullptr, *kbuf = nullptr; // the conv outputs kept in the EVTA layout (qk)
  std::vector<const Value *> freeAfter;            // their deallocations (before the matmul in the IR) moved after it
};

struct LutRegion {
  std::vector<const Instruction *> instrs; // the region's instructions (in order; allocs / deallocs / views between them
                                           // are emitted as usual)
  const Value *root = nullptr, *out = nullptr; // origins
  size_t n = 0;                                // elements
  const Instruction *outAlloc = nullptr;       // the out's allocation when it lies inside the region (emitted first)
};

class VTASaveContext {
public:
  VTASaveContext(IRFunction::VariableMap *vmap) {
    variable_map = vmap;
    constantWeightVarsMemSize = 0;
    mutableWeightVarsMemSize = 0;
  }
  std::vector<struct SymbolTableEntry> *getSymbols() { return &syms; }
  std::vector<struct SymbolTableEntry> *getConstantSymbols() { return &csyms; }
  std::vector<struct SymbolTableEntry> *getTempSymbols() { return &tsyms; }
  uint64_t getCMemSize() { return constantWeightVarsMemSize; }
  void setCMemSize(uint64_t size) { constantWeightVarsMemSize = size; }
  uint64_t getMMemSize() { return mutableWeightVarsMemSize; }
  void setMMemSize(uint64_t size) { mutableWeightVarsMemSize = size; }
  void setWeightFileStream(std::ofstream *fos) { wfos = fos; }
  std::ofstream *getWeightFileStream() { return wfos; }
  IRFunction::VariableMap *getVariableMap() { return variable_map; }
  void setIdxMultiEVTA(uint32_t idx) { idxMultiEVTA = idx; }
  uint32_t getIdxMultiEVTA() { return idxMultiEVTA; }
  //#### JO added (2026-11-09): the bundle calls the generic CPU ops (NESTC_CPU_OPS is written into it)
  bool usesCpuOps = false;
  //#### JO added (2026-10-31): activations kept in the EVTA layout (findPackedActivations)
  std::set<const Value *> *getPacked() { return &packed; }
  bool isPacked(const Value *v) const { return packed.count(v) != 0; }
  //#### JO added (2026-10-31): fused conv + elementadd + relu (findResidualFusions)
  std::map<const Instruction *, ResidualFusion> *getFusions() { return &fusions; }
  std::set<const Instruction *> *getFusedAway() { return &fusedAway; }
  const ResidualFusion *getFusion(const Instruction *I) const {
    auto it = fusions.find(I);
    return it == fusions.end() ? nullptr : &it->second;
  }
  bool isFusedAway(const Instruction *I) const { return fusedAway.count(I) != 0; }
  std::set<const Value *> *getHoisted() { return &hoisted; }
  std::set<const Instruction *> *getEvtaMaxPools() { return &evtaMaxPools; }
  bool isEvtaMaxPool(const Instruction *I) const { return evtaMaxPools.count(I) != 0; }
  std::set<const Instruction *> *getEvtaDepthwise() { return &evtaDepthwise; }
  bool isEvtaDepthwise(const Instruction *I) const { return evtaDepthwise.count(I) != 0; }
  //#### JO added (2026-11-02): findInputPreprocess
  std::map<const Instruction *, InputPreprocess> *getPreprocs() { return &preprocs; }
  const InputPreprocess *getPreproc(const Instruction *I) const {
    auto it = preprocs.find(I);
    return it == preprocs.end() ? nullptr : &it->second;
  }
  std::set<const Instruction *> *getPreFused() { return &preFused; }
  //#### JO added (2026-11-02): findPoolFusions
  std::map<const Instruction *, PoolFusion> *getPoolFusions() { return &poolFusions; }
  const PoolFusion *getPoolFusion(const Instruction *I) const {
    auto it = poolFusions.find(I);
    return it == poolFusions.end() ? nullptr : &it->second;
  }
  bool isPreFused(const Instruction *I) const { return preFused.count(I) != 0; }
  //#### JO added (2026-11-09): findLutRegions (by the region's first instruction; the others are in lutAway)
  std::map<const Instruction *, LutRegion> *getLutRegions() { return &lutRegions; }
  std::set<const Instruction *> *getLutAway() { return &lutAway; }
  bool isLutAway(const Instruction *I) const { return lutAway.count(I) != 0; }
  std::string lutFuncs; // the functions that make the tables (file scope, before load_module)
  //#### JO added (2026-11-09): softmax of a dequantized int8 tensor emitted at the dequantize (findSoftmaxI8)
  std::map<const Instruction *, const SoftMaxInst *> softmaxAtDq;
  std::set<const Instruction *> softmaxAway;
  std::set<const Value *> softmaxHoist; // dest allocations after the dequantize
  std::map<const Instruction *, LayerNormFusion> layerNorms; // by the last instruction
  std::set<const Instruction *> layerNormAway;
  std::map<const Instruction *, MatMulGroup> matMulGroups; // by the first matmul
  std::set<const Instruction *> matMulAway;
  std::map<const Instruction *, MatMulGroup> *getMatMulGroups() { return &matMulGroups; }
  std::set<const Instruction *> *getMatMulAway() { return &matMulAway; }
  // a name that lives as long as the symbol tables (constants that are not IR values)
  const char *keepName(const std::string &name) {
    names.push_back(name);
    return names.back().c_str();
  }

private:
  IRFunction::VariableMap *variable_map;
  std::vector<struct SymbolTableEntry> syms;
  std::vector<struct SymbolTableEntry> csyms;
  std::vector<struct SymbolTableEntry> tsyms;
  uint64_t constantWeightVarsMemSize;
  uint64_t mutableWeightVarsMemSize;
  std::ofstream *wfos;
  uint32_t idxMultiEVTA;
  std::set<const Value *> packed;
  std::map<const Instruction *, ResidualFusion> fusions;
  std::set<const Instruction *> fusedAway;
  std::set<const Value *> hoisted; // allocations already emitted before a fused conv
  std::set<const Instruction *> evtaMaxPools; // max pools run by maxpool_evta
  std::set<const Instruction *> evtaDepthwise; //#### JO added (2026-11-08): depthwise convs run by dw_convolution_evta
  std::map<const Instruction *, InputPreprocess> preprocs; // by the first conv
  std::set<const Instruction *> preFused; // quantize / elemsub / elemdiv / transpose done by a first conv's input copy
  std::map<const Instruction *, PoolFusion> poolFusions; // by the first conv
  std::map<const Instruction *, LutRegion> lutRegions;
  std::set<const Instruction *> lutAway;
  std::list<std::string> names;
};

struct SymbolTableEntry {
  // Name of a variable.
  const char *name;
  // Offset of the variable inside the memory area.
  uint64_t offset;
  // The number of elements inside this variable.
  uint64_t size;
  // Variable kind: 1 if it is a mutable variable, 0 otherwise.
  char kind;
  WeightVar *wgt = nullptr;
};

SymbolTableEntry addSymbolEntry(WeightVar *wgt, VTASaveContext *ctx) {
  auto syms = ctx->getSymbols();
  struct SymbolTableEntry ste;
  for (auto s : *syms) {
    auto name = (const char *)(wgt->getName().data());

    if (strcmp(s.name, name) == 0) {
      ste = s;
      return ste;
    }
  }
  syms->push_back(
      {wgt->getName().data(), ctx->getMMemSize(), wgt->size(), '1', wgt});
  ste = syms->back();
  ctx->setMMemSize(ctx->getMMemSize() + wgt->getSizeInBytes());
  return ste;
}

SymbolTableEntry addSymbolEntryGenBundle(WeightVar *wgt, std::string *bundle,
                                         VTASaveContext *ctx) {
  auto syms = ctx->getSymbols();
  struct SymbolTableEntry ste;
  for (auto s : *syms) {
    auto name = (const char *)(wgt->getName().data());

    if (strcmp(s.name, name) == 0) {
      ste = s;
      return ste;
    }
  }

  auto csyms = ctx->getConstantSymbols();
  for (auto s : *csyms) {
    auto name = (const char *)(wgt->getName().data());

    if (strcmp(s.name, name) == 0) {
      ste = s;
      return ste;
    }
  }

  syms->push_back(
      {wgt->getName().data(), ctx->getMMemSize(), wgt->size(), '1', wgt});
  ste = syms->back();
  ctx->setMMemSize(ctx->getMMemSize() + wgt->getSizeInBytes());

  bundle->append("  int8_t* ");
  bundle->append(ste.name);
  bundle->append(" = (int8_t*)mutableWeight + ");
  bundle->append(std::to_string(ste.offset));
  bundle->append(";\n");

  return ste;
}

SymbolTableEntry addConstantSymbolEntry(Value* val, VTASaveContext *ctx){

  auto csyms = ctx->getConstantSymbols();
  struct SymbolTableEntry ste;
  for (auto s : *csyms) {
    auto name = (const char *) (val->getName().data());
    if (strcmp(s.name, name) == 0) {
      ste = s;
      return ste;
    }
  }
  csyms->push_back({val->getName().data(), ctx->getCMemSize(), val->getSizeInBytes(), '0'});
  ctx->setCMemSize(ctx->getCMemSize() + val->getSizeInBytes());
  ste = csyms->back();

  return ste;
}

//#### JO added (2026-11-02): a constant whose bytes in the weights file are not its own size (a layout with padding)
SymbolTableEntry addConstantSymbolEntrySized(Value *val, uint64_t size, VTASaveContext *ctx) {
  auto csyms = ctx->getConstantSymbols();
  csyms->push_back({val->getName().data(), ctx->getCMemSize(), size, '0'});
  ctx->setCMemSize(ctx->getCMemSize() + size);
  return csyms->back();
}

//#### JO added (2026-11-02): a constant made here that is not an IR value (its bytes written by the caller)
SymbolTableEntry addConstantSymbolEntryNamed(const std::string &name, uint64_t size, VTASaveContext *ctx) {
  auto csyms = ctx->getConstantSymbols();
  csyms->push_back({ctx->keepName(name), ctx->getCMemSize(), size, '0'});
  ctx->setCMemSize(ctx->getCMemSize() + size);
  return csyms->back();
}

static bool hasConstantSymbol(Value *val, VTASaveContext *ctx) {
  for (auto s : *ctx->getConstantSymbols())
    if (strcmp(s.name, val->getName().data()) == 0)
      return true;
  return false;
}

//#### JO added (2026-11-09): the bias symbol of an EVTA conv: <conv>_rbias when prepareVTAConvolutionInst gave it its own
//#### copy (a shared bias with the -vta-round-shift term), else the bias
static std::string convBiasSymbol(const Instruction *I, Value *bias, VTASaveContext *ctx) {
  const std::string own = I->getName().str() + "_rbias";
  for (auto s : *ctx->getConstantSymbols())
    if (own == s.name)
      return own;
  return addConstantSymbolEntry(bias, ctx).name;
}

//#### JO added (2026-11-09): CPU access to an IR value (the generic CPU ops of DeiT-tiny, constant operands of elemadd,
//#### tensor views of constants): a mutable weight (input / output) is declared from mutableWeight on its first use; a
//#### constant is written to the weights file on its first use, once (a VTA buffer, read through its virtual address);
//#### an activation or a tensor view is its pointer. The expression is an int8_t *.
static bool isConstantWeight(const Value *v) {
  auto *w = llvm::dyn_cast<WeightVar>(v);
  return w && w->getMutability() != WeightVar::MutabilityKind::Mutable;
}

static std::string cpuPtr(Value *v, std::string *bundle, VTASaveContext *ctx) {
  if (auto *w = llvm::dyn_cast<WeightVar>(v)) {
    if (w->getMutability() == WeightVar::MutabilityKind::Mutable)
      return addSymbolEntryGenBundle(w, bundle, ctx).name;
    if (!hasConstantSymbol(v, ctx)) {
      const Tensor *t = nullptr;
      for (auto &it : *ctx->getVariableMap())
        if (it.second == v && it.first->getKind() == Kinded::Kind::ConstantKind)
          t = &llvm::cast<Constant>(it.first)->getPayload();
      if (!t || t->getSizeInBytes() != v->getSizeInBytes()) {
        llvm::errs() << "VTASave: no payload for the constant " << v->getName() << "\n";
        std::exit(1);
      }
      addConstantSymbolEntry(v, ctx);
      ctx->getWeightFileStream()->write(t->getUnsafePtr(), t->getSizeInBytes());
    }
    return "(int8_t *)VTABufferGetVirtAddr(" + v->getName().str() + ")";
  }
  return v->getName().str();
}

SymbolTableEntry addConstantSymbolEntry_BNN(Value* val, VTASaveContext *ctx, bool first=false){

  auto csyms = ctx->getConstantSymbols();
  struct SymbolTableEntry ste;
  for (auto s : *csyms) {
    auto name = (const char *) (val->getName().data());
    if (strcmp(s.name, name) == 0) {
      ste = s;
      return ste;
    }
  }
  if(val->dims().size() == 4 && val->dims()[2] == 3 && val->dims()[1] == 3 && val->dims()[3] % 128 == 0 && val->dims()[0] % 128 == 0) {
    csyms->push_back({val->getName().data(), ctx->getCMemSize(), val->getSizeInBytes()/8, '0'});
    ctx->setCMemSize(ctx->getCMemSize() + val->getSizeInBytes()/8);
    ste = csyms->back();
  }
	else if (val->dims().size() == 4 && val->dims()[2] == 3 && val->dims()[1] == 3 && val->dims()[3] < 128 && val->dims()[0] % 16 == 0) {
    csyms->push_back({val->getName().data(), ctx->getCMemSize(), val->dims()[0]*val->dims()[1]*val->dims()[2]*16, '0'});
    ctx->setCMemSize(ctx->getCMemSize() + val->dims()[0]*val->dims()[1]*val->dims()[2]*16);
    ste = csyms->back();
  }
  else if (first) {
    csyms->push_back({val->getName().data(), ctx->getCMemSize(), val->dims()[0]*val->dims()[1]*val->dims()[2]*16, '0'});
    ctx->setCMemSize(ctx->getCMemSize() + val->dims()[0]*val->dims()[1]*val->dims()[2]*16);
    ste = csyms->back();
  }
  else {
    csyms->push_back({val->getName().data(), ctx->getCMemSize(), val->getSizeInBytes(), '0'});
    ctx->setCMemSize(ctx->getCMemSize() + val->getSizeInBytes());
    ste = csyms->back();
  }
  return ste;
}

//#### JO added (2026-11-10): scales compared up to float rounding: with non-power-of-two scales (-vta-pow2-ratio) the bias
//#### scale lhs * rhs and the conv's (1 / in) * (1 / w) differ in the last bits
static bool sameScale(float a, float b) { return std::fabs(a - b) <= 1e-5f * std::fabs(a); }
//#### JO added (2026-11-10): the callers pass a float ratio (1 / in) * (1 / w) / (1 / out); with non-power-of-two scales
//#### (-vta-pow2-ratio) it is 2^k only up to float rounding, and the int conversion of 7.9999 gave -1. Rounds to the
//#### nearest power of two when within 1e-4, else -1 as before.
int getExpofPowerofTwo(int x);
int getExpofPowerofTwo(float x) {
  if (!(x > 0))
    return getExpofPowerofTwo((int)x);
  const double l = std::log2((double)x);
  const double k = std::round(l);
  if (std::fabs(l - k) > 1e-4 / std::log(2.0))
    return -1;
  return k < 0 ? -1 : (int)k;
}
int getExpofPowerofTwo(int x) {
  int n = 0;
  // checks whether a number is zero or not
  if (x == -1)
    return 0;

  // true till x is not equal to 1
  while (x != 1) {
    // checks whether a number is divisible by 2
    if (x % 2 != 0)
      return -1;
    x /= 2;
    n++;
  }
  return n;
}

//#### JO added (2026-11-06): the conv's fused relu: RELU, or CLIP(0, max) (ReLU6 of MobileNetV2, VTA::supportsFusedActivation)
//#### = a relu with an upper clip at max in the output's int8 units (convClipHi, 127 = none); the call is put between
//#### VTASetActivationClip(hi) and VTASetActivationClip(127) (saveConvolutionInst), vtalib's convs clip to it
static bool convRelu(const glow::ConvolutionInst *I) {
  auto a = I->getFusedActivation();
  return a == RELU || (a == CLIP && I->getFusedActivationArgs().size() == 2 && I->getFusedActivationArgs()[0] == 0.0f);
}
static int convClipHi(const glow::ConvolutionInst *I) {
  if (I->getFusedActivation() != CLIP || I->getFusedActivationArgs().size() != 2)
    return 127;
  auto *T = I->getDest()->getType();
  long hi = std::lround(I->getFusedActivationArgs()[1] / T->getScale()) + T->getOffset();
  return hi < 127 ? (int)hi : 127;
}

void saveFloatConvolutionInst(const glow::ConvolutionInst *Inst,
                              std::string *bundle, VTASaveContext *ctx) {
  auto pad = Inst->getPads();
  auto strides = Inst->getStrides();
  auto dilation = Inst->getDilation();
  // TODO group
  // auto group = Inst->getGroup();
  auto src = Inst->getSrc();
  auto srcDims = src->dims();
  auto dest = Inst->getDest();
  auto filter = Inst->getFilter();
  auto filterDims = filter->dims();

  int N = srcDims[0];
  int H = srcDims[1];
  int W = srcDims[2];
  int C = srcDims[3];
  int KN = filterDims[0];
  int KH = filterDims[1];
  int KW = filterDims[2];

  auto bias = Inst->getBias();
  assert(bias->dims()[0] == filterDims[0]);

  int pad_size = pad[0];
  int stride_size = strides[0];
  int dilation_size = dilation[0];

  // save Weight file
  auto vMap = ctx->getVariableMap();
  const Tensor *tensor = NULL;

  for (auto it = vMap->begin(); it != vMap->end(); it++) {
    if (it->second == filter) {
      auto storage = it->first;
      if (storage->getKind() == glow::Kinded::Kind::ConstantKind) {
        const auto *constant = llvm::cast<Constant>(storage);
        tensor = &(constant->getPayload());
      }
      break;
    }
  }
  assert(tensor);

  auto handle = tensor->getHandle();
  {
    auto fos = ctx->getWeightFileStream();

    for (size_t i = 0, e = handle.size(); i < e; i++) {
      auto data = handle.raw(i);
      fos->write((const char *)(&data), sizeof(float));
    }
  }

  auto filterSte = addConstantSymbolEntry(filter, ctx);

  // save Bias file
  const Tensor *tensorBias = NULL;
  for (auto it = vMap->begin(); it != vMap->end(); it++) {
    if (it->second == bias) {
      auto storage = it->first;
      if (storage->getKind() == glow::Kinded::Kind::ConstantKind) {
        const auto *constant = llvm::cast<Constant>(storage);
        tensorBias = &(constant->getPayload());
      }
      break;
    }
  }
  assert(tensorBias);

  auto handleBias = tensorBias->getHandle();
  {
    auto fos = ctx->getWeightFileStream();
    for (size_t i = 0, e = handleBias.size(); i < e; i++) {
      auto data = handleBias.raw(i);
      fos->write((const char *)&data, sizeof(float));
    }
  }
  auto biasSte = addConstantSymbolEntry(bias, ctx);

  auto srcWeight = static_cast<WeightVar *>(src);
  if (srcWeight->getMutability() == glow::WeightVar::MutabilityKind::Mutable) {
    addSymbolEntryGenBundle(srcWeight, bundle, ctx);
  }

  auto destWeight = static_cast<WeightVar *>(dest);
  if (destWeight->getMutability() == glow::WeightVar::MutabilityKind::Mutable) {
    addSymbolEntryGenBundle(destWeight, bundle, ctx);
  }

  bundle->append("  convolutionFloat(");

  if (srcWeight->getMutability() == glow::WeightVar::MutabilityKind::Mutable) {
    auto ste = addSymbolEntry(srcWeight, ctx);
    bundle->append(ste.name);
    bundle->append(", ");
  } else {
    bundle->append(src->getName());
    bundle->append(", ");
  }

  bundle->append("(int8_t*)VTABufferGetVirtAddr(");
  bundle->append(filterSte.name);
  bundle->append("), (int8_t *)VTABufferGetVirtAddr(");
  bundle->append(biasSte.name);
  bundle->append("), ");

  if (destWeight->getMutability() == glow::WeightVar::MutabilityKind::Mutable) {
    auto ste = addSymbolEntry(destWeight, ctx);
    bundle->append(ste.name);
    bundle->append(", ");
  } else {
    bundle->append(dest->getName());
    bundle->append(", ");
  }

  bundle->append(std::to_string(N));
  bundle->append(", ");
  bundle->append(std::to_string(H));
  bundle->append(", ");
  bundle->append(std::to_string(W));
  bundle->append(", ");
  bundle->append(std::to_string(C));
  bundle->append(", ");
  bundle->append(std::to_string(KN));
  bundle->append(", ");
  bundle->append(std::to_string(KH));
  bundle->append(", ");
  bundle->append(std::to_string(KW));
  bundle->append(", ");
  bundle->append(std::to_string(pad_size));
  bundle->append(", ");
  bundle->append(std::to_string(stride_size));
  bundle->append(", ");
  bundle->append(std::to_string(Inst->getGroup()));
  bundle->append(", ");
  bundle->append(std::to_string(dilation_size));
  bundle->append(", ");
  bundle->append(std::to_string(Inst->getFusedActivation() ==
                                RELU)); // Fused Activation Disabled
  bundle->append(", ");
  bundle->append(std::to_string(1));
  bundle->append(", ");
  bundle->append(std::to_string(dest->dims()[1]));
  bundle->append(", ");
  bundle->append(std::to_string(dest->dims()[2]));
  bundle->append(" );\n");
}

void saveVTADWConvolutionInst(const glow::ConvolutionInst *Inst,
                              std::string *bundle, std::string *initConst,
                              VTASaveContext *ctx) {
  bundle->append("  dwc(...);\n");
}

void saveNonVTAConvolutionInst(
    const glow::ConvolutionInst *Inst, std::string *bundle,
    std::string *initConst,
    VTASaveContext *ctx) { // NON VTA Quantized Convolution
  auto pad = Inst->getPads();
  auto strides = Inst->getStrides();
  auto dilation = Inst->getDilation();
  // TODO : consider group
  // auto group = Inst->getGroup();
  auto src = Inst->getSrc();
  auto srcDims = src->dims();
  auto dest = Inst->getDest();
  auto filter = Inst->getFilter();
  auto filterDims = filter->dims();

  int N = srcDims[0];
  int H = srcDims[1];
  int W = srcDims[2];
  int C = srcDims[3];
  int KN = filterDims[0];
  int KH = filterDims[1];
  int KW = filterDims[2];

  auto bias = Inst->getBias();

  int pad_size = pad[0];
  int stride_size = strides[0];
  int dilation_size = dilation[0];

  //#### JO changed (2026-11-06): + CLIP (convRelu)
  // bool doRelu = Inst->getFusedActivation() == RELU;
  bool doRelu = convRelu(Inst);
  float inScale = src->getType()->getScale();
  float filterScale = filter->getType()->getScale();
  float outScale = Inst->getDest()->getType()->getScale();
  float biasScale = bias->getType()->getScale();

  filterScale = 1 / filterScale;
  inScale = 1 / inScale;
  biasScale = 1 / biasScale;
  outScale = 1 / outScale;

  // save Weight file
  auto vMap = ctx->getVariableMap();
  const Tensor *tensor = NULL;

  for (auto it = vMap->begin(); it != vMap->end(); it++) {
    if (it->second == filter) {
      auto storage = it->first;
      if (storage->getKind() == glow::Kinded::Kind::ConstantKind) {
        const auto *constant = llvm::cast<Constant>(storage);
        tensor = &(constant->getPayload());
      }
      break;
    }
  }
  assert(tensor);
  auto handle = tensor->getHandle<int8_t>();
  assert(handle.size() % 2 == 0);
  {
    auto fos = ctx->getWeightFileStream();
    int16_t data16 = 0;
    for (size_t i = 0, e = handle.size(); i < e; i++) {
      auto data = handle.raw(i);
      if (data > 127)
        data = 127.0;
      if (data < -128)
        data = -128.0;
      int8_t clip_data = std::floor(data);
      if (i % 2 == 0) {
        data16 = 0xff & clip_data;
      } else {
        data16 = data16 | clip_data << 8;
        fos->write((const char *)&data16, 2);
      }
    }
  }

  auto filterSte = addConstantSymbolEntry(filter, ctx);

  // save Bias file

  bool doBias = true;

  const Tensor *tensorBias = NULL;

  for (auto it = vMap->begin(); it != vMap->end(); it++) {
    if (it->second == bias) {
      auto storage = it->first;
      if (storage->getKind() == glow::Kinded::Kind::ConstantKind) {
        const auto *constant = llvm::cast<Constant>(storage);
        tensorBias = &(constant->getPayload());
      }
      break;
    }
  }
  assert(tensorBias);
  auto handleBias = tensorBias->getHandle<int32_t>();
  assert(handleBias.size() % 2 == 0);
  {
    auto fos = ctx->getWeightFileStream();
    for (size_t i = 0, e = handleBias.size(); i < e; i++) {
      auto data = handleBias.raw(i);
      fos->write((const char *)&data, 4);
    }
  }

  assert(sameScale(biasScale, inScale * filterScale));

  auto biasSte = addConstantSymbolEntry(bias, ctx);

  auto srcWeight = static_cast<WeightVar *>(src);
  if (srcWeight->getMutability() == glow::WeightVar::MutabilityKind::Mutable) {
    addSymbolEntryGenBundle(srcWeight, bundle, ctx);
  }

  auto destWeight = static_cast<WeightVar *>(dest);
  if (destWeight->getMutability() == glow::WeightVar::MutabilityKind::Mutable) {
    addSymbolEntryGenBundle(destWeight, bundle, ctx);
  }

  bundle->append("  nonvtaconvolution(");

  if (srcWeight->getMutability() == glow::WeightVar::MutabilityKind::Mutable) {
    auto ste = addSymbolEntry(srcWeight, ctx);
    bundle->append(ste.name);
    bundle->append(", ");
  } else {
    bundle->append(src->getName());
    bundle->append(", ");
  }
  bundle->append("1.0/");
  bundle->append(std::to_string(inScale));
  bundle->append(", 0, ");

  bundle->append("(int8_t *)VTABufferGetVirtAddr(");
  bundle->append(filterSte.name);
  bundle->append(")");

  bundle->append(", ");
  bundle->append("1.0/");
  bundle->append(std::to_string(filterScale));
  bundle->append(", 0, ");

  bundle->append("(int8_t *)VTABufferGetVirtAddr(");
  bundle->append(biasSte.name);
  bundle->append(")");
  bundle->append(", ");
  bundle->append("1.0/");
  bundle->append(std::to_string(biasScale));
  bundle->append(", 0, ");

  if (destWeight->getMutability() == glow::WeightVar::MutabilityKind::Mutable) {
    auto ste = addSymbolEntry(destWeight, ctx);
    bundle->append(ste.name);
    bundle->append(", ");
  } else {
    bundle->append(dest->getName());
    bundle->append(", ");
  }
  bundle->append("1.0/");
  bundle->append(std::to_string(outScale));
  bundle->append(", 0, ");

  bundle->append(std::to_string(N));
  bundle->append(", ");
  bundle->append(std::to_string(H));
  bundle->append(", ");
  bundle->append(std::to_string(W));
  bundle->append(", ");
  bundle->append(std::to_string(C));
  bundle->append(", ");
  bundle->append(std::to_string(KN));
  bundle->append(", ");
  bundle->append(std::to_string(KH));
  bundle->append(", ");
  bundle->append(std::to_string(KW));
  bundle->append(", ");
  bundle->append(std::to_string(pad_size));
  bundle->append(", ");
  bundle->append(std::to_string(stride_size));
  bundle->append(", ");
  bundle->append(std::to_string(Inst->getGroup()));
  bundle->append(", ");
  bundle->append(std::to_string(dilation_size));
  bundle->append(", ");
  bundle->append(std::to_string(doRelu));
  bundle->append(", ");
  bundle->append(std::to_string(doBias));
  bundle->append(", ");
  bundle->append(std::to_string(dest->dims()[1]));
  bundle->append(", ");
  bundle->append(std::to_string(dest->dims()[2]));
  bundle->append(" );\n");
}

//#### JO added (2026-11-07): the channelwise quantized conv (the depthwise convs with -enable-channelwise-depthwise) on the
//#### CPU, vtalib cw_convolution, bit-exact with Glow's Interpreter (fwdChannelwiseQuantizedConv2DInstImpl): per output
//#### channel d the bias in the accumulator scale, B[d] = round(float(bias[d]) * (biasScale[d] / matMulScale[d])), and the
//#### output multiplier M[d] = matMulScale[d] / outScale (matMulScale[d] = inScale * filterScale[d]), all computed here;
//#### out = clip(round(float(sum + B[d]) * M[d]), lo, hi) with the fused RELU / CLIP as lo / hi. A depthwise filter
//#### (group = C = KN) is written as [KH][KW][C] (16 channels per NEON vector), any other as Glow's [KN][KH][KW][C/group].
//#### Offsets must be 0 (symmetric schemas).
static void getConvSchedule(int N, int H, int W, int C, int KN, int KH, int KW, int stride, int pad, int *vt,
                            int *th, int *tw);
static void saveChannelwiseConvolutionInst(const glow::ChannelwiseQuantizedConvolutionInst *Inst, std::string *bundle,
                                           VTASaveContext *ctx) {
  auto src = Inst->getSrc();
  auto dest = Inst->getDest();
  auto filter = Inst->getFilter();
  auto bias = Inst->getBias();
  auto sd = src->dims(), fd = filter->dims(), dd = dest->dims();
  const int N = sd[0], H = sd[1], W = sd[2], C = sd[3], KN = fd[0], KH = fd[1], KW = fd[2], CG = fd[3];
  const int group = Inst->getGroup();
  auto P = Inst->getPads();
  auto S = Inst->getStrides();
  auto D = Inst->getDilation();

  auto payload = [&](Value *v) -> const Tensor * {
    auto vMap = ctx->getVariableMap();
    for (auto it = vMap->begin(); it != vMap->end(); it++)
      if (it->second == v && it->first->getKind() == glow::Kinded::Kind::ConstantKind)
        return &(llvm::cast<Constant>(it->first)->getPayload());
    return nullptr;
  };
  const Tensor *tF = payload(filter), *tB = payload(bias), *tFS = payload(Inst->getFilterScales()),
               *tFO = payload(Inst->getFilterOffsets()), *tBS = payload(Inst->getBiasScales()),
               *tBO = payload(Inst->getBiasOffsets());
  if (!tF || !tB || !tFS || !tFO || !tBS || !tBO || bias->getElementType() != ElemKind::Int32QTy ||
      src->getType()->getOffset() || dest->getType()->getOffset() || (int)dd[3] != KN || C != CG * group) {
    llvm::errs() << "ChannelwiseQuantizedConvolution " << Inst->getName()
                 << ": needs constant int8 filter, int32 bias, scales and offsets, offsets 0\n";
    std::exit(1);
  }
  auto hF = tF->getHandle<int8_t>();
  auto hB = tB->getHandle<int32_t>();
  auto hFS = tFS->getHandle<float>(), hBS = tBS->getHandle<float>();
  auto hFO = tFO->getHandle<int32_t>(), hBO = tBO->getHandle<int32_t>();
  const float inScale = src->getType()->getScale(), outScale = dest->getType()->getScale();
  std::vector<int32_t> b(KN);
  std::vector<float> m(KN);
  for (int d = 0; d < KN; d++) {
    if (hFO.raw(d) || hBO.raw(d)) {
      llvm::errs() << "ChannelwiseQuantizedConvolution " << Inst->getName() << ": filter / bias offset not 0\n";
      std::exit(1);
    }
    const float matMulScale = inScale * hFS.raw(d);
    b[d] = std::round(float(hB.raw(d)) * (hBS.raw(d) / matMulScale));
    m[d] = matMulScale / outScale;
  }
  const bool dw = group == C && KN == C && CG == 1;
  std::vector<int8_t> w(hF.size());
  for (int d = 0; d < KN; d++)
    for (int y = 0; y < KH; y++)
      for (int x = 0; x < KW; x++)
        for (int c = 0; c < CG; c++) {
          size_t from = (((size_t)d * KH + y) * KW + x) * CG + c;
          w[dw ? ((size_t)y * KW + x) * C + d : from] = hF.raw(from);
        }

  // the fused activation as the clip of the int8 output
  int lo = -128, hi = 127;
  auto act = Inst->getFusedActivation();
  auto args = Inst->getFusedActivationArgs();
  if (act == RELU) {
    lo = 0;
  } else if (act == CLIP && args.size() == 2) {
    lo = std::max<long>(-128, std::lround(args[0] / outScale));
    hi = std::min<long>(127, std::lround(args[1] / outScale));
  } else if (act != NONE) {
    llvm::errs() << "ChannelwiseQuantizedConvolution " << Inst->getName() << ": fused activation not supported\n";
    std::exit(1);
  }

  //#### JO added (2026-11-08): -vta-evta-depthwise: on the EVTA (dw_convolution_evta) when the input and output are kept
  //#### in the EVTA layout, N = 1, pads top = left (the bottom / right follow from the output size), no dilation and every
  //#### M[d] is 2^-shift[d]. Weights: per 16-channel block KH x KW wgt entries of 16 x 16 with w[c][y][x] on the
  //#### diagonal; bias B[d] + 2^(shift[d] - 1) (round half up); the shifts int32 per channel.
  std::vector<int32_t> sh(KN, -1);
  for (int d = 0; d < KN; d++)
    for (int k = 0; k <= 31; k++)
      if (m[d] == std::ldexp(1.0f, -k))
        sh[d] = k;
  if (vtaEvtaDepthwise && dw && C % 16 == 0 && N == 1 && ctx->isPacked(src) && ctx->isPacked(dest) && P[0] == P[1] &&
      (int)P[0] < KH && (int)P[1] < KW && D[0] == 1 && D[1] == 1 && S[0] == S[1] &&
      ((int)dd[1] - 1) * (int)S[0] + KH <= H + 2 * (KH - 1) && ((int)dd[2] - 1) * (int)S[1] + KW <= W + 2 * (KW - 1) &&
      std::all_of(sh.begin(), sh.end(), [](int32_t k) { return k >= 0; })) {
    const int CB = C / 16, KK = KH * KW;
    std::vector<int8_t> dwW((size_t)CB * KK * 256, 0);
    for (int d = 0; d < C; d++)
      for (int t = 0; t < KK; t++)
        dwW[((size_t)(d / 16) * KK + t) * 256 + (d % 16) * 17] = hF.raw((size_t)d * KK + t);
    std::vector<int32_t> dwB(KN);
    for (int d = 0; d < KN; d++)
      dwB[d] = b[d] + (sh[d] ? (int32_t)1 << (sh[d] - 1) : 0);
    auto fos = ctx->getWeightFileStream();
    fos->write((const char *)dwW.data(), dwW.size());
    auto filterSte = addConstantSymbolEntrySized(filter, dwW.size(), ctx);
    fos->write((const char *)dwB.data(), dwB.size() * 4);
    auto biasSte = addConstantSymbolEntrySized(bias, dwB.size() * 4, ctx);
    fos->write((const char *)sh.data(), sh.size() * 4);
    auto shiftSte = addConstantSymbolEntryNamed(std::string(Inst->getName()) + "_shift", sh.size() * 4, ctx);
    auto srcWeight = static_cast<WeightVar *>(src);
    auto destWeight = static_cast<WeightVar *>(dest);
    if (destWeight->getMutability() == glow::WeightVar::MutabilityKind::Mutable)
      addSymbolEntryGenBundle(destWeight, bundle, ctx);
    (void)srcWeight;
    int vt = 1, th = 8, tw = 8;
    getConvSchedule(N, H, W, C, 0, KH, KW, S[0], P[0], &vt, &th, &tw);
    uint32_t idx = ctx->getIdxMultiEVTA();
    bundle->append("  dw_convolution_evta(");
    bundle->append(src->getName().data());
    bundle->append("_vbuf, ");
    bundle->append(filterSte.name);
    bundle->append(", ");
    bundle->append(biasSte.name);
    bundle->append(", ");
    bundle->append(shiftSte.name);
    bundle->append(", ");
    bundle->append(dest->getName().data());
    bundle->append("_vbuf");
    for (int v : {N, H, W, C, KH, KW, (int)P[0], (int)S[0], (int)dd[1], (int)dd[2], lo, hi}) {
      bundle->append(", ");
      bundle->append(std::to_string(v));
    }
    bundle->append(", vtaCmdH");
    if (idx)
      bundle->append(std::to_string(idx));
    for (int v : {vt, th, tw, (int)idx}) {
      bundle->append(", ");
      bundle->append(std::to_string(v));
    }
    bundle->append(");\n  VTABufferInvalidate(");
    bundle->append(dest->getName().data());
    bundle->append("_vbuf, 0, ");
    bundle->append(std::to_string(dest->getSizeInBytes()));
    bundle->append(");\n");
    ctx->getEvtaDepthwise()->insert(Inst);
    return;
  }

  auto fos = ctx->getWeightFileStream();
  fos->write((const char *)w.data(), w.size());
  auto filterSte = addConstantSymbolEntrySized(filter, w.size(), ctx);
  fos->write((const char *)b.data(), b.size() * 4);
  auto biasSte = addConstantSymbolEntrySized(bias, b.size() * 4, ctx);
  fos->write((const char *)m.data(), m.size() * 4);
  auto multSte = addConstantSymbolEntryNamed(std::string(Inst->getName()) + "_mult", m.size() * 4, ctx);

  auto srcWeight = static_cast<WeightVar *>(src);
  auto destWeight = static_cast<WeightVar *>(dest);
  bool srcMut = srcWeight->getMutability() == glow::WeightVar::MutabilityKind::Mutable;
  bool destMut = destWeight->getMutability() == glow::WeightVar::MutabilityKind::Mutable;
  if (srcMut)
    addSymbolEntryGenBundle(srcWeight, bundle, ctx);
  if (destMut)
    addSymbolEntryGenBundle(destWeight, bundle, ctx);

  bundle->append("  cw_convolution(");
  bundle->append(srcMut ? addSymbolEntry(srcWeight, ctx).name : src->getName().data());
  bundle->append(", (int8_t *)VTABufferGetVirtAddr(");
  bundle->append(filterSte.name);
  bundle->append("), (int32_t *)VTABufferGetVirtAddr(");
  bundle->append(biasSte.name);
  bundle->append("), (float *)VTABufferGetVirtAddr(");
  bundle->append(multSte.name);
  bundle->append("), ");
  bundle->append(destMut ? addSymbolEntry(destWeight, ctx).name : dest->getName().data());
  //#### JO changed (2026-11-08): + the layouts of input and output (EVTA layout if kept packed, findPackedActivations)
  for (int v : {N, H, W, C, KN, KH, KW, (int)P[0], (int)P[1], (int)S[0], (int)S[1], (int)D[0], (int)D[1], group,
                (int)dd[1], (int)dd[2], lo, hi, (int)ctx->isPacked(src), (int)ctx->isPacked(dest)}) {
    bundle->append(", ");
    bundle->append(std::to_string(v));
  }
  bundle->append(");\n");
}

void saveBNNNonVTAConvolutionInst(const glow::BNNConvolutionInst *Inst,
                                  std::string *bundle,
                                  std::string *initConst,
                                  VTASaveContext *ctx) { //NON VTA Quantized Convolution
  auto pad = Inst->getPads();
  auto strides = Inst->getStrides();
  //TODO : consider group
  //auto group = Inst->getGroup();
  auto src = Inst->getSrc();
  auto srcDims = src->dims();
  auto dest = Inst->getDest();
  auto filter = Inst->getFilter();
  auto filterDims = filter->dims();

  int N = srcDims[0];
  int H = srcDims[1];
  int W = srcDims[2];
  int C = srcDims[3];
  int KN = filterDims[0];
  int KH = filterDims[1];
  int KW = filterDims[2];


  auto bias = Inst->getBias();

  int pad_size = pad[0];
  int stride_size = strides[0];

  //bool doRelu = Inst->getFusedActivation()==RELU;
  float inScale = src->getType()->getScale();
  float filterScale = filter->getType()->getScale();
  float outScale = Inst->getDest()->getType()->getScale();
  float biasScale = bias->getType()->getScale();


  filterScale = 1/filterScale;
  inScale = 1/inScale;
  biasScale = 1/biasScale;
  outScale = 1/outScale;


  //save Weight file
  auto vMap = ctx->getVariableMap();
  const Tensor* tensor = NULL;

  for(auto it = vMap->begin(); it != vMap->end(); it++)
  {
    if(it->second == filter)
    {
      auto storage = it->first;
      if (storage->getKind() == glow::Kinded::Kind::ConstantKind) {
        const auto *constant = llvm::cast<Constant>(storage);
        tensor = &(constant->getPayload());
      }
      break;
    }
  }
  assert(tensor);
  auto handle = tensor->getHandle<int8_t>();
  assert(handle.size() %2 == 0);
  {
    auto fos = ctx->getWeightFileStream();
    int16_t data16 = 0;
    for (size_t i = 0, e = handle.size(); i < e; i++) {
      auto data = handle.raw(i);
      if(data>127) data=127.0;
      if(data<-128) data=-128.0;
      int8_t clip_data = std::floor(data);
      if(i%2 == 0)
      {
        data16 = 0xff & clip_data;
      }
      else{
        data16 = data16 | clip_data<<8;
        fos->write((const char *)&data16, 2);
      }
    }
  }

  auto filterSte = addConstantSymbolEntry(filter, ctx);

  //save Bias file

  bool doBias = true;



  const Tensor* tensorBias = NULL;

  for(auto it = vMap->begin(); it != vMap->end(); it++)
  {
    if(it->second == bias)
    {
      auto storage = it->first;
      if (storage->getKind() == glow::Kinded::Kind::ConstantKind) {
        const auto *constant = llvm::cast<Constant>(storage);
        tensorBias = &(constant->getPayload());
      }
      break;
    }
  }
  assert(tensorBias);
  auto handleBias = tensorBias->getHandle<int32_t>();
  assert(handleBias.size() %2 == 0);
  {
    auto fos = ctx->getWeightFileStream();
    for (size_t i = 0, e = handleBias.size(); i < e; i++) {
      auto data = handleBias.raw(i);
      fos->write((const char *) &data, 4);
    }
  }

  assert(sameScale(biasScale, inScale * filterScale));

  auto biasSte = addConstantSymbolEntry(bias, ctx);

  auto srcWeight = static_cast<WeightVar *>(src);
  if (srcWeight->getMutability() == glow::WeightVar::MutabilityKind::Mutable) {
    addSymbolEntryGenBundle(srcWeight, bundle, ctx);
  }

  auto destWeight = static_cast<WeightVar *>(dest);
  if (destWeight->getMutability() == glow::WeightVar::MutabilityKind::Mutable) {
    addSymbolEntryGenBundle(destWeight, bundle, ctx);
  }

  bundle->append("  bnn_nonvtaconvolution(");

  if (srcWeight->getMutability() == glow::WeightVar::MutabilityKind::Mutable) {
    auto ste = addSymbolEntry(srcWeight, ctx);
    bundle->append(ste.name);
    bundle->append(", ");
  } else {
    bundle->append(src->getName());
    bundle->append(", ");
  }
  bundle->append("1.0/");
  bundle->append(std::to_string(inScale));
  bundle->append(", 0, ");


  bundle->append("(int8_t *)VTABufferGetVirtAddr(");
  bundle->append(filterSte.name);
  bundle->append(")");

  bundle->append(", ");
  bundle->append("1.0/");
  bundle->append(std::to_string(filterScale));
  bundle->append(", 0, ");


  bundle->append("(int8_t *)VTABufferGetVirtAddr(");
  bundle->append(biasSte.name);
  bundle->append(")");
  bundle->append(", ");
  bundle->append("1.0/");
  bundle->append(std::to_string(biasScale));
  bundle->append(", 0, ");

  if (destWeight->getMutability() == glow::WeightVar::MutabilityKind::Mutable) {
    auto ste = addSymbolEntry(destWeight, ctx);
    bundle->append(ste.name);
    bundle->append(", ");
  } else {
    bundle->append(dest->getName());
    bundle->append(", ");
  }
  bundle->append("1.0/");
  bundle->append(std::to_string(outScale));
  bundle->append(", 0, ");


  bundle->append(std::to_string(N));
  bundle->append(", ");
  bundle->append(std::to_string(H));
  bundle->append(", ");
  bundle->append(std::to_string(W));
  bundle->append(", ");
  bundle->append(std::to_string(C));
  bundle->append(", ");
  bundle->append(std::to_string(KN));
  bundle->append(", ");
  bundle->append(std::to_string(KH));
  bundle->append(", ");
  bundle->append(std::to_string(KW));
  bundle->append(", ");
  bundle->append(std::to_string(pad_size));
  bundle->append(", ");
  bundle->append(std::to_string(stride_size));
  bundle->append(", ");
  bundle->append(std::to_string(Inst->getGroup()));
  bundle->append(", ");
  bundle->append(std::to_string(Inst->getDilation()[0]));
  bundle->append(", ");
  bundle->append(std::to_string(0));
  bundle->append(", ");
  bundle->append(std::to_string(doBias));
  bundle->append(", ");
  bundle->append(std::to_string(dest->dims()[1]));
  bundle->append(", ");
  bundle->append(std::to_string(dest->dims()[2]));
  bundle->append(" );\n");


}

//#### JO added (2026-11-07): the EVTA conv's output shift truncates (acc >> shift, ALU SHR or requantize without rounding);
//#### the CPU convs and Glow's Interpreter round (std::round). -vta-round-shift adds 2^(shift - 1) to the bias at compile
//#### time, so the shift rounds half up (= std::round but for negative halves). Not for the fused convs (residual / pool:
//#### their shift comes after the add or the pool). Off by default: it changes every EVTA conv output of the other models.
static llvm::cl::opt<bool> vtaRoundShift(
    "vta-round-shift",
    llvm::cl::desc("Round the output shift of the EVTA convs (2^(shift-1) added to the bias) instead of truncating"),
    llvm::cl::init(false));

static int32_t convRoundHalf(const glow::ConvolutionInst *Inst, VTASaveContext *ctx) {
  if (!vtaRoundShift || ctx->getFusion(Inst) || ctx->getPoolFusion(Inst))
    return 0;
  float scale = (1 / Inst->getSrc()->getType()->getScale()) * (1 / Inst->getFilter()->getType()->getScale()) /
                (1 / Inst->getDest()->getType()->getScale());
  int shift = getExpofPowerofTwo(scale);
  return shift >= 1 ? (int32_t)1 << (shift - 1) : 0;
}

void prepareVTAConvolutionInst(const glow::ConvolutionInst *Inst,
                               std::string *bundle, std::string *initConst,
                               VTASaveContext *ctx) {
  // TODO : consider group
  // auto group = Inst->getGroup();
  auto src = Inst->getSrc();
  auto dest = Inst->getDest();
  auto filter = Inst->getFilter();
  auto filterDims = filter->dims();

  auto bias = Inst->getBias();
  assert(bias->dims()[0] == filterDims[0]);
  assert(filterDims[3] == src->dims()[3]);

  float inScale = src->getType()->getScale();
  float filterScale = filter->getType()->getScale();
  float outScale = Inst->getDest()->getType()->getScale();

  assert(Inst->getDest()->getType()->getOffset() == 0);
  assert(src->getType()->getOffset() == 0);
  assert(filter->getType()->getOffset() == 0);
  assert(bias->getType()->getOffset() == 0);

  filterScale = 1 / filterScale;
  inScale = 1 / inScale;
  outScale = 1 / outScale;
  float matMulScale = inScale * filterScale;
  assert(matMulScale / outScale > 1);

  // save Weight file
  auto vMap = ctx->getVariableMap();
  const Tensor *tensor = NULL;
  for (auto it = vMap->begin(); it != vMap->end(); it++) {
    if (it->second == filter) {
      auto storage = it->first;
      if (storage->getKind() == glow::Kinded::Kind::ConstantKind) {
        const auto *constant = llvm::cast<Constant>(storage);
        tensor = &(constant->getPayload());
      }
      break;
    }
  }
  assert(tensor);
  auto filterType = Type(tensor->getType());
  std::array<dim_t, 6> reshapeFilterS{{(dim_t)filterDims[0] / 16, 16,
                                       (dim_t)filterDims[1], filterDims[2],
                                       filterDims[3] / 16, 16}};
  llvm::ArrayRef<dim_t> reshapeFilterShape(reshapeFilterS);
  auto filterReshapeType = glow::Type::newShape(filterType, reshapeFilterShape);
  // auto filterReshapeDims = filterReshapeType.dims();
  Tensor filterReshapeTensor(filterReshapeType);
  filterReshapeTensor.zero();
  auto handle = tensor->getHandle<int8_t>();
  auto reshapeHandle = filterReshapeTensor.getHandle<int8_t>();

  for (int i = 0; i < handle.size(); i++) {
    auto value = handle.raw(i);
    reshapeHandle.raw(i) = value;
  }

  std::array<unsigned_t, 6> transposeFilterShuffle{{0, 4, 2, 3, 1, 5}};

  std::array<dim_t, 6> transposeFilterS{
      {(dim_t)reshapeFilterShape[0], (dim_t)reshapeFilterShape[2],
       (dim_t)reshapeFilterShape[4], (dim_t)reshapeFilterShape[5],
       (dim_t)reshapeFilterShape[1], (dim_t)reshapeFilterShape[3]}};
  llvm::ArrayRef<dim_t> transposeFilterShape(transposeFilterS);
  auto filterTransposeType =
      glow::Type::newShape(filterReshapeType, transposeFilterShape);
  Tensor filterTransposeTensor(filterTransposeType);

  filterReshapeTensor.transpose(&filterTransposeTensor, transposeFilterShuffle);

  auto transposeHandle = filterTransposeTensor.getHandle<int8_t>();

  assert(handle.size() % 2 == 0);
  //#### JO changed (2026-11-09): a filter shared by several convs is written once (its symbol is found by name, so a
  //#### second copy shifted every later constant; was: written for every conv)
  if (!hasConstantSymbol(filter, ctx)) {
    auto fos = ctx->getWeightFileStream();
    int16_t data16 = 0;
    for (size_t i = 0, e = transposeHandle.size(); i < e; i++) {
      auto data = transposeHandle.raw(i);
      if (data > 127)
        data = 127.0;
      if (data < -128)
        data = -128.0;
      int8_t clip_data = std::floor(data);
      if (i % 2 == 0) {
        data16 = 0xff & clip_data;
      } else {
        data16 = data16 | clip_data << 8;
        fos->write((const char *)&data16, 2);
      }
    }
  }

  addConstantSymbolEntry(filter, ctx);

  // save Bias file

  bool doBias = false;

  const Tensor *tensorBias = NULL;

  for (auto it = vMap->begin(); it != vMap->end(); it++) {
    if (it->second == bias) {
      auto storage = it->first;
      if (storage->getKind() == glow::Kinded::Kind::ConstantKind) {
        const auto *constant = llvm::cast<Constant>(storage);
        tensorBias = &(constant->getPayload());
      }
      break;
    }
  }
  assert(tensorBias);
  auto handleBias = tensorBias->getHandle<int32_t>();
  //#### JO added (2026-11-07): -vta-round-shift (convRoundHalf)
  const int32_t roundHalf = convRoundHalf(Inst, ctx);
  {
    doBias = roundHalf != 0;
    for (size_t i = 0, e = handleBias.size(); i < e; i++) {
      auto data = handleBias.raw(i);
      if (data != 0) {
        doBias = true;
        break;
      }
    }
    //#### JO changed (2026-11-09): the bias is written whenever its symbol is new, zero or not (the symbol takes its size
    //#### in the weights file either way; was: only when doBias, so a zero bias shifted the later constants), and a bias
    //#### shared by several convs (DeiT: the zero bias of the -vta-matmul-conv convs) once; with a rounding term each
    //#### later conv gets its own copy <conv>_rbias (convBiasSymbol), as the shift differs per conv
    const bool shared = hasConstantSymbol(bias, ctx);
    if (!shared || roundHalf != 0) {
      auto fos = ctx->getWeightFileStream();
      for (size_t i = 0, e = handleBias.size(); i < e; i++) {
        int32_t data = handleBias.raw(i) + roundHalf;
        fos->write((const char *)&data, 4);
      }
    }
    if (shared && roundHalf != 0)
      addConstantSymbolEntryNamed(Inst->getName().str() + "_rbias", bias->getSizeInBytes(), ctx);
  }
  if (doBias) {
    assert(!doBias || (1 / (bias->getType()->getScale()) == matMulScale));
  }

  addConstantSymbolEntry(bias, ctx);

  auto srcWeight = static_cast<WeightVar *>(src);
  if (srcWeight->getMutability() == glow::WeightVar::MutabilityKind::Mutable) {
    addSymbolEntryGenBundle(srcWeight, bundle, ctx);
  }

  auto destWeight = static_cast<WeightVar *>(dest);
  if (destWeight->getMutability() == glow::WeightVar::MutabilityKind::Mutable) {
    addSymbolEntryGenBundle(destWeight, bundle, ctx);
  }
}

void prepareEVTAConvolutionInst(const glow::VTAConvolutionInst *Inst,
                                std::string *bundle, std::string *initConst,
                                VTASaveContext *ctx) {
  // TODO : consider group
  // auto group = Inst->getGroup();
  auto src = Inst->getSrc();
  auto dest = Inst->getDest();
  auto filter = Inst->getFilter();
  auto filterDims = filter->dims();

  auto bias = Inst->getBias();
  // assert(bias->dims()[0]==filterDims[0]);
  // assert(filterDims[3] == src->dims()[3]);

  float inScale = src->getType()->getScale();
  float filterScale = filter->getType()->getScale();
  float outScale = Inst->getDest()->getType()->getScale();

  // assert(Inst->getDest()->getType()->getOffset() == 0);
  // assert(src->getType()->getOffset() == 0);
  // assert(filter->getType()->getOffset() == 0);
  // assert(bias->getType()->getOffset() == 0);

  filterScale = 1 / filterScale;
  inScale = 1 / inScale;
  outScale = 1 / outScale;
  float matMulScale = inScale * filterScale;
  // assert(matMulScale / outScale > 1);

  // save Weight file
  auto vMap = ctx->getVariableMap();
  const Tensor *tensor = NULL;
  for (auto it = vMap->begin(); it != vMap->end(); it++) {
    if (it->second == filter) {
      auto storage = it->first;
      if (storage->getKind() == glow::Kinded::Kind::ConstantKind) {
        const auto *constant = llvm::cast<Constant>(storage);
        tensor = &(constant->getPayload());
      }
      break;
    }
  }
  //    //assert(tensor);
  //    auto filterType = Type(tensor->getType());
  //    std::array<dim_t, 6> reshapeFilterS{{(dim_t)filterDims[0]/16, 16,
  //    (dim_t)filterDims[1], filterDims[2],filterDims[3]/16, 16}};
  //    llvm::ArrayRef<dim_t> reshapeFilterShape(reshapeFilterS);
  //    auto filterReshapeType = glow::Type::newShape(filterType,
  //    reshapeFilterShape);
  //    //auto filterReshapeDims = filterReshapeType.dims();
  //    Tensor filterReshapeTensor(filterReshapeType);
  //    filterReshapeTensor.zero();
  //    auto handle = tensor->getHandle<int8_t>();
  //    auto reshapeHandle = filterReshapeTensor.getHandle<int8_t>();
  //
  //    for(int i = 0; i < handle.size(); i++){
  //        auto value = handle.raw(i);
  //        reshapeHandle.raw(i) = value;
  //    }
  //
  //
  //    std::array<unsigned_t, 6> transposeFilterShuffle{{0, 4, 2, 3, 1, 5}};
  //
  //
  //    std::array<dim_t, 6> transposeFilterS{{(dim_t)reshapeFilterShape[0],
  //    (dim_t)reshapeFilterShape[2], (dim_t)reshapeFilterShape[4],
  //    (dim_t)reshapeFilterShape[5], (dim_t)reshapeFilterShape[1],
  //    (dim_t)reshapeFilterShape[3]}}; llvm::ArrayRef<dim_t>
  //    transposeFilterShape(transposeFilterS); auto filterTransposeType =
  //    glow::Type::newShape(filterReshapeType, transposeFilterShape); Tensor
  //    filterTransposeTensor(filterTransposeType);
  //
  //    filterReshapeTensor.transpose(&filterTransposeTensor,
  //    transposeFilterShuffle);
  //
  //    auto transposeHandle = filterTransposeTensor.getHandle<int8_t>();
  auto transposeHandle = tensor->getHandle<int8_t>();

  // assert(handle.size() %2 == 0);
  {
    auto fos = ctx->getWeightFileStream();
    int16_t data16 = 0;
    for (size_t i = 0, e = transposeHandle.size(); i < e; i++) {
      auto data = transposeHandle.raw(i);
      if (data > 127)
        data = 127.0;
      if (data < -128)
        data = -128.0;
      int8_t clip_data = std::floor(data);
      if (i % 2 == 0) {
        data16 = 0xff & clip_data;
      } else {
        data16 = data16 | clip_data << 8;
        fos->write((const char *)&data16, 2);
      }
    }
  }

  addConstantSymbolEntry(filter, ctx);

  // save Bias file

  bool doBias = false;

  const Tensor *tensorBias = NULL;

  for (auto it = vMap->begin(); it != vMap->end(); it++) {
    if (it->second == bias) {
      auto storage = it->first;
      if (storage->getKind() == glow::Kinded::Kind::ConstantKind) {
        const auto *constant = llvm::cast<Constant>(storage);
        tensorBias = &(constant->getPayload());
      }
      break;
    }
  }
  // assert(tensorBias);
  auto handleBias = tensorBias->getHandle<int32_t>();
  {
    for (size_t i = 0, e = handleBias.size(); i < e; i++) {
      auto data = handleBias.raw(i);
      if (data != 0) {
        doBias = true;
        break;
      }
    }
    if (doBias) {
      auto fos = ctx->getWeightFileStream();
      for (size_t i = 0, e = handleBias.size(); i < e; i++) {
        auto data = handleBias.raw(i);
        fos->write((const char *)&data, 4);
      }
    }
  }
  //    if(doBias){
  //        assert(!doBias || (1/(bias->getType()->getScale()) == matMulScale));
  //    }

  addConstantSymbolEntry(bias, ctx);

  auto srcWeight = static_cast<WeightVar *>(src);
  if (srcWeight->getMutability() == glow::WeightVar::MutabilityKind::Mutable) {
    addSymbolEntryGenBundle(srcWeight, bundle, ctx);
  }

  auto destWeight = static_cast<WeightVar *>(dest);
  if (destWeight->getMutability() == glow::WeightVar::MutabilityKind::Mutable) {
    addSymbolEntryGenBundle(destWeight, bundle, ctx);
  }
}

//#### JO added (2026-11-02): the schedule of a conv (VTASchedules.h), else the default 1 thread, 8 x 8
static void getConvSchedule(int N, int H, int W, int C, int KN, int KH, int KW, int stride, int pad, int *vt,
                            int *th, int *tw) {
#include "VTASchedules.h"
  for (auto elem : convTune.ConvolutionTune_)
    if (elem.isMatch(N, H, W, C, KN, KH, KW, C, stride, pad)) {
      *vt = elem.nVirtualThread_, *th = elem.tileHSize_, *tw = elem.tileWSize_;
      return;
    }
  *vt = 1, *th = 8, *tw = 8;
}

//#### JO added (2026-11-02): -vta-conv0-pack. The KH x KW / stride s conv over C channels (pad p) as a KH2 = ceil(KH/s)
//#### x 1 / stride 1 conv without pad over CO = s * KW * C (rounded up to 16) channels: packed pixel (q, x), channel
//#### (dh * KW + j) * C + c = padded input (s * q + dh, s * x + j, c) (vtalib conv0pack_nhwc2vtaio), filter tap (k, 0,
//#### (dh * KW + j) * C + c) = (s * k + dh, j, c), 0 where s * k + dh >= KH. Same sums, so the same output. Used when the
//#### schedule of the packed conv divides the output and its input tile fits half of the inp SRAM of the multi-EVTA
//#### buffers (1024 entries), else the zero-filled conv.
struct Conv0Pack {
  int H2, W2, CO, KH2;
};
static bool getConv0Pack(const glow::ConvolutionInst *Inst, Conv0Pack *p) {
  if (!vtaConv0Pack)
    return false;
  Conv0Pack tmp;
  if (!p)
    p = &tmp;
  auto srcDims = Inst->getSrc()->dims();
  auto filterDims = Inst->getFilter()->dims();
  auto destDims = Inst->getDest()->dims();
  auto pads = Inst->getPads();
  auto strides = Inst->getStrides();
  int N = srcDims[0], H = srcDims[1], W = srcDims[2], C = srcDims[3];
  int KN = filterDims[0], KH = filterDims[1], KW = filterDims[2];
  //#### JO changed (2026-11-07): pads top = left and bottom = right (TF SAME of a stride-2 conv, MobileNetV2: 0 and 1;
  //#### vtalib takes the bottom / right one as padEnd), were all equal
  int s = strides[0], pad = pads[0], padEnd = pads[2];
  if (strides[1] != s || pads[1] != pad || pads[3] != padEnd || s < 1 || KN % 16)
    return false;
  int outH = destDims[1], outW = destDims[2];
  if ((H + pad + padEnd - KH) / s + 1 != outH || (W + pad + padEnd - KW) / s + 1 != outW)
    return false;
  p->KH2 = (KH + s - 1) / s;
  p->CO = (s * KW * C + 15) / 16 * 16;
  p->H2 = outH + p->KH2 - 1;
  p->W2 = outW;
  //#### JO changed (2026-11-09): up to 1024 channels (DeiT patch embedding 16x16 / 16 over 3 channels: a 1x1 conv over 768;
  //#### the zero-filled 16x16 conv's input tile does not fit the inp SRAM), was 256 (vtalib's packing buffer)
  if (p->CO > 1024)
    return false;
  int vt, th, tw;
  getConvSchedule(N, p->H2, p->W2, p->CO, KN, p->KH2, 1, 1, 0, &vt, &th, &tw);
  //#### JO changed (2026-11-10): a 1x1 packed conv (KH2 = 1, DeiT) LOADs its input one channel block at a time: one block's
  //#### tile must fit (was all CO / 16 blocks, which kept the DeiT patch embedding at 7 x 2 tiles)
  // return outH % th == 0 && outW % tw == 0 && (th + p->KH2 - 1) * tw * (p->CO / 16) <= 1024;
  return outH % th == 0 && outW % tw == 0 && (th + p->KH2 - 1) * tw * (p->KH2 == 1 ? 1 : p->CO / 16) <= 1024;
}

// the packed conv's filter in the EVTA layout [KN/16][CO/16][KH2][1][16][16] into the weights file
static void writeConv0PackFilter(const glow::ConvolutionInst *Inst, const Tensor *tensor, const Conv0Pack &p,
                                 VTASaveContext *ctx) {
  auto filterDims = Inst->getFilter()->dims();
  int KN = filterDims[0], KH = filterDims[1], KW = filterDims[2], C = filterDims[3];
  int s = Inst->getStrides()[0], CB = p.CO / 16;
  auto handle = tensor->getHandle<int8_t>();
  std::vector<int8_t> out((size_t)KN * p.KH2 * p.CO, 0);
  for (int o = 0; o < KN; o++)
    for (int k = 0; k < p.KH2; k++)
      for (int dh = 0; dh < s && s * k + dh < KH; dh++)
        for (int j = 0; j < KW; j++)
          for (int c = 0; c < C; c++) {
            int ch = (dh * KW + j) * C + c;
            size_t idx = ((((size_t)(o / 16) * CB + ch / 16) * p.KH2 + k) * 16 + o % 16) * 16 + ch % 16;
            out[idx] = handle.at({(dim_t)o, (dim_t)(s * k + dh), (dim_t)j, (dim_t)c});
          }
  ctx->getWeightFileStream()->write((const char *)out.data(), out.size());
  addConstantSymbolEntrySized(Inst->getFilter(), out.size(), ctx);
}

void prepareFirstBNNVTAConvolutionInst(const glow::ConvolutionInst *Inst,
                                      std::string *bundle,
                                      std::string *initConst,
                                      VTASaveContext *ctx){
  auto src = Inst->getSrc();
  auto dest = Inst->getDest();
  auto filter = Inst->getFilter();
  auto filterDims = filter->dims();

  assert(filterDims[3] == 3);

  auto bias = Inst->getBias();
  float inScale = src->getType()->getScale();
  float filterScale = filter->getType()->getScale();
  float outScale = Inst->getDest()->getType()->getScale();
  filterScale = 1/filterScale;
  inScale = 1/inScale;
  outScale = 1/outScale;
  float matMulScale = inScale * filterScale;

  //save Weight file
  auto vMap = ctx->getVariableMap();
  const Tensor* tensor = NULL;
  for(auto it = vMap->begin(); it != vMap->end(); it++)
  {
    if(it->second == filter)
    {
      auto storage = it->first;
      if (storage->getKind() == glow::Kinded::Kind::ConstantKind) {
        const auto *constant = llvm::cast<Constant>(storage);
        tensor = &(constant->getPayload());
      }
      break;
    }
  }
  assert(tensor);

  //#### JO added (2026-11-02): -vta-conv0-pack: the packed conv's filter; the zero-filled one below otherwise
  Conv0Pack pack;
  if (getConv0Pack(Inst, &pack)) {
    writeConv0PackFilter(Inst, tensor, pack, ctx);
  } else {
  auto TempfilterType = Type(tensor->getType());
	std::array<dim_t, 4> TempFilterS{{(dim_t)filterDims[0], (dim_t)filterDims[1], (dim_t)filterDims[2], (dim_t)16}};
	llvm::ArrayRef<dim_t> TempFilterShape(TempFilterS);
  auto TempsignFilterShapeType = glow::Type::newShape(TempfilterType, TempFilterShape);
  Tensor TempFilterTensor(TempsignFilterShapeType);
  TempFilterTensor.zero();

  auto handle = tensor->getHandle<int8_t>();
  auto TempHandle = TempFilterTensor.getHandle<int8_t>();

  auto TempN = TempFilterS[0];
  auto TempH = TempFilterS[1];
  auto TempW = TempFilterS[2];
  auto TempC = TempFilterS[3];

  for(int i = 0; i < TempHandle.size(); i++){
    TempHandle.raw(i) = 0;
  }

  for (dim_t b = 0; b < TempN; ++b) {
    for (dim_t h = 0; h < TempH; ++h) {
      for (dim_t w = 0; w < TempW; ++w) {
        for (dim_t c = 0; c < (int)filterDims[3]; ++c) {
          TempHandle.at({b, h, w, c}) = handle.at({b, h, w, c});
        }
      }
    }
  }

  std::array<dim_t, 6> reshapeFilterS{{(dim_t)TempFilterShape[0]/16, 16, (dim_t)TempFilterShape[1], TempFilterShape[2], 16/16, 16}};
  llvm::ArrayRef<dim_t> reshapeFilterShape(reshapeFilterS);
  auto filterReshapeType = glow::Type::newShape(TempfilterType, reshapeFilterShape);
  Tensor filterReshapeTensor(filterReshapeType);
  filterReshapeTensor.zero();
  auto reshapeHandle = filterReshapeTensor.getHandle<int8_t>();

  for(int i = 0; i < TempHandle.size(); i++){
    auto value = TempHandle.raw(i);
    reshapeHandle.raw(i) = value;
  }

  std::array<unsigned_t, 6> transposeFilterShuffle{{0, 4, 2, 3, 1, 5}};
  std::array<dim_t, 6> transposeFilterS{{(dim_t)reshapeFilterShape[0], (dim_t)reshapeFilterShape[2], (dim_t)reshapeFilterShape[4], (dim_t)reshapeFilterShape[5], (dim_t)reshapeFilterShape[1], (dim_t)reshapeFilterShape[3]}};
  llvm::ArrayRef<dim_t> transposeFilterShape(transposeFilterS);
  auto filterTransposeType = glow::Type::newShape(filterReshapeType, transposeFilterShape);
  Tensor filterTransposeTensor(filterTransposeType);

  filterReshapeTensor.transpose(&filterTransposeTensor, transposeFilterShuffle);

  auto transposeHandle = filterTransposeTensor.getHandle<int8_t>();

  assert(handle.size() %2 == 0);
  {
    auto fos = ctx->getWeightFileStream();
    int16_t data16 = 0;
    for (size_t i = 0, e = transposeHandle.size(); i < e; i++) {
      auto data = transposeHandle.raw(i);
      if(data>127) data=127.0;
      if(data<-128) data=-128.0;
      int8_t clip_data = std::floor(data);
      if(i%2 == 0)
      {
        data16 = 0xff & clip_data;
      }
      else{
        data16 = data16 | clip_data<<8;
        fos->write((const char *)&data16, 2);
      }
    }
  }
  addConstantSymbolEntry_BNN(filter, ctx, true);
  }


  //save Bias file
  bool doBias = false;

  const Tensor* tensorBias = NULL;

  for(auto it = vMap->begin(); it != vMap->end(); it++)
  {
    if(it->second == bias)
    {
      auto storage = it->first;
      if (storage->getKind() == glow::Kinded::Kind::ConstantKind) {
        const auto *constant = llvm::cast<Constant>(storage);
        tensorBias = &(constant->getPayload());
      }
      break;
    }
  }
  assert(tensorBias);
  auto handleBias = tensorBias->getHandle<int32_t>();
  {
    for (size_t i = 0, e = handleBias.size(); i < e; i++) {
      auto data = handleBias.raw(i);
      if(data!=0){
        doBias = true;
        break;
      }
    }
    if(doBias) {
      auto fos = ctx->getWeightFileStream();
      for (size_t i = 0, e = handleBias.size(); i < e; i++) {
        auto data = handleBias.raw(i);
        fos->write((const char *) &data, 4);
      }
    }
  }

  addConstantSymbolEntry(bias, ctx);

  auto srcWeight = static_cast<WeightVar *>(src);
  if (srcWeight->getMutability() == glow::WeightVar::MutabilityKind::Mutable) {
    addSymbolEntryGenBundle(srcWeight, bundle, ctx);
  }

  auto destWeight = static_cast<WeightVar *>(dest);
  if (destWeight->getMutability() == glow::WeightVar::MutabilityKind::Mutable) {
    addSymbolEntryGenBundle(destWeight, bundle, ctx);
  }
}

void prepareBNNVTAConvolutionInst(const glow::BNNConvolutionInst *Inst,
                                  std::string *bundle,
                                  std::string *initConst,
                                  VTASaveContext *ctx,
                                  bool bnn_with_scale){
  auto src = Inst->getSrc();
  auto dest = Inst->getDest();
  auto filter = Inst->getFilter();
  auto filterDims = filter->dims();

  assert(filterDims[3] % 128 == 0);

  auto bias = Inst->getBias();
  assert(bias->dims()[0]==filterDims[0]);
  assert(filterDims[3] == src->dims()[3]);

  float inScale = src->getType()->getScale();
  float filterScale = filter->getType()->getScale();
  float outScale = Inst->getDest()->getType()->getScale();
  filterScale = 1/filterScale;
  inScale = 1/inScale;
  outScale = 1/outScale;
  float matMulScale = inScale * filterScale;

  //save Weight file
  auto vMap = ctx->getVariableMap();
  const Tensor* tensor = NULL;
  for(auto it = vMap->begin(); it != vMap->end(); it++)
  {
    if(it->second == filter)
    {
      auto storage = it->first;
      if (storage->getKind() == glow::Kinded::Kind::ConstantKind) {
        const auto *constant = llvm::cast<Constant>(storage);
        tensor = &(constant->getPayload());
      }
      break;
    }
  }
  assert(tensor);

  auto filterType = Type(tensor->getType());
  std::array<dim_t, 4> signFilterS{{(dim_t)filterDims[0], (dim_t)filterDims[1], (dim_t)filterDims[2], (dim_t)((int)filterDims[3]/8)}};
  llvm::ArrayRef<dim_t> signFilterShape(signFilterS);
  auto signFilterShapeType = glow::Type::newShape(filterType, signFilterShape);
  Tensor signFilterTensor(signFilterShapeType);
  signFilterTensor.zero();
  auto handle = tensor->getHandle<int8_t>();
  auto signHandle = signFilterTensor.getHandle<int8_t>();

  auto packN = signFilterS[0];
  auto packH = signFilterS[1];
  auto packW = signFilterS[2];
  auto packC = signFilterS[3];
  for (dim_t b = 0; b < packN; ++b) {
    for (dim_t h = 0; h < packH; ++h) {
      for (dim_t w = 0; w < packW; ++w) {
        for (dim_t c = 0; c < packC; ++c) {
          for(dim_t bw = 0; bw < 8; ++bw){
            signHandle.at({b, h, w, c})
                    += ((handle.at({b, h, w, (c*8 + bw)}) > 0) <<bw);
          }
        }
      }
    }
  }

  std::array<dim_t, 6> reshapeFilterS{{(dim_t)signFilterS[0]/16, 16, (dim_t)signFilterS[1], signFilterS[2],signFilterS[3]/16, 16}};
  llvm::ArrayRef<dim_t> reshapeFilterShape(reshapeFilterS);
  auto filterReshapeType = glow::Type::newShape(filterType, reshapeFilterShape);
  Tensor filterReshapeTensor(filterReshapeType);
  filterReshapeTensor.zero();
  auto reshapeHandle = filterReshapeTensor.getHandle<int8_t>();

  for(int i = 0; i < signHandle.size(); i++){
    auto value = signHandle.raw(i);
    reshapeHandle.raw(i) = value;
  }

  std::array<unsigned_t, 6> transposeFilterShuffle{{0, 4, 2, 3, 1, 5}};
  std::array<dim_t, 6> transposeFilterS{{(dim_t)reshapeFilterShape[0], (dim_t)reshapeFilterShape[2], (dim_t)reshapeFilterShape[4], (dim_t)reshapeFilterShape[5], (dim_t)reshapeFilterShape[1], (dim_t)reshapeFilterShape[3]}};
  llvm::ArrayRef<dim_t> transposeFilterShape(transposeFilterS);
  auto filterTransposeType = glow::Type::newShape(filterReshapeType, transposeFilterShape);
  Tensor filterTransposeTensor(filterTransposeType);

  filterReshapeTensor.transpose(&filterTransposeTensor, transposeFilterShuffle);

  auto transposeHandle = filterTransposeTensor.getHandle<int8_t>();

  assert(handle.size() %2 == 0);
  {
    auto fos = ctx->getWeightFileStream();
    int16_t data16 = 0;
    for (size_t i = 0, e = transposeHandle.size(); i < e; i++) {
      auto data = transposeHandle.raw(i);
      if(data>127) data=127.0;
      if(data<-128) data=-128.0;
      int8_t clip_data = std::floor(data);
      if(i%2 == 0)
      {
        data16 = 0xff & clip_data;
      }
      else{
        data16 = data16 | clip_data<<8;
        fos->write((const char *)&data16, 2);
      }
    }
  }
  addConstantSymbolEntry_BNN(filter, ctx);

  //save Bias file
  bool doBias = false;

  const Tensor* tensorBias = NULL;
  for(auto it = vMap->begin(); it != vMap->end(); it++)
  {
    if(it->second == bias)
    {
      auto storage = it->first;
      if (storage->getKind() == glow::Kinded::Kind::ConstantKind) {
        const auto *constant = llvm::cast<Constant>(storage);
        tensorBias = &(constant->getPayload());
      }
      break;
    }
  }
  assert(tensorBias);
  auto handleBias = tensorBias->getHandle<int32_t>();
  {
    for (size_t i = 0, e = handleBias.size(); i < e; i++) {
      auto data = handleBias.raw(i);
      if(data!=0){
        doBias = true;
        break;
      }
    }
    if(doBias) {
      auto fos = ctx->getWeightFileStream();
      for (size_t i = 0, e = handleBias.size(); i < e; i++) {
        auto data = handleBias.raw(i);
        fos->write((const char *) &data, 4);
      }
    }
  }

  addConstantSymbolEntry_BNN(bias, ctx);

  if (bnn_with_scale) {
    auto scalingfactor = Inst->getScalingfactor();
    assert(scalingfactor->dims()[0]==filterDims[0]);

    bool doMul = false;

    const Tensor* tensorScalingfactor = NULL;
    for(auto it = vMap->begin(); it != vMap->end(); it++)
    {
      if(it->second == scalingfactor)
      {
        auto storage = it->first;
        if (storage->getKind() == glow::Kinded::Kind::ConstantKind) {
          const auto *constant = llvm::cast<Constant>(storage);
          tensorScalingfactor = &(constant->getPayload());
        }
        break;
      }
    }
    assert(tensorScalingfactor);
    auto handleScalingfactor = tensorScalingfactor->getHandle<int32_t>();
    {
      for (size_t i = 0, e = handleScalingfactor.size(); i < e; i++) {
        auto data = handleScalingfactor.raw(i);
        if(data!=0){
          doMul = true;
          break;
        }
      }
      if(doMul) {
        auto fos = ctx->getWeightFileStream();
        for (size_t i = 0, e = handleScalingfactor.size(); i < e; i++) {
          auto data = handleScalingfactor.raw(i);
          fos->write((const char *) &data, 4);
        }
      }
      addConstantSymbolEntry_BNN(scalingfactor, ctx);
    }
  }
  auto srcWeight = static_cast<WeightVar *>(src);
  if (srcWeight->getMutability() == glow::WeightVar::MutabilityKind::Mutable) {
    addSymbolEntryGenBundle(srcWeight, bundle, ctx);
  }

  auto destWeight = static_cast<WeightVar *>(dest);
  if (destWeight->getMutability() == glow::WeightVar::MutabilityKind::Mutable) {
    addSymbolEntryGenBundle(destWeight, bundle, ctx);
  }
}

void preparezeroFillBNNVTAConvolutionInst(const glow::BNNConvolutionInst *Inst,
                                          std::string *bundle,
                                          std::string *initConst,
                                          VTASaveContext *ctx,
                                          bool bnn_with_scale){
  auto src = Inst->getSrc();
  auto dest = Inst->getDest();
  auto filter = Inst->getFilter();
  auto filterDims = filter->dims();

  assert(filterDims[3] != 3 && filterDims[1] != 1);

  auto bias = Inst->getBias();
  assert(bias->dims()[0]==filterDims[0]);
  assert(filterDims[3] == src->dims()[3]);

  float inScale = src->getType()->getScale();
  float filterScale = filter->getType()->getScale();
  float outScale = Inst->getDest()->getType()->getScale();
  filterScale = 1/filterScale;
  inScale = 1/inScale;
  outScale = 1/outScale;
  float matMulScale = inScale * filterScale;

  //save Weight file
  auto vMap = ctx->getVariableMap();
  const Tensor* tensor = NULL;
  for(auto it = vMap->begin(); it != vMap->end(); it++)
  {
    if(it->second == filter)
    {
      auto storage = it->first;
      if (storage->getKind() == glow::Kinded::Kind::ConstantKind) {
        const auto *constant = llvm::cast<Constant>(storage);
        tensor = &(constant->getPayload());
      }
      break;
    }
  }
  assert(tensor);

  auto TempfilterType = Type(tensor->getType());
	std::array<dim_t, 4> TempFilterS{{(dim_t)filterDims[0], (dim_t)filterDims[1], (dim_t)filterDims[2], (dim_t)128}};
	llvm::ArrayRef<dim_t> TempFilterShape(TempFilterS);
  auto TempsignFilterShapeType = glow::Type::newShape(TempfilterType, TempFilterShape);
  Tensor TempsignFilterTensor(TempsignFilterShapeType);
  TempsignFilterTensor.zero();

  auto handle = tensor->getHandle<int8_t>();
  auto TempsignHandle = TempsignFilterTensor.getHandle<int8_t>();

  auto TempN = TempFilterS[0];
  auto TempH = TempFilterS[1];
  auto TempW = TempFilterS[2];
  auto TempC = TempFilterS[3];

  for (dim_t b = 0; b < TempN; ++b) {
    for (dim_t h = 0; h < TempH; ++h) {
      for (dim_t w = 0; w < TempW; ++w) {
        for (dim_t c = 0; c < 128; ++c) {
					if(c % 2 == 0) TempsignHandle.at({b, h, w, c}) = 1;
					else TempsignHandle.at({b, h, w, c}) = 0;
        }
      }
    }
  }

  for (dim_t b = 0; b < TempN; ++b) {
    for (dim_t h = 0; h < TempH; ++h) {
      for (dim_t w = 0; w < TempW; ++w) {
        for (dim_t c = 0; c < (int)filterDims[3]; ++c) {
          TempsignHandle.at({b, h, w, c}) = handle.at({b, h, w, c});
        }
      }
    }
  }

  auto filterType = Type(tensor->getType());
  //std::array<dim_t, 4> signFilterS{{(dim_t)filterDims[0], (dim_t)filterDims[1], (dim_t)filterDims[2], (dim_t)((int)filterDims[3]/8)}};
  std::array<dim_t, 4> signFilterS{{(dim_t)filterDims[0], (dim_t)filterDims[1], (dim_t)filterDims[2], (dim_t)(128/8)}};
  llvm::ArrayRef<dim_t> signFilterShape(signFilterS);
  auto signFilterShapeType = glow::Type::newShape(filterType, signFilterShape);
  Tensor signFilterTensor(signFilterShapeType);
  signFilterTensor.zero();

	auto signHandle = signFilterTensor.getHandle<int8_t>();

  auto packN = signFilterS[0];
  auto packH = signFilterS[1];
  auto packW = signFilterS[2];
  auto packC = signFilterS[3];
  for (dim_t b = 0; b < packN; ++b) {
    for (dim_t h = 0; h < packH; ++h) {
      for (dim_t w = 0; w < packW; ++w) {
        for (dim_t c = 0; c < packC; ++c) {
          for(dim_t bw = 0; bw < 8; ++bw){
            signHandle.at({b, h, w, c})
                    += ((TempsignHandle.at({b, h, w, (c*8 + bw)}) > 0) <<bw);
          }
        }
      }
    }
  }

  //std::array<dim_t, 6> reshapeFilterS{{(dim_t)signFilterS[0]/16, 16, (dim_t)signFilterS[1], signFilterS[2],signFilterS[3]/16, 16}};
  std::array<dim_t, 6> reshapeFilterS{{(dim_t)signFilterS[0]/16, 16, (dim_t)signFilterS[1], signFilterS[2],signFilterS[3]/16, 16}};
  llvm::ArrayRef<dim_t> reshapeFilterShape(reshapeFilterS);
  auto filterReshapeType = glow::Type::newShape(filterType, reshapeFilterShape);
  Tensor filterReshapeTensor(filterReshapeType);
  filterReshapeTensor.zero();
  auto reshapeHandle = filterReshapeTensor.getHandle<int8_t>();

  for(int i = 0; i < signHandle.size(); i++){
    auto value = signHandle.raw(i);
    reshapeHandle.raw(i) = value;
  }

  std::array<unsigned_t, 6> transposeFilterShuffle{{0, 4, 2, 3, 1, 5}};
  std::array<dim_t, 6> transposeFilterS{{(dim_t)reshapeFilterShape[0], (dim_t)reshapeFilterShape[2], (dim_t)reshapeFilterShape[4], (dim_t)reshapeFilterShape[5], (dim_t)reshapeFilterShape[1], (dim_t)reshapeFilterShape[3]}};
  llvm::ArrayRef<dim_t> transposeFilterShape(transposeFilterS);
  auto filterTransposeType = glow::Type::newShape(filterReshapeType, transposeFilterShape);
  Tensor filterTransposeTensor(filterTransposeType);

  filterReshapeTensor.transpose(&filterTransposeTensor, transposeFilterShuffle);

  auto transposeHandle = filterTransposeTensor.getHandle<int8_t>();

  assert(handle.size() %2 == 0);
  {
    auto fos = ctx->getWeightFileStream();
    int16_t data16 = 0;
    for (size_t i = 0, e = transposeHandle.size(); i < e; i++) {
      auto data = transposeHandle.raw(i);
      if(data>127) data=127.0;
      if(data<-128) data=-128.0;
      int8_t clip_data = std::floor(data);
      if(i%2 == 0)
      {
        data16 = 0xff & clip_data;
      }
      else{
        data16 = data16 | clip_data<<8;
        fos->write((const char *)&data16, 2);
      }
    }
  }
  addConstantSymbolEntry_BNN(filter, ctx);


  //save Bias file
  bool doBias = false;

  const Tensor* tensorBias = NULL;

  for(auto it = vMap->begin(); it != vMap->end(); it++)
  {
    if(it->second == bias)
    {
      auto storage = it->first;
      if (storage->getKind() == glow::Kinded::Kind::ConstantKind) {
        const auto *constant = llvm::cast<Constant>(storage);
        tensorBias = &(constant->getPayload());
      }
      break;
    }
  }
  assert(tensorBias);
  auto handleBias = tensorBias->getHandle<int32_t>();
  {
    for (size_t i = 0, e = handleBias.size(); i < e; i++) {
      auto data = handleBias.raw(i);
      if(data!=0){
        doBias = true;
        break;
      }
    }
    if(doBias) {
      auto fos = ctx->getWeightFileStream();
      for (size_t i = 0, e = handleBias.size(); i < e; i++) {
        auto data = handleBias.raw(i);
        fos->write((const char *) &data, 4);
      }
    }
  }

  addConstantSymbolEntry_BNN(bias, ctx);

  if (bnn_with_scale) {
    auto scalingfactor = Inst->getScalingfactor();
    assert(scalingfactor->dims()[0]==filterDims[0]);

    bool doMul = false;

    const Tensor* tensorScalingfactor = NULL;
    for(auto it = vMap->begin(); it != vMap->end(); it++)
    {
      if(it->second == scalingfactor)
      {
        auto storage = it->first;
        if (storage->getKind() == glow::Kinded::Kind::ConstantKind) {
          const auto *constant = llvm::cast<Constant>(storage);
          tensorScalingfactor = &(constant->getPayload());
        }
        break;
      }
    }
    assert(tensorScalingfactor);
    auto handleScalingfactor = tensorScalingfactor->getHandle<int32_t>();
    {
      for (size_t i = 0, e = handleScalingfactor.size(); i < e; i++) {
        auto data = handleScalingfactor.raw(i);
        if(data!=0){
          doMul = true;
          break;
        }
      }
      if(doMul) {
        auto fos = ctx->getWeightFileStream();
        for (size_t i = 0, e = handleScalingfactor.size(); i < e; i++) {
          auto data = handleScalingfactor.raw(i);
          fos->write((const char *) &data, 4);
        }
      }
    }
    addConstantSymbolEntry_BNN(scalingfactor, ctx);
  }

  auto srcWeight = static_cast<WeightVar *>(src);
  if (srcWeight->getMutability() == glow::WeightVar::MutabilityKind::Mutable) {
    addSymbolEntryGenBundle(srcWeight, bundle, ctx);
  }

  auto destWeight = static_cast<WeightVar *>(dest);
  if (destWeight->getMutability() == glow::WeightVar::MutabilityKind::Mutable) {
    addSymbolEntryGenBundle(destWeight, bundle, ctx);
  }
}

void generateInputTranspose(const glow::ConvolutionInst *Inst,
                            std::string *bundle, std::string *initConst,
                            VTASaveContext *ctx) {
  // TODO : consider group
  // auto group = Inst->getGroup();
  auto src = Inst->getSrc();
  auto srcDims = src->dims();
  auto dest = Inst->getDest();
  auto filter = Inst->getFilter();

  int N = srcDims[0];
  int H = srcDims[1];
  int W = srcDims[2];
  int C = srcDims[3];
  //#### JO added (2026-10-31): the input is already in the EVTA layout (flushed by the CPU op that wrote it)
  if (ctx->isPacked(src)) {
    bundle->append("  int8_t* ");
    bundle->append(Inst->getName());
    bundle->append("_input_transpose = ");
    bundle->append(src->getName());
    bundle->append("_vbuf;\n");
    return;
  }
#ifdef VTA_PROFILE
  bundle->append("  clock_t ");
  bundle->append(Inst->getName());
  bundle->append("_input_transpose_start = clock();\n");
#endif

  bundle->append("  int8_t* ");
  bundle->append(Inst->getName());
  bundle->append("_input_transpose = (int8_t *)VTABufferAlloc(");
  bundle->append(std::to_string(N * H * W * C));
  bundle->append(");\n");
  bundle->append("  transpose_nhwc2vtaio(");
  auto srcWeight = static_cast<WeightVar *>(src);
  if (srcWeight->getMutability() == glow::WeightVar::MutabilityKind::Mutable) {
    auto ste = addSymbolEntry(srcWeight, ctx);
    bundle->append(ste.name);
    bundle->append(", ");
  } else {
    bundle->append(src->getName());
    bundle->append(", ");
  }

  bundle->append("(int8_t* )VTABufferGetVirtAddr(");
  bundle->append(Inst->getName());
  bundle->append("_input_transpose), ");

  bundle->append(std::to_string(N));
  bundle->append(", ");
  bundle->append(std::to_string(H));
  bundle->append(", ");
  bundle->append(std::to_string(W));
  bundle->append(", ");
  bundle->append(std::to_string(C));
  bundle->append(");\n");
  //#### JO added: flush the CPU-written transposed input before the EVTA reads it
  //#### (non-coherent EVTA, VTA_COHERENT_ACCESSES=false; no-op otherwise)
  bundle->append("  VTABufferFlush(");
  bundle->append(Inst->getName());
  bundle->append("_input_transpose, 0, ");
  bundle->append(std::to_string(N * H * W * C));
  bundle->append(");\n");

#ifdef VTA_PROFILE
  bundle->append("  clock_t ");
  bundle->append(Inst->getName());
  bundle->append("_input_transpose_end = clock();\n");
  bundle->append("  prof_out<<\"");
  bundle->append(Inst->getKindName());
  bundle->append("_input_transpose:");
  bundle->append(Inst->getName());
  bundle->append(" : \"<<");
  bundle->append("(double)(");
  bundle->append(Inst->getName());
  bundle->append("_input_transpose_end - ");
  bundle->append(Inst->getName());
  bundle->append("_input_transpose_start)/CLOCKS_PER_SEC*1000 << std::endl;\n");
#endif
}

void generateEVTAInputTranspose(const glow::VTAConvolutionInst *Inst,
                                std::string *bundle, std::string *initConst,
                                VTASaveContext *ctx) {
  // TODO : consider group
  // auto group = Inst->getGroup();
  auto src = Inst->getSrc();
  auto srcDims = src->dims();
  auto dest = Inst->getDest();
  auto filter = Inst->getFilter();

  int Nm = srcDims[0];
  int Cm = srcDims[1];
  int H = srcDims[2];
  int W = srcDims[3];
  int Ns = srcDims[4];
  int Cs = srcDims[5];

#ifdef VTA_PROFILE
  bundle->append("  clock_t ");
  bundle->append(Inst->getName());
  bundle->append("_input_transpose_start = clock();\n");
#endif

  //  VTABufferCopy(transposeInput_res, 0, conv__1_input_transpose, 0, 6272, 1);

  bundle->append("  int8_t* ");
  bundle->append(Inst->getName());
  bundle->append("_input_transpose = (int8_t *)VTABufferAlloc(");
  bundle->append(std::to_string(Nm * Cm * H * W * Ns * Cs));
  bundle->append(");\n");
  bundle->append("  VTABufferCopy(");
  auto srcWeight = static_cast<WeightVar *>(src);
  if (srcWeight->getMutability() == glow::WeightVar::MutabilityKind::Mutable) {
    auto ste = addSymbolEntry(srcWeight, ctx);
    bundle->append(ste.name);
    bundle->append(", ");
  } else {
    bundle->append(src->getName());
    bundle->append(", ");
  }
  bundle->append(std::to_string(0));
  bundle->append(", ");
  bundle->append(Inst->getName());
  bundle->append("_input_transpose, ");

  bundle->append(std::to_string(0));
  bundle->append(", ");
  bundle->append(std::to_string(Nm * Cm * H * W * Ns * Cs));
  bundle->append(", ");
  bundle->append(std::to_string(1));
  bundle->append(");\n");

#ifdef VTA_PROFILE
  bundle->append("  clock_t ");
  bundle->append(Inst->getName());
  bundle->append("_input_transpose_end = clock();\n");
  bundle->append("  prof_out<<\"");
  bundle->append(Inst->getKindName());
  bundle->append("_input_transpose:");
  bundle->append(Inst->getName());
  bundle->append(" : \"<<");
  bundle->append("(double)(");
  bundle->append(Inst->getName());
  bundle->append("_input_transpose_end - ");
  bundle->append(Inst->getName());
  bundle->append("_input_transpose_start)/CLOCKS_PER_SEC*1000 << std::endl;\n");
#endif
}

void generateFirstBNNInputTranspose(const glow::ConvolutionInst *Inst,
                                    std::string *bundle,
                                    std::string *initConst,
                                    VTASaveContext *ctx){

    auto src = Inst->getSrc();
    auto srcDims = src->dims();
    auto dest = Inst->getDest();
    auto filter = Inst->getFilter();

    int N = srcDims[0];
    int H = srcDims[1];
    int W = srcDims[2];
    int C = srcDims[3];

    if(C == 3) C = 16;
    //#### JO added (2026-11-02): -vta-conv0-pack: the packed input (conv0pack_nhwc2vtaio / preprocess_conv0pack)
    Conv0Pack pack;
    const bool packed = getConv0Pack(Inst, &pack);
    const std::string inBytes = std::to_string(packed ? (size_t)N * pack.CO * pack.H2 * pack.W2 : (size_t)N * H * W * C);
    auto packArgs = [&]() {
      for (int v : {(int)filter->dims()[1], (int)filter->dims()[2], (int)Inst->getStrides()[0], (int)Inst->getPads()[0],
                    pack.CO}) {
        bundle->append(", ");
        bundle->append(std::to_string(v));
      }
      //#### JO added (2026-11-07): the bottom / right pad when it is not the top / left one (padEnd, getConv0Pack)
      if (Inst->getPads()[2] != Inst->getPads()[0])
        bundle->append(", " + std::to_string(Inst->getPads()[2]));
    };

  #ifdef VTA_PROFILE
    bundle->append("  clock_t ");
    bundle->append(Inst->getName());
    bundle->append("_input_transpose_start = clock();\n");
  #endif

    bundle->append("  int8_t* ");
    bundle->append(Inst->getName());
    bundle->append("_input_transpose = (int8_t *)VTABufferAlloc(");
    //bundle->append(std::to_string(N*H*W*C));
    bundle->append(inBytes);
    bundle->append(");\n");
    //#### JO added (2026-11-02): the quantize (and elemsub / elemdiv / transpose) before it in this copy (findInputPreprocess)
    if (auto *pre = ctx->getPreproc(Inst)) {
      auto qsrc = pre->quantize->getSrc();
      auto qdestTy = pre->quantize->getDest()->getType();
      auto qsrcWeight = static_cast<WeightVar *>(qsrc);
      bool qsrcMut = qsrcWeight->getMutability() == glow::WeightVar::MutabilityKind::Mutable;
      if (qsrcMut)
        addSymbolEntryGenBundle(qsrcWeight, bundle, ctx);
      std::string lutName = "nullptr";
      if (!pre->lut.empty()) {
        auto ste = addConstantSymbolEntryNamed(std::string(Inst->getName()) + "_preprocess_lut", pre->lut.size(), ctx);
        ctx->getWeightFileStream()->write((const char *)pre->lut.data(), pre->lut.size());
        lutName = std::string("(const int8_t *)VTABufferGetVirtAddr(") + ste.name + ")";
      }
      //#### JO changed (2026-11-05): the packed input as preprocess_conv0pack_stream (into the VTA buffer, flushed; the
      //#### systolic vtalib writes it while the conv runs)
      bundle->append(packed ? "  preprocess_conv0pack_stream((const float *)" : "  preprocess_nhwc2vtaio_zerofill((const float *)");
      bundle->append(qsrcMut ? addSymbolEntry(qsrcWeight, ctx).name : qsrc->getName().data());
      bundle->append(packed ? ", " : ", (int8_t* )VTABufferGetVirtAddr(");
      bundle->append(Inst->getName());
      bundle->append(packed ? "_input_transpose, " : "_input_transpose), ");
      if (packed) {
        for (int v : {N, H, W, (int)srcDims[3], (int)pre->nchw}) {
          bundle->append(std::to_string(v));
          bundle->append(", ");
        }
      } else {
      for (int v : {N, H, W, (int)srcDims[3], C, (int)pre->nchw}) {
        bundle->append(std::to_string(v));
        bundle->append(", ");
      }
      }
      // the scale as saveQuantizeInst writes it
      bundle->append("1/");
      bundle->append(std::to_string(1 / qdestTy->getScale()));
      bundle->append(", ");
      bundle->append(std::to_string(qdestTy->getOffset()));
      bundle->append(", ");
      bundle->append(lutName);
      if (packed)
        packArgs();
      bundle->append(");\n");
    } else {
    bundle->append(packed ? "  conv0pack_nhwc2vtaio(" : "  transpose_nhwc2vtaio_zerofill(");
    auto srcWeight = static_cast<WeightVar *>(src);
    if (srcWeight->getMutability() == glow::WeightVar::MutabilityKind::Mutable) {
      auto ste = addSymbolEntry(srcWeight, ctx);
      bundle->append(ste.name);
      bundle->append(", ");
    } else {
      bundle->append(src->getName());
      bundle->append(", ");
    }

    bundle->append("(int8_t* )VTABufferGetVirtAddr(");
    bundle->append(Inst->getName());
    bundle->append("_input_transpose), ");

    bundle->append(std::to_string(N));
    bundle->append(", ");
    bundle->append(std::to_string(H));
    bundle->append(", ");
    bundle->append(std::to_string(W));
    bundle->append(", ");
    if (packed) {
      bundle->append(std::to_string(srcDims[3]));
      packArgs();
    } else {
    bundle->append(std::to_string(C));
    }
    bundle->append(");\n");
    }
    //#### JO added: flush the CPU-written transposed input before the EVTA reads it
    //#### (non-coherent EVTA, VTA_COHERENT_ACCESSES=false; no-op otherwise; preprocess_conv0pack_stream flushes itself)
    if (!(packed && ctx->getPreproc(Inst))) {
    bundle->append("  VTABufferFlush(");
    bundle->append(Inst->getName());
    bundle->append("_input_transpose, 0, ");
    bundle->append(inBytes);
    bundle->append(");\n");
    }

  #ifdef VTA_PROFILE
    bundle->append("  clock_t ");
      bundle->append(Inst->getName());
      bundle->append("_input_transpose_end = clock();\n");
      bundle->append("  prof_out<<\"");
      bundle->append(Inst->getKindName());
      bundle->append("_input_transpose:");
      bundle->append(Inst->getName());
      bundle->append(" : \"<<");
      bundle->append("(double)(");
      bundle->append(Inst->getName());
      bundle->append("_input_transpose_end - ");
      bundle->append(Inst->getName());
      bundle->append("_input_transpose_start)/CLOCKS_PER_SEC*1000 << std::endl;\n");
  #endif
}

void generateBNNInputTranspose(const glow::BNNConvolutionInst *Inst,
                              std::string *bundle,
                              std::string *initConst,
                              VTASaveContext *ctx){
  //TODO : consider group
  //auto group = Inst->getGroup();
  auto src = Inst->getSrc();
  auto srcDims = src->dims();
  auto dest = Inst->getDest();
  auto filter = Inst->getFilter();

  int N = srcDims[0];
  int H = srcDims[1];
  int W = srcDims[2];
  int C = srcDims[3];
  C = C/8;
#ifdef VTA_PROFILE
  bundle->append("  clock_t ");
  bundle->append(Inst->getName());
  bundle->append("_input_transpose_start = clock();\n");
#endif

  bundle->append("  int8_t* ");
  bundle->append(Inst->getName());
  bundle->append("_input_transpose = (int8_t *)VTABufferAlloc(");
  bundle->append(std::to_string(N*H*W*C));
  bundle->append(");\n");
  bundle->append("  transpose_nhwc2vtaio_pack(");
  auto srcWeight = static_cast<WeightVar *>(src);
  if (srcWeight->getMutability() == glow::WeightVar::MutabilityKind::Mutable) {
    auto ste = addSymbolEntry(srcWeight, ctx);
    bundle->append(ste.name);
    bundle->append(", ");
  } else {
    bundle->append(src->getName());
    bundle->append(", ");
  }

  bundle->append("(int8_t* )VTABufferGetVirtAddr(");
  bundle->append(Inst->getName());
  bundle->append("_input_transpose), ");

  bundle->append(std::to_string(N));
  bundle->append(", ");
  bundle->append(std::to_string(H));
  bundle->append(", ");
  bundle->append(std::to_string(W));
  bundle->append(", ");
  bundle->append(std::to_string(C));
  bundle->append(");\n");
  //#### JO added: flush the CPU-written transposed input before the EVTA reads it
  //#### (non-coherent EVTA, VTA_COHERENT_ACCESSES=false; no-op otherwise)
  bundle->append("  VTABufferFlush(");
  bundle->append(Inst->getName());
  bundle->append("_input_transpose, 0, ");
  bundle->append(std::to_string(N * H * W * C));
  bundle->append(");\n");

#ifdef VTA_PROFILE
  bundle->append("  clock_t ");
    bundle->append(Inst->getName());
    bundle->append("_input_transpose_end = clock();\n");
    bundle->append("  prof_out<<\"");
    bundle->append(Inst->getKindName());
    bundle->append("_input_transpose:");
    bundle->append(Inst->getName());
    bundle->append(" : \"<<");
    bundle->append("(double)(");
    bundle->append(Inst->getName());
    bundle->append("_input_transpose_end - ");
    bundle->append(Inst->getName());
    bundle->append("_input_transpose_start)/CLOCKS_PER_SEC*1000 << std::endl;\n");
#endif
}

void generatezeroFillBNNInputTranspose(const glow::BNNConvolutionInst *Inst,
                                      std::string *bundle,
                                      std::string *initConst,
                                      VTASaveContext *ctx){
  //TODO : consider group
  //auto group = Inst->getGroup();
  auto src = Inst->getSrc();
  auto srcDims = src->dims();
  auto dest = Inst->getDest();
  auto filter = Inst->getFilter();

  int N = srcDims[0];
  int H = srcDims[1];
  int W = srcDims[2];
  int C = srcDims[3];

	C = C/8;
	//C = 128/8;

#ifdef VTA_PROFILE
  bundle->append("  clock_t ");
  bundle->append(Inst->getName());
  bundle->append("_input_transpose_start = clock();\n");
#endif

  bundle->append("  int8_t* ");
  bundle->append(Inst->getName());
  bundle->append("_input_transpose = (int8_t *)VTABufferAlloc(");
  //bundle->append(std::to_string(N*H*W*C));
  bundle->append(std::to_string(N*H*W*16));
  bundle->append(");\n");
  bundle->append("  transpose_nhwc2vtaio_pack_zerofill(");
  auto srcWeight = static_cast<WeightVar *>(src);
  if (srcWeight->getMutability() == glow::WeightVar::MutabilityKind::Mutable) {
    auto ste = addSymbolEntry(srcWeight, ctx);
    bundle->append(ste.name);
    bundle->append(", ");
  } else {
    bundle->append(src->getName());
    bundle->append(", ");
  }

  bundle->append("(int8_t* )VTABufferGetVirtAddr(");
  bundle->append(Inst->getName());
  bundle->append("_input_transpose), ");

  bundle->append(std::to_string(N));
  bundle->append(", ");
  bundle->append(std::to_string(H));
  bundle->append(", ");
  bundle->append(std::to_string(W));
  bundle->append(", ");
  bundle->append(std::to_string(C));
  bundle->append(");\n");
  //#### JO added: flush the CPU-written transposed input before the EVTA reads it
  //#### (non-coherent EVTA, VTA_COHERENT_ACCESSES=false; no-op otherwise)
  bundle->append("  VTABufferFlush(");
  bundle->append(Inst->getName());
  bundle->append("_input_transpose, 0, ");
  bundle->append(std::to_string(N * H * W * 16));
  bundle->append(");\n");

#ifdef VTA_PROFILE
  bundle->append("  clock_t ");
    bundle->append(Inst->getName());
    bundle->append("_input_transpose_end = clock();\n");
    bundle->append("  prof_out<<\"");
    bundle->append(Inst->getKindName());
    bundle->append("_input_transpose:");
    bundle->append(Inst->getName());
    bundle->append(" : \"<<");
    bundle->append("(double)(");
    bundle->append(Inst->getName());
    bundle->append("_input_transpose_end - ");
    bundle->append(Inst->getName());
    bundle->append("_input_transpose_start)/CLOCKS_PER_SEC*1000 << std::endl;\n");
#endif
}

void declareOutputTranspose(const glow::ConvolutionInst *Inst,
                            std::string *bundle, std::string *initConst,
                            VTASaveContext *ctx) {
  // TODO : consider group
  // auto group = Inst->getGroup();
  auto dest = Inst->getDest();
  auto destDims = dest->dims();
  int outN = destDims[0];
  int outH = destDims[1];
  int outW = destDims[2];
  int outC = destDims[3];
  //#### JO added (2026-10-31): the EVTA writes straight into the activation kept in its layout
  //#### JO changed (2026-10-31): a fused conv writes the dest of the relu (findResidualFusions)
  if (ctx->isPacked(dest)) {
    auto *fu = ctx->getFusion(Inst);
    bundle->append("  int8_t* ");
    bundle->append(Inst->getName());
    bundle->append("_output_bef_transpose = ");
    //#### JO changed (2026-11-10): the activation under a view (the DeiT query / key convs, -vta-attn-keep-layout)
    bundle->append(fu ? fu->out->getName() : getOrigin(dest)->getName());
    bundle->append("_vbuf;\n");
    return;
  }

  bundle->append("  int8_t* ");
  bundle->append(Inst->getName());
  bundle->append("_output_bef_transpose = (int8_t *)VTABufferAlloc(");
  bundle->append(std::to_string(outN * outH * outW * outC));
  bundle->append(");\n");
}

void declareOutputTranspose(const glow::BNNConvolutionInst *Inst,
                            std::string *bundle,
                            std::string *initConst){
  //TODO : consider group
  //auto group = Inst->getGroup();
  auto dest = Inst->getDest();
  auto destDims = dest->dims();
  int outN = destDims[0];
  int outH = destDims[1];
  int outW = destDims[2];
  int outC = destDims[3];

  bundle->append("  int8_t* ");
  bundle->append(Inst->getName());
  bundle->append("_output_bef_transpose = (int8_t *)VTABufferAlloc(");
  bundle->append(std::to_string(outN*outH*outW*outC));
  bundle->append(");\n");
}

void declareEVTAOutputTranspose(const glow::VTAConvolutionInst *Inst,
                                std::string *bundle, std::string *initConst) {
  // TODO : consider group
  // auto group = Inst->getGroup();
  auto dest = Inst->getDest();
  auto destDims = dest->dims();
  int Nm = destDims[0];
  int Cm = destDims[1];
  int H = destDims[2];
  int W = destDims[3];
  int Ns = destDims[4];
  int Cs = destDims[5];

  bundle->append("  int8_t* ");
  bundle->append(Inst->getName());
  bundle->append("_output_bef_transpose = (int8_t *)VTABufferAlloc(");
  bundle->append(std::to_string(Nm * Cm * H * W * Ns * Cs));
  bundle->append(");\n");
}

void generateVTAConvolutionCall(const glow::ConvolutionInst *Inst,
                                std::string *bundle, std::string *initConst,
                                VTASaveContext *ctx) {
  auto pad = Inst->getPads();
  auto strides = Inst->getStrides();
  // TODO : consider group
  // auto group = Inst->getGroup();
  auto src = Inst->getSrc();
  auto srcDims = src->dims();
  auto dest = Inst->getDest();
  auto filter = Inst->getFilter();
  auto filterDims = filter->dims();
  auto bias = Inst->getBias();
  int N = srcDims[0];
  int H = srcDims[1];
  int W = srcDims[2];
  int C = srcDims[3];
  int KN = filterDims[0];
  int KH = filterDims[1];
  int KW = filterDims[2];
  int pad_size = pad[0];
  int stride_size = strides[0];
  //#### JO changed (2026-11-06): + CLIP (convRelu)
  // bool doRelu = Inst->getFusedActivation() == RELU;
  bool doRelu = convRelu(Inst);

  bool doBias = false;

  const Tensor *tensorBias = NULL;
  auto vMap = ctx->getVariableMap();
  for (auto it = vMap->begin(); it != vMap->end(); it++) {
    if (it->second == bias) {
      auto storage = it->first;
      if (storage->getKind() == glow::Kinded::Kind::ConstantKind) {
        const auto *constant = llvm::cast<Constant>(storage);
        tensorBias = &(constant->getPayload());
      }
      break;
    }
  }
  auto handleBias = tensorBias->getHandle<int32_t>();
  //#### JO changed (2026-11-07): + the rounding bias of -vta-round-shift (convRoundHalf)
  doBias = convRoundHalf(Inst, ctx) != 0;
  {
    for (size_t i = 0, e = handleBias.size(); i < e; i++) {
      auto data = handleBias.raw(i);
      if (data != 0) {
        doBias = true;
        break;
      }
    }
  }

  float inScale = src->getType()->getScale();
  float filterScale = filter->getType()->getScale();
  float outScale = Inst->getDest()->getType()->getScale();
  filterScale = 1 / filterScale;
  inScale = 1 / inScale;
  outScale = 1 / outScale;
  float matMulScale = inScale * filterScale;
  float scale = matMulScale / outScale;
  int shift = getExpofPowerofTwo(scale);

  auto filterSte = addConstantSymbolEntry(filter, ctx);
  //#### JO changed (2026-10-31): conv + elementadd + relu (findResidualFusions)
  // bundle->append("  convolution_wo_tr(");
  auto *fu = ctx->getFusion(Inst);
  bundle->append(fu ? "  convolution_wo_tr_res(" : "  convolution_wo_tr(");
  bundle->append(Inst->getName());
  bundle->append("_input_transpose, ");
  bundle->append(filterSte.name);
  bundle->append(", ");

  bundle->append("(int32_t *)");
  //#### JO changed (2026-11-09): convBiasSymbol (was: the bias symbol)
  bundle->append(convBiasSymbol(Inst, bias, ctx));
  bundle->append(", ");

  bundle->append(Inst->getName());
  bundle->append("_output_bef_transpose, ");

  bundle->append(std::to_string(N));
  bundle->append(", ");
  bundle->append(std::to_string(H));
  bundle->append(", ");
  bundle->append(std::to_string(W));
  bundle->append(", ");
  bundle->append(std::to_string(C));
  bundle->append(", ");
  bundle->append(std::to_string(KN));
  bundle->append(", ");
  bundle->append(std::to_string(KH));
  bundle->append(", ");
  bundle->append(std::to_string(KW));
  bundle->append(", ");
  bundle->append(std::to_string(pad_size));
  bundle->append(", ");
  bundle->append(std::to_string(stride_size));
  bundle->append(", ");
  bundle->append(std::to_string(doRelu));
  bundle->append(", ");
  bundle->append(std::to_string(doBias));
  bundle->append(", ");
  bundle->append(std::to_string(shift));
  bundle->append(", ");
  bundle->append(std::to_string(dest->dims()[1]));
  bundle->append(", ");
  bundle->append(std::to_string(dest->dims()[2]));
  if (fu) {
    bundle->append(", ");
    bundle->append(fu->residual->getName());
    bundle->append("_vbuf, ");
    bundle->append(std::to_string(fu->convShl));
    bundle->append(", ");
    bundle->append(std::to_string(fu->resShl));
    bundle->append(", ");
    bundle->append(std::to_string(fu->sumShr));
    //#### JO changed (2026-11-05): 1 + reluShl (vtalib sumRelu: 1 = relu, 1 + q = relu, << q, clip 127)
    // bundle->append(", 1");
    bundle->append(", ");
    bundle->append(std::to_string(1 + fu->reluShl));
  }
  bundle->append(", vtaCmdH");
  uint32_t idxMultiEVTA = ctx->getIdxMultiEVTA();
  if (idxMultiEVTA) {
    bundle->append(std::to_string(idxMultiEVTA));
  }
#ifdef NESTC_EVTA_PROFILE_AUTOTUNE
  bundle->append(", input1, input2, input3, ");
  bundle->append(std::to_string(ctx->getIdxMultiEVTA()));
#else
#include "VTASchedules.h"
  bool isTuned = 0;
  for (auto elem : convTune.ConvolutionTune_) {
    if (elem.isMatch(N, H, W, C, KN, KH, KW, C, stride_size, pad_size)) {
      bundle->append(", ");
      bundle->append(std::to_string(elem.nVirtualThread_));
      bundle->append(", ");
      bundle->append(std::to_string(elem.tileHSize_));
      bundle->append(", ");
      bundle->append(std::to_string(elem.tileWSize_));
      bundle->append(", ");
      bundle->append(std::to_string(ctx->getIdxMultiEVTA()));
      isTuned = 1;
      break;
    }
  }

  if (!isTuned) {
    bundle->append(", 1, 14, 14, ");
    bundle->append(std::to_string(ctx->getIdxMultiEVTA()));
  }
#endif
  bundle->append(");\n");
}

void generateEVTAConvolutionCall(const glow::VTAConvolutionInst *Inst,
                                 std::string *bundle, std::string *initConst,
                                 VTASaveContext *ctx) {
  auto pad = Inst->getPads();
  auto strides = Inst->getStrides();
  // TODO : consider group
  // auto group = Inst->getGroup();
  auto src = Inst->getSrc();
  auto srcDims = src->dims();
  auto dest = Inst->getDest();
  auto filter = Inst->getFilter();
  auto filterDims = filter->dims();
  auto bias = Inst->getBias();
  int Nm = srcDims[0];
  int Cm = srcDims[1];
  int H = srcDims[2];
  int W = srcDims[3];
  int Ns = srcDims[4];
  int Cs = srcDims[5];

  int KNm = filterDims[0];
  int KCm = filterDims[1];
  int KH = filterDims[2];
  int KW = filterDims[3];
  int KNs = filterDims[4];

  int pad_size = pad[0];
  int stride_size = strides[0];
  bool doRelu = Inst->getFusedActivation() == RELU;

  bool doBias = false;

  const Tensor *tensorBias = NULL;
  auto vMap = ctx->getVariableMap();
  for (auto it = vMap->begin(); it != vMap->end(); it++) {
    if (it->second == bias) {
      auto storage = it->first;
      if (storage->getKind() == glow::Kinded::Kind::ConstantKind) {
        const auto *constant = llvm::cast<Constant>(storage);
        tensorBias = &(constant->getPayload());
      }
      break;
    }
  }
  auto handleBias = tensorBias->getHandle<int32_t>();
  {
    for (size_t i = 0, e = handleBias.size(); i < e; i++) {
      auto data = handleBias.raw(i);
      if (data != 0) {
        doBias = true;
        break;
      }
    }
  }

  float inScale = src->getType()->getScale();
  float filterScale = filter->getType()->getScale();
  float outScale = Inst->getDest()->getType()->getScale();
  filterScale = 1 / filterScale;
  inScale = 1 / inScale;
  outScale = 1 / outScale;
  float matMulScale = inScale * filterScale;
  float scale = matMulScale / outScale;
  int shift = getExpofPowerofTwo(scale);

  auto filterSte = addConstantSymbolEntry(filter, ctx);
  bundle->append("  convolution_wo_tr(");
  bundle->append(Inst->getName());
  bundle->append("_input_transpose, ");
  bundle->append(filterSte.name);
  bundle->append(", ");

  bundle->append("(int32_t *)");
  auto biasSte = addConstantSymbolEntry(bias, ctx);
  bundle->append(biasSte.name);
  bundle->append(", ");

  bundle->append(Inst->getName());
  bundle->append("_output_bef_transpose, ");

  bundle->append(std::to_string(Nm * Ns));
  bundle->append(", ");
  bundle->append(std::to_string(H));
  bundle->append(", ");
  bundle->append(std::to_string(W));
  bundle->append(", ");
  bundle->append(std::to_string(Cm * Cs));
  bundle->append(", ");
  bundle->append(std::to_string(KNm * KNs));
  bundle->append(", ");
  bundle->append(std::to_string(KH));
  bundle->append(", ");
  bundle->append(std::to_string(KW));
  bundle->append(", ");
  bundle->append(std::to_string(pad_size));
  bundle->append(", ");
  bundle->append(std::to_string(stride_size));
  bundle->append(", ");
  bundle->append(std::to_string(doRelu));
  bundle->append(", ");
  bundle->append(std::to_string(doBias));
  bundle->append(", ");
  bundle->append(std::to_string(shift));
  bundle->append(", ");
  bundle->append(std::to_string(dest->dims()[2]));
  bundle->append(", ");
  bundle->append(std::to_string(dest->dims()[3]));
  bundle->append(", vtaCmdH");
  uint32_t idxMultiEVTA = ctx->getIdxMultiEVTA();
  if (idxMultiEVTA) {
    bundle->append(std::to_string(idxMultiEVTA));
  }
#include "VTASchedules.h"
  bool isTuned = 0;
  for (auto elem : convTune.ConvolutionTune_) {
    if (elem.isMatch(Nm * Ns, H, W, Cm * Cs, KNm * KNs, KH, KW, Cm * Cs,
                     stride_size, pad_size)) {
      bundle->append(", ");
      bundle->append(std::to_string(elem.nVirtualThread_));
      bundle->append(", ");
      bundle->append(std::to_string(elem.tileHSize_));
      bundle->append(", ");
      bundle->append(std::to_string(elem.tileWSize_));
      bundle->append(", ");
      bundle->append(std::to_string(ctx->getIdxMultiEVTA()));
      isTuned = 1;
      break;
    }
  }

  if (!isTuned) {
    bundle->append(", 1, 14, 14, ");
    bundle->append(std::to_string(ctx->getIdxMultiEVTA()));
  }

  bundle->append(");\n");
}

void generateFirstBNNVTAConvolutionCall(const glow::ConvolutionInst *Inst,
                                        std::string *bundle,
                                        std::string *initConst,
                                        VTASaveContext *ctx) {
  auto pad = Inst->getPads();
  auto strides = Inst->getStrides();
  //TODO : consider group
  //auto group = Inst->getGroup();
  auto src = Inst->getSrc();
  auto srcDims = src->dims();
  auto dest = Inst->getDest();
  auto filter = Inst->getFilter();
  auto filterDims = filter->dims();
  auto bias = Inst->getBias();
  int N = srcDims[0];
  int H = srcDims[1];
  int W = srcDims[2];
  int C = srcDims[3];
  if(C == 3) C = 16;
  int KN = filterDims[0];
  int KH = filterDims[1];
  int KW = filterDims[2];
  int pad_size = pad[0];
  int stride_size = strides[0];
  //#### JO added (2026-11-02): -vta-conv0-pack: the KH2 x 1 / stride 1 conv without pad over the packed input
  Conv0Pack pack;
  if (getConv0Pack(Inst, &pack)) {
    H = pack.H2, W = pack.W2, C = pack.CO, KH = pack.KH2, KW = 1, pad_size = 0, stride_size = 1;
  }
  //#### JO changed (2026-11-06): + CLIP (convRelu)
  // bool doRelu = Inst->getFusedActivation()==RELU;
  bool doRelu = convRelu(Inst);
  bool doBias = false;

  const Tensor* tensorBias = NULL;
  auto vMap = ctx->getVariableMap();
  for(auto it = vMap->begin(); it != vMap->end(); it++)
  {
    if(it->second == bias)
    {
      auto storage = it->first;
      if (storage->getKind() == glow::Kinded::Kind::ConstantKind) {
        const auto *constant = llvm::cast<Constant>(storage);
        tensorBias = &(constant->getPayload());
      }
      break;
    }
  }
  auto handleBias = tensorBias->getHandle<int32_t>();
  {
    for (size_t i = 0, e = handleBias.size(); i < e; i++) {
      auto data = handleBias.raw(i);
      if(data!=0){
        doBias = true;
        break;
      }
    }
  }

  float inScale = src->getType()->getScale();
  float filterScale = filter->getType()->getScale();
  float outScale = Inst->getDest()->getType()->getScale();
  filterScale = 1/filterScale;
  inScale = 1/inScale;
  outScale = 1/outScale;
  float matMulScale = inScale * filterScale;
  float scale =  matMulScale / outScale;
  int shift = getExpofPowerofTwo(scale);

  auto filterSte = addConstantSymbolEntry(filter, ctx);
  //#### JO added (2026-11-02): with the max pool after it (findPoolFusions)
  const PoolFusion *pf = ctx->getPoolFusion(Inst);
  bundle->append(pf ? "  convolution_wo_tr_pool(" : "  convolution_wo_tr(");
  bundle->append(Inst->getName());
  bundle->append("_input_transpose, ");
  bundle->append(filterSte.name);
  bundle->append(", ");


  bundle->append("(int32_t *)");
  auto biasSte = addConstantSymbolEntry(bias, ctx);
  bundle->append(biasSte.name);
  bundle->append(", ");

  bundle->append(Inst->getName());
  bundle->append("_output_bef_transpose, ");


  bundle->append(std::to_string(N));
  bundle->append(", ");
  bundle->append(std::to_string(H));
  bundle->append(", ");
  bundle->append(std::to_string(W));
  bundle->append(", ");
  bundle->append(std::to_string(C));
  bundle->append(", ");
  bundle->append(std::to_string(KN));
  bundle->append(", ");
  bundle->append(std::to_string(KH));
  bundle->append(", ");
  bundle->append(std::to_string(KW));
  bundle->append(", ");
  bundle->append(std::to_string(pad_size));
  bundle->append(", ");
  bundle->append(std::to_string(stride_size));
  bundle->append(", ");
  bundle->append(std::to_string(doRelu));
  bundle->append(", ");
  bundle->append(std::to_string(doBias));
  bundle->append(", ");
  bundle->append(std::to_string(shift));
  bundle->append(", ");
  bundle->append(std::to_string(dest->dims()[1]));
  bundle->append(", ");
  bundle->append(std::to_string(dest->dims()[2]));
  bundle->append(", vtaCmdH");
  uint32_t idxMultiEVTA = ctx->getIdxMultiEVTA();
  if(idxMultiEVTA){
    bundle->append(std::to_string(idxMultiEVTA));
  }
#ifdef NESTC_EVTA_PROFILE_AUTOTUNE
    bundle->append(", input1, input2, input3, ");
    bundle->append(std::to_string(ctx->getIdxMultiEVTA()));
#else
#include "VTASchedules.h"
  bool isTuned = 0;
  for(auto elem : convTune.ConvolutionTune_){
    if(elem.isMatch(N, H, W, C, KN, KH, KW, C, stride_size, pad_size))
    {
      bundle->append(", ");
      bundle->append(std::to_string(elem.nVirtualThread_));
      bundle->append(", ");
      bundle->append(std::to_string(elem.tileHSize_));
      bundle->append(", ");
      bundle->append(std::to_string(elem.tileWSize_));
      bundle->append(", ");
      bundle->append(std::to_string(ctx->getIdxMultiEVTA()));
      isTuned = 1;
      break;
    }
  }

  if(!isTuned){
    bundle->append(", 1, 8, 8, ");
    bundle->append(std::to_string(ctx->getIdxMultiEVTA()));
  }
#endif
  if (pf) {
    auto *P = llvm::cast<MaxPoolInst>(pf->pool);
    bundle->append(", ");
    bundle->append(pf->out->getName());
    bundle->append("_vbuf");
    for (int v : {(int)P->getKernels()[0], (int)P->getStrides()[0], (int)P->getPads()[0]}) {
      bundle->append(", ");
      bundle->append(std::to_string(v));
    }
  }
  bundle->append(");\n");
  if (pf) {
    bundle->append("  VTABufferInvalidate(");
    bundle->append(pf->out->getName());
    bundle->append("_vbuf, 0, ");
    bundle->append(std::to_string(pf->out->getSizeInBytes()));
    bundle->append(");\n");
  }
}

void generateBNNVTAConvolutionCall(const glow::BNNConvolutionInst *Inst,
                                  std::string *bundle,
                                  std::string *initConst,
                                  VTASaveContext *ctx,
                                  bool bnn_with_scale = false) {
  auto pad = Inst->getPads();
  auto strides = Inst->getStrides();
  //TODO : consider group
  //auto group = Inst->getGroup();
  auto src = Inst->getSrc();
  auto srcDims = src->dims();
  auto dest = Inst->getDest();
  auto filter = Inst->getFilter();
  auto filterDims = filter->dims();
  auto bias = Inst->getBias();
  int N = srcDims[0];
  int H = srcDims[1];
  int W = srcDims[2];
  int C = srcDims[3];

  C = C/8;
	if(C < 16) C = 16;

  int KN = filterDims[0];
  int KH = filterDims[1];
  int KW = filterDims[2];
  int pad_size = pad[0];
  int stride_size = strides[0];

  bool doBias = false;

  const Tensor* tensorBias = NULL;
  auto vMap = ctx->getVariableMap();
  for(auto it = vMap->begin(); it != vMap->end(); it++)
  {
    if(it->second == bias)
    {
      auto storage = it->first;
      if (storage->getKind() == glow::Kinded::Kind::ConstantKind) {
        const auto *constant = llvm::cast<Constant>(storage);
        tensorBias = &(constant->getPayload());
      }
      break;
    }
  }
  auto handleBias = tensorBias->getHandle<int32_t>();
  {
    for (size_t i = 0, e = handleBias.size(); i < e; i++) {
      auto data = handleBias.raw(i);
      if(data!=0){
        doBias = true;
        break;
      }
    }
  }

  float inScale = src->getType()->getScale();
  float filterScale = filter->getType()->getScale();
  float outScale = Inst->getDest()->getType()->getScale();
  filterScale = 1/filterScale;
  inScale = 1/inScale;
  outScale = 1/outScale;
  float matMulScale = inScale * filterScale;
  float scale =  matMulScale / outScale;
  int shift = getExpofPowerofTwo(scale);
  //shift -= 6;
  //if(bnn_with_scale) shift += 6;
  auto filterSte = addConstantSymbolEntry(filter, ctx);

  bundle->append("  xp_convolution_wo_tr(");
  bundle->append(Inst->getName());
  bundle->append("_input_transpose, ");
  bundle->append(filterSte.name);
  bundle->append(", ");

  bundle->append("(int32_t *)");
  auto biasSte = addConstantSymbolEntry(bias, ctx);
  bundle->append(biasSte.name);
  bundle->append(", ");

  auto scalefactor = Inst->getScalingfactor();

  if (bnn_with_scale) {
    auto scaleSte = addConstantSymbolEntry(scalefactor, ctx);
    bundle->append("(int32_t *)");
    bundle->append(scaleSte.name);
    bundle->append(", ");
  }
  else {
    auto biasSte = addConstantSymbolEntry(bias, ctx);
    bundle->append("(int32_t *)");
    bundle->append(biasSte.name);
    bundle->append(", ");
  }

  bundle->append(Inst->getName());
  bundle->append("_output_bef_transpose, ");


  bundle->append(std::to_string(N));
  bundle->append(", ");
  bundle->append(std::to_string(H));
  bundle->append(", ");
  bundle->append(std::to_string(W));
  bundle->append(", ");
  bundle->append(std::to_string(C));
  bundle->append(", ");
  bundle->append(std::to_string(KN));
  bundle->append(", ");
  bundle->append(std::to_string(KH));
  bundle->append(", ");
  bundle->append(std::to_string(KW));
  bundle->append(", ");
  bundle->append(std::to_string(pad_size));
  bundle->append(", ");
  bundle->append(std::to_string(stride_size));
  bundle->append(", ");
  bundle->append(std::to_string(0));
  bundle->append(", ");
  bundle->append(std::to_string(doBias));
  bundle->append(", ");
  bundle->append(std::to_string(shift));
  bundle->append(", ");
  bundle->append(std::to_string(dest->dims()[1]));
  bundle->append(", ");
  bundle->append(std::to_string(dest->dims()[2]));
  bundle->append(", vtaCmdH");
  uint32_t idxMultiEVTA = ctx->getIdxMultiEVTA();
  if(idxMultiEVTA){
    bundle->append(std::to_string(idxMultiEVTA));
  }
#include "VTASchedules.h"
  bool isTuned = 0;
  for(auto elem : convTune.ConvolutionTune_){
    if(elem.isMatch(N, H, W, srcDims[3], KN, KH, KW, srcDims[3], stride_size, pad_size))
    {
      bundle->append(", ");
      bundle->append(std::to_string(elem.nVirtualThread_));
      bundle->append(", ");
      bundle->append(std::to_string(elem.tileHSize_));
      bundle->append(", ");
      bundle->append(std::to_string(elem.tileWSize_));
      bundle->append(", ");
      isTuned = 1;
      break;
    }
  }
  if(!isTuned){
    if(bnn_with_scale) {
    bundle->append(", 1, 14, 14, ");
    }
    else {
      if(dest->dims()[1] > 8) {
        bundle->append(", 1, 8, 8, ");
      }
      else if(dest->dims()[1] <= 8) {
        bundle->append(", 1, 4, 4, ");
      }
    }
  }
  bundle->append(std::to_string(ctx->getIdxMultiEVTA()));
  bundle->append(", ");
  bundle->append(std::to_string(int(bnn_with_scale)));
  bundle->append(");\n");
}

void generateOutputTranspose(const glow::ConvolutionInst *Inst,
                             std::string *bundle, std::string *initConst,
                             VTASaveContext *ctx) {
  // TODO : consider group
  // auto group = Inst->getGroup();
  auto src = Inst->getSrc();
  auto dest = Inst->getDest();
  auto destDims = dest->dims();
  auto filter = Inst->getFilter();

  int outN = destDims[0];
  int outH = destDims[1];
  int outW = destDims[2];
  int outC = destDims[3];

  auto destWeight = static_cast<WeightVar *>(dest);

#ifdef VTA_PROFILE
  bundle->append("  clock_t ");
  bundle->append(Inst->getName());
  bundle->append("_output_transpose_start = clock();\n");
#endif
  //#### JO added: invalidate stale CPU cache lines before the CPU reads the EVTA output
  //#### (non-coherent EVTA, VTA_COHERENT_ACCESSES=false; no-op otherwise)
  bundle->append("  VTABufferInvalidate(");
  bundle->append(Inst->getName());
  bundle->append("_output_bef_transpose, 0, ");
  bundle->append(std::to_string(outN * outH * outW * outC));
  bundle->append(");\n");
  //#### JO added (2026-10-31): an output kept in the EVTA layout is not transposed
  if (ctx->isPacked(dest))
    return;
  bundle->append("  transpose_vtaio2nhwc(");
  bundle->append("(int8_t* )VTABufferGetVirtAddr(");
  bundle->append(Inst->getName());
  bundle->append("_output_bef_transpose), ");

  if (destWeight->getMutability() == glow::WeightVar::MutabilityKind::Mutable) {
    auto ste = addSymbolEntry(destWeight, ctx);
    bundle->append(ste.name);
    bundle->append(", ");
  } else {
    bundle->append(dest->getName());
    bundle->append(", ");
  }
  bundle->append(std::to_string(outN));
  bundle->append(", ");
  bundle->append(std::to_string(outH));
  bundle->append(", ");
  bundle->append(std::to_string(outW));
  bundle->append(", ");
  bundle->append(std::to_string(outC));
  bundle->append(" );\n");

#ifdef VTA_PROFILE
  bundle->append("  clock_t ");
  bundle->append(Inst->getName());
  bundle->append("_output_transpose_end = clock();\n");
  bundle->append("  prof_out<<\"");
  bundle->append(Inst->getKindName());
  bundle->append("_output_transpose:");
  bundle->append(Inst->getName());
  bundle->append(" : \"<<");
  bundle->append("(double)(");
  bundle->append(Inst->getName());
  bundle->append("_output_transpose_end - ");
  bundle->append(Inst->getName());
  bundle->append(
      "_output_transpose_start)/CLOCKS_PER_SEC*1000 << std::endl;\n");
#endif
}

void generateOutputTranspose(const glow::BNNConvolutionInst *Inst,
                            std::string *bundle,
                            std::string *initConst,
                            VTASaveContext *ctx){
  //TODO : consider group
  //auto group = Inst->getGroup();
  auto src = Inst->getSrc();
  auto dest = Inst->getDest();
  auto destDims = dest->dims();
  auto filter = Inst->getFilter();

  int outN = destDims[0];
  int outH = destDims[1];
  int outW = destDims[2];
  int outC = destDims[3];

  auto destWeight = static_cast<WeightVar *>(dest);

#ifdef VTA_PROFILE
  bundle->append("  clock_t ");
    bundle->append(Inst->getName());
    bundle->append("_output_transpose_start = clock();\n");
#endif
  //#### JO added: invalidate stale CPU cache lines before the CPU reads the EVTA output
  //#### (non-coherent EVTA, VTA_COHERENT_ACCESSES=false; no-op otherwise)
  bundle->append("  VTABufferInvalidate(");
  bundle->append(Inst->getName());
  bundle->append("_output_bef_transpose, 0, ");
  bundle->append(std::to_string(outN * outH * outW * outC));
  bundle->append(");\n");
  bundle->append("  transpose_vtaio2nhwc(");
  bundle->append("(int8_t* )VTABufferGetVirtAddr(");
  bundle->append(Inst->getName());
  bundle->append("_output_bef_transpose), ");

  if (destWeight->getMutability() ==
      glow::WeightVar::MutabilityKind::Mutable) {
    auto ste = addSymbolEntry(destWeight, ctx);
    bundle->append(ste.name);
    bundle->append(", ");
  } else {
    bundle->append(dest->getName());
    bundle->append(", ");
  }
  bundle->append(std::to_string(outN));
  bundle->append(", ");
  bundle->append(std::to_string(outH));
  bundle->append(", ");
  bundle->append(std::to_string(outW));
  bundle->append(", ");
  bundle->append(std::to_string(outC));
  bundle->append(" );\n");

#ifdef VTA_PROFILE
  bundle->append("  clock_t ");
    bundle->append(Inst->getName());
    bundle->append("_output_transpose_end = clock();\n");
    bundle->append("  prof_out<<\"");
    bundle->append(Inst->getKindName());
    bundle->append("_output_transpose:");
    bundle->append(Inst->getName());
    bundle->append(" : \"<<");
    bundle->append("(double)(");
    bundle->append(Inst->getName());
    bundle->append("_output_transpose_end - ");
    bundle->append(Inst->getName());
    bundle->append("_output_transpose_start)/CLOCKS_PER_SEC*1000 << std::endl;\n");
#endif
}

void generateEVTAOutputTranspose(const glow::VTAConvolutionInst *Inst,
                                 std::string *bundle, std::string *initConst,
                                 VTASaveContext *ctx) {
  // TODO : consider group
  // auto group = Inst->getGroup();
  auto src = Inst->getSrc();
  auto dest = Inst->getDest();
  auto destDims = dest->dims();
  auto filter = Inst->getFilter();

  int Nm = destDims[0];
  int Cm = destDims[1];
  int H = destDims[2];
  int W = destDims[3];
  int Ns = destDims[4];
  int Cs = destDims[5];

  auto destWeight = static_cast<WeightVar *>(dest);

#ifdef VTA_PROFILE
  bundle->append("  clock_t ");
  bundle->append(Inst->getName());
  bundle->append("_output_transpose_start = clock();\n");
#endif
  bundle->append("  VTABufferCopy(");
  bundle->append(Inst->getName());
  bundle->append("_output_bef_transpose, ");
  bundle->append(std::to_string(0));
  bundle->append(", ");

  if (destWeight->getMutability() == glow::WeightVar::MutabilityKind::Mutable) {
    auto ste = addSymbolEntry(destWeight, ctx);
    bundle->append(ste.name);
    bundle->append(", ");
  } else {
    bundle->append(dest->getName());
    bundle->append(", ");
  }
  bundle->append(std::to_string(0));
  bundle->append(", ");
  bundle->append(std::to_string(Nm * Cm * H * W * Ns * Cs));
  bundle->append(", ");
  bundle->append(std::to_string(2));
  bundle->append(");\n");

#ifdef VTA_PROFILE
  bundle->append("  clock_t ");
  bundle->append(Inst->getName());
  bundle->append("_output_transpose_end = clock();\n");
  bundle->append("  prof_out<<\"");
  bundle->append(Inst->getKindName());
  bundle->append("_output_transpose:");
  bundle->append(Inst->getName());
  bundle->append(" : \"<<");
  bundle->append("(double)(");
  bundle->append(Inst->getName());
  bundle->append("_output_transpose_end - ");
  bundle->append(Inst->getName());
  bundle->append(
      "_output_transpose_start)/CLOCKS_PER_SEC*1000 << std::endl;\n");
#endif
}

void generateVTABufferFree(const glow::ConvolutionInst *Inst,
                           std::string *bundle, VTASaveContext *ctx) {
  //#### JO changed (2026-10-31): activations kept in the EVTA layout are freed by their deallocactivation
  if (!ctx->isPacked(Inst->getSrc())) {
    bundle->append("  VTABufferFree(");
    bundle->append(Inst->getName());
    bundle->append("_input_transpose);\n");
  }

  if (!ctx->isPacked(Inst->getDest())) {
    bundle->append("  VTABufferFree(");
    bundle->append(Inst->getName());
    bundle->append("_output_bef_transpose);\n");
  }
}

void generateEVTABufferFree(const glow::VTAConvolutionInst *Inst,
                            std::string *bundle) {
  bundle->append("  VTABufferFree(");
  bundle->append(Inst->getName());
  bundle->append("_input_transpose);\n");

  bundle->append("  VTABufferFree(");
  bundle->append(Inst->getName());
  bundle->append("_output_bef_transpose);\n");
}

void generateVTABufferFree(const glow::BNNConvolutionInst *Inst, std::string *bundle){
  bundle->append("  VTABufferFree(");
  bundle->append(Inst->getName());
  bundle->append("_input_transpose);\n");

  bundle->append("  VTABufferFree(");
  bundle->append(Inst->getName());
  bundle->append("_output_bef_transpose);\n");
}

void saveVTAConvolutionInst(const glow::ConvolutionInst *Inst,
                            std::string *bundle, std::string *initConst,
                            VTASaveContext *ctx) {
  prepareVTAConvolutionInst(Inst, bundle, initConst, ctx);
  generateInputTranspose(Inst, bundle, initConst, ctx);
  declareOutputTranspose(Inst, bundle, initConst, ctx);

#ifdef VTA_PROFILE
  bundle->append("  clock_t ");
  bundle->append(Inst->getName());
  bundle->append("_core_start = clock();\n");
#endif

  generateVTAConvolutionCall(Inst, bundle, initConst, ctx);

#ifdef VTA_PROFILE
  bundle->append("  clock_t ");
  bundle->append(Inst->getName());
  bundle->append("_core_end = clock();\n");
  bundle->append("  prof_out<<\"");
  bundle->append(Inst->getKindName());
  bundle->append("_core:");
  bundle->append(Inst->getName());
  bundle->append(" : \"<<");
  bundle->append("(double)(");
  bundle->append(Inst->getName());
  bundle->append("_core_end - ");
  bundle->append(Inst->getName());
  bundle->append("_core_start)/CLOCKS_PER_SEC*1000 << std::endl;\n");
#endif

  generateOutputTranspose(Inst, bundle, initConst, ctx);
  generateVTABufferFree(Inst, bundle, ctx);

  return;
}
void saveEVTAConvolutionInst(const glow::VTAConvolutionInst *Inst,
                             std::string *bundle, std::string *initConst,
                             VTASaveContext *ctx) {

  prepareEVTAConvolutionInst(Inst, bundle, initConst, ctx);
  generateEVTAInputTranspose(Inst, bundle, initConst, ctx);
  declareEVTAOutputTranspose(Inst, bundle, initConst);

#ifdef VTA_PROFILE
  bundle->append("  clock_t ");
  bundle->append(Inst->getName());
  bundle->append("_core_start = clock();\n");
#endif

  generateEVTAConvolutionCall(Inst, bundle, initConst, ctx);

#ifdef VTA_PROFILE
  bundle->append("  clock_t ");
  bundle->append(Inst->getName());
  bundle->append("_core_end = clock();\n");
  bundle->append("  prof_out<<\"");
  bundle->append(Inst->getKindName());
  bundle->append("_core:");
  bundle->append(Inst->getName());
  bundle->append(" : \"<<");
  bundle->append("(double)(");
  bundle->append(Inst->getName());
  bundle->append("_core_end - ");
  bundle->append(Inst->getName());
  bundle->append("_core_start)/CLOCKS_PER_SEC*1000 << std::endl;\n");
#endif

  generateEVTAOutputTranspose(Inst, bundle, initConst, ctx);
  generateEVTABufferFree(Inst, bundle);

  return;
}

void saveBNNVTAConvolutionInst(const glow::BNNConvolutionInst *Inst,
                              std::string *bundle,
                              std::string *initConst,
                              VTASaveContext *ctx,
                              bool bnn_with_scale){
  prepareBNNVTAConvolutionInst(Inst, bundle, initConst, ctx, bnn_with_scale);
  generateBNNInputTranspose(Inst, bundle, initConst, ctx);
  declareOutputTranspose(Inst, bundle, initConst);

#ifdef VTA_PROFILE
  bundle->append("  clock_t ");
  bundle->append(Inst->getName());
  bundle->append("vta_core_start = clock();\n");
#endif

  generateBNNVTAConvolutionCall(Inst, bundle, initConst, ctx, bnn_with_scale);

#ifdef VTA_PROFILE
  bundle->append("  clock_t ");
  bundle->append(Inst->getName());
  bundle->append("_core_end = clock();\n");
  bundle->append("  prof_out<<\"");
  bundle->append(Inst->getKindName());
  bundle->append("_core:");
  bundle->append(Inst->getName());
  bundle->append(" : \"<<");
  bundle->append("(double)(");
  bundle->append(Inst->getName());
  bundle->append("_core_end - ");
  bundle->append(Inst->getName());
  bundle->append("_core_start)/CLOCKS_PER_SEC*1000 << std::endl;\n");
#endif

  generateOutputTranspose(Inst, bundle, initConst, ctx);
  generateVTABufferFree(Inst, bundle);

  return;
}

void savezeroFillBNNVTAConvolutionInst(const glow::BNNConvolutionInst *Inst,
                                      std::string *bundle,
                                      std::string *initConst,
                                      VTASaveContext *ctx,
                                      bool bnn_with_scale){
  preparezeroFillBNNVTAConvolutionInst(Inst, bundle, initConst, ctx, bnn_with_scale);
  generatezeroFillBNNInputTranspose(Inst, bundle, initConst, ctx);
  declareOutputTranspose(Inst, bundle, initConst);

#ifdef VTA_PROFILE
  bundle->append("  clock_t ");
  bundle->append(Inst->getName());
  bundle->append("vta_core_start = clock();\n");
#endif

  generateBNNVTAConvolutionCall(Inst, bundle, initConst, ctx, bnn_with_scale);

#ifdef VTA_PROFILE
  bundle->append("  clock_t ");
  bundle->append(Inst->getName());
  bundle->append("_core_end = clock();\n");
  bundle->append("  prof_out<<\"");
  bundle->append(Inst->getKindName());
  bundle->append("_core:");
  bundle->append(Inst->getName());
  bundle->append(" : \"<<");
  bundle->append("(double)(");
  bundle->append(Inst->getName());
  bundle->append("_core_end - ");
  bundle->append(Inst->getName());
  bundle->append("_core_start)/CLOCKS_PER_SEC*1000 << std::endl;\n");
#endif

  generateOutputTranspose(Inst, bundle, initConst, ctx);
  generateVTABufferFree(Inst, bundle);

  return;
}

void saveFirstBNNVTAConvolutionInst(const glow::ConvolutionInst *Inst,
                                    std::string *bundle,
                                    std::string *initConst,
                                    VTASaveContext *ctx){
  prepareFirstBNNVTAConvolutionInst(Inst, bundle, initConst, ctx);
  generateFirstBNNInputTranspose(Inst, bundle, initConst, ctx);
  declareOutputTranspose(Inst, bundle, initConst, ctx);

  generateFirstBNNVTAConvolutionCall(Inst, bundle, initConst, ctx);

  generateOutputTranspose(Inst, bundle, initConst, ctx);
  generateVTABufferFree(Inst, bundle, ctx);

  return;
}

//#### JO changed (2026-10-31): the choice of saveConvolutionInst as a function, also used by findPackedActivations
enum class ConvPath { Float, VTA, DepthwiseCPU, FirstBNN, Other };

ConvPath getConvPath(const glow::ConvolutionInst *Inst) {
  auto src = Inst->getSrc();
  auto srcDims = src->dims();
  auto filter = Inst->getFilter();
  auto filterDims = filter->dims();

  auto bias = Inst->getBias();
  assert(bias->dims()[0] == filterDims[0]);

  if (!Inst->getSrc()->getType()->isQuantizedType())
    return ConvPath::Float;

  float inScale = src->getType()->getScale();
  float filterScale = filter->getType()->getScale();
  float outScale = Inst->getDest()->getType()->getScale();
  float biasScale = bias->getType()->getScale();

  int32_t outOffset = Inst->getDest()->getType()->getOffset();
  int32_t inOffset = src->getType()->getOffset();
  int32_t filterOffset = filter->getType()->getOffset();
  int32_t biasOffset = bias->getType()->getOffset();

  filterScale = 1 / filterScale;
  inScale = 1 / inScale;
  biasScale = 1 / biasScale;
  outScale = 1 / outScale;
  float matMulScale = inScale * filterScale;
  float scale = matMulScale / outScale;

  int shift = getExpofPowerofTwo(scale);
  //#### JO added (2026-11-06): the EVTA convs take one pad for all sides; TF SAME padding of a stride-2 conv
  //#### (MobileNetV2: top / left 0, bottom / right 1) goes to the CPU conv, which takes the top pad and
  //#### skips the taps past the input (out_h / out_w give the rest)
  const auto P = Inst->getPads();
  const bool symPad = P.size() == 4 && P[0] == P[1] && P[1] == P[2] && P[2] == P[3];

  if (symPad && Inst->getGroup() == 1 && scale > 1 && srcDims[3] % 16 == 0 &&
      filterDims[0] % 16 == 0 && sameScale(biasScale, matMulScale) && outOffset == 0 &&
      inOffset == 0 && filterOffset == 0 && biasOffset == 0 && shift >= 0) {
    return ConvPath::VTA;
  } else if (Inst->getGroup() == srcDims[3] && scale > 1 &&
             sameScale(biasScale, matMulScale) && outOffset == 0 && inOffset == 0 &&
             filterOffset == 0 && biasOffset == 0 && shift >= 0) {
    return ConvPath::DepthwiseCPU;
  //#### JO added (2026-10-29): a quantized 3-channel conv (the image conv0 when it is not skipped from
  //#### quantization) runs on EVTA with its input and filter zero-filled to 16 channels, the path of the
  //#### first BNN conv; it is plain int8 (convolution_wo_tr), so it does not need VTA_BNN
  //#### JO changed (2026-11-07): + asymmetric pads when the packed conv0 takes them (getConv0Pack)
  } else if ((symPad || getConv0Pack(Inst, nullptr)) && Inst->getGroup() == 1 && scale > 1 && srcDims[3] == 3 &&
             filterDims[3] == 3 && filterDims[0] % 16 == 0 &&
             sameScale(biasScale, matMulScale) && outOffset == 0 && inOffset == 0 &&
             filterOffset == 0 && biasOffset == 0 && shift >= 0) {
    return ConvPath::FirstBNN;
  }
  return ConvPath::Other;
}

static bool saveEvtaConvAsFc(const glow::ConvolutionInst *Inst, std::string *bundle, VTASaveContext *ctx);

void saveConvolutionInst(const glow::ConvolutionInst *Inst, std::string *bundle,
                         std::string *initConst, VTASaveContext *ctx) {
  //#### JO added (2026-11-08): a 1x1 conv over a 1x1 input as the EVTA fc (saveEvtaConvAsFc)
  if (saveEvtaConvAsFc(Inst, bundle, ctx))
    return;
  //#### JO added (2026-11-06): a fused Clip(0, max): the relu's upper clip for this call (convClipHi)
  const int clipHi = convClipHi(Inst);
  if (clipHi < 127)
    bundle->append("  VTASetActivationClip(" + std::to_string(clipHi) + ");\n");
  switch (getConvPath(Inst)) {
  case ConvPath::Float:
    saveFloatConvolutionInst(Inst, bundle, ctx);
    break;
  case ConvPath::VTA:
    saveVTAConvolutionInst(Inst, bundle, initConst, ctx);
    break;
  case ConvPath::DepthwiseCPU:
    // saveVTADWConvolutionInst(Inst, bundle, initConst, ctx);
    saveNonVTAConvolutionInst(Inst, bundle, initConst, ctx);
    break;
  case ConvPath::FirstBNN:
    saveFirstBNNVTAConvolutionInst(Inst, bundle, initConst, ctx);
    break;
  case ConvPath::Other:
#if defined(VTA_BNN)
    saveFirstBNNVTAConvolutionInst(Inst, bundle, initConst, ctx);
#else
    saveNonVTAConvolutionInst(Inst, bundle, initConst, ctx);
#endif
    break;
  }
  if (clipHi < 127)
    bundle->append("  VTASetActivationClip(127);\n");
}

//#### JO added (2026-10-31): activations kept in the EVTA layout N{C/16}HW16 between the layers
//#### (-vta-keep-layout, default on). The EVTA conv reads its input and writes its output in that layout, so
//#### conv -> conv needs no transpose whatever the channel counts are (C only sets the number of 16-channel
//#### blocks); elementadd and relu are elementwise on equal shapes, so they run unchanged; maxpool and avgpool
//#### run with the 16-channel blocks as the batch (N*C/16, H, W, 16), and a 1x1 output of a pool is the same
//#### in both layouts. An activation (4-D, N = 1, int8, C % 16 = 0, H*W > 1) is kept packed only if every
//#### instruction that uses it is one of those; the 4-D operands of one elementwise op or pool are packed
//#### together or not at all. Everything else (graph inputs/outputs, tensor views, other ops) stays NHWC.
//#### A packed activation is a VTA buffer (<name>_vbuf, <name> = its virtual address).
static bool isPackCandidate(const Value *v) {
  if (!llvm::isa<AllocActivationInst>(v))
    return false;
  auto ty = v->getType();
  auto d = ty->dims();
  return ty->getElementType() == ElemKind::Int8QTy && d.size() == 4 &&
         d[0] == 1 && d[3] % 16 == 0 && d[1] * d[2] > 1;
}

static bool isLayoutNeutral(const Value *v) {
  auto d = v->dims();
  return d.size() == 4 && d[1] == 1 && d[2] == 1;
}

//#### JO added (2026-11-08): a depthwise channelwise conv whose input and output can be in the EVTA layout
static bool isDepthwiseCW(const glow::ChannelwiseQuantizedConvolutionInst *I) {
  auto sd = I->getSrc()->dims(), fd = I->getFilter()->dims();
  return sd.size() == 4 && fd.size() == 4 && (dim_t)I->getGroup() == sd[3] && fd[0] == sd[3] && fd[3] == 1 &&
         sd[3] % 16 == 0 && I->getDest()->dims()[3] == sd[3];
}

void findPackedActivations(const IRFunction *IR, VTASaveContext *ctx) {
  auto &packed = *ctx->getPacked();
  packed.clear();
#ifdef VTA_MEMOPT_DISABLE
  return;
#endif
  if (!vtaKeepLayout)
    return;
  for (const auto &I : IR->getInstrs())
    for (const auto &op : I.getOperands())
      if (isPackCandidate(op.first))
        packed.insert(op.first);

  bool changed = true;
  auto unpack = [&](const Value *v) {
    if (packed.erase(v))
      changed = true;
  };
  while (changed) {
    changed = false;
    for (const auto &I : IR->getInstrs()) {
      bool group = false;
      switch (I.getKind()) {
      case Kinded::Kind::AllocActivationInstKind:
      case Kinded::Kind::DeallocActivationInstKind:
        continue;
      case Kinded::Kind::ConvolutionInstKind: {
        auto *C = llvm::cast<ConvolutionInst>(&I);
        auto path = getConvPath(C);
        if (path == ConvPath::VTA)
          continue;
        // the zero-filled first conv reads NHWC but writes the EVTA layout
        if (path == ConvPath::FirstBNN) {
          for (const auto &op : I.getOperands())
            if (op.first != C->getDest())
              unpack(op.first);
          continue;
        }
        break;
      }
      //#### JO added (2026-11-08): a depthwise channelwise conv (vtalib cw_convolution) reads and writes either layout,
      //#### input and output each on its own (C % 16 = 0, isDepthwiseCW)
      case Kinded::Kind::ChannelwiseQuantizedConvolutionInstKind:
        if (isDepthwiseCW(llvm::cast<ChannelwiseQuantizedConvolutionInst>(&I)))
          continue;
        break;
      case Kinded::Kind::ElementAddInstKind:
      case Kinded::Kind::ReluInstKind:
      case Kinded::Kind::MaxPoolInstKind:
      case Kinded::Kind::AvgPoolInstKind:
        group = true;
        break;
      default:
        break;
      }
      if (!group) {
        for (const auto &op : I.getOperands())
          unpack(op.first);
        continue;
      }
      bool all = true;
      for (const auto &op : I.getOperands())
        if (!isLayoutNeutral(op.first) && !packed.count(op.first))
          all = false;
      if (!all)
        for (const auto &op : I.getOperands())
          unpack(op.first);
    }
  }
}

//#### JO added (2026-10-31): conv + elementadd + relu in one EVTA call (-vta-fuse-residual, convolution_wo_tr_res):
//####   EVTA conv C -> D;  elementadd E(D, R) -> O;  relu(O) -> O'
//#### with only deallocactivations between them becomes one call at the place of C that writes O' (C, E and the relu
//#### then emit nothing else). Needs D, R, O, O' in the EVTA layout (findPackedActivations), power-of-two ratios of
//#### the scales (sD / sO = 2^-a, sR / sO = 2^-b, offsets 0), the relu with sO / sO' = 2^q, q = 0..7 (2026-11-05; was equal
//#### scales only; q > 0: the relu'd sum << q, clipped to 127), O' not the conv input,
//#### O' allocated before C, and no other user of D (unless D is O or O') or of O (unless O is O').
//#### (An allocation of O' between C and the relu is moved before C.)
//#### With m = max(a, b, 0): O' = relu(round((D << (m - a)) + (R << (m - b))) >> m), exactly the CPU elemadd + relu
//#### (the EVTA rounds .5 up, the CPU away from zero: they differ only for negative sums, which the relu makes 0).
static bool powerOfTwoExp(float r, int *e) {
  int ex;
  float m = std::frexp(r, &ex);
  if (m != 0.5f)
    return false;
  *e = ex - 1; // r = 2^(ex - 1)
  return true;
}

//#### JO added (2026-10-31): <name>_vbuf (VTA buffer) and <name> (its virtual address) of an activation in the EVTA layout
static void emitPackedAlloc(const Value *v, std::string *bundle) {
  bundle->append("  int8_t *");
  bundle->append(v->getName());
  bundle->append("_vbuf = (int8_t *)VTABufferAlloc(");
  bundle->append(std::to_string(v->getSizeInBytes()));
  bundle->append(");\n");
  bundle->append("  int8_t *");
  bundle->append(v->getName());
  bundle->append(" = (int8_t *)VTABufferGetVirtAddr(");
  bundle->append(v->getName());
  bundle->append("_vbuf);\n");
}

void findResidualFusions(const IRFunction *IR, VTASaveContext *ctx) {
  ctx->getFusions()->clear();
  ctx->getFusedAway()->clear();
  ctx->getHoisted()->clear();
  if (!vtaFuseResidual || ctx->getPacked()->empty())
    return;
  std::vector<const Instruction *> ins;
  std::map<const Value *, size_t> allocAt;
  std::map<const Value *, int> users; // instructions other than alloc/dealloc that use a value
  for (const auto &I : IR->getInstrs()) {
    if (auto *A = llvm::dyn_cast<AllocActivationInst>(&I))
      allocAt[A] = ins.size();
    if (!llvm::isa<AllocActivationInst>(&I) && !llvm::isa<DeallocActivationInst>(&I)) {
      std::set<const Value *> ops; // once per instruction (relu in place lists its value twice)
      for (const auto &op : I.getOperands())
        ops.insert(op.first);
      for (auto *v : ops)
        users[v]++;
    }
    ins.push_back(&I);
  }
  auto next = [&](size_t i) {
    for (i++; i < ins.size(); i++)
      if (!llvm::isa<DeallocActivationInst>(ins[i]) && !llvm::isa<AllocActivationInst>(ins[i]))
        return i;
    return ins.size();
  };
  for (size_t i = 0; i < ins.size(); i++) {
    auto *C = llvm::dyn_cast<ConvolutionInst>(ins[i]);
    if (!C || getConvPath(C) != ConvPath::VTA || !ctx->isPacked(C->getDest()))
      continue;
    size_t j = next(i);
    if (j >= ins.size())
      continue;
    auto *E = llvm::dyn_cast<ElementAddInst>(ins[j]);
    if (!E)
      continue;
    const Value *D = C->getDest();
    const Value *R = E->getLHS() == D ? E->getRHS() : E->getRHS() == D ? E->getLHS() : nullptr;
    const Value *O = E->getDest();
    if (!R || R == D)
      continue;
    size_t k = next(j);
    if (k >= ins.size())
      continue;
    auto *Rl = llvm::dyn_cast<ReluInst>(ins[k]);
    if (!Rl || Rl->getSrc() != O)
      continue;
    const Value *O2 = Rl->getDest();
    if (!ctx->isPacked(R) || !ctx->isPacked(O) || !ctx->isPacked(O2) || O2 == C->getSrc() ||
        O->dims() != D->dims() || O2->dims() != D->dims() || R->dims() != D->dims())
      continue;
    if (O2 != D && O2 != R && !allocAt.count(O2))
      continue;
    bool hoist = O2 != D && O2 != R && allocAt[O2] > i;
    if (hoist && allocAt[O2] > k)
      continue;
    if (D != O && D != O2 && users[D] != 2) // C, E
      continue;
    if (O != O2 && users[O] != 2) // E, relu
      continue;
    auto tD = D->getType(), tR = R->getType(), tO = O->getType(), tO2 = O2->getType();
    //#### JO changed (2026-11-05): the relu may also go to a scale 2^q smaller (Resnet50 res2_0: 1/8 -> 1/16); the call
    //#### then shifts the relu'd sum left by q and clips it to 127, as the CPU relu (which clips since 2026-11-05)
    // if (tD->getOffset() || tR->getOffset() || tO->getOffset() || tO2->getOffset() ||
    //     tO->getScale() != tO2->getScale())
    //   continue;
    int q = 0; // sO / sO2 = 2^q
    if (tD->getOffset() || tR->getOffset() || tO->getOffset() || tO2->getOffset() ||
        !powerOfTwoExp(tO->getScale() / tO2->getScale(), &q) || q < 0 || q > 7)
      continue;
    int eD, eR; // sD / sO = 2^eD, sR / sO = 2^eR
    if (!powerOfTwoExp(tD->getScale() / tO->getScale(), &eD) ||
        !powerOfTwoExp(tR->getScale() / tO->getScale(), &eR))
      continue;
    int m = std::max(0, std::max(-eD, -eR));
    ResidualFusion f = {R, O2, m + eD, m + eR, m, q, hoist};
    if (f.convShl > 8 || f.resShl > 6 || f.sumShr > 15)
      continue;
    (*ctx->getFusions())[C] = f;
    ctx->getFusedAway()->insert(E);
    ctx->getFusedAway()->insert(Rl);
  }
}

//#### JO added (2026-11-02): the EVTA max pool (findEvtaMaxPools) 3x3 / stride 2 / pad 1 that is the next instruction after a
//#### packed first conv (getConv0Pack) with relu and the only other user of its output, when the conv's schedule tiles are
//#### even: done by the conv's call (convolution_wo_tr_pool); the pool's dest is allocated before the conv if needed.
void findPoolFusions(const IRFunction *IR, VTASaveContext *ctx) {
  ctx->getPoolFusions()->clear();
  if (!vtaFusePool)
    return;
  std::vector<const Instruction *> ins;
  std::map<const Value *, size_t> allocAt;
  std::map<const Value *, int> users; // instructions other than alloc/dealloc that use a value
  for (const auto &I : IR->getInstrs()) {
    if (auto *A = llvm::dyn_cast<AllocActivationInst>(&I))
      allocAt[A] = ins.size();
    if (!llvm::isa<AllocActivationInst>(&I) && !llvm::isa<DeallocActivationInst>(&I)) {
      std::set<const Value *> ops;
      for (const auto &op : I.getOperands())
        ops.insert(op.first);
      for (auto *v : ops)
        users[v]++;
    }
    ins.push_back(&I);
  }
  for (size_t i = 0; i < ins.size(); i++) {
    auto *C = llvm::dyn_cast<ConvolutionInst>(ins[i]);
    Conv0Pack pk;
    if (!C || getConvPath(C) != ConvPath::FirstBNN || C->getFusedActivation() != RELU || !getConv0Pack(C, &pk))
      continue;
    size_t j = i + 1;
    while (j < ins.size() && (llvm::isa<AllocActivationInst>(ins[j]) || llvm::isa<DeallocActivationInst>(ins[j])))
      j++;
    if (j >= ins.size())
      continue;
    auto *P = llvm::dyn_cast<MaxPoolInst>(ins[j]);
    const Value *D = C->getDest();
    if (!P || P->getSrc() != D || !ctx->isEvtaMaxPool(P) || users[D] != 2)
      continue;
    auto k = P->getKernels(), st = P->getStrides(), pads = P->getPads();
    if (k[0] != 3 || k[1] != 3 || st[0] != 2 || st[1] != 2)
      continue;
    bool pad1 = true;
    for (auto p : pads)
      pad1 = pad1 && p == 1;
    int vt, th, tw;
    getConvSchedule(C->getSrc()->dims()[0], pk.H2, pk.W2, pk.CO, C->getFilter()->dims()[0], pk.KH2, 1, 1, 0, &vt, &th,
                    &tw);
    const Value *O = P->getDest();
    if (!pad1 || th % 2 || tw % 2 || !allocAt.count(O))
      continue;
    (*ctx->getPoolFusions())[C] = {P, O, allocAt[O] > i};
    ctx->getFusedAway()->insert(P);
  }
}

//#### JO added (2026-11-02): the input preprocessing before a zero-filled first conv (ConvPath::FirstBNN) done by its input
//#### copy (preprocess_nhwc2vtaio_zerofill, -vta-fuse-preprocess): a quantize of a float value, then any of elemsub /
//#### elemdiv by an int8 constant that is the same for every element of a channel and an NCHW -> NHWC transpose (at most
//#### one), each value used only by the next of them. The elemsub / elemdiv chain depends only on the quantized value and
//#### the channel, so it becomes a table [C][256], computed as the CPU elemsub / elemdiv compute it with the scales as
//#### the bundle writes them (1.0/<1/scale with 6 decimals>, offsets 0); bit-exact with the instructions it replaces.
static float bundleScale(float scale) { return (float)(1.0 / std::stod(std::to_string(1 / scale))); }

static const Tensor *constantPayload(const Value *v, VTASaveContext *ctx) {
  auto vMap = ctx->getVariableMap();
  for (auto it = vMap->begin(); it != vMap->end(); it++)
    if (it->second == v)
      return it->first->getKind() == glow::Kinded::Kind::ConstantKind ? &llvm::cast<Constant>(it->first)->getPayload()
                                                                       : nullptr;
  return nullptr;
}

void findInputPreprocess(const IRFunction *IR, VTASaveContext *ctx) {
  ctx->getPreprocs()->clear();
  ctx->getPreFused()->clear();
  if (!vtaFusePreprocess)
    return;
  std::vector<const Instruction *> ins;
  std::map<const Value *, int> users; // instructions other than alloc/dealloc that use a value
  for (const auto &I : IR->getInstrs()) {
    if (!llvm::isa<AllocActivationInst>(&I) && !llvm::isa<DeallocActivationInst>(&I)) {
      std::set<const Value *> ops;
      for (const auto &op : I.getOperands())
        ops.insert(op.first);
      for (auto *v : ops)
        users[v]++;
    }
    ins.push_back(&I);
  }
  auto next = [&](size_t i) {
    for (i++; i < ins.size(); i++)
      if (!llvm::isa<DeallocActivationInst>(ins[i]) && !llvm::isa<AllocActivationInst>(ins[i]))
        return i;
    return ins.size();
  };
  auto plainInt8 = [](const Value *v) {
    return v->getElementType() == ElemKind::Int8QTy && v->getType()->getOffset() == 0;
  };
  for (size_t i = 0; i < ins.size(); i++) {
    auto *Q = llvm::dyn_cast<QuantizeInst>(ins[i]);
    if (!Q || Q->getSrc()->getElementType() != ElemKind::FloatTy || Q->getSrc()->dims().size() != 4 ||
        Q->getDest()->getElementType() != ElemKind::Int8QTy)
      continue;
    std::vector<const Instruction *> chain;
    const Value *cur = Q->getDest();
    const ConvolutionInst *C = nullptr;
    bool transposed = false;
    for (size_t j = next(i); j < ins.size(); j = next(j)) {
      if (users[cur] != 2) // the producer and the next instruction
        break;
      const Instruction *I = ins[j];
      if (auto *Cv = llvm::dyn_cast<ConvolutionInst>(I)) {
        if (Cv->getSrc() == cur && getConvPath(Cv) == ConvPath::FirstBNN && Cv->getSrc()->dims()[0] == 1)
          C = Cv;
        break;
      }
      const Value *lhs = nullptr, *rhs = nullptr, *dest = nullptr;
      if (auto *E = llvm::dyn_cast<ElementSubInst>(I))
        lhs = E->getLHS(), rhs = E->getRHS(), dest = E->getDest();
      else if (auto *E = llvm::dyn_cast<ElementDivInst>(I))
        lhs = E->getLHS(), rhs = E->getRHS(), dest = E->getDest();
      if (lhs) {
        if (lhs != cur || dest == cur || !plainInt8(rhs) || !plainInt8(dest) || !plainInt8(cur) ||
            rhs->dims() != cur->dims() || !constantPayload(rhs, ctx))
          break;
        chain.push_back(I);
        cur = dest;
        continue;
      }
      if (auto *T = llvm::dyn_cast<TransposeInst>(I)) {
        auto sh = T->getShuffle();
        if (transposed || T->getSrc() != cur || sh.size() != 4 || sh[0] != 0 || sh[1] != 2 || sh[2] != 3 ||
            sh[3] != 1 || T->getDest()->getType()->getScale() != cur->getType()->getScale() ||
            T->getDest()->getType()->getOffset() != cur->getType()->getOffset())
          break;
        chain.push_back(I);
        transposed = true;
        cur = T->getDest();
        continue;
      }
      break;
    }
    if (!C || C->getSrc()->dims()[3] > 16)
      continue;
    const int CH = C->getSrc()->dims()[3];
    // channel of element e of a value before (NCHW) or after (NHWC) the transpose
    auto channel = [&](const Value *v, size_t e, bool nchw) {
      auto d = v->dims();
      return nchw ? (int)((e / (d[2] * d[3])) % d[1]) : (int)(e % d[3]);
    };
    std::vector<int8_t> lut;
    bool ok = true, nchwNow = transposed;
    std::vector<int32_t> val(CH * 256);
    for (int c = 0; c < CH; c++)
      for (int q = 0; q < 256; q++)
        val[c * 256 + q] = q - 128;
    for (auto *I : chain) {
      if (llvm::isa<TransposeInst>(I)) {
        nchwNow = false;
        continue;
      }
      bool sub = llvm::isa<ElementSubInst>(I);
      const Value *lhs = sub ? llvm::cast<ElementSubInst>(I)->getLHS() : llvm::cast<ElementDivInst>(I)->getLHS();
      const Value *rhs = sub ? llvm::cast<ElementSubInst>(I)->getRHS() : llvm::cast<ElementDivInst>(I)->getRHS();
      const Value *dest = sub ? llvm::cast<ElementSubInst>(I)->getDest() : llvm::cast<ElementDivInst>(I)->getDest();
      auto h = constantPayload(rhs, ctx)->getHandle<int8_t>();
      std::vector<int> per(CH, 1000);
      for (size_t e = 0, n = h.size(); e < n && ok; e++) {
        int c = channel(rhs, e, nchwNow);
        if (per[c] == 1000)
          per[c] = h.raw(e);
        ok = per[c] == h.raw(e);
      }
      if (!ok)
        break;
      const float s0 = bundleScale(lhs->getType()->getScale()), s1 = bundleScale(rhs->getType()->getScale()),
                  sd = bundleScale(dest->getType()->getScale());
      for (int c = 0; c < CH; c++)
        for (int q = 0; q < 256; q++) {
          int32_t L = val[c * 256 + q], R = per[c], out32;
          if (sub) { // CPUBundle_generic.cpp elemsub
            float l = (s0 / sd) * float(L - 0);
            float r = (s1 / sd) * float(R - 0);
            out32 = std::round(l - r + 0);
          } else { // elemdiv
            float l = (s0)*float(L - 0);
            float r = s1 * sd * float(R - 0);
            out32 = std::round(l / r + 0);
          }
          val[c * 256 + q] = out32 > 127 ? 127 : (out32 < -128 ? -128 : out32);
        }
      lut.resize(CH * 256);
    }
    if (!ok)
      continue;
    for (size_t k = 0; k < lut.size(); k++)
      lut[k] = (int8_t)val[k];
    (*ctx->getPreprocs())[C] = {Q, transposed, lut};
    ctx->getPreFused()->insert(Q);
    for (auto *I : chain)
      ctx->getPreFused()->insert(I);
  }
}

//#### JO added (2026-10-31): max pools run on the EVTA (-vta-evta-maxpool, maxpool_evta): input and output in the EVTA
//#### layout, N = 1, square kernel, equal pads and strides, and the input >= 0: the instruction that last wrote it
//#### before the pool is an EVTA conv (also the zero-filled conv0) with relu, a fused conv + elementadd + relu, or a relu (the EVTA LOAD pads with
//#### 0, which then gives the CPU's max over the in-bounds window).
void findEvtaMaxPools(const IRFunction *IR, VTASaveContext *ctx) {
  ctx->getEvtaMaxPools()->clear();
  if (!vtaEvtaMaxPool || ctx->getPacked()->empty())
    return;
  std::map<const Value *, const Instruction *> writer;
  for (const auto &I : IR->getInstrs()) {
    if (auto *P = llvm::dyn_cast<MaxPoolInst>(&I)) {
      auto src = P->getSrc(), dest = P->getDest();
      auto k = P->getKernels(), pads = P->getPads(), st = P->getStrides();
      bool nonneg = false;
      if (writer.count(src)) {
        auto *W = writer[src];
        if (auto *C = llvm::dyn_cast<ConvolutionInst>(W))
          //#### FirstBNN: the zero-filled image conv0 (also an EVTA conv, convolution_wo_tr)
          nonneg = (getConvPath(C) == ConvPath::VTA || getConvPath(C) == ConvPath::FirstBNN) &&
                   (convRelu(C) || ctx->getFusion(C));   //#### JO changed (2026-11-06): == RELU -> convRelu
        else if (llvm::isa<ReluInst>(W))
          nonneg = true;
      }
      bool sq = k.size() == 2 && k[0] == k[1] && st.size() == 2 && st[0] == st[1];
      for (auto p : pads)
        sq = sq && p == pads[0];
      if (nonneg && sq && ctx->isPacked(src) && ctx->isPacked(dest) && src->dims()[0] == 1 &&
          src->getType()->getScale() == dest->getType()->getScale() && !src->getType()->getOffset() &&
          !dest->getType()->getOffset())
        ctx->getEvtaMaxPools()->insert(P);
    }
    for (const auto &op : I.getOperands())
      if (op.second != OperandKind::In) {
        //#### a fused conv writes the relu dest, not its own dest
        const Instruction *w = &I;
        writer[op.first] = w;
      }
    if (auto *C = llvm::dyn_cast<ConvolutionInst>(&I))
      if (auto *fu = ctx->getFusion(C))
        writer[fu->out] = C;
  }
}

void saveBNNConvolutionInst(const glow::BNNConvolutionInst *Inst,
                            std::string *bundle,
                            std::string *initConst,
                            VTASaveContext *ctx,
                            bool bnn_with_scale) {
  auto src = Inst->getSrc();
  auto srcDims = src->dims();
  auto filter = Inst->getFilter();
  auto filterDims = filter->dims();

  auto bias = Inst->getBias();
  assert(bias->dims()[0]==filterDims[0]);

  float inScale = src->getType()->getScale();
  float filterScale = filter->getType()->getScale();
  float outScale = Inst->getDest()->getType()->getScale();
  float biasScale = bias->getType()->getScale();

  int32_t outOffset = Inst->getDest()->getType()->getOffset();
  int32_t inOffset = src->getType()->getOffset();
  int32_t filterOffset = filter->getType()->getOffset();
  int32_t biasOffset = bias->getType()->getOffset();

  filterScale = 1/filterScale;
  inScale = 1/inScale;
  biasScale = 1/biasScale;
  outScale = 1/outScale;

  float matMulScale = inScale * filterScale;
  float scale =  matMulScale / outScale;

  int shift = getExpofPowerofTwo(scale);

  if (Inst->getGroup() == 1 && srcDims[3] % 128 == 0 &&
      filterDims[0] % 128 == 0 && filterDims[1] == 3 && filterDims[2] == 3) {
    saveBNNVTAConvolutionInst(Inst, bundle, initConst, ctx, bnn_with_scale);
  }
  else if (Inst->getGroup() == 1 && srcDims[3] < 128 &&
      filterDims[0] % 16 == 0 && filterDims[1] == 3 && filterDims[2] == 3) {
    savezeroFillBNNVTAConvolutionInst(Inst, bundle, initConst, ctx, bnn_with_scale);
  }
  else {
    saveBNNNonVTAConvolutionInst(Inst, bundle, initConst, ctx);
  }
}

//#### JO added (2026-11-02): fully connected layer on the EVTA (-vta-evta-fc). Batch 1, offsets 0, the input size a
//#### multiple of 16 and in * weight / out scale = 2^-shift (shift 0..15, ALU SHR). The weights go into the weights file in
//#### the EVTA layout of vtalib fullyconnected_evta_packed: NB = ceil(OUT/16) output blocks x IN/16 input blocks of 16x16
//#### ([16 out][16 in]), output-block major, zero padded; the bias as NB x 16 int32, rounded to the GEMM scale as the CPU
//#### fullyconnected does. The plan of the chunks must match vtalib fc_evta_plan (half of the 1024 wgt entries per chunk,
//#### 2 NB acc entries). Returns false (nothing written) when it does not fit.
//#### 2026-11-05: chunks of any size (was >= 8 output blocks, which only the HLS GEMM needs: vtalib fc_evta_plan checks it
//#### there and computes the fc on the CPU from the packed weights, fc_packed_cpu); Resnet50 2048 -> 1000 runs on the EVTA.
//#### JO changed (2026-11-08): the body as saveEvtaFcPacked for the fc and for a 1x1 conv over a 1x1 input (convFilter:
//#### filter [OUT][1][1][IN] instead of the fc's [IN][OUT]; MobileNetV2's logits 1280 -> 1001)
static bool saveEvtaFcPacked(Value *src, Value *dest, Value *filter, Value *bias, uint32_t IN, uint32_t OUT,
                             bool convFilter, std::string *bundle, VTASaveContext *ctx) {
  if (filter->getElementType() != ElemKind::Int8QTy || bias->getElementType() != ElemKind::Int32QTy || IN % 16)
    return false;
  if (src->getType()->getOffset() || filter->getType()->getOffset() || dest->getType()->getOffset() ||
      bias->getType()->getOffset())
    return false;
  const float matMulScale = src->getType()->getScale() * filter->getType()->getScale();
  const float ratio = matMulScale / dest->getType()->getScale();
  int shift = -1;
  for (int k = 0; k <= 15; k++)
    if (ratio == std::ldexp(1.0f, -k))
      shift = k;
  const uint32_t CIN = IN / 16, NB = (OUT + 15) / 16, g = std::min<uint32_t>(NB, 512 / CIN);
  const uint32_t nChunks = g ? (NB + g - 1) / g : 0, gEven = nChunks ? (NB + nChunks - 1) / nChunks : 0;
  if (shift < 0 || gEven < 1 || 2 * NB > 2048 || hasConstantSymbol(filter, ctx) || hasConstantSymbol(bias, ctx))
    return false;

  const Tensor *tensor = nullptr, *tensorBias = nullptr;
  auto vMap = ctx->getVariableMap();
  for (auto it = vMap->begin(); it != vMap->end(); it++) {
    if (it->first->getKind() != glow::Kinded::Kind::ConstantKind)
      continue;
    if (it->second == filter)
      tensor = &(llvm::cast<Constant>(it->first)->getPayload());
    if (it->second == bias)
      tensorBias = &(llvm::cast<Constant>(it->first)->getPayload());
  }
  if (!tensor || !tensorBias)
    return false;
  auto handle = tensor->getHandle<int8_t>();
  auto handleBias = tensorBias->getHandle<int32_t>();
  const float biasRatio = bias->getType()->getScale() / matMulScale;

  std::vector<int8_t> w((size_t)NB * CIN * 256, 0);
  for (uint32_t k = 0; k < IN; k++)
    for (uint32_t j = 0; j < OUT; j++)
      w[((size_t)(j / 16) * CIN + k / 16) * 256 + (j % 16) * 16 + k % 16] =
          handle.raw(convFilter ? (size_t)j * IN + k : (size_t)k * OUT + j);
  std::vector<int32_t> b((size_t)NB * 16, 0);
  for (uint32_t j = 0; j < OUT; j++)
    b[j] = std::round(float(handleBias.raw(j)) * biasRatio);
  auto fos = ctx->getWeightFileStream();
  fos->write((const char *)w.data(), w.size());
  auto filterSte = addConstantSymbolEntrySized(filter, w.size(), ctx);
  fos->write((const char *)b.data(), b.size() * 4);
  auto biasSte = addConstantSymbolEntrySized(bias, b.size() * 4, ctx);

  auto srcWeight = static_cast<WeightVar *>(src);
  auto destWeight = static_cast<WeightVar *>(dest);
  bool srcMut = srcWeight->getMutability() == glow::WeightVar::MutabilityKind::Mutable;
  bool destMut = destWeight->getMutability() == glow::WeightVar::MutabilityKind::Mutable;
  if (srcMut)
    addSymbolEntryGenBundle(srcWeight, bundle, ctx);
  if (destMut)
    addSymbolEntryGenBundle(destWeight, bundle, ctx);

  uint32_t idx = ctx->getIdxMultiEVTA();
  bundle->append("  fullyconnected_evta_packed(");
  bundle->append(srcMut ? addSymbolEntry(srcWeight, ctx).name : src->getName().data());
  bundle->append(", ");
  bundle->append(filterSte.name);
  bundle->append(", ");
  bundle->append(biasSte.name);
  bundle->append(", ");
  bundle->append(destMut ? addSymbolEntry(destWeight, ctx).name : dest->getName().data());
  for (int v : {(int)IN, (int)OUT, shift}) {
    bundle->append(", ");
    bundle->append(std::to_string(v));
  }
  bundle->append(", vtaCmdH");
  if (idx)
    bundle->append(std::to_string(idx));
  bundle->append(", ");
  bundle->append(std::to_string(idx));
  bundle->append(");\n");
  return true;
}

static bool saveEvtaFullyConnectedInst(const glow::FullyConnectedInst *Inst, std::string *bundle,
                                       VTASaveContext *ctx) {
  auto src = Inst->getSrc(), dest = Inst->getDest(), filter = Inst->getWeights();
  if (src->dims().size() != 2 || dest->dims().size() != 2 || filter->dims().size() != 2 || src->dims()[0] != 1 ||
      dest->dims()[0] != 1)
    return false;
  const uint32_t IN = src->dims()[1], OUT = dest->dims()[1];
  if (filter->dims()[0] != IN || filter->dims()[1] != OUT)
    return false;
  return saveEvtaFcPacked(src, dest, filter, Inst->getBias(), IN, OUT, false, bundle, ctx);
}

//#### JO added (2026-11-08): a 1x1 conv over a 1x1 input without activation (N = 1, group 1, stride / dilation
//#### irrelevant, pads 0) is an fc: the EVTA fc path, any OUT (MobileNetV2's logits 1280 -> 1001; 1001 % 16 kept it
//#### off the EVTA conv path). -vta-evta-fc.
static bool isConvAsFc(const glow::ConvolutionInst *Inst) {
  auto sd = Inst->getSrc()->dims(), fd = Inst->getFilter()->dims(), dd = Inst->getDest()->dims();
  auto P = Inst->getPads();
  return vtaEvtaFc && Inst->getSrc()->getType()->isQuantizedType() && sd.size() == 4 && sd[0] == 1 && sd[1] == 1 &&
         sd[2] == 1 && fd[1] == 1 && fd[2] == 1 && dd[1] == 1 && dd[2] == 1 && Inst->getGroup() == 1 &&
         Inst->getFusedActivation() == NONE && std::all_of(P.begin(), P.end(), [](unsigned p) { return p == 0; });
}
static bool saveEvtaConvAsFc(const glow::ConvolutionInst *Inst, std::string *bundle, VTASaveContext *ctx) {
  return isConvAsFc(Inst) &&
         saveEvtaFcPacked(Inst->getSrc(), Inst->getDest(), Inst->getFilter(), Inst->getBias(),
                          Inst->getSrc()->dims()[3], Inst->getDest()->dims()[3], true, bundle, ctx);
}

void saveFullyConnectedInst(const glow::FullyConnectedInst *Inst,
                            std::string *bundle, VTASaveContext *ctx) {
  if (vtaEvtaFc && saveEvtaFullyConnectedInst(Inst, bundle, ctx))
    return;
  auto src = Inst->getSrc();
  auto srcDims = src->dims();
  auto dest = Inst->getDest();
  auto destDims = dest->dims();
  auto filter = Inst->getWeights();
  auto filterDims = filter->dims();
  auto bias = Inst->getBias();

  assert(srcDims.size() == 2);
  assert(bias->dims().size() == 1);
  assert(filterDims.size() == 2);
  assert(destDims.size() == 2);

  float inScale = src->getType()->getScale();
  float filterScale = filter->getType()->getScale();
  float outScale = Inst->getDest()->getType()->getScale();
  float biasScale = bias->getType()->getScale();

  filterScale = 1 / filterScale;
  inScale = 1 / inScale;
  biasScale = 1 / biasScale;
  outScale = 1 / outScale;

  // save Weight file
  auto vMap = ctx->getVariableMap();
  const Tensor *tensor = NULL;

  for (auto it = vMap->begin(); it != vMap->end(); it++) {
    if (it->second == filter) {
      auto storage = it->first;
      if (storage->getKind() == glow::Kinded::Kind::ConstantKind) {
        const auto *constant = llvm::cast<Constant>(storage);
        tensor = &(constant->getPayload());
      }
      break;
    }
  }
  assert(tensor);
  auto handle = tensor->getHandle<int8_t>();
  assert(handle.size() % 2 == 0);
  {
    auto fos = ctx->getWeightFileStream();
    int16_t data16 = 0;
    for (size_t i = 0, e = handle.size(); i < e; i++) {
      auto data = handle.raw(i);
      if (data > 127)
        data = 127.0;
      if (data < -128)
        data = -128.0;
      int8_t clip_data = std::floor(data);
      if (i % 2 == 0) {
        data16 = 0xff & clip_data;
      } else {
        data16 = data16 | clip_data << 8;
        fos->write((const char *)&data16, 2);
      }
    }
  }

  auto filterSte = addConstantSymbolEntry(filter, ctx);

  // save Bias file
  const Tensor *tensorBias = NULL;

  for (auto it = vMap->begin(); it != vMap->end(); it++) {
    if (it->second == bias) {
      auto storage = it->first;
      if (storage->getKind() == glow::Kinded::Kind::ConstantKind) {
        const auto *constant = llvm::cast<Constant>(storage);
        tensorBias = &(constant->getPayload());
      }
      break;
    }
  }
  assert(tensorBias);
  auto handleBias = tensorBias->getHandle<int32_t>();
  assert(handleBias.size() % 2 == 0);
  {
    auto fos = ctx->getWeightFileStream();
    for (size_t i = 0, e = handleBias.size(); i < e; i++) {
      auto data = handleBias.raw(i);
      fos->write((const char *)&data, 4);
    }
  }
  auto biasSte = addConstantSymbolEntry(bias, ctx);

  assert(sameScale(biasScale, inScale * filterScale));

  auto srcWeight = static_cast<WeightVar *>(src);
  if (srcWeight->getMutability() == glow::WeightVar::MutabilityKind::Mutable) {
    addSymbolEntryGenBundle(srcWeight, bundle, ctx);
  }

  auto destWeight = static_cast<WeightVar *>(dest);
  if (destWeight->getMutability() == glow::WeightVar::MutabilityKind::Mutable) {
    addSymbolEntryGenBundle(destWeight, bundle, ctx);
  }

  bundle->append("  fullyconnected(");

  if (srcWeight->getMutability() == glow::WeightVar::MutabilityKind::Mutable) {
    auto ste = addSymbolEntry(srcWeight, ctx);
    bundle->append(ste.name);
    bundle->append(", ");
  } else {
    bundle->append(src->getName());
    bundle->append(", ");
  }
  bundle->append("1.0/");
  bundle->append(std::to_string(inScale));
  bundle->append(", 0, ");

  if (filterSte.kind == '0') {
    bundle->append("(int8_t *)VTABufferGetVirtAddr(");
    bundle->append(filterSte.name);
    bundle->append(")");
  } else {
    bundle->append(filterSte.name);
  }

  bundle->append(", ");
  bundle->append("1.0/");
  bundle->append(std::to_string(filterScale));
  bundle->append(", 0, ");

  if (biasSte.kind == '0') {
    bundle->append("(int8_t *)VTABufferGetVirtAddr(");
    bundle->append(biasSte.name);
    bundle->append(")");
  } else {
    bundle->append(biasSte.name);
  }

  bundle->append(", ");
  bundle->append("1.0/");
  bundle->append(std::to_string(biasScale));
  bundle->append(", 0, ");

  if (destWeight->getMutability() == glow::WeightVar::MutabilityKind::Mutable) {
    auto ste = addSymbolEntry(destWeight, ctx);
    bundle->append(ste.name);
    bundle->append(", ");
  } else {
    bundle->append(dest->getName());
    bundle->append(", ");
  }
  bundle->append("1.0/");
  bundle->append(std::to_string(outScale));
  bundle->append(", 0, ");

  bundle->append(std::to_string(srcDims[0]));
  bundle->append(", ");
  bundle->append(std::to_string(srcDims[1]));
  bundle->append(", ");
  bundle->append(std::to_string(filterDims[0]));
  bundle->append(", ");
  bundle->append(std::to_string(filterDims[1]));
  bundle->append(", ");
  bundle->append(std::to_string(destDims[0]));
  bundle->append(", ");
  bundle->append(std::to_string(destDims[1]));
  bundle->append(", ");
  bundle->append(std::to_string(1)); // doBias
  bundle->append(" );\n");
}

void saveAvgPoolInst(const glow::AvgPoolInst *Inst, std::string *bundle,
                     VTASaveContext *ctx) {
  auto src = Inst->getSrc();
  auto srcDims = src->dims();
  auto dest = Inst->getDest();
  auto destDims = dest->dims();

  float inScale = src->getType()->getScale();
  float outScale = Inst->getDest()->getType()->getScale();

  inScale = 1 / inScale;
  outScale = 1 / outScale;

  auto pad = Inst->getPads();
  assert(pad[0] == pad[1]);
  int pad_size = pad[0];

  auto strides = Inst->getStrides();
  assert(strides[0] == strides[1]);
  int stride_size = strides[0];

  auto kernels = Inst->getKernels();
  int KH = kernels[0];
  int KW = kernels[1];

  auto srcWeight = static_cast<WeightVar *>(src);
  if (srcWeight->getMutability() == glow::WeightVar::MutabilityKind::Mutable) {
    addSymbolEntryGenBundle(srcWeight, bundle, ctx);
  }

  auto destWeight = static_cast<WeightVar *>(dest);
  if (destWeight->getMutability() == glow::WeightVar::MutabilityKind::Mutable) {
    addSymbolEntryGenBundle(destWeight, bundle, ctx);
  }

  bundle->append("  avgpool(");

  if (srcWeight->getMutability() == glow::WeightVar::MutabilityKind::Mutable) {
    auto ste = addSymbolEntry(srcWeight, ctx);
    bundle->append(ste.name);
    bundle->append(", ");
  } else {
    bundle->append(src->getName());
    bundle->append(", ");
  }
  bundle->append("1.0/");
  bundle->append(std::to_string(inScale));
  bundle->append(", 0, ");

  if (destWeight->getMutability() == glow::WeightVar::MutabilityKind::Mutable) {
    auto ste = addSymbolEntry(destWeight, ctx);
    bundle->append(ste.name);
    bundle->append(", ");
  } else {
    bundle->append(dest->getName());
    bundle->append(", ");
  }
  bundle->append("1.0/");
  bundle->append(std::to_string(outScale));
  bundle->append(", 0, ");

  //#### JO added (2026-10-31): EVTA layout N{C/16}HW16 = NHWC with the 16-channel blocks as the batch
  //#### (a 1x1 output is then plain NHWC)
  dim_t inN = srcDims[0], inC = srcDims[3], outN = destDims[0], outC = destDims[3];
  if (ctx->isPacked(src)) {
    inN = inN * inC / 16;
    inC = 16;
    outN = outN * outC / 16;
    outC = 16;
  }
  bundle->append(std::to_string(inN));
  bundle->append(", ");
  bundle->append(std::to_string(srcDims[1]));
  bundle->append(", ");
  bundle->append(std::to_string(srcDims[2]));
  bundle->append(", ");
  bundle->append(std::to_string(inC));
  bundle->append(", ");
  bundle->append(std::to_string(outN));
  bundle->append(", ");
  bundle->append(std::to_string(destDims[1]));
  bundle->append(", ");
  bundle->append(std::to_string(destDims[2]));
  bundle->append(", ");
  bundle->append(std::to_string(outC));
  bundle->append(", ");
  bundle->append(std::to_string(KH));
  bundle->append(", ");
  bundle->append(std::to_string(KW));
  bundle->append(", ");
  bundle->append(std::to_string(pad_size));
  bundle->append(", ");
  bundle->append(std::to_string(stride_size));
  bundle->append(" );\n");
}

void saveTensorViewInst(const glow::TensorViewInst *Inst, std::string *bundle,
                        VTASaveContext *ctx) {
  auto src = Inst->getSrc();
  //#### JO added (2026-11-09): a view at non-zero offsets starts there (DeiT: the patch tokens in the concat, the attention
  //#### heads), a view of a constant reads it through its virtual address (was: the source pointer for any offsets, the
  //#### constant's buffer handle). Views at offset 0 of activations / mutable weights are emitted as before.
  {
    auto offs = Inst->getOffsets();
    auto sd = src->dims();
    uint64_t off = 0, stride = 1;
    for (int i = (int)offs.size() - 1; i >= 0; i--) {
      off += offs[i] * stride;
      stride *= sd[i];
    }
    off *= src->getType()->getElementSize();
    if (off != 0 || isConstantWeight(src)) {
      std::string p = cpuPtr(src, bundle, ctx);
      bundle->append("  int8_t* " + Inst->getName().str() + " = " + p);
      if (off != 0)
        bundle->append(" + " + std::to_string(off));
      bundle->append(";\n");
      return;
    }
  }
  auto srcWeight = static_cast<WeightVar *>(src);
  if (srcWeight->getMutability() == glow::WeightVar::MutabilityKind::Mutable) {
    addSymbolEntryGenBundle(srcWeight, bundle, ctx);
  }

  bundle->append("  int8_t* ");
  bundle->append(Inst->getName().str());
  bundle->append(" = ");

  if (srcWeight->getMutability() == glow::WeightVar::MutabilityKind::Mutable) {
    auto ste = addSymbolEntry(srcWeight, ctx);
    bundle->append(ste.name);
  } else {
    bundle->append(src->getName());
  }

  // bundle->append(Inst->getSrc()->getName().str());
  bundle->append(";\n");
  return;
}
void saveMaxPoolInst(const glow::MaxPoolInst *Inst, std::string *bundle,
                     VTASaveContext *ctx) {
  assert(Inst->getSrc()->getType()->isQuantizedType());
  auto pad = Inst->getPads();
  assert(pad[0] == pad[1]);
  auto strides = Inst->getStrides();
  assert(strides[0] == strides[1]);

  auto src = Inst->getSrc();
  auto srcDims = src->dims();

  auto dest = Inst->getDest();
  auto kernels = Inst->getKernels();

  int N = srcDims[0];
  int H = srcDims[1];
  int W = srcDims[2];
  int C = srcDims[3];
  int KH = kernels[0];
  int KW = kernels[1];

  int pad_size = pad[0];
  int stride_size = strides[0];
  //#### JO added (2026-10-31): on the EVTA (findEvtaMaxPools)
  if (ctx->isEvtaMaxPool(Inst)) {
    uint32_t idx = ctx->getIdxMultiEVTA();
    bundle->append("  maxpool_evta(");
    bundle->append(src->getName());
    bundle->append("_vbuf, ");
    bundle->append(dest->getName());
    bundle->append("_vbuf, ");
    for (int v : {N, H, W, C, KH, KW, pad_size, stride_size}) {
      bundle->append(std::to_string(v));
      bundle->append(", ");
    }
    bundle->append("vtaCmdH");
    if (idx)
      bundle->append(std::to_string(idx));
    bundle->append(", ");
    bundle->append(std::to_string(idx));
    bundle->append(");\n");
    bundle->append("  VTABufferInvalidate(");
    bundle->append(dest->getName());
    bundle->append("_vbuf, 0, ");
    bundle->append(std::to_string(dest->getSizeInBytes()));
    bundle->append(");\n");
    return;
  }
  //#### JO added (2026-10-31): EVTA layout N{C/16}HW16 = NHWC with the 16-channel blocks as the batch
  if (ctx->isPacked(src)) {
    N = N * C / 16;
    C = 16;
  }

  auto srcWeight = static_cast<WeightVar *>(src);
  if (srcWeight->getMutability() == glow::WeightVar::MutabilityKind::Mutable) {
    addSymbolEntryGenBundle(srcWeight, bundle, ctx);
  }

  auto destWeight = static_cast<WeightVar *>(dest);
  if (destWeight->getMutability() == glow::WeightVar::MutabilityKind::Mutable) {
    addSymbolEntryGenBundle(destWeight, bundle, ctx);
  }

  bundle->append("  maxpool(");

  if (srcWeight->getMutability() == glow::WeightVar::MutabilityKind::Mutable) {
    auto ste = addSymbolEntry(srcWeight, ctx);
    bundle->append(ste.name);
    bundle->append(", ");
  } else {
    bundle->append(src->getName());
    bundle->append(", ");
  }

  if (destWeight->getMutability() == glow::WeightVar::MutabilityKind::Mutable) {
    auto ste = addSymbolEntry(destWeight, ctx);
    bundle->append(ste.name);
    bundle->append(", ");
  } else {
    bundle->append(dest->getName());
    bundle->append(", ");
  }

  bundle->append(std::to_string(N));
  bundle->append(", ");
  bundle->append(std::to_string(H));
  bundle->append(", ");
  bundle->append(std::to_string(W));
  bundle->append(", ");
  bundle->append(std::to_string(C));
  bundle->append(", ");
  bundle->append(std::to_string(KH));
  bundle->append(", ");
  bundle->append(std::to_string(KW));
  bundle->append(", ");
  bundle->append(std::to_string(pad_size));
  bundle->append(", ");
  bundle->append(std::to_string(stride_size));
  bundle->append(" );\n");
}

void saveElemSignInst(const glow::ElementSignInst *Inst, std::string *bundle,
                      VTASaveContext *ctx) {
  assert(Inst->getSrc()->getType()->isQuantizedType());
  auto src = Inst->getSrc();
  auto srcDims = src->dims();
  auto dest = Inst->getDest();
  auto destDims = dest->dims();

  auto srcWeight = static_cast<WeightVar *>(src);
  if (srcWeight->getMutability() == glow::WeightVar::MutabilityKind::Mutable) {
    addSymbolEntryGenBundle(srcWeight, bundle, ctx);
  }

  auto destWeight = static_cast<WeightVar *>(dest);
  if (destWeight->getMutability() == glow::WeightVar::MutabilityKind::Mutable) {
    addSymbolEntryGenBundle(destWeight, bundle, ctx);
  }

  bundle->append("  elemsign(");

  if (srcWeight->getMutability() == glow::WeightVar::MutabilityKind::Mutable) {
    auto ste = addSymbolEntry(srcWeight, ctx);
    bundle->append(ste.name);
    bundle->append(", ");
  } else {
    bundle->append(src->getName());
    bundle->append(", ");
  }

  if (destWeight->getMutability() == glow::WeightVar::MutabilityKind::Mutable) {
    auto ste = addSymbolEntry(destWeight, ctx);
    bundle->append(ste.name);
    bundle->append(", ");
  } else {
    bundle->append(dest->getName());
    bundle->append(", ");
  }

  unsigned_t size = 1;
  for (int i = 0; i < destDims.size(); i++) {
    size *= destDims[i];
  }

  bundle->append(std::to_string(size));
  bundle->append(");\n ");
}

void saveQuantizeInst(const glow::QuantizeInst *Inst, std::string *bundle,
                      VTASaveContext *ctx) {

  auto src = Inst->getSrc();

  auto dest = Inst->getDest();
  auto destTy = dest->getType();
  auto scale = destTy->getScale();
  auto offset = destTy->getOffset();
  auto revScale = 1 / scale;

  auto srcWeight = static_cast<WeightVar *>(src);
  if (srcWeight->getMutability() == glow::WeightVar::MutabilityKind::Mutable) {
    addSymbolEntryGenBundle(srcWeight, bundle, ctx);
  }

  auto destWeight = static_cast<WeightVar *>(dest);
  if (destWeight->getMutability() == glow::WeightVar::MutabilityKind::Mutable) {
    addSymbolEntryGenBundle(destWeight, bundle, ctx);
  }

  bundle->append("  quantize(");

  if (srcWeight->getMutability() == glow::WeightVar::MutabilityKind::Mutable) {
    auto ste = addSymbolEntry(srcWeight, ctx);
    bundle->append(ste.name);
    bundle->append(", ");
  } else {
    bundle->append(src->getName());
    bundle->append(", ");
  }

  if (destWeight->getMutability() == glow::WeightVar::MutabilityKind::Mutable) {
    auto ste = addSymbolEntry(destWeight, ctx);
    bundle->append(ste.name);
    bundle->append(", ");
  } else {
    bundle->append(dest->getName());
    bundle->append(", ");
  }

  bundle->append(std::to_string(src->size()));
  bundle->append(", 1/");
  bundle->append(std::to_string(revScale));
  bundle->append(", ");
  bundle->append(std::to_string(offset));
  bundle->append(" );\n");
}

void saveDequantizeInst(const glow::DequantizeInst *Inst, std::string *bundle,
                        VTASaveContext *ctx) {

  auto src = Inst->getSrc();
  auto srcTy = src->getType();

  auto dest = Inst->getDest();
  auto scale = srcTy->getScale();
  auto offset = srcTy->getOffset();
  auto revScale = 1 / scale;

  auto srcWeight = static_cast<WeightVar *>(src);
  if (srcWeight->getMutability() == glow::WeightVar::MutabilityKind::Mutable) {
    addSymbolEntryGenBundle(srcWeight, bundle, ctx);
  }

  auto destWeight = static_cast<WeightVar *>(dest);
  if (destWeight->getMutability() == glow::WeightVar::MutabilityKind::Mutable) {
    addSymbolEntryGenBundle(destWeight, bundle, ctx);
  }

  bundle->append("  dequantize(");

  if (srcWeight->getMutability() == glow::WeightVar::MutabilityKind::Mutable) {
    auto ste = addSymbolEntry(srcWeight, ctx);
    bundle->append(ste.name);
    bundle->append(", ");
  } else {
    bundle->append(src->getName());
    bundle->append(", ");
  }

  if (destWeight->getMutability() == glow::WeightVar::MutabilityKind::Mutable) {
    auto ste = addSymbolEntry(destWeight, ctx);
    bundle->append(ste.name);
    bundle->append(", ");
  } else {
    bundle->append(dest->getName());
    bundle->append(", ");
  }

  bundle->append(std::to_string(src->size()));
  bundle->append(", 1/");
  bundle->append(std::to_string(revScale));
  bundle->append(", ");
  bundle->append(std::to_string(offset));
  bundle->append(" );\n");
}

void saveSplatInst(const glow::SplatInst *Inst, std::string *bundle,
                   VTASaveContext *ctx) {
  auto dest = Inst->getDest();
  auto destDims = dest->dims();

  auto destWeight = static_cast<WeightVar *>(dest);
  if (destWeight->getMutability() == glow::WeightVar::MutabilityKind::Mutable) {
    addSymbolEntryGenBundle(destWeight, bundle, ctx);
  }

  bundle->append("  splat(");

  if (destWeight->getMutability() == glow::WeightVar::MutabilityKind::Mutable) {
    auto ste = addSymbolEntry(destWeight, ctx);
    bundle->append(ste.name);
    bundle->append(", ");
  } else {
    bundle->append(dest->getName());
    bundle->append(", ");
  }

  unsigned_t size = 1;
  for (int i = 0; i < destDims.size(); i++) {
    size *= destDims[i];
  }

  bundle->append(std::to_string(size));
  bundle->append(", ");

  auto destTy = Inst->getDest()->getType();
  TensorQuantizationParams destQ{destTy->getScale(), destTy->getOffset()};
  float val = Inst->getValue();

  bundle->append(std::to_string((quantization::quantize(val, destQ))));
  bundle->append(" );\n");
}

void saveTransposeInst(const glow::TransposeInst *Inst, std::string *bundle,
                       VTASaveContext *ctx) {
  auto src = Inst->getSrc();
  auto srcDims = src->dims();

  auto dest = Inst->getDest();
  auto destDims = dest->dims();

  auto shuffle = Inst->getShuffle();

  auto srcWeight = static_cast<WeightVar *>(src);
  if (srcWeight->getMutability() == glow::WeightVar::MutabilityKind::Mutable) {
    addSymbolEntryGenBundle(srcWeight, bundle, ctx);
  }

  auto destWeight = static_cast<WeightVar *>(dest);
  if (destWeight->getMutability() == glow::WeightVar::MutabilityKind::Mutable) {
    addSymbolEntryGenBundle(destWeight, bundle, ctx);
  }

  bundle->append("  transpose(");

  if (srcWeight->getMutability() == glow::WeightVar::MutabilityKind::Mutable) {
    auto ste = addSymbolEntry(srcWeight, ctx);
    bundle->append(ste.name);
    bundle->append(", ");
  } else {
    bundle->append(src->getName());
    bundle->append(", ");
  }

  if (destWeight->getMutability() == glow::WeightVar::MutabilityKind::Mutable) {
    auto ste = addSymbolEntry(destWeight, ctx);
    bundle->append(ste.name);
    bundle->append(", ");
  } else {
    bundle->append(dest->getName());
    bundle->append(", ");
  }

  bundle->append(std::to_string(srcDims[0]));
  bundle->append(", ");
  bundle->append(std::to_string(srcDims[1]));
  bundle->append(", ");
  bundle->append(std::to_string(srcDims[2]));
  bundle->append(", ");
  bundle->append(std::to_string(srcDims[3]));
  bundle->append(", ");
  bundle->append(std::to_string(destDims[0]));
  bundle->append(", ");
  bundle->append(std::to_string(destDims[1]));
  bundle->append(", ");
  bundle->append(std::to_string(destDims[2]));
  bundle->append(", ");
  bundle->append(std::to_string(destDims[3]));
  bundle->append(", ");
  bundle->append(std::to_string(shuffle[0]));
  bundle->append(", ");
  bundle->append(std::to_string(shuffle[1]));
  bundle->append(", ");
  bundle->append(std::to_string(shuffle[2]));
  bundle->append(", ");
  bundle->append(std::to_string(shuffle[3]));
  bundle->append(" );\n");
}

void saveTranspose_6dim(const glow::TransposeInst *Inst, std::string *bundle,
                        VTASaveContext *ctx) {
  // TODO : consider group
  // auto group = Inst->getGroup();
  auto src = Inst->getSrc();
  auto srcDims = src->dims();
  auto dest = Inst->getDest();
  auto destDims = dest->dims();

  auto shuffle = Inst->getShuffle();

  auto srcWeight = static_cast<WeightVar *>(src);
  if (srcWeight->getMutability() == glow::WeightVar::MutabilityKind::Mutable) {
    addSymbolEntryGenBundle(srcWeight, bundle, ctx);
  }

  auto destWeight = static_cast<WeightVar *>(dest);
  if (destWeight->getMutability() == glow::WeightVar::MutabilityKind::Mutable) {
    addSymbolEntryGenBundle(destWeight, bundle, ctx);
  }

  bundle->append("  transpose_6dim(");
  if (srcWeight->getMutability() == glow::WeightVar::MutabilityKind::Mutable) {
    auto ste = addSymbolEntry(srcWeight, ctx);
    bundle->append(ste.name);
    bundle->append(", ");
  } else {
    bundle->append(src->getName());
    bundle->append(", ");
  }

  if (destWeight->getMutability() == glow::WeightVar::MutabilityKind::Mutable) {
    auto ste = addSymbolEntry(destWeight, ctx);
    bundle->append(ste.name);
    bundle->append(", ");
  } else {
    bundle->append(dest->getName());
    bundle->append(", ");
  }

  bundle->append(std::to_string(srcDims[0]));
  bundle->append(", ");
  bundle->append(std::to_string(srcDims[1]));
  bundle->append(", ");
  bundle->append(std::to_string(srcDims[2]));
  bundle->append(", ");
  bundle->append(std::to_string(srcDims[3]));
  bundle->append(", ");
  bundle->append(std::to_string(srcDims[4]));
  bundle->append(", ");
  bundle->append(std::to_string(srcDims[5]));
  bundle->append(", ");

  bundle->append(std::to_string(destDims[0]));
  bundle->append(", ");
  bundle->append(std::to_string(destDims[1]));
  bundle->append(", ");
  bundle->append(std::to_string(destDims[2]));
  bundle->append(", ");
  bundle->append(std::to_string(destDims[3]));
  bundle->append(", ");
  bundle->append(std::to_string(destDims[4]));
  bundle->append(", ");
  bundle->append(std::to_string(destDims[5]));
  bundle->append(", ");

  bundle->append(std::to_string(shuffle[0]));
  bundle->append(", ");
  bundle->append(std::to_string(shuffle[1]));
  bundle->append(", ");
  bundle->append(std::to_string(shuffle[2]));
  bundle->append(", ");
  bundle->append(std::to_string(shuffle[3]));
  bundle->append(", ");
  bundle->append(std::to_string(shuffle[4]));
  bundle->append(", ");
  bundle->append(std::to_string(shuffle[5]));
  bundle->append(" );\n");
}

void saveElemAddInst(const glow::ElementAddInst *Inst, std::string *bundle,
                     VTASaveContext *ctx) {
  auto src0 = Inst->getLHS();
  auto src1 = Inst->getRHS();

  auto dest = Inst->getDest();
  auto destDims = dest->dims();

  auto srcWeight0 = static_cast<WeightVar *>(src0);
  if (srcWeight0->getMutability() == glow::WeightVar::MutabilityKind::Mutable) {
    addSymbolEntryGenBundle(srcWeight0, bundle, ctx);
  }

  auto srcWeight1 = static_cast<WeightVar *>(src1);
  if (srcWeight1->getMutability() == glow::WeightVar::MutabilityKind::Mutable) {
    addSymbolEntryGenBundle(srcWeight1, bundle, ctx);
  }

  auto destWeight = static_cast<WeightVar *>(dest);
  if (destWeight->getMutability() == glow::WeightVar::MutabilityKind::Mutable) {
    addSymbolEntryGenBundle(destWeight, bundle, ctx);
  }

  bundle->append("  elemadd(");

  if (srcWeight0->getMutability() == glow::WeightVar::MutabilityKind::Mutable) {
    auto ste = addSymbolEntry(srcWeight0, ctx);
    bundle->append(ste.name);
    bundle->append(", ");
  } else {
    //#### JO changed (2026-11-09): a constant operand through its virtual address (DeiT: biases, position embedding)
    bundle->append(isConstantWeight(src0) ? cpuPtr(src0, bundle, ctx) : src0->getName().str());
    bundle->append(", ");
  }

  bundle->append("1.0/");
  bundle->append(std::to_string(1 / src0->getType()->getScale()));
  bundle->append(", 0, ");

  if (srcWeight1->getMutability() == glow::WeightVar::MutabilityKind::Mutable) {
    auto ste = addSymbolEntry(srcWeight1, ctx);
    bundle->append(ste.name);
    bundle->append(", ");
  } else {
    //#### JO changed (2026-11-09): a constant operand through its virtual address (DeiT: biases, position embedding)
    bundle->append(isConstantWeight(src1) ? cpuPtr(src1, bundle, ctx) : src1->getName().str());
    bundle->append(", ");
  }

  bundle->append("1.0/");
  bundle->append(std::to_string(1 / src1->getType()->getScale()));
  bundle->append(", 0, ");

  if (destWeight->getMutability() == glow::WeightVar::MutabilityKind::Mutable) {
    auto ste = addSymbolEntry(destWeight, ctx);
    bundle->append(ste.name);
    bundle->append(", ");
  } else {
    bundle->append(dest->getName());
    bundle->append(", ");
  }

  bundle->append("1.0/");
  bundle->append(std::to_string(1 / dest->getType()->getScale()));
  bundle->append(", 0, ");

  unsigned_t size = 1;
  for (int i = 0; i < destDims.size(); i++) {
    size *= destDims[i];
  }

  bundle->append(std::to_string(size));
  bundle->append(" );\n");
}

void saveElemSubInst(const glow::ElementSubInst *Inst, std::string *bundle,
                     VTASaveContext *ctx) {

  if (Inst->getLHS()->getType()->isQuantizedType()) {
    auto src0 = Inst->getLHS();
    auto src1 = Inst->getRHS();
    auto dest = Inst->getDest();
    auto destDims = dest->dims();

    auto srcWeight0 = static_cast<WeightVar *>(src0);
    if (srcWeight0->getMutability() ==
        glow::WeightVar::MutabilityKind::Mutable) {
      addSymbolEntryGenBundle(srcWeight0, bundle, ctx);
    }

    auto srcWeight1 = static_cast<WeightVar *>(src1);
    if (srcWeight1->getMutability() ==
        glow::WeightVar::MutabilityKind::Mutable) {
      addSymbolEntryGenBundle(srcWeight1, bundle, ctx);
    }
    // TODO : yongin apply this code to other saves
    else if (isConstantWeight(src1) && !hasConstantSymbol(src1, ctx)) {
      //#### JO changed (2026-11-09): only a constant, and its bytes once (DeiT: an activation divisor / subtrahend; one
      //#### constant shared by 12 elemdivs was written 12 times, shifting every later constant)
      addConstantSymbolEntry(srcWeight1, ctx);
      {
        auto vMap = ctx->getVariableMap();
        const Tensor *tensor = NULL;
        for (auto it = vMap->begin(); it != vMap->end(); it++) {
          if (it->second == src1) {
            auto storage = it->first;
            if (storage->getKind() == glow::Kinded::Kind::ConstantKind) {
              const auto *constant = llvm::cast<Constant>(storage);
              tensor = &(constant->getPayload());
            }
            break;
          }
        }
        assert(tensor);
        auto handle = tensor->getHandle<int8_t>();
        auto fos = ctx->getWeightFileStream();
        int16_t data16 = 0;
        for (size_t i = 0, e = handle.size(); i < e; i++) {
          auto data = handle.raw(i);
          if (data > 127)
            data = 127.0;
          if (data < -128)
            data = -128.0;
          int8_t clip_data = std::floor(data);
          if (i % 2 == 0) {
            data16 = 0xff & clip_data;
          } else {
            data16 = data16 | clip_data << 8;
            fos->write((const char *)&data16, 2);
          }
        }
      }
    }

    auto destWeight = static_cast<WeightVar *>(dest);
    if (destWeight->getMutability() ==
        glow::WeightVar::MutabilityKind::Mutable) {
      addSymbolEntryGenBundle(destWeight, bundle, ctx);
    }

    bundle->append("  elemsub(");

    if (srcWeight0->getMutability() ==
        glow::WeightVar::MutabilityKind::Mutable) {
      auto ste = addSymbolEntry(srcWeight0, ctx);
      bundle->append(ste.name);
      bundle->append(", ");
    } else {
      auto ste = addConstantSymbolEntry(srcWeight0, ctx);
      if (ste.kind == '0') {
        bundle->append("(int8_t *)VTABufferGetVirtAddr(");
        bundle->append(src0->getName());
        bundle->append(")");
        bundle->append(", ");
      } else {
        bundle->append(src0->getName());
        bundle->append(", ");
      }
    }

    bundle->append("1.0/");
    bundle->append(std::to_string(1 / src0->getType()->getScale()));
    bundle->append(", 0, ");

    if (srcWeight1->getMutability() ==
        glow::WeightVar::MutabilityKind::Mutable) {
      auto ste = addSymbolEntry(srcWeight1, ctx);
      bundle->append(ste.name);
      bundle->append(", ");
    } else {
      auto ste = addConstantSymbolEntry(srcWeight1, ctx);
      if (ste.kind == '0') {
        bundle->append("(int8_t *)VTABufferGetVirtAddr(");
        bundle->append(src1->getName());
        bundle->append(")");
        bundle->append(", ");
      } else {
        bundle->append(src1->getName());
        bundle->append(", ");
      }
    }

    bundle->append("1.0/");
    bundle->append(std::to_string(1 / src1->getType()->getScale()));
    bundle->append(", 0, ");

    if (destWeight->getMutability() ==
        glow::WeightVar::MutabilityKind::Mutable) {
      auto ste = addSymbolEntry(destWeight, ctx);
      bundle->append(ste.name);
      bundle->append(", ");
    } else {
      bundle->append(dest->getName());
      bundle->append(", ");
    }

    bundle->append("1.0/");
    bundle->append(std::to_string(1 / dest->getType()->getScale()));
    bundle->append(", 0, ");

    unsigned_t size = 1;
    for (int i = 0; i < destDims.size(); i++) {
      size *= destDims[i];
    }

    bundle->append(std::to_string(size));
    bundle->append(" );\n");
  } else {
    auto src0 = Inst->getLHS();
    auto src1 = Inst->getRHS();
    auto dest = Inst->getDest();
    auto destDims = dest->dims();

    auto srcWeight0 = static_cast<WeightVar *>(src0);
    if (srcWeight0->getMutability() ==
        glow::WeightVar::MutabilityKind::Mutable) {
      addSymbolEntryGenBundle(srcWeight0, bundle, ctx);
    }

    auto srcWeight1 = static_cast<WeightVar *>(src1);
    if (srcWeight1->getMutability() ==
        glow::WeightVar::MutabilityKind::Mutable) {
      addSymbolEntryGenBundle(srcWeight1, bundle, ctx);
    }
    // TODO : yongin apply this code to other saves
    else if (isConstantWeight(src1) && !hasConstantSymbol(src1, ctx)) {
      //#### JO changed (2026-11-09): only a constant, and its bytes once (DeiT: an activation divisor / subtrahend; one
      //#### constant shared by 12 elemdivs was written 12 times, shifting every later constant)
      addConstantSymbolEntry(srcWeight1, ctx);
      {
        auto vMap = ctx->getVariableMap();
        const Tensor *tensor = NULL;
        for (auto it = vMap->begin(); it != vMap->end(); it++) {
          if (it->second == src1) {
            auto storage = it->first;
            if (storage->getKind() == glow::Kinded::Kind::ConstantKind) {
              const auto *constant = llvm::cast<Constant>(storage);
              tensor = &(constant->getPayload());
            }
            break;
          }
        }
        assert(tensor);
        auto handle = tensor->getHandle<float>();
        auto fos = ctx->getWeightFileStream();
        for (size_t i = 0, e = handle.size(); i < e; i++) {
          auto data = handle.raw(i);
          fos->write((const char *)&data, 4);
        }
      }
    }

    auto destWeight = static_cast<WeightVar *>(dest);
    if (destWeight->getMutability() ==
        glow::WeightVar::MutabilityKind::Mutable) {
      addSymbolEntryGenBundle(destWeight, bundle, ctx);
    }

    bundle->append("  elemsub_f32(");

    if (srcWeight0->getMutability() ==
        glow::WeightVar::MutabilityKind::Mutable) {
      auto ste = addSymbolEntry(srcWeight0, ctx);
      bundle->append(ste.name);
      bundle->append(", ");
    } else {
      auto ste = addConstantSymbolEntry(srcWeight0, ctx);
      if (ste.kind == '0') {
        bundle->append("(int8_t *)VTABufferGetVirtAddr(");
        bundle->append(src0->getName());
        bundle->append(")");
        bundle->append(", ");
      } else {
        bundle->append(src0->getName());
        bundle->append(", ");
      }
    }

    bundle->append("1.0/");
    bundle->append(std::to_string(1 / 1));
    bundle->append(", 0, ");

    if (srcWeight1->getMutability() ==
        glow::WeightVar::MutabilityKind::Mutable) {
      auto ste = addSymbolEntry(srcWeight1, ctx);
      bundle->append(ste.name);
      bundle->append(", ");
    } else {
      auto ste = addConstantSymbolEntry(srcWeight1, ctx);
      if (ste.kind == '0') {
        bundle->append("(int8_t *)VTABufferGetVirtAddr(");
        bundle->append(src1->getName());
        bundle->append(")");
        bundle->append(", ");
      } else {
        bundle->append(src1->getName());
        bundle->append(", ");
      }
    }

    bundle->append("1.0/");
    bundle->append(std::to_string(1 / 1));
    bundle->append(", 0, ");

    if (destWeight->getMutability() ==
        glow::WeightVar::MutabilityKind::Mutable) {
      auto ste = addSymbolEntry(destWeight, ctx);
      bundle->append(ste.name);
      bundle->append(", ");
    } else {
      bundle->append(dest->getName());
      bundle->append(", ");
    }

    bundle->append("1.0/");
    bundle->append(std::to_string(1 / 1));
    bundle->append(", 0, ");

    unsigned_t size = 1;
    for (int i = 0; i < destDims.size(); i++) {
      size *= destDims[i];
    }

    bundle->append(std::to_string(size));
    bundle->append(" );\n");
  }
}

void saveElemDivInst(const glow::ElementDivInst *Inst, std::string *bundle,
                     VTASaveContext *ctx) {
  if (Inst->getLHS()->getType()->isQuantizedType()) {
    auto src0 = Inst->getLHS();
    auto src1 = Inst->getRHS();

    auto dest = Inst->getDest();
    auto destDims = dest->dims();

    auto srcWeight0 = static_cast<WeightVar *>(src0);
    if (srcWeight0->getMutability() ==
        glow::WeightVar::MutabilityKind::Mutable) {
      addSymbolEntryGenBundle(srcWeight0, bundle, ctx);
    }

    auto srcWeight1 = static_cast<WeightVar *>(src1);
    if (srcWeight1->getMutability() ==
        glow::WeightVar::MutabilityKind::Mutable) {
      addSymbolEntryGenBundle(srcWeight1, bundle, ctx);
    } else if (isConstantWeight(src1) && !hasConstantSymbol(src1, ctx)) {
      //#### JO changed (2026-11-09): only a constant, and its bytes once (DeiT: an activation divisor / subtrahend; one
      //#### constant shared by 12 elemdivs was written 12 times, shifting every later constant)
      addConstantSymbolEntry(srcWeight1, ctx);
      {
        auto vMap = ctx->getVariableMap();
        const Tensor *tensor = NULL;
        for (auto it = vMap->begin(); it != vMap->end(); it++) {
          if (it->second == src1) {
            auto storage = it->first;
            if (storage->getKind() == glow::Kinded::Kind::ConstantKind) {
              const auto *constant = llvm::cast<Constant>(storage);
              tensor = &(constant->getPayload());
            }
            break;
          }
        }
        assert(tensor);
        auto handle = tensor->getHandle<int8_t>();
        auto fos = ctx->getWeightFileStream();
        int16_t data16 = 0;
        for (size_t i = 0, e = handle.size(); i < e; i++) {
          auto data = handle.raw(i);
          if (data > 127)
            data = 127.0;
          if (data < -128)
            data = -128.0;
          int8_t clip_data = std::floor(data);
          if (i % 2 == 0) {
            data16 = 0xff & clip_data;
          } else {
            data16 = data16 | clip_data << 8;
            fos->write((const char *)&data16, 2);
          }
        }
      }
    }

    auto destWeight = static_cast<WeightVar *>(dest);
    if (destWeight->getMutability() ==
        glow::WeightVar::MutabilityKind::Mutable) {
      addSymbolEntryGenBundle(destWeight, bundle, ctx);
    }

    bundle->append("  elemdiv(");

    if (srcWeight0->getMutability() ==
        glow::WeightVar::MutabilityKind::Mutable) {
      auto ste = addSymbolEntry(srcWeight0, ctx);
      bundle->append(ste.name);
      bundle->append(", ");
    } else {
      auto ste = addConstantSymbolEntry(srcWeight0, ctx);
      if (ste.kind == '0') {
        bundle->append("(int8_t *)VTABufferGetVirtAddr(");
        bundle->append(src0->getName());
        bundle->append(")");
        bundle->append(", ");
      } else {
        bundle->append(src0->getName());
        bundle->append(", ");
      }
    }

    bundle->append("1.0/");
    bundle->append(std::to_string(1 / src0->getType()->getScale()));
    bundle->append(", 0, ");

    if (srcWeight1->getMutability() ==
        glow::WeightVar::MutabilityKind::Mutable) {
      auto ste = addSymbolEntry(srcWeight1, ctx);
      bundle->append(ste.name);
      bundle->append(", ");
    } else {
      auto ste = addConstantSymbolEntry(srcWeight1, ctx);
      if (ste.kind == '0') {
        bundle->append("(int8_t *)VTABufferGetVirtAddr(");
        bundle->append(src1->getName());
        bundle->append(")");
        bundle->append(", ");
      } else {
        bundle->append(src1->getName());
        bundle->append(", ");
      }
    }

    bundle->append("1.0/");
    bundle->append(std::to_string(1 / src1->getType()->getScale()));
    bundle->append(", 0, ");

    if (destWeight->getMutability() ==
        glow::WeightVar::MutabilityKind::Mutable) {
      auto ste = addSymbolEntry(destWeight, ctx);
      bundle->append(ste.name);
      bundle->append(", ");
    } else {
      bundle->append(dest->getName());
      bundle->append(", ");
    }

    bundle->append("1.0/");
    bundle->append(std::to_string(1 / dest->getType()->getScale()));
    bundle->append(", 0, ");

    unsigned_t size = 1;
    for (int i = 0; i < destDims.size(); i++) {
      size *= destDims[i];
    }

    bundle->append(std::to_string(size));
    bundle->append(" );\n");
  } else {
    auto src0 = Inst->getLHS();
    auto src1 = Inst->getRHS();

    auto dest = Inst->getDest();
    auto destDims = dest->dims();

    auto srcWeight0 = static_cast<WeightVar *>(src0);
    if (srcWeight0->getMutability() ==
        glow::WeightVar::MutabilityKind::Mutable) {
      addSymbolEntryGenBundle(srcWeight0, bundle, ctx);
    }

    auto srcWeight1 = static_cast<WeightVar *>(src1);
    if (srcWeight1->getMutability() ==
        glow::WeightVar::MutabilityKind::Mutable) {
      addSymbolEntryGenBundle(srcWeight1, bundle, ctx);
    } else if (isConstantWeight(src1) && !hasConstantSymbol(src1, ctx)) {
      //#### JO changed (2026-11-09): only a constant, and its bytes once (DeiT: an activation divisor / subtrahend; one
      //#### constant shared by 12 elemdivs was written 12 times, shifting every later constant)
      addConstantSymbolEntry(srcWeight1, ctx);
      {
        auto vMap = ctx->getVariableMap();
        const Tensor *tensor = NULL;
        for (auto it = vMap->begin(); it != vMap->end(); it++) {
          if (it->second == src1) {
            auto storage = it->first;
            if (storage->getKind() == glow::Kinded::Kind::ConstantKind) {
              const auto *constant = llvm::cast<Constant>(storage);
              tensor = &(constant->getPayload());
            }
            break;
          }
        }
        assert(tensor);
        auto handle = tensor->getHandle<float>();
        auto fos = ctx->getWeightFileStream();
        for (size_t i = 0, e = handle.size(); i < e; i++) {
          auto data = handle.raw(i);
          fos->write((const char *)&data, 4);
        }
      }
    }

    auto destWeight = static_cast<WeightVar *>(dest);
    if (destWeight->getMutability() ==
        glow::WeightVar::MutabilityKind::Mutable) {
      addSymbolEntryGenBundle(destWeight, bundle, ctx);
    }

    bundle->append("  elemdiv_f32(");

    if (srcWeight0->getMutability() ==
        glow::WeightVar::MutabilityKind::Mutable) {
      auto ste = addSymbolEntry(srcWeight0, ctx);
      bundle->append(ste.name);
      bundle->append(", ");
    } else {
      auto ste = addConstantSymbolEntry(srcWeight0, ctx);
      if (ste.kind == '0') {
        bundle->append("(int8_t *)VTABufferGetVirtAddr(");
        bundle->append(src0->getName());
        bundle->append(")");
        bundle->append(", ");
      } else {
        bundle->append(src0->getName());
        bundle->append(", ");
      }
    }

    bundle->append("1.0/");
    bundle->append(std::to_string(1 / 1));
    bundle->append(", 0, ");

    if (srcWeight1->getMutability() ==
        glow::WeightVar::MutabilityKind::Mutable) {
      auto ste = addSymbolEntry(srcWeight1, ctx);
      bundle->append(ste.name);
      bundle->append(", ");
    } else {
      auto ste = addConstantSymbolEntry(srcWeight1, ctx);
      if (ste.kind == '0') {
        bundle->append("(int8_t *)VTABufferGetVirtAddr(");
        bundle->append(src1->getName());
        bundle->append(")");
        bundle->append(", ");
      } else {
        bundle->append(src1->getName());
        bundle->append(", ");
      }
    }

    bundle->append("1.0/");
    bundle->append(std::to_string(1 / 1));
    bundle->append(", 0, ");

    if (destWeight->getMutability() ==
        glow::WeightVar::MutabilityKind::Mutable) {
      auto ste = addSymbolEntry(destWeight, ctx);
      bundle->append(ste.name);
      bundle->append(", ");
    } else {
      bundle->append(dest->getName());
      bundle->append(", ");
    }

    bundle->append("1.0/");
    bundle->append(std::to_string(1 / 1));
    bundle->append(", 0, ");

    unsigned_t size = 1;
    for (int i = 0; i < destDims.size(); i++) {
      size *= destDims[i];
    }

    bundle->append(std::to_string(size));
    bundle->append(" );\n");
  }
}

void saveElemMaxInst(const glow::ElementMaxInst *Inst, std::string *bundle,
                     VTASaveContext *ctx) {
  auto src0 = Inst->getLHS();
  auto src1 = Inst->getRHS();

  auto dest = Inst->getDest();
  auto destDims = dest->dims();

  auto srcWeight0 = static_cast<WeightVar *>(src0);
  if (srcWeight0->getMutability() == glow::WeightVar::MutabilityKind::Mutable) {
    addSymbolEntryGenBundle(srcWeight0, bundle, ctx);
  }

  auto srcWeight1 = static_cast<WeightVar *>(src1);
  if (srcWeight1->getMutability() == glow::WeightVar::MutabilityKind::Mutable) {
    addSymbolEntryGenBundle(srcWeight1, bundle, ctx);
  }

  auto destWeight = static_cast<WeightVar *>(dest);
  if (destWeight->getMutability() == glow::WeightVar::MutabilityKind::Mutable) {
    addSymbolEntryGenBundle(destWeight, bundle, ctx);
  }

  bundle->append("  elemmax(");

  if (srcWeight0->getMutability() == glow::WeightVar::MutabilityKind::Mutable) {
    auto ste = addSymbolEntry(srcWeight0, ctx);
    bundle->append(ste.name);
    bundle->append(", ");
  } else {
    bundle->append(src0->getName());
    bundle->append(", ");
  }

  bundle->append("1.0/");
  bundle->append(std::to_string(1 / src0->getType()->getScale()));
  bundle->append(", 0, ");

  if (srcWeight1->getMutability() == glow::WeightVar::MutabilityKind::Mutable) {
    auto ste = addSymbolEntry(srcWeight1, ctx);
    bundle->append(ste.name);
    bundle->append(", ");
  } else {
    bundle->append(src1->getName());
    bundle->append(", ");
  }

  bundle->append("1.0/");
  bundle->append(std::to_string(1 / src1->getType()->getScale()));
  bundle->append(", 0, ");

  if (destWeight->getMutability() == glow::WeightVar::MutabilityKind::Mutable) {
    auto ste = addSymbolEntry(destWeight, ctx);
    bundle->append(ste.name);
    bundle->append(", ");
  } else {
    bundle->append(dest->getName());
    bundle->append(", ");
  }

  bundle->append("1.0/");
  bundle->append(std::to_string(1 / dest->getType()->getScale()));
  bundle->append(", 0, ");

  unsigned_t size = 1;
  for (int i = 0; i < destDims.size(); i++) {
    size *= destDims[i];
  }

  bundle->append(std::to_string(size));
  bundle->append(" );\n");
}

void saveDebugPrintInst(const glow::DebugPrintInst *Inst, std::string *bundle,
                        VTASaveContext *ctx) {
  auto *src = Inst->getSrc();
  auto srcDims = src->dims();
  auto elemType = src->getElementType();
  auto srcWeight = static_cast<WeightVar *>(src);

  std::string format = Inst->getFormat();
  std::string filename = Inst->getFileName();
  auto pos = filename.find_last_of("/");
  filename.erase(0, pos);
  filename = "./debug" + filename;
  if (srcWeight->getMutability() == glow::WeightVar::MutabilityKind::Constant) {
    addConstantSymbolEntry(src, ctx);
  }
  if (srcWeight->getMutability() == glow::WeightVar::MutabilityKind::Mutable) {
    addSymbolEntryGenBundle(srcWeight, bundle, ctx);
  }
  bundle->append("  debugprint(");

  if (srcWeight->getMutability() == glow::WeightVar::MutabilityKind::Mutable) {
    auto ste = addSymbolEntry(srcWeight, ctx);
    bundle->append(ste.name);
    bundle->append(", ");
  } else {
    auto ste = addConstantSymbolEntry(srcWeight, ctx);
    if (ste.kind == '0') {
      bundle->append("(int8_t *)VTABufferGetVirtAddr(");
      bundle->append(src->getName());
      bundle->append(")");
      bundle->append(", ");
    } else {
      bundle->append(src->getName());
      bundle->append(", ");
    }
  }

  unsigned_t size = 1;
  for (int i = 0; i < srcDims.size(); i++) {
    size *= srcDims[i];
  }

  bundle->append(std::to_string(size));
  bundle->append(", \"");
  bundle->append(filename);
  bundle->append("\", ");
  if (elemType == ElemKind::Int8QTy) {
    bundle->append("1);\n");
  } else if (elemType == ElemKind::FloatTy) {
    bundle->append("0);\n");
  } else if (elemType == ElemKind::Int32QTy) {
    bundle->append("0);\n");
  } else {
    std::string msg = Inst->getKindName();
    msg.append(" is an unhandled element type.");
    llvm_unreachable(msg.c_str());
  }
}
void saveSoftMaxInst(const glow::SoftMaxInst *Inst, std::string *bundle,
                     VTASaveContext *ctx) {
  auto src = Inst->getSrc();
  auto srcDims = src->dims();

  auto dest = Inst->getDest();
  auto destDims = dest->dims();

  auto srcWeight = static_cast<WeightVar *>(src);
  if (srcWeight->getMutability() == glow::WeightVar::MutabilityKind::Mutable) {
    addSymbolEntryGenBundle(srcWeight, bundle, ctx);
  }

  auto destWeight = static_cast<WeightVar *>(dest);
  if (destWeight->getMutability() == glow::WeightVar::MutabilityKind::Mutable) {
    addSymbolEntryGenBundle(destWeight, bundle, ctx);
  }

  bundle->append("  softmax(");

  if (srcWeight->getMutability() == glow::WeightVar::MutabilityKind::Mutable) {
    auto ste = addSymbolEntry(srcWeight, ctx);
    bundle->append(ste.name);
    bundle->append(", ");

  } else {
    bundle->append(src->getName());
    bundle->append(", ");
  }

  if (destWeight->getMutability() == glow::WeightVar::MutabilityKind::Mutable) {
    auto ste = addSymbolEntry(destWeight, ctx);
    bundle->append(ste.name);
    bundle->append(", ");
  } else {
    bundle->append(dest->getName());
    bundle->append(", ");
  }
  bundle->append(std::to_string(srcDims[0]));
  bundle->append(", ");
  bundle->append(std::to_string(srcDims[1]));
  bundle->append(", ");
  bundle->append(std::to_string(destDims[0]));
  bundle->append(", ");
  bundle->append(std::to_string(destDims[1]));
  bundle->append(" );\n");
}

void saveReluInst(const glow::ReluInst *Inst, std::string *bundle,
                  VTASaveContext *ctx) {
  auto src = Inst->getSrc();
  auto srcDims = src->dims();
  float inScale = src->getType()->getScale();

  auto dest = Inst->getDest();
  float outScale = Inst->getDest()->getType()->getScale();

  auto srcWeight = static_cast<WeightVar *>(src);
  if (srcWeight->getMutability() == glow::WeightVar::MutabilityKind::Mutable) {
    addSymbolEntryGenBundle(srcWeight, bundle, ctx);
  }

  auto destWeight = static_cast<WeightVar *>(dest);
  if (destWeight->getMutability() == glow::WeightVar::MutabilityKind::Mutable) {
    addSymbolEntryGenBundle(destWeight, bundle, ctx);
  }

  bundle->append("  relu(");

  if (srcWeight->getMutability() == glow::WeightVar::MutabilityKind::Mutable) {
    auto ste = addSymbolEntry(srcWeight, ctx);
    bundle->append(ste.name);
    bundle->append(", ");

  } else {
    bundle->append(src->getName());
    bundle->append(", ");
  }

  if (destWeight->getMutability() == glow::WeightVar::MutabilityKind::Mutable) {
    auto ste = addSymbolEntry(destWeight, ctx);
    bundle->append(ste.name);
    bundle->append(", ");
  } else {
    bundle->append(dest->getName());
    bundle->append(", ");
  }

  int src_size = 1;

  for (int i = 0; i < srcDims.size(); i++) {
    src_size *= srcDims[i];
  }
  bundle->append(std::to_string(src_size));
  if (inScale != outScale) {
    bundle->append(", ");
    bundle->append(std::to_string(inScale));
    bundle->append(", ");
    bundle->append(std::to_string(outScale));
  }

  bundle->append(" );\n");
}

//#### JO added (2026-11-09): generic CPU ops for the instructions with no vtalib function (DeiT-tiny: matmul, the layer norm
//#### and GELU pieces left in float, the concat / slice / head copies, 3-D and float transposes, softmax over rows). The
//#### functions are written into the bundle .cpp (NESTC_CPU_OPS, only when one is used), so any vtalib builds it.
//#### Row-major tensors of rank <= 6; int8_t * arguments as the rest of the bundle, cast in the functions.
static const char *const NESTC_CPU_OPS = R"~(
//#### NEST-C VTASave generic CPU ops (2026-11-09): instructions with no vtalib function, row-major tensors of rank <= 6
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <algorithm>
#if defined(__aarch64__)
#include <arm_neon.h>
#endif
namespace nestc_cpu {
static inline void strides(const size_t *d, int r, size_t *s) {
  s[r - 1] = 1;
  for (int i = r - 2; i >= 0; i--) s[i] = s[i + 1] * d[i + 1];
}
// copies the box `box` of src (shape sd) at so to dst (shape dd) at dof, es bytes per element
static void copyBox(int8_t *dst, const size_t *dd, const size_t *dof, const int8_t *src, const size_t *sd,
                    const size_t *so, const size_t *box, int r, size_t es) {
  size_t ss[6], ds[6], idx[6] = {0};
  strides(sd, r, ss);
  strides(dd, r, ds);
  size_t rows = 1;
  for (int i = 0; i < r - 1; i++) rows *= box[i];
  const size_t run = box[r - 1] * es;
  for (size_t t = 0; t < rows; t++) {
    size_t s = so[r - 1], d = dof[r - 1];
    for (int i = 0; i < r - 1; i++) {
      s += (so[i] + idx[i]) * ss[i];
      d += (dof[i] + idx[i]) * ds[i];
    }
    memmove(dst + d * es, src + s * es, run);
    for (int i = r - 2; i >= 0; i--) {
      if (++idx[i] < box[i]) break;
      idx[i] = 0;
    }
  }
}
// Glow InsertTensor: src at off in dst, count copies along axis
static void insert(int8_t *dst, const size_t *dd, const int8_t *src, const size_t *sd, const size_t *off,
                   size_t count, int axis, int r, size_t es) {
  size_t o[6], z[6] = {0};
  for (int i = 0; i < r; i++) o[i] = off[i];
  for (size_t c = 0; c < count; c++, o[axis] += sd[axis]) copyBox(dst, dd, o, src, sd, z, sd, r, es);
}
// Glow ExtractTensor: the dst-shaped box of src at off
static void extract(int8_t *dst, const size_t *dd, const int8_t *src, const size_t *sd, const size_t *off, int r,
                    size_t es) {
  size_t z[6] = {0};
  copyBox(dst, dd, z, src, sd, off, dd, r, es);
}
// Glow Transpose: dst dim i is src dim sh[i]
static void transpose(int8_t *dst, const int8_t *src, const size_t *sd, const unsigned *sh, int r, size_t es) {
  size_t ss[6], od[6], os[6], idx[6] = {0};
  strides(sd, r, ss);
  for (int i = 0; i < r; i++) {
    od[i] = sd[sh[i]];
    os[i] = ss[sh[i]];
  }
  size_t rows = 1;
  for (int i = 0; i < r - 1; i++) rows *= od[i];
  const size_t n = od[r - 1], st = os[r - 1];
  for (size_t t = 0; t < rows; t++) {
    size_t s = 0;
    for (int i = 0; i < r - 1; i++) s += idx[i] * os[i];
    int8_t *d = dst + t * n * es;
    if (es == 4) {
      const uint32_t *p = (const uint32_t *)src + s;
      uint32_t *q = (uint32_t *)d;
      for (size_t j = 0; j < n; j++) q[j] = p[j * st];
    } else if (es == 1) {
      const int8_t *p = src + s;
      for (size_t j = 0; j < n; j++) d[j] = p[j * st];
    } else {
      for (size_t j = 0; j < n; j++) memcpy(d + j * es, src + (s + j * st) * es, es);
    }
    for (int i = r - 2; i >= 0; i--) {
      if (++idx[i] < od[i]) break;
      idx[i] = 0;
    }
  }
}
// C[M][N] = A[M][K] B[K][N], float
static void matmul(int8_t *c8, const int8_t *a8, const int8_t *b8, size_t M, size_t K, size_t N) {
  float *C = (float *)c8;
  const float *A = (const float *)a8, *B = (const float *)b8;
#pragma omp parallel for schedule(static)
  for (long m = 0; m < (long)M; m++) {
    float *c = C + m * N;
    const float *a = A + m * K;
    for (size_t n = 0; n < N; n++) c[n] = 0.f;
    for (size_t k = 0; k < K; k++) {
      const float av = a[k];
      const float *b = B + k * N;
      for (size_t n = 0; n < N; n++) c[n] += av * b[n];
    }
  }
}
// int8 MatMul (-vta-matmul-conv, the products with an activation on the right): C = clip(round(A B * scale)),
// scale = lhs scale * rhs scale / dest scale (offsets 0), the Glow Interpreter's rounding; NEON: 16 columns per step,
// int8 x int8 -> int16 products widened into int32 sums (exact, so the same result as the scalar loop)
static inline int8_t satRound(int32_t acc, float scale) {
  const float v = std::round((float)acc * scale);
  return (int8_t)(v > 127.f ? 127.f : v < -128.f ? -128.f : v);
}
static void matmulI8(int8_t *C, const int8_t *A, const int8_t *B, size_t M, size_t K, size_t N, float scale) {
  // rows in pairs: each 16 columns of B loaded once for two rows of A (8 independent sums)
#pragma omp parallel for schedule(static)
  for (long p = 0; p < (long)(M + 1) / 2; p++) {
    const size_t m = 2 * p;
    const bool two = m + 1 < M;
    const int8_t *a0 = A + m * K, *a1 = two ? a0 + K : a0;
    int8_t *c0 = C + m * N, *c1 = c0 + N;
    size_t n = 0;
#if defined(__aarch64__)
    for (; n + 16 <= N; n += 16) {
      int32x4_t s0 = vdupq_n_s32(0), s1 = s0, s2 = s0, s3 = s0, t0 = s0, t1 = s0, t2 = s0, t3 = s0;
      const int8_t *b = B + n;
      for (size_t k = 0; k < K; k++, b += N) {
        const int8x16_t vb = vld1q_s8(b);
        const int8x8_t bl = vget_low_s8(vb), bh = vget_high_s8(vb);
        const int8x8_t x0 = vdup_n_s8(a0[k]), x1 = vdup_n_s8(a1[k]);
        const int16x8_t l0 = vmull_s8(bl, x0), h0 = vmull_s8(bh, x0), l1 = vmull_s8(bl, x1), h1 = vmull_s8(bh, x1);
        s0 = vaddw_s16(s0, vget_low_s16(l0));
        s1 = vaddw_high_s16(s1, l0);
        s2 = vaddw_s16(s2, vget_low_s16(h0));
        s3 = vaddw_high_s16(s3, h0);
        t0 = vaddw_s16(t0, vget_low_s16(l1));
        t1 = vaddw_high_s16(t1, l1);
        t2 = vaddw_s16(t2, vget_low_s16(h1));
        t3 = vaddw_high_s16(t3, h1);
      }
      int32_t acc[32];
      vst1q_s32(acc, s0);
      vst1q_s32(acc + 4, s1);
      vst1q_s32(acc + 8, s2);
      vst1q_s32(acc + 12, s3);
      vst1q_s32(acc + 16, t0);
      vst1q_s32(acc + 20, t1);
      vst1q_s32(acc + 24, t2);
      vst1q_s32(acc + 28, t3);
      for (int j = 0; j < 16; j++) c0[n + j] = satRound(acc[j], scale);
      if (two)
        for (int j = 0; j < 16; j++) c1[n + j] = satRound(acc[16 + j], scale);
    }
#endif
    for (; n < N; n++) {
      int32_t x = 0, y = 0;
      for (size_t k = 0; k < K; k++) {
        x += (int32_t)a0[k] * B[k * N + n];
        y += (int32_t)a1[k] * B[k * N + n];
      }
      c0[n] = satRound(x, scale);
      if (two)
        c1[n] = satRound(y, scale);
    }
  }
}
// Glow BatchedReduceAdd over the middle dimension of [outer][len][inner], float
static void reduceAdd(int8_t *o8, const int8_t *i8, size_t outer, size_t len, size_t inner) {
  float *out = (float *)o8;
  const float *in = (const float *)i8;
  size_t o0 = 0;
  // inner 1 (rows, the layer norm means): four rows at a time, each summed in order (independent chains)
  if (inner == 1)
    for (; o0 + 4 <= outer; o0 += 4) {
      const float *a = in + o0 * len, *b = a + len, *c = b + len, *d = c + len;
      float sa = 0.f, sb = 0.f, sc = 0.f, sd = 0.f;
      for (size_t l = 0; l < len; l++) {
        sa += a[l];
        sb += b[l];
        sc += c[l];
        sd += d[l];
      }
      out[o0] = sa;
      out[o0 + 1] = sb;
      out[o0 + 2] = sc;
      out[o0 + 3] = sd;
    }
  for (size_t o = o0; o < outer; o++)
    for (size_t i = 0; i < inner; i++) {
      float s = 0.f;
      for (size_t l = 0; l < len; l++) s += in[(o * len + l) * inner + i];
      out[o * inner + i] = s;
    }
}
// softmax of each row of [rows][cols], float
static void softmax(int8_t *o8, const int8_t *i8, size_t rows, size_t cols) {
  float *out = (float *)o8;
  const float *in = (const float *)i8;
  for (size_t r = 0; r < rows; r++) {
    const float *x = in + r * cols;
    float *y = out + r * cols, m = x[0], sum = 0.f;
    for (size_t i = 1; i < cols; i++) m = x[i] > m ? x[i] : m;
    for (size_t i = 0; i < cols; i++) sum += (y[i] = std::exp(x[i] - m));
    for (size_t i = 0; i < cols; i++) y[i] /= sum;
  }
}
// the q of a value x = q * s (int8 q), -1000 if x is not one (callers then compute it directly)
static inline int quantIndex(float x, float s, float inv) {
  const float t = x * inv;
  const int q = (int)(t + (t >= 0.f ? .5f : -.5f));
  return q >= -128 && q < 128 && (float)q * s == x ? q : -1000;
}
// softmax of values dequantized from int8 at scale s (DeiT attention): x - max = -d * s exactly (d = 0..255), so exp
// comes from a table of the 256 std::exp values; the sums and divisions as softmax, the same result
static void softmaxQ(int8_t *o8, const int8_t *i8, size_t rows, size_t cols, float s) {
  float *out = (float *)o8;
  const float *in = (const float *)i8;
  const float inv = 1.f / s;
  float T[256];
  for (int d = 0; d < 256; d++) T[d] = std::exp(-(float)d * s);
  for (size_t r = 0; r < rows; r++) {
    const float *x = in + r * cols;
    float *y = out + r * cols, m = x[0], sum = 0.f;
    for (size_t i = 1; i < cols; i++) m = x[i] > m ? x[i] : m;
    const int qm = quantIndex(m, s, inv);
    for (size_t i = 0; i < cols; i++) {
      const int q = qm == -1000 ? -1000 : quantIndex(x[i], s, inv);
      sum += (y[i] = q != -1000 ? T[qm - q] : std::exp(x[i] - m));
    }
    for (size_t i = 0; i < cols; i++) y[i] /= sum;
  }
}
// softmaxQ of the dequantized int8 input read directly (VTASave findSoftmaxI8: the dequantize and the softmax in one):
// the same table and sums in the same order (four rows at a time, interleaved); approx 0: the same divisions, so the
// same result as softmaxQ; approx 1 (-vta-cpu-approx): times 1 / sum instead (values may differ in the last bit)
//#### JO added (2026-11-10): the integer softmax planned for the EVTA ALU (docs/evta_isa_ext.md 8.2, 8.6): RMAX, SUB, a LUT for
//#### exp (NESTC_SM_INT=1: 8 bits, 255 * exp(-k * s); 2: 16 bits, 65535 * exp(-k * s); k = max - x clipped to 255), RSUM, the
//#### reciprocal by CLZ + an 8-bit LUT, MUL and a rounding shift to a probability with 16 fraction bits (the next quantize
//#### takes it to its scale; accuracy experiments, the output stays float).
static void softmaxInt(float *out, const int8_t *q8, size_t rows, size_t cols, float s, int bits) {
  uint16_t E[256];
  uint8_t RT[256];
  const double emax = bits > 8 ? 65535.0 : 255.0;
  for (int k = 0; k < 256; k++) {
    E[k] = (uint16_t)std::lround(emax * std::exp(-(double)k * s));
    RT[k] = (uint8_t)(k < 128 ? 0 : std::lround(32768.0 / (k + 0.5)));
  }
#pragma omp parallel for schedule(static)
  for (long r = 0; r < (long)rows; r++) {
    const int8_t *x = q8 + r * cols;
    int m = -128;
    for (size_t i = 0; i < cols; i++) m = x[i] > m ? x[i] : m;
    int64_t S = 0;
    for (size_t i = 0; i < cols; i++) S += E[std::min(m - x[i], 255)];
    // S = Sn * 2^sh with Sn in [2^15, 2^16); 1 / Sn ~ RT[Sn >> 8] / 2^23
    const int bl = 64 - __builtin_clzll((uint64_t)S), sh = bl - 16;
    const uint64_t Sn = sh >= 0 ? (uint64_t)S >> sh : (uint64_t)S << -sh;
    const int64_t R = RT[Sn >> 8];
    // p * 2^16 = e * R / 2^(23 - 16 + sh)
    const int t = 7 + sh;
    for (size_t i = 0; i < cols; i++) {
      const int64_t v = (int64_t)E[std::min(m - x[i], 255)] * R;
      const int64_t p = t > 0 ? (v + ((int64_t)1 << (t - 1))) >> t : v << -t;
      out[r * cols + i] = (float)p * (1.f / 65536);
    }
  }
}
static void softmaxI8(int8_t *o8, const int8_t *q8, size_t rows, size_t cols, float s, int approx) {
  float *out = (float *)o8;
  static const int smInt = getenv("NESTC_SM_INT") ? atoi(getenv("NESTC_SM_INT")) : 0;
  if (smInt > 0) {
    softmaxInt(out, q8, rows, cols, s, smInt > 1 ? 16 : 8);
    return;
  }
  float T[256];
  for (int d = 0; d < 256; d++) T[d] = std::exp(-(float)d * s);
  const long groups = (long)((rows + 3) / 4);
#pragma omp parallel for schedule(static)
  for (long g = 0; g < groups; g++) {
    const size_t r0 = g * 4, nr = std::min<size_t>(4, rows - r0);
    int m[4] = {-128, -128, -128, -128};
    for (size_t k = 0; k < nr; k++) {
      const int8_t *x = q8 + (r0 + k) * cols;
      size_t i = 0;
      int mm = -128;
#if defined(__aarch64__)
      int8x16_t vm = vdupq_n_s8(-128);
      for (; i + 16 <= cols; i += 16) vm = vmaxq_s8(vm, vld1q_s8(x + i));
      mm = vmaxvq_s8(vm);
#endif
      for (; i < cols; i++) mm = x[i] > mm ? x[i] : mm;
      m[k] = mm;
    }
    float sum[4] = {0.f, 0.f, 0.f, 0.f};
    if (nr == 4) {
      const int8_t *xa = q8 + r0 * cols, *xb = xa + cols, *xc = xb + cols, *xd = xc + cols;
      float *ya = out + r0 * cols, *yb = ya + cols, *yc = yb + cols, *yd = yc + cols;
      const float *Ta = T + m[0], *Tb = T + m[1], *Tc = T + m[2], *Td = T + m[3];
      float sa = 0.f, sb = 0.f, sc = 0.f, sd = 0.f;
      for (size_t i = 0; i < cols; i++) {
        const float ea = Ta[-xa[i]], eb = Tb[-xb[i]], ec = Tc[-xc[i]], ed = Td[-xd[i]];
        ya[i] = ea;
        yb[i] = eb;
        yc[i] = ec;
        yd[i] = ed;
        sa += ea;
        sb += eb;
        sc += ec;
        sd += ed;
      }
      sum[0] = sa;
      sum[1] = sb;
      sum[2] = sc;
      sum[3] = sd;
    } else {
      for (size_t k = 0; k < nr; k++) {
        const int8_t *x = q8 + (r0 + k) * cols;
        float *y = out + (r0 + k) * cols, ss = 0.f;
        for (size_t i = 0; i < cols; i++) ss += (y[i] = T[m[k] - x[i]]);
        sum[k] = ss;
      }
    }
    for (size_t k = 0; k < nr; k++) {
      float *y = out + (r0 + k) * cols;
      size_t i = 0;
      if (approx) {
        const float inv = 1.f / sum[k];
#if defined(__aarch64__)
        const float32x4_t vi = vdupq_n_f32(inv);
        for (; i + 4 <= cols; i += 4) vst1q_f32(y + i, vmulq_f32(vld1q_f32(y + i), vi));
#endif
        for (; i < cols; i++) y[i] *= inv;
      } else {
#if defined(__aarch64__)
        const float32x4_t vs = vdupq_n_f32(sum[k]);
        for (; i + 4 <= cols; i += 4) vst1q_f32(y + i, vdivq_f32(vld1q_f32(y + i), vs));
#endif
        for (; i < cols; i++) y[i] /= sum[k];
      }
    }
  }
}
static void mul(int8_t *o8, const int8_t *a8, const int8_t *b8, size_t n) {
  float *o = (float *)o8;
  const float *a = (const float *)a8, *b = (const float *)b8;
  for (size_t i = 0; i < n; i++) o[i] = a[i] * b[i];
}
// exponents 2 and 0.5 (layer norm) as a * a and sqrt (both correctly rounded, as std::pow)
static void pow(int8_t *o8, const int8_t *a8, const int8_t *b8, size_t n) {
  float *o = (float *)o8;
  const float *a = (const float *)a8, *b = (const float *)b8;
  for (size_t i = 0; i < n; i++) {
    const float e = b[i];
    o[i] = e == 2.f ? a[i] * a[i] : e == 0.5f && a[i] >= 0.f ? std::sqrt(a[i]) : std::pow(a[i], e);
  }
}
// a * a (pow with the constant exponent 2: the layer norm variance)
static void square(int8_t *o8, const int8_t *a8, size_t n) {
  float *o = (float *)o8;
  const float *a = (const float *)a8;
  size_t i = 0;
#if defined(__aarch64__)
  for (; i + 4 <= n; i += 4) {
    const float32x4_t x = vld1q_f32(a + i);
    vst1q_f32(o + i, vmulq_f32(x, x));
  }
#endif
  for (; i < n; i++) o[i] = a[i] * a[i];
}
static void erf(int8_t *o8, const int8_t *a8, size_t n) {
  float *o = (float *)o8;
  const float *a = (const float *)a8;
  for (size_t i = 0; i < n; i++) o[i] = std::erf(a[i]);
}
// erf of values dequantized from int8 at scale s (GELU): a table of the 256 std::erf values, the same result; a value
// that is not q * s falls back to std::erf
static void erfQ(int8_t *o8, const int8_t *a8, size_t n, float s) {
  float *o = (float *)o8;
  const float *a = (const float *)a8;
  float T[256];
  for (int q = -128; q < 128; q++) T[q + 128] = std::erf((float)q * s);
  const float inv = 1.f / s;
  for (size_t i = 0; i < n; i++) {
    const int q = quantIndex(a[i], s, inv);
    o[i] = q != -1000 ? T[q + 128] : std::erf(a[i]);
  }
}
// o[i] = t[a[i] + 128] (an elementwise region as a table, VTASave findLutRegions); NEON: 4 x 64-byte table lookups
struct Lut8 {
  int8_t v[256];
};
static void lut8(int8_t *o, const int8_t *a, const Lut8 &lut, size_t n) {
  const int8_t *t = lut.v;
  size_t i = 0;
#if defined(__aarch64__)
  const uint8_t *tu = (const uint8_t *)t;
  uint8x16x4_t T[4];
  for (int k = 0; k < 4; k++)
    for (int j = 0; j < 4; j++)
      T[k].val[j] = vld1q_u8(tu + 64 * k + 16 * j);
  const uint8x16_t x80 = vdupq_n_u8(0x80), c64 = vdupq_n_u8(64);
  for (; i + 16 <= n; i += 16) {
    uint8x16_t idx = veorq_u8(vld1q_u8((const uint8_t *)a + i), x80);
    uint8x16_t r = vqtbl4q_u8(T[0], idx);
    idx = vsubq_u8(idx, c64);
    r = vqtbx4q_u8(r, T[1], idx);
    idx = vsubq_u8(idx, c64);
    r = vqtbx4q_u8(r, T[2], idx);
    idx = vsubq_u8(idx, c64);
    r = vqtbx4q_u8(r, T[3], idx);
    vst1q_u8((uint8_t *)o + i, r);
  }
#endif
  for (; i < n; i++)
    o[i] = t[(uint8_t)a[i] ^ 0x80];
}
// the DeiT layer norm as VTASave findLayerNorms matched it (21 instructions), with the same arithmetic as the vtalib /
// nestc_cpu functions it replaces (dequantize, reduceAdd, quantize, elemdiv, elemsub, square, elemadd, pow, mul), so the
// same result. x: [rows][cols] int8; p1 / p2: the int8 divisors of the two means (per row), eps: int8 per row, e: the
// float exponent per row (0.5), w: float weight [rows][cols], b: int8 bias [rows][cols]. Four rows at a time (the row
// sums are chains in order, interleaved); quantize by x * (1 / scale) when that is exact, as vtalib quantize.
static inline int8_t lnClip(float v) {
  const int32_t q = (int32_t)std::round(v);
  return (int8_t)(q > 127 ? 127 : q < -128 ? -128 : q);
}
static inline bool lnPow2Inv(float s) {
  int e;
  const float inv = 1.f / s;
  return std::frexp(inv, &e) == 0.5f && inv * s == 1.f;
}
static inline float lnQuant(float x, float s) { return lnPow2Inv(s) ? x * (1.f / s) : x / s; }
static inline int8_t lnDiv(int8_t l, int8_t r, float s0, float s1, float sd) {
  const float c1 = s1 * sd;
  const float num = s0 * float(l), den = c1 * float(r);
  return lnClip(num / den);
}
static inline int8_t lnAdd(int8_t l, int8_t r, float s0, float s1, float sd) {
  const float large = float(1) / (1 << 15);
  const float k0 = s0 / large, k1 = s1 / large, kd = large / sd;
  const int32_t L32 = (int32_t)std::round(float(l) * k0), R32 = (int32_t)std::round(float(r) * k1);
  return lnClip(float(L32 + R32) * kd);
}
// sums over each of four int8 rows of float(v) * s (squared when sq), each in column order: NEON lane k = row k
static inline void lnSum4(const int8_t *ra, const int8_t *rb, const int8_t *rc, const int8_t *rd, size_t cols, float s,
                          bool sq, float out[4]) {
  size_t l = 0;
  float acc[4] = {0.f, 0.f, 0.f, 0.f};
#if defined(__aarch64__)
  float32x4_t S = vdupq_n_f32(0.f);
  const float32x4_t vs = vdupq_n_f32(s);
  for (; l + 16 <= cols; l += 16) {
    const int8x16_t a = vld1q_s8(ra + l), b = vld1q_s8(rb + l), c = vld1q_s8(rc + l), d = vld1q_s8(rd + l);
    const int8x16_t ab0 = vzip1q_s8(a, b), ab1 = vzip2q_s8(a, b), cd0 = vzip1q_s8(c, d), cd1 = vzip2q_s8(c, d);
    // column l + i of the four rows = bytes 4i..4i+3 of q[i / 4]
    const int8x16_t q[4] = {
        vreinterpretq_s8_s16(vzip1q_s16(vreinterpretq_s16_s8(ab0), vreinterpretq_s16_s8(cd0))),
        vreinterpretq_s8_s16(vzip2q_s16(vreinterpretq_s16_s8(ab0), vreinterpretq_s16_s8(cd0))),
        vreinterpretq_s8_s16(vzip1q_s16(vreinterpretq_s16_s8(ab1), vreinterpretq_s16_s8(cd1))),
        vreinterpretq_s8_s16(vzip2q_s16(vreinterpretq_s16_s8(ab1), vreinterpretq_s16_s8(cd1)))};
    for (int g = 0; g < 4; g++) {
      const int16x8_t lo = vmovl_s8(vget_low_s8(q[g])), hi = vmovl_s8(vget_high_s8(q[g]));
      const int32x4_t col[4] = {vmovl_s16(vget_low_s16(lo)), vmovl_s16(vget_high_s16(lo)), vmovl_s16(vget_low_s16(hi)),
                                vmovl_s16(vget_high_s16(hi))};
      for (int i = 0; i < 4; i++) {
        float32x4_t f = vmulq_f32(vcvtq_f32_s32(col[i]), vs);
        if (sq)
          f = vmulq_f32(f, f);
        S = vaddq_f32(S, f);
      }
    }
  }
  vst1q_f32(acc, S);
#endif
  const int8_t *rows[4] = {ra, rb, rc, rd};
  for (int k = 0; k < 4; k++) {
    float a = acc[k];
    for (size_t j = l; j < cols; j++) {
      const float f = float(rows[k][j]) * s;
      const float f2 = sq ? f * f : f;
      a += f2;
    }
    out[k] = a;
  }
}
static void layerNormI8(int8_t *out, const int8_t *x, size_t rows, size_t cols, const int8_t *p1, const int8_t *p2,
                        const int8_t *eps, const int8_t *e8, const int8_t *w8, const int8_t *b, float sD0, float sQ1,
                        float v1a, float v1b, float v1d, float e1a, float e1b, float e1d, float sD2, float sQ2, float v2a,
                        float v2b, float v2d, float a1a, float a1b, float a1d, float sD3, float sQ3, float v3a, float v3b,
                        float v3d, float sD4, float sQ4, float a2a, float a2b, float a2d, int approx) {
  const float *e = (const float *)e8, *w = (const float *)w8;
  //#### JO added (2026-11-10): NESTC_LN_FLOAT=1 computes the layer norm in float from the dequantized int8 input
  //#### (mean, variance, 1 / sqrt, * w + b) and quantizes only the output: the loss of the int8 chain in between
  //#### (mean / variance / sd quantized to int8) apart from the int8 input and output (accuracy experiments only)
  //#### JO added (2026-11-10): NESTC_LN_INT=1: the integer layer norm planned for the EVTA ALU (docs/evta_isa_ext.md 8.2, 8.6),
  //#### exact up to 1 / sqrt: (x - mean) / sd = (C x - S) / sqrt(W) with S = sum x, W = C sum x^2 - S^2 (all within 32 bits
  //#### for C <= 1024: ADD / RSUM, MUL), 1 / sqrt(W) by an even normalizing shift (CLZ) and an 8-bit LUT, then
  //#### ((C x - S) * T >> 8) * G[l] with G[l] < 2^14 the per-channel gamma / out scale, a rounding shift and the int bias.
  //#### The constants come from the float parameters at each call (compile-time constants on the EVTA).
  static const bool lnInt = getenv("NESTC_LN_INT") && atoi(getenv("NESTC_LN_INT")) > 0;
  if (lnInt) {
    uint8_t T[256];
    for (int k = 0; k < 256; k++)
      T[k] = (uint8_t)(k < 64 ? 0 : std::min(255L, std::lround(255.0 / std::sqrt((k + 0.5) / 64.0))));
    const int32_t C = (int32_t)cols;
    // 1 / sqrt(W) = T / (255 * 2^7 * 2^(sh / 2)); y = (C x - S) / sqrt(W) * gamma / a2d
    const double gk = 1.0 / ((double)a2d * 255.0 * 128.0);
#pragma omp parallel for schedule(static)
    for (long r = 0; r < (long)rows; r++) {
      const int8_t *xr = x + r * cols;
      const float *wr = w + r * cols;
      double gmax = 0;
      for (size_t l = 0; l < cols; l++) gmax = std::max(gmax, std::fabs((double)wr[l]));
      int gs = 0;
      while (gmax * gk * std::ldexp(1.0, gs + 1) < 16383.0 && gs < 40) gs++;
      int32_t S = 0, Q = 0;
      for (size_t l = 0; l < cols; l++) {
        S += xr[l];
        Q += xr[l] * xr[l];
      }
      int32_t W = C * Q - S * S;
      if (W < 1) W = 1;
      // W = Wn * 2^sh, Wn in [2^14, 2^16), sh even
      int sh = (32 - __builtin_clz((uint32_t)W)) - 16;
      if (sh & 1) sh++;
      const uint32_t Wn = sh >= 0 ? (uint32_t)W >> sh : (uint32_t)W << -sh;
      const int32_t Tv = T[Wn >> 8];
      const int t = gs + sh / 2 - 8;
      for (size_t l = 0; l < cols; l++) {
        const int32_t G = (int32_t)std::lround(wr[l] * gk * std::ldexp(1.0, gs));
        const int32_t p = ((C * xr[l] - S) * Tv) >> 8;
        const int32_t v = p * G;
        const int32_t y = t > 0 ? (v + (1 << (t - 1))) >> t : v << -t;
        const int32_t bq = (int32_t)std::lround(b[r * cols + l] * (double)a2b / a2d);
        const int32_t o = y + bq;
        out[r * cols + l] = (int8_t)(o > 127 ? 127 : o < -128 ? -128 : o);
      }
    }
    return;
  }
  static const bool lnFloat = getenv("NESTC_LN_FLOAT") && atoi(getenv("NESTC_LN_FLOAT")) > 0;
  if (lnFloat) {
#pragma omp parallel for schedule(static)
    for (long r = 0; r < (long)rows; r++) {
      const int8_t *xr = x + r * cols;
      double m = 0, v = 0;
      for (size_t l = 0; l < cols; l++)
        m += xr[l] * (double)sD0;
      m /= cols;
      for (size_t l = 0; l < cols; l++) {
        const double d = xr[l] * (double)sD0 - m;
        v += d * d;
      }
      v /= cols;
      const double ep = std::max((double)eps[r] * a1b, 1e-12), rs = 1.0 / std::sqrt(v + ep);
      for (size_t l = 0; l < cols; l++) {
        const double y = (xr[l] * (double)sD0 - m) * rs * w[r * cols + l] + b[r * cols + l] * (double)a2b;
        out[r * cols + l] = lnClip((float)(y / a2d));
      }
    }
    return;
  }
  const float large = float(1) / (1 << 15);
  const float ek0 = e1a / e1d, ek1 = e1b / e1d;
  const float c3 = v3b * v3d;
  const float ak0 = a2a / large, ak1 = a2b / large, akd = large / a2d;
  const bool q4mul = lnPow2Inv(sQ4);
  const float q4k = q4mul ? 1.f / sQ4 : sQ4;
  const long groups = (long)((rows + 3) / 4);
#pragma omp parallel for schedule(static)
  for (long g = 0; g < groups; g++) {
    const size_t r0 = g * 4, nr = std::min<size_t>(4, rows - r0);
    int8_t d[4][1024];
    // means: sums of the dequantized rows in order (four chains), quantize, elemdiv
    float s1[4] = {0.f, 0.f, 0.f, 0.f};
    if (nr == 4) {
      lnSum4(x + r0 * cols, x + (r0 + 1) * cols, x + (r0 + 2) * cols, x + (r0 + 3) * cols, cols, sD0, false, s1);
    } else {
      for (size_t k = 0; k < nr; k++) {
        float s = 0.f;
        for (size_t l = 0; l < cols; l++) {
          const float f = float(x[(r0 + k) * cols + l]) * sD0;
          s += f;
        }
        s1[k] = s;
      }
    }
    for (size_t k = 0; k < nr; k++) {
      const size_t r = r0 + k;
      const int8_t *xr = x + r * cols;
      const int8_t m = lnDiv(lnClip(lnQuant(s1[k], sQ1)), p1[r], v1a, v1b, v1d);
      // d = elemsub(x, m)
      size_t l = 0;
#if defined(__aarch64__)
      const float32x4_t k0 = vdupq_n_f32(ek0), rm = vdupq_n_f32(ek1 * float(m));
      for (; l + 16 <= cols; l += 16) {
        const int8x16_t v = vld1q_s8(xr + l);
        const int16x8_t lo = vmovl_s8(vget_low_s8(v)), hi = vmovl_s8(vget_high_s8(v));
        const int32x4_t L[4] = {vmovl_s16(vget_low_s16(lo)), vmovl_s16(vget_high_s16(lo)),
                                vmovl_s16(vget_low_s16(hi)), vmovl_s16(vget_high_s16(hi))};
        int32x4_t S[4];
        for (int j = 0; j < 4; j++)
          S[j] = vcvtaq_s32_f32(vsubq_f32(vmulq_f32(k0, vcvtq_f32_s32(L[j])), rm));
        const int16x8_t n0 = vcombine_s16(vqmovn_s32(S[0]), vqmovn_s32(S[1]));
        const int16x8_t n1 = vcombine_s16(vqmovn_s32(S[2]), vqmovn_s32(S[3]));
        vst1q_s8(d[k] + l, vcombine_s8(vqmovn_s16(n0), vqmovn_s16(n1)));
      }
#endif
      for (; l < cols; l++) {
        const float a = ek0 * float(xr[l]);
        const float bb = ek1 * float(m);
        d[k][l] = lnClip(a - bb);
      }
    }
    // variances: sums of the squares of the dequantized d in order (four chains)
    float s2[4] = {0.f, 0.f, 0.f, 0.f};
    if (nr == 4) {
      lnSum4(d[0], d[1], d[2], d[3], cols, sD2, true, s2);
    } else {
      for (size_t k = 0; k < nr; k++) {
        float s = 0.f;
        for (size_t l = 0; l < cols; l++) {
          const float f = float(d[k][l]) * sD2;
          const float f2 = f * f;
          s += f2;
        }
        s2[k] = s;
      }
    }
    for (size_t k = 0; k < nr; k++) {
      const size_t r = r0 + k;
      const int8_t v = lnAdd(lnDiv(lnClip(lnQuant(s2[k], sQ2)), p2[r], v2a, v2b, v2d), eps[r], a1a, a1b, a1d);
      const float f3 = float(v) * sD3, ex = e[r];
      const float pw = ex == 2.f ? f3 * f3 : ex == 0.5f && f3 >= 0.f ? std::sqrt(f3) : std::pow(f3, ex);
      const int8_t sd = lnClip(lnQuant(pw, sQ3));
      // out = elemadd(quantize(dequantize(elemdiv(d, sd)) * w), b)
      const float den = c3 * float(sd);
      const float *wr = w + r * cols;
      const int8_t *br = b + r * cols;
      int8_t *o = out + r * cols;
      size_t l = 0;
#if defined(__aarch64__)
      const float32x4_t vrden = vdupq_n_f32(1.f / den), va = vdupq_n_f32(v3a), vden = vdupq_n_f32(den), vd4 = vdupq_n_f32(sD4),
                        vq4 = vdupq_n_f32(q4k), vk0 = vdupq_n_f32(ak0), vk1 = vdupq_n_f32(ak1),
                        vkd = vdupq_n_f32(akd);
      const int32x4_t hi127 = vdupq_n_s32(127), lo128 = vdupq_n_s32(-128);
      for (; l + 16 <= cols; l += 16) {
        const int8x16_t dv = vld1q_s8(d[k] + l), bv = vld1q_s8(br + l);
        const int16x8_t dlo = vmovl_s8(vget_low_s8(dv)), dhi = vmovl_s8(vget_high_s8(dv));
        const int16x8_t blo = vmovl_s8(vget_low_s8(bv)), bhi = vmovl_s8(vget_high_s8(bv));
        const int32x4_t D[4] = {vmovl_s16(vget_low_s16(dlo)), vmovl_s16(vget_high_s16(dlo)),
                                vmovl_s16(vget_low_s16(dhi)), vmovl_s16(vget_high_s16(dhi))};
        const int32x4_t Bv[4] = {vmovl_s16(vget_low_s16(blo)), vmovl_s16(vget_high_s16(blo)),
                                 vmovl_s16(vget_low_s16(bhi)), vmovl_s16(vget_high_s16(bhi))};
        int32x4_t S[4];
        for (int j = 0; j < 4; j++) {
          // approx (-vta-cpu-approx): times 1 / den instead of the division of elemdiv
          const float32x4_t num = vmulq_f32(va, vcvtq_f32_s32(D[j]));
          int32x4_t n = vcvtaq_s32_f32(approx ? vmulq_f32(num, vrden) : vdivq_f32(num, vden));
          n = vmaxq_s32(vminq_s32(n, hi127), lo128);
          const float32x4_t f4 = vmulq_f32(vmulq_f32(vcvtq_f32_s32(n), vd4), vld1q_f32(wr + l + 4 * j));
          int32x4_t q4 = vcvtaq_s32_f32(q4mul ? vmulq_f32(f4, vq4) : vdivq_f32(f4, vq4));
          q4 = vmaxq_s32(vminq_s32(q4, hi127), lo128);
          const int32x4_t L32 = vcvtaq_s32_f32(vmulq_f32(vcvtq_f32_s32(q4), vk0));
          const int32x4_t R32 = vcvtaq_s32_f32(vmulq_f32(vcvtq_f32_s32(Bv[j]), vk1));
          S[j] = vcvtaq_s32_f32(vmulq_f32(vcvtq_f32_s32(vaddq_s32(L32, R32)), vkd));
        }
        const int16x8_t n0 = vcombine_s16(vqmovn_s32(S[0]), vqmovn_s32(S[1]));
        const int16x8_t n1 = vcombine_s16(vqmovn_s32(S[2]), vqmovn_s32(S[3]));
        vst1q_s8(o + l, vcombine_s8(vqmovn_s16(n0), vqmovn_s16(n1)));
      }
#endif
      for (; l < cols; l++) {
        const float num = v3a * float(d[k][l]);
        const int8_t n = lnClip(approx ? num * (1.f / den) : num / den);
        const float f4 = float(n) * sD4;
        const float f4w = f4 * wr[l];
        o[l] = lnAdd(lnClip(lnQuant(f4w, sQ4)), br[l], a2a, a2b, a2d);
      }
    }
  }
}
static void fill(int8_t *o8, size_t n, float v) {
  float *o = (float *)o8;
  for (size_t i = 0; i < n; i++) o[i] = v;
}
} // namespace nestc_cpu
)~";

static std::string dimsList(llvm::ArrayRef<dim_t> d) {
  std::string s = "{";
  for (size_t i = 0; i < d.size(); i++)
    s += (i ? ", " : "") + std::to_string(d[i]);
  return s + "}";
}

static void requireFloat(const Instruction *I, std::initializer_list<const Value *> vs) {
  for (const Value *v : vs)
    if (v->getType()->getElementType() != ElemKind::FloatTy) {
      llvm::errs() << "VTASave: " << I->getKindName() << " " << I->getName()
                   << ": only float is supported on the CPU path\n";
      std::exit(1);
    }
}

//#### JO added (2026-11-09): the dequantize that wrote v last before I (int8, offset 0), if no other instruction writes v
//#### (or a view of it) in between: then v holds q * scale values at I (erfQ, softmaxQ)
static const DequantizeInst *dequantizedBefore(const Instruction &I, const Value *v) {
  const Value *ov = getOrigin(const_cast<Value *>(v));
  auto &instrs = I.getParent()->getInstrs();
  auto it = I.getIterator();
  while (it != instrs.begin()) {
    --it;
    const Instruction &J = *it;
    for (unsigned i = 0, e = J.getNumOperands(); i < e; i++) {
      auto op = J.getOperand(i);
      if (op.second == OperandKind::In || getOrigin(const_cast<Value *>(op.first)) != ov)
        continue;
      auto *dq = llvm::dyn_cast<DequantizeInst>(&J);
      // v or a view of it (DeiT softmax: a reshape of the dequantized scores), written whole by the dequantize
      if (dq && getOrigin(dq->getDest()) == ov && dq->getDest()->getType()->size() == ov->getType()->size() &&
          dq->getSrc()->getType()->getElementType() == ElemKind::Int8QTy &&
          dq->getSrc()->getType()->getOffset() == 0)
        return dq;
      return nullptr;
    }
  }
  return nullptr;
}

static void requireRank(const Instruction *I, const Value *v) {
  if (v->dims().size() < 1 || v->dims().size() > 6) {
    llvm::errs() << "VTASave: " << I->getKindName() << " " << I->getName() << ": rank " << v->dims().size()
                 << " (1-6 supported)\n";
    std::exit(1);
  }
}

static std::string sizeStr(const Value *v) { return std::to_string(v->getType()->size()); }
static std::string esStr(const Value *v) { return std::to_string(v->getType()->getElementSize()); }

// one generic CPU op; returns false for an instruction it does not handle
static bool isUniformConstant(const Value *v, VTASaveContext *ctx);
static std::string matMulSchedArgs(dim_t M, dim_t K, dim_t N);  //#### JO added (2026-11-10): below
static std::string matMulCallTail(VTASaveContext *ctx, const std::string &sched);
static bool saveGenericCpuInst(const Instruction &I, std::string *bundle, VTASaveContext *ctx) {
  auto P = [&](Value *v) { return cpuPtr(v, bundle, ctx); };
  std::string call;
  switch (I.getKind()) {
  case Kinded::Kind::TouchInstKind:
    bundle->append("  // touch: no data\n");
    return true;
  case Kinded::Kind::CopyInstKind: {
    auto *C = llvm::cast<CopyInst>(&I);
    std::string d = P(C->getDest()), s = P(C->getSrc());
    call = "memmove(" + d + ", " + s + ", " + std::to_string(C->getDest()->getSizeInBytes()) + ");";
    break;
  }
  case Kinded::Kind::InsertTensorInstKind: {
    auto *C = llvm::cast<InsertTensorInst>(&I);
    requireRank(&I, C->getDest());
    std::string d = P(C->getDest()), s = P(C->getSrc());
    call = "{ const size_t dd[] = " + dimsList(C->getDest()->dims()) + ", sd[] = " + dimsList(C->getSrc()->dims()) +
           ", off[] = " + dimsList(C->getOffsets()) + "; nestc_cpu::insert(" + d + ", dd, " + s + ", sd, off, " +
           std::to_string(C->getCount()) + ", " + std::to_string(C->getAxis()) + ", " +
           std::to_string(C->getDest()->dims().size()) + ", " + esStr(C->getDest()) + "); }";
    break;
  }
  case Kinded::Kind::ExtractTensorInstKind: {
    auto *C = llvm::cast<ExtractTensorInst>(&I);
    requireRank(&I, C->getSrc());
    std::string d = P(C->getDest()), s = P(C->getSrc());
    call = "{ const size_t dd[] = " + dimsList(C->getDest()->dims()) + ", sd[] = " + dimsList(C->getSrc()->dims()) +
           ", off[] = " + dimsList(C->getOffsets()) + "; nestc_cpu::extract(" + d + ", dd, " + s + ", sd, off, " +
           std::to_string(C->getSrc()->dims().size()) + ", " + esStr(C->getDest()) + "); }";
    break;
  }
  case Kinded::Kind::TransposeInstKind: {
    auto *C = llvm::cast<TransposeInst>(&I);
    requireRank(&I, C->getSrc());
    std::string d = P(C->getDest()), s = P(C->getSrc()), sh = "{";
    for (size_t i = 0; i < C->getShuffle().size(); i++)
      sh += (i ? ", " : "") + std::to_string(C->getShuffle()[i]);
    call = "{ const size_t sd[] = " + dimsList(C->getSrc()->dims()) + "; const unsigned sh[] = " + sh +
           "}; nestc_cpu::transpose(" + d + ", " + s + ", sd, sh, " + std::to_string(C->getSrc()->dims().size()) +
           ", " + esStr(C->getSrc()) + "); }";
    break;
  }
  case Kinded::Kind::MatMulInstKind: {
    auto *C = llvm::cast<MatMulInst>(&I);
    auto ld = C->getLHS()->dims(), rd = C->getRHS()->dims();
    //#### JO added (2026-11-09): int8 (-vta-matmul-conv; the ones with a constant rhs are EVTA convs, VTA.cpp)
    if (C->getDest()->getType()->getElementType() == ElemKind::Int8QTy &&
        C->getLHS()->getType()->getElementType() == ElemKind::Int8QTy &&
        C->getRHS()->getType()->getElementType() == ElemKind::Int8QTy && !C->getDest()->getType()->getOffset() &&
        !C->getLHS()->getType()->getOffset() && !C->getRHS()->getType()->getOffset()) {
      std::string d = P(C->getDest()), a = P(C->getLHS()), b = P(C->getRHS());
      const float ratio = C->getLHS()->getType()->getScale() * C->getRHS()->getType()->getScale() /
                          C->getDest()->getType()->getScale();
      //#### JO added (2026-11-09): -vta-evta-matmul (rounding half up instead of std::round's half away from zero)
      int e2 = 0;
      const float mant = std::frexp(ratio, &e2);
      const int shift = 1 - e2;
      dim_t w = 1;
      for (dim_t q = 1; q * q <= ld[0]; q++)
        if (ld[0] % q == 0)
          w = q;
      if (vtaEvtaMatMul && mant == 0.5f && shift >= 1 && shift <= 15 && w >= 7) {
        call = "matmul_evta(" + a + ", " + b + ", " + d + ", " + std::to_string(ld[0]) + ", " + std::to_string(ld[1]) +
               ", " + std::to_string(rd[1]) + ", " + std::to_string(shift) +
               matMulCallTail(ctx, matMulSchedArgs(ld[0], ld[1], rd[1])) + ");";
        break;
      }
      char sc[32];
      snprintf(sc, sizeof(sc), "%.9g", ratio);
      call = "nestc_cpu::matmulI8(" + d + ", " + a + ", " + b + ", " + std::to_string(ld[0]) + ", " +
             std::to_string(ld[1]) + ", " + std::to_string(rd[1]) + ", " + sc + "f);";
      break;
    }
    requireFloat(&I, {C->getDest(), C->getLHS(), C->getRHS()});
    std::string d = P(C->getDest()), a = P(C->getLHS()), b = P(C->getRHS());
    call = "nestc_cpu::matmul(" + d + ", " + a + ", " + b + ", " + std::to_string(ld[0]) + ", " +
           std::to_string(ld[1]) + ", " + std::to_string(rd[1]) + ");";
    break;
  }
  case Kinded::Kind::BatchedReduceAddInstKind: {
    auto *C = llvm::cast<BatchedReduceAddInst>(&I);
    requireFloat(&I, {C->getDest(), C->getBatch()});
    auto bd = C->getBatch()->dims();
    size_t outer = 1, inner = 1, ax = C->getAxis();
    for (size_t i = 0; i < ax; i++) outer *= bd[i];
    for (size_t i = ax + 1; i < bd.size(); i++) inner *= bd[i];
    std::string d = P(C->getDest()), s = P(C->getBatch());
    call = "nestc_cpu::reduceAdd(" + d + ", " + s + ", " + std::to_string(outer) + ", " + std::to_string(bd[ax]) +
           ", " + std::to_string(inner) + ");";
    break;
  }
  case Kinded::Kind::SoftMaxInstKind: {
    auto *C = llvm::cast<SoftMaxInst>(&I);
    requireFloat(&I, {C->getDest(), C->getSrc()});
    auto sd = C->getSrc()->dims();
    std::string d = P(C->getDest()), s = P(C->getSrc());
    //#### JO added (2026-11-09): softmaxQ when the input was dequantized from int8 (dequantizedBefore)
    if (auto *dq = dequantizedBefore(I, C->getSrc())) {
      char sc[32];
      snprintf(sc, sizeof(sc), "%.9g", dq->getSrc()->getType()->getScale());
      call = "nestc_cpu::softmaxQ(" + d + ", " + s + ", " + std::to_string(sd[0]) + ", " +
             std::to_string(C->getSrc()->getType()->size() / sd[0]) + ", " + sc + "f);";
      break;
    }
    call = "nestc_cpu::softmax(" + d + ", " + s + ", " + std::to_string(sd[0]) + ", " +
           std::to_string(C->getSrc()->getType()->size() / sd[0]) + ");";
    break;
  }
  case Kinded::Kind::ElementMulInstKind:
  case Kinded::Kind::ElementPowInstKind: {
    const bool isMul = I.getKind() == Kinded::Kind::ElementMulInstKind;
    Value *dv = isMul ? llvm::cast<ElementMulInst>(&I)->getDest() : llvm::cast<ElementPowInst>(&I)->getDest();
    Value *lv = isMul ? llvm::cast<ElementMulInst>(&I)->getLHS() : llvm::cast<ElementPowInst>(&I)->getLHS();
    Value *rv = isMul ? llvm::cast<ElementMulInst>(&I)->getRHS() : llvm::cast<ElementPowInst>(&I)->getRHS();
    requireFloat(&I, {dv, lv, rv});
    std::string d = P(dv), a = P(lv), b = P(rv);
    //#### JO added (2026-11-09): pow by a constant 2 in every element as a * a (nestc_cpu::square)
    if (!isMul && isUniformConstant(rv, ctx)) {
      float e;
      memcpy(&e, constantPayload(rv, ctx)->getUnsafePtr(), 4);
      if (e == 2.f) {
        call = "nestc_cpu::square(" + d + ", " + a + ", " + sizeStr(dv) + ");";
        break;
      }
    }
    call = std::string("nestc_cpu::") + (isMul ? "mul(" : "pow(") + d + ", " + a + ", " + b + ", " + sizeStr(dv) + ");";
    break;
  }
  case Kinded::Kind::ElementErfInstKind: {
    auto *C = llvm::cast<ElementErfInst>(&I);
    requireFloat(&I, {C->getDest(), C->getSrc()});
    std::string d = P(C->getDest()), s = P(C->getSrc());
    //#### JO added (2026-11-09): erfQ when the input was dequantized from int8 (dequantizedBefore)
    if (auto *dq = dequantizedBefore(I, C->getSrc())) {
      char sc[32];
      snprintf(sc, sizeof(sc), "%.9g", dq->getSrc()->getType()->getScale());
      call = "nestc_cpu::erfQ(" + d + ", " + s + ", " + sizeStr(C->getDest()) + ", " + sc + "f);";
      break;
    }
    call = "nestc_cpu::erf(" + d + ", " + s + ", " + sizeStr(C->getDest()) + ");";
    break;
  }
  case Kinded::Kind::SplatInstKind: {
    auto *C = llvm::cast<SplatInst>(&I);
    requireFloat(&I, {C->getDest()});
    std::string d = P(C->getDest());
    char v[32];
    snprintf(v, sizeof(v), "%.9g", C->getValue());
    call = "nestc_cpu::fill(" + d + ", " + sizeStr(C->getDest()) + ", (float)" + std::string(v) + ");";
    break;
  }
  default:
    return false;
  }
  ctx->usesCpuOps = true;
  bundle->append("  " + call + "\n");
  return true;
}

struct BundleSaveCtx {
  std::string bundle;
  std::string initConst;
  std::string destroyConst;
  std::string includeHeader;
  std::string symbolTable;
  std::string bundleConfig;
};

void initBundleSave(struct BundleSaveCtx &bundleCtx, llvm::StringRef bundleName,
                    llvm::StringRef mainEntryName) {
  auto &bundle = bundleCtx.bundle;
  // bundleCtx.bundle = new std::string();
  bundle = "";
  bundle.append("int ");
  bundle.append(mainEntryName);
#ifdef NESTC_EVTA_PROFILE_AUTOTUNE
  bundle.append("(uint8_t *constantWeight, uint8_t *mutableWeight, uint8_t "
                "*activations, int input1, int input2, int input3){\n");
#else
  bundle.append("(uint8_t *constantWeight, uint8_t *mutableWeight, uint8_t "
                "*activations){\n");
#endif
#ifdef VTA_ALLINONE_FUNCTION
  bundle.append("  ");
  bundle.append(bundleName);
  bundle.append("_load_module(constantWeight);\n");
#endif
#ifdef VTA_RESET_CMDH
  bundle.append("  vtaCmdH = VTATLSCommandHandle();\n");
#endif

#ifdef VTA_PROFILE
  bundle.append("  std::ofstream prof_out(\"profileResult.txt\");\n");
#endif

  bundleCtx.initConst = "";
  bundleCtx.destroyConst = "";

  // Generate Include Header
  bundleCtx.includeHeader = "#include \"";
  bundleCtx.includeHeader.append(mainEntryName);
  bundleCtx.includeHeader.append(".h\"\n");
}

void finalizeBundleEntrySave(struct BundleSaveCtx &bundleCtx,
                             llvm::StringRef bundleName,
                             llvm::StringRef mainEntryName,
                             VTASaveContext &ctx) {
  auto &bundle = bundleCtx.bundle;
#ifdef VTA_ALLINONE_FUNCTION
  bundle.append("  ");
  bundle.append(bundleName);
  bundle.append("_destroy_module();\n");
#endif
#ifdef VTA_RESET_CMDH
  bundle.append("  VTARuntimeShutdown();\n");
#endif

  bundle.append("  return 0;\n");
  bundle.append("}");

  // Generate SymbolTable struct
  auto &symbolTable = bundleCtx.symbolTable;
  symbolTable = "SymbolTableEntry symbolTableEntry_";
  symbolTable.append(bundleName);
  symbolTable.append("[");
  symbolTable.append(std::to_string(ctx.getSymbols()->size()));
  symbolTable.append("]={");
  for (auto sym : *(ctx.getSymbols())) {
    symbolTable.append("{\"");
    symbolTable.append(sym.name);
    symbolTable.append("\",");
    symbolTable.append(std::to_string(sym.offset));
    symbolTable.append(",");
    symbolTable.append(std::to_string(sym.size));
    symbolTable.append(",'1'},");
  }
  symbolTable.erase(symbolTable.size() - 1, 1);
  symbolTable.append("};\n");

  // Generate BundleConfig struct
  auto &bundleConfig = bundleCtx.bundleConfig;
  bundleConfig = "BundleConfig ";
  bundleConfig.append(bundleName);
  bundleConfig.append("_config = {");
  bundleConfig.append(std::to_string(ctx.getCMemSize()));
  bundleConfig.append(", ");
  bundleConfig.append(std::to_string(ctx.getMMemSize()));
  bundleConfig.append(", 0, 64, ");
  bundleConfig.append(std::to_string(ctx.getSymbols()->size()));
  bundleConfig.append(", symbolTableEntry_");
  bundleConfig.append(bundleName);
  bundleConfig.append("};\n");

  auto &initConst = bundleCtx.initConst;
  //#### JO added (2026-11-09): the generic CPU ops the bundle calls
  if (ctx.usesCpuOps)
    bundleCtx.includeHeader.append(NESTC_CPU_OPS);
  initConst.append("namespace namespace_");
  initConst.append(bundleName);
  initConst.append(" {\n");
  // Generate Init Constant values
  for (auto ste : *ctx.getConstantSymbols()) {
    if (ste.kind == '0') {
      initConst.append("int");
      initConst.append(std::to_string(8));
      initConst.append("_t* ");
      initConst.append(ste.name);
      initConst.append(";\n");
    }
#ifdef VTA_MEMOPT_DISABLE
    else if (ste.kind == '1') {
      initConst.append("int");
      initConst.append(std::to_string(8));
      initConst.append("_t* ");
      initConst.append(ste.name);
      initConst.append(";\n");
    }
#endif
  }
  initConst.append("}\n");
  initConst.append("using namespace namespace_");
  initConst.append(bundleName);
  initConst.append(";\n");
  initConst.append("extern VTACommandHandle vtaCmdH");
  if (ctx.getIdxMultiEVTA()) {
    initConst.append(std::to_string(ctx.getIdxMultiEVTA()));
  }
  initConst.append(";\n");
  //#### JO added (2026-11-09): -vta-lut-elementwise
  initConst.append(ctx.lutFuncs);

  initConst.append("\nvoid ");
  initConst.append(mainEntryName);
  initConst.append("_load_module(uint8_t *constantWeight){\n");
  // initConst.append("  xlnk_reset();\n");
  // initConst.append("  vtaCmdH = VTATLSCommandHandle();\n");

  for (auto ste : *ctx.getConstantSymbols()) {
    //#### JO changed (2026-11-06): VTAConstBufferAlloc (VTABufferAlloc + VTABufferCopy, one buffer for the same bytes in
    //#### the process: the multi-EVTA copies of a model share their weights)
    if (ste.kind == '0') {
      initConst.append("  ");
      initConst.append(ste.name);
      initConst.append(" = (int8_t *)VTAConstBufferAlloc(constantWeight + ");
      initConst.append(std::to_string(ste.offset));
      initConst.append(", ");
      initConst.append(std::to_string(ste.size));
      initConst.append(");\n");
    }
#ifdef VTA_MEMOPT_DISABLE
    else if (ste.kind == '1') {
      initConst.append("  ");
      initConst.append(ste.name);
      initConst.append(" = (int8_t *)malloc(");
      initConst.append(std::to_string(ste.size));
      initConst.append(");\n");
    }
#endif
  }
  initConst.append("}\n");

  auto &destroyConst = bundleCtx.destroyConst;
  destroyConst.append("\nvoid ");
  destroyConst.append(mainEntryName);
  destroyConst.append("_destroy_module(){\n");
  for (auto ste : *ctx.getConstantSymbols()) {
    if (ste.kind == '0') {
      destroyConst.append("  ");
      destroyConst.append("VTAConstBufferFree(");
      destroyConst.append(ste.name);
      destroyConst.append(");\n");
    }
#ifdef VTA_MEMOPT_DISABLE
    else if (ste.kind == '1') {
      destroyConst.append("  ");
      destroyConst.append("free(");
      destroyConst.append(ste.name);
      destroyConst.append(");\n");
    }
#endif
  }
  destroyConst.append("}\n");
}

//#### JO added (2026-11-08): the values of the options that change the generated code (defaults included), written at the
//#### top of the bundle .cpp, so a bundle shows how it was made (MobileNetV2Test: -vta-round-shift, -vta-evta-depthwise,
//#### -enable-channelwise-depthwise; a hand-run model-compiler that misses one differs only in its numbers otherwise).
//#### Options defined in other files (VTA.cpp, the quantizer, the loader) are looked up by name; all are cl::opt<bool>.
//#### JO added (2026-11-09): -vta-lut-elementwise (findLutRegions / emitLutRegion; LutRegion)
static llvm::cl::opt<bool> vtaLutElementwise(
    "vta-lut-elementwise",
    llvm::cl::desc("Emit runs of elementwise instructions from one int8 buffer to an int8 value (other operands "
                   "constants with one value) as a 256-entry table computed once at run time by the same code"),
    llvm::cl::init(true));

// a constant whose elements all have the same bytes (a broadcast scalar)
static bool isUniformConstant(const Value *v, VTASaveContext *ctx) {
  if (!isConstantWeight(v))
    return false;
  const Tensor *t = constantPayload(v, ctx);
  if (!t || t->getSizeInBytes() != v->getSizeInBytes())
    return false;
  const size_t es = v->getType()->getElementSize(), n = v->size();
  const char *p = t->getUnsafePtr();
  for (size_t i = 1; i < n; i++)
    if (memcmp(p, p + i * es, es))
      return false;
  return true;
}

// the activation under v through tensor views at offset 0, nullptr if a view has an offset or it is not an activation
static const Value *zeroOffsetActivation(const Value *v) {
  while (auto *tv = llvm::dyn_cast<TensorViewInst>(v)) {
    for (auto o : tv->getOffsets())
      if (o)
        return nullptr;
    v = tv->getSrc();
  }
  return llvm::isa<AllocActivationInst>(v) ? v : nullptr;
}

static bool lutEligibleKind(const Instruction &I) {
  switch (I.getKind()) {
  case Kinded::Kind::QuantizeInstKind:
  case Kinded::Kind::DequantizeInstKind:
  case Kinded::Kind::ElementAddInstKind:
  case Kinded::Kind::ElementSubInstKind:
  case Kinded::Kind::ElementMulInstKind:
  case Kinded::Kind::ElementDivInstKind:
  case Kinded::Kind::ElementMaxInstKind:
  case Kinded::Kind::ElementErfInstKind:
  case Kinded::Kind::ElementPowInstKind:
    return true;
  default:
    return false;
  }
}

static void emitElementwiseInst(const Instruction &I, std::string *b, VTASaveContext *ctx) {
  switch (I.getKind()) {
  case Kinded::Kind::QuantizeInstKind:
    saveQuantizeInst(llvm::cast<QuantizeInst>(&I), b, ctx);
    break;
  case Kinded::Kind::DequantizeInstKind:
    saveDequantizeInst(llvm::cast<DequantizeInst>(&I), b, ctx);
    break;
  case Kinded::Kind::ElementAddInstKind:
    saveElemAddInst(llvm::cast<ElementAddInst>(&I), b, ctx);
    break;
  case Kinded::Kind::ElementSubInstKind:
    saveElemSubInst(llvm::cast<ElementSubInst>(&I), b, ctx);
    break;
  case Kinded::Kind::ElementDivInstKind:
    saveElemDivInst(llvm::cast<ElementDivInst>(&I), b, ctx);
    break;
  case Kinded::Kind::ElementMaxInstKind:
    saveElemMaxInst(llvm::cast<ElementMaxInst>(&I), b, ctx);
    break;
  default:
    saveGenericCpuInst(I, b, ctx);
  }
}

static void findLutRegions(IRFunction *F, VTASaveContext *ctx) {
  if (!vtaLutElementwise)
    return;
  std::vector<const Instruction *> L;
  std::map<const Instruction *, size_t> pos;
  for (const auto &I : F->getInstrs()) {
    pos[&I] = L.size();
    L.push_back(&I);
  }
  auto skippable = [](const Instruction *I) {
    return I->getKind() == Kinded::Kind::AllocActivationInstKind ||
           I->getKind() == Kinded::Kind::DeallocActivationInstKind ||
           I->getKind() == Kinded::Kind::TensorViewInstKind;
  };
  auto touches = [](const Instruction *I, const Value *b) {
    for (unsigned k = 0, e = I->getNumOperands(); k < e; k++)
      if (getOrigin(const_cast<Value *>(I->getOperand(k).first)) == b)
        return true;
    return false;
  };
  for (size_t i = 0; i < L.size(); i++) {
    const Instruction *I0 = L[i];
    if (!lutEligibleKind(*I0) || ctx->isFusedAway(I0) || ctx->isPreFused(I0))
      continue;
    const size_t n = I0->getOperand(0).first->size();
    if (n < 1024)
      continue;
    // the root: the one int8 activation the first instruction reads
    const Value *root = nullptr;
    bool ok = true;
    for (unsigned k = 0, e = I0->getNumOperands(); k < e && ok; k++) {
      auto op = I0->getOperand(k);
      if (op.second != OperandKind::In || isConstantWeight(op.first))
        continue;
      const Value *o = zeroOffsetActivation(op.first);
      ok = o && (!root || root == o) && op.first->getType()->getElementType() == ElemKind::Int8QTy;
      root = o;
    }
    if (!ok || !root)
      continue;
    std::set<const Value *> written;
    auto accepts = [&](const Instruction *J) {
      if (!lutEligibleKind(*J) || ctx->isFusedAway(J) || ctx->isPreFused(J))
        return false;
      std::vector<const Value *> outs;
      for (unsigned k = 0, e = J->getNumOperands(); k < e; k++) {
        auto op = J->getOperand(k);
        if (op.first->size() != n)
          return false;
        if (op.second == OperandKind::In && isConstantWeight(op.first)) {
          if (!isUniformConstant(op.first, ctx))
            return false;
          continue;
        }
        const Value *o = zeroOffsetActivation(op.first);
        if (!o || o->size() != n || ctx->isPacked(o) || ctx->isPacked(op.first))
          return false;
        if (op.second == OperandKind::In) {
          if (o != root && !written.count(o))
            return false;
        } else if (op.second == OperandKind::Out) {
          outs.push_back(o);
        } else {
          return false;
        }
      }
      written.insert(outs.begin(), outs.end());
      return true;
    };
    std::vector<size_t> members;
    for (size_t j = i; j < L.size(); j++) {
      if (j > i && skippable(L[j]))
        continue;
      if (!accepts(L[j]))
        break;
      members.push_back(j);
    }
    // the longest prefix that ends with an int8 value and leaves nothing else that is read later
    for (size_t m = members.size(); m > 0; m--) {
      const Instruction *E = L[members[m - 1]];
      const Value *outV = E->getOperand(0).first;
      if (E->getOperand(0).second != OperandKind::Out || outV->getType()->getElementType() != ElemKind::Int8QTy)
        continue;
      const Value *out = zeroOffsetActivation(outV);
      std::set<const Value *> w;
      for (size_t k = 0; k < m; k++)
        for (unsigned q = 0, e = L[members[k]]->getNumOperands(); q < e; q++)
          if (L[members[k]]->getOperand(q).second == OperandKind::Out)
            w.insert(zeroOffsetActivation(L[members[k]]->getOperand(q).first));
      // every other buffer the region writes is freed (or not used) before anything reads it
      bool dead = true;
      for (const Value *b : w) {
        if (b == out)
          continue;
        for (size_t k = members[m - 1] + 1; k < L.size(); k++)
          if (touches(L[k], b)) {
            dead = L[k]->getKind() == Kinded::Kind::DeallocActivationInstKind;
            break;
          }
        if (!dead)
          break;
      }
      if (!dead)
        continue;
      LutRegion r;
      for (size_t k = 0; k < m; k++)
        r.instrs.push_back(L[members[k]]);
      r.root = root;
      r.out = out;
      r.n = n;
      if (pos.count(llvm::cast<Instruction>(out)) && pos[llvm::cast<Instruction>(out)] > i)
        r.outAlloc = llvm::cast<Instruction>(out);
      for (size_t k = 1; k < m; k++)
        ctx->getLutAway()->insert(L[members[k]]);
      (*ctx->getLutRegions())[I0] = r;
      i = members[m - 1];
      break;
    }
  }
}

// identifiers in s renamed by ren; a number token equal to num between ',' / '(' and ',' / ')' becomes rep
// (count: how many)
static std::string renameTokens(const std::string &s, const std::map<std::string, std::string> &ren,
                                const std::string &num, const std::string &rep, int *count) {
  std::string o;
  size_t i = 0;
  while (i < s.size()) {
    const char c = s[i];
    if (std::isalpha((unsigned char)c) || c == '_') {
      size_t j = i;
      while (j < s.size() && (std::isalnum((unsigned char)s[j]) || s[j] == '_'))
        j++;
      const std::string id = s.substr(i, j - i);
      auto it = ren.find(id);
      o += it == ren.end() ? id : it->second;
      i = j;
    } else if (std::isdigit((unsigned char)c)) {
      size_t j = i;
      while (j < s.size() && (std::isalnum((unsigned char)s[j]) || s[j] == '.'))
        j++;
      const std::string tok = s.substr(i, j - i);
      size_t a = o.find_last_not_of(' '), b = s.find_first_not_of(' ', j);
      const bool arg = a != std::string::npos && (o[a] == ',' || o[a] == '(') && b != std::string::npos &&
                       (s[b] == ',' || s[b] == ')');
      if (tok == num && arg) {
        o += rep;
        (*count)++;
      } else {
        o += tok;
      }
      i = j;
    } else {
      o += c;
      i++;
    }
  }
  return o;
}

static void emitLutRegion(const LutRegion &r, std::string *bundle, VTASaveContext *ctx) {
  static int id = 0;
  std::map<std::string, std::string> ren;
  std::map<const Value *, std::string> probe;
  auto probeOf = [&](const Value *v) {
    const Value *o = zeroOffsetActivation(v);
    auto it = probe.find(o);
    if (it != probe.end())
      return it->second;
    std::string p = "lutp" + std::to_string(probe.size());
    probe[o] = p;
    ren[o->getName().str()] = p;
    return p;
  };
  probeOf(r.root);
  std::string body;
  const std::string num = std::to_string(r.n);
  for (const Instruction *J : r.instrs) {
    for (unsigned k = 0, e = J->getNumOperands(); k < e; k++) {
      auto op = J->getOperand(k);
      if (op.second == OperandKind::In && isConstantWeight(op.first))
        continue;
      ren[op.first->getName().str()] = probeOf(op.first);
      // an activation (or a view defined later inside the region) known as one before the emitters look it up: else
      // addConstantSymbolEntry makes it a constant (VTABufferGetVirtAddr, and the weights offsets after it shift)
      if (!hasConstantSymbol(op.first, ctx))
        ctx->getConstantSymbols()->push_back({op.first->getName().data(), 0, op.first->getSizeInBytes(), '1'});
    }
    std::string tmp;
    emitElementwiseInst(*J, &tmp, ctx);
    int cnt = 0;
    std::string t2 = renameTokens(tmp, ren, num, "256", &cnt);
    if (cnt != 1 || t2.find(num) != std::string::npos || t2.find(std::to_string(r.n * 4)) != std::string::npos) {
      llvm::errs() << "VTASave: -vta-lut-elementwise: unexpected code for " << J->getName() << ": " << tmp;
      std::exit(1);
    }
    body += "    " + t2;
  }
  ctx->usesCpuOps = true;
  if (r.outAlloc && ctx->getHoisted()->insert(r.outAlloc).second)
    *bundle += "  int8_t *" + r.out->getName().str() + " = (int8_t *)malloc(" +
               std::to_string(r.out->getSizeInBytes()) + ");\n";
  std::string decl;
  for (auto &p : probe)
    decl += "    alignas(16) int8_t " + p.second + "[1024];\n";
  const std::string lut = "lut" + std::to_string(id++);
  *bundle += "  //#### " + std::to_string(r.instrs.size()) + " elementwise instructions (" +
             r.instrs.front()->getName().str() + " .. " + r.instrs.back()->getName().str() +
             ") as a table of their 256 results (-vta-lut-elementwise)\n";
  // the probe in a function of its own at file scope: one statement in the inference function (timed_bundle.py)
  ctx->lutFuncs += "\n// " + std::to_string(r.instrs.size()) + " elementwise instructions from " +
                   r.instrs.front()->getName().str() + " (-vta-lut-elementwise)\nstatic nestc_cpu::Lut8 " + lut +
                   "_make() {\n" + decl + "    for (int q = 0; q < 256; q++) " + probe[r.root] +
                   "[q] = (int8_t)(q - 128);\n" + body + "    nestc_cpu::Lut8 t;\n    memcpy(t.v, " + probe[r.out] +
                   ", 256);\n    return t;\n}\n";
  *bundle += "  static const nestc_cpu::Lut8 " + lut + " = " + lut + "_make();\n";
  *bundle += "  nestc_cpu::lut8(" + r.out->getName().str() + ", " + r.root->getName().str() + ", " + lut + ", " +
             num + ");\n";
}

//#### JO added (2026-11-09): a softmax over rows of a tensor dequantized from int8 (softmaxQ) whose dequantized copy is
//#### read by nothing else: nestc_cpu::softmaxI8 reads the int8 tensor at the dequantize (it may be freed before the
//#### softmax, DeiT attention); the softmax's dest allocation is moved there
static void findSoftmaxI8(IRFunction *F, VTASaveContext *ctx) {
  std::vector<const Instruction *> L;
  std::map<const Instruction *, size_t> pos;
  for (const auto &I : F->getInstrs()) {
    pos[&I] = L.size();
    L.push_back(&I);
  }
  for (size_t i = 0; i < L.size(); i++) {
    auto *S = llvm::dyn_cast<SoftMaxInst>(L[i]);
    if (!S || S->getSrc()->dims().size() != 2 || S->getSrc()->dims()[0] <= 1 ||
        S->getDest()->getType()->getElementType() != ElemKind::FloatTy)
      continue;
    const DequantizeInst *dq = dequantizedBefore(*S, S->getSrc());
    if (!dq || !pos.count(dq) || ctx->isLutAway(dq) || ctx->getLutRegions()->count(dq))
      continue;
    const Value *Fv = zeroOffsetActivation(dq->getDest()), *D = zeroOffsetActivation(S->getDest());
    if (!Fv || !D || zeroOffsetActivation(S->getSrc()) != Fv || S->getSrc()->size() != Fv->size() ||
        D->size() != S->getDest()->size())
      continue;
    bool ok = true;
    // the dequantized copy: only views, the softmax and its deallocation after the dequantize
    for (size_t k = pos[dq] + 1; k < L.size() && ok; k++) {
      const Instruction *J = L[k];
      bool t = false;
      for (unsigned q = 0, e = J->getNumOperands(); q < e; q++)
        t |= getOrigin(const_cast<Value *>(J->getOperand(q).first)) == Fv;
      if (!t || J == S || J->getKind() == Kinded::Kind::TensorViewInstKind)
        continue;
      if (J->getKind() == Kinded::Kind::DeallocActivationInstKind)
        break;
      ok = false;
    }
    // the dest: nothing but its allocation and views between the dequantize and the softmax
    for (size_t k = pos[dq] + 1; k < i && ok; k++) {
      const Instruction *J = L[k];
      if (J == D || J->getKind() == Kinded::Kind::TensorViewInstKind)
        continue;
      for (unsigned q = 0, e = J->getNumOperands(); q < e; q++)
        ok &= getOrigin(const_cast<Value *>(J->getOperand(q).first)) != D;
    }
    if (!ok)
      continue;
    ctx->softmaxAtDq[dq] = S;
    ctx->softmaxAway.insert(S);
    auto *DI = llvm::cast<Instruction>(D);
    if (pos.count(DI) && pos[DI] > pos[dq])
      ctx->softmaxHoist.insert(D);
  }
}

static void emitSoftmaxI8(const DequantizeInst *dq, const SoftMaxInst *S, std::string *bundle, VTASaveContext *ctx) {
  const Value *D = zeroOffsetActivation(S->getDest());
  if (ctx->softmaxHoist.count(D) && ctx->getHoisted()->insert(D).second)
    *bundle += "  int8_t *" + D->getName().str() + " = (int8_t *)malloc(" + std::to_string(D->getSizeInBytes()) + ");\n";
  char sc[32];
  snprintf(sc, sizeof(sc), "%.9g", dq->getSrc()->getType()->getScale());
  ctx->usesCpuOps = true;
  *bundle += "  // the dequantize and the softmax " + S->getName().str() + " in one (softmaxI8)\n";
  *bundle += "  nestc_cpu::softmaxI8(" + D->getName().str() + ", " + cpuPtr(dq->getSrc(), bundle, ctx) + ", " +
             std::to_string(S->getSrc()->dims()[0]) + ", " + std::to_string(S->getSrc()->dims()[1]) + ", " + sc +
             (vtaCpuApprox ? "f, 1);\n" : "f, 0);\n");
}

//#### JO added (2026-11-10): the DeiT layer norm (21 instructions from an int8 x: dequantize, reduceAdd, quantize, elemdiv,
//#### broadcast, elemsub, dequantize, pow 2, reduceAdd, quantize, elemdiv, elemadd eps, dequantize, pow, quantize, broadcast,
//#### elemdiv, dequantize, mul weight, quantize, elemadd bias) as one nestc_cpu::layerNormI8 call at the last instruction,
//#### with the same arithmetic (-vta-fuse-layernorm, findLayerNorms)
static llvm::cl::opt<bool> vtaFuseLayerNorm(
    "vta-fuse-layernorm",
    llvm::cl::desc("Run the int8 layer norm chain (DeiT) as one CPU function (nestc_cpu::layerNormI8)"),
    llvm::cl::init(true));

static void findLayerNorms(IRFunction *F, VTASaveContext *ctx) {
  if (!vtaFuseLayerNorm)
    return;
  std::vector<const Instruction *> L;
  for (const auto &I : F->getInstrs())
    L.push_back(&I);
  auto org = [](const Value *v) { return getOrigin(const_cast<Value *>(v)); };
  auto interleaved = [](const Instruction *I) {
    switch (I->getKind()) {
    case Kinded::Kind::AllocActivationInstKind:
    case Kinded::Kind::DeallocActivationInstKind:
    case Kinded::Kind::TensorViewInstKind:
    case Kinded::Kind::TouchInstKind:
    case Kinded::Kind::SplatInstKind:
      return true;
    default:
      return false;
    }
  };
  auto sc = [](const Value *v) { return v->getType()->getScale(); };
  for (size_t i0 = 0; i0 < L.size(); i0++) {
    auto *D0 = llvm::dyn_cast<DequantizeInst>(L[i0]);
    if (!D0 || D0->getSrc()->getType()->getElementType() != ElemKind::Int8QTy || ctx->isLutAway(D0) ||
        ctx->getLutRegions()->count(D0) || ctx->softmaxAtDq.count(D0))
      continue;
    size_t c = i0;
    std::vector<const Instruction *> mem{D0};
    std::vector<const Instruction *> inter;
    // the next member: the next instruction that is not interleaved, if it has kind K
    auto next = [&](Kinded::Kind K) -> const Instruction * {
      for (c++; c < L.size(); c++) {
        if (interleaved(L[c])) {
          inter.push_back(L[c]);
          continue;
        }
        if (L[c]->getKind() != K)
          return nullptr;
        mem.push_back(L[c]);
        return L[c];
      }
      return nullptr;
    };
    const Value *x = D0->getSrc();
    auto *R1 = llvm::dyn_cast_or_null<BatchedReduceAddInst>(next(Kinded::Kind::BatchedReduceAddInstKind));
    if (!R1 || org(R1->getBatch()) != org(D0->getDest()))
      continue;
    auto bd = R1->getBatch()->dims();
    if (R1->getAxis() != bd.size() - 1)
      continue;
    const size_t cols = bd.back(), rows = R1->getBatch()->size() / cols;
    if (cols > 1024 || x->size() != rows * cols)
      continue;
    auto *Q1 = llvm::dyn_cast_or_null<QuantizeInst>(next(Kinded::Kind::QuantizeInstKind));
    if (!Q1 || org(Q1->getSrc()) != org(R1->getDest()))
      continue;
    auto *V1 = llvm::dyn_cast_or_null<ElementDivInst>(next(Kinded::Kind::ElementDivInstKind));
    if (!V1 || org(V1->getLHS()) != org(Q1->getDest()) || V1->getRHS()->size() != rows || isConstantWeight(V1->getRHS()))
      continue;
    auto *I1 = llvm::dyn_cast_or_null<InsertTensorInst>(next(Kinded::Kind::InsertTensorInstKind));
    if (!I1 || org(I1->getSrc()) != org(V1->getDest()) || I1->getCount() != cols)
      continue;
    auto *E1 = llvm::dyn_cast_or_null<ElementSubInst>(next(Kinded::Kind::ElementSubInstKind));
    if (!E1 || org(E1->getLHS()) != org(x) || org(E1->getRHS()) != org(I1->getDest()))
      continue;
    auto *D2 = llvm::dyn_cast_or_null<DequantizeInst>(next(Kinded::Kind::DequantizeInstKind));
    if (!D2 || org(D2->getSrc()) != org(E1->getDest()))
      continue;
    auto *P2 = llvm::dyn_cast_or_null<ElementPowInst>(next(Kinded::Kind::ElementPowInstKind));
    if (!P2 || org(P2->getLHS()) != org(D2->getDest()) || !isUniformConstant(P2->getRHS(), ctx))
      continue;
    {
      float e;
      memcpy(&e, constantPayload(P2->getRHS(), ctx)->getUnsafePtr(), 4);
      if (e != 2.f)
        continue;
    }
    auto *R2 = llvm::dyn_cast_or_null<BatchedReduceAddInst>(next(Kinded::Kind::BatchedReduceAddInstKind));
    if (!R2 || org(R2->getBatch()) != org(P2->getDest()) || R2->getAxis() != bd.size() - 1)
      continue;
    auto *Q2 = llvm::dyn_cast_or_null<QuantizeInst>(next(Kinded::Kind::QuantizeInstKind));
    if (!Q2 || org(Q2->getSrc()) != org(R2->getDest()))
      continue;
    auto *V2 = llvm::dyn_cast_or_null<ElementDivInst>(next(Kinded::Kind::ElementDivInstKind));
    if (!V2 || org(V2->getLHS()) != org(Q2->getDest()) || V2->getRHS()->size() != rows || isConstantWeight(V2->getRHS()))
      continue;
    auto *A1 = llvm::dyn_cast_or_null<ElementAddInst>(next(Kinded::Kind::ElementAddInstKind));
    if (!A1)
      continue;
    const bool a1swap = org(A1->getRHS()) == org(V2->getDest());
    const Value *vA1 = a1swap ? A1->getRHS() : A1->getLHS(), *eps = a1swap ? A1->getLHS() : A1->getRHS();
    if (org(vA1) != org(V2->getDest()) || !isConstantWeight(eps) || eps->size() != rows)
      continue;
    auto *D3 = llvm::dyn_cast_or_null<DequantizeInst>(next(Kinded::Kind::DequantizeInstKind));
    if (!D3 || org(D3->getSrc()) != org(A1->getDest()))
      continue;
    auto *PW = llvm::dyn_cast_or_null<ElementPowInst>(next(Kinded::Kind::ElementPowInstKind));
    if (!PW || org(PW->getLHS()) != org(D3->getDest()) || isConstantWeight(PW->getRHS()) ||
        PW->getRHS()->size() != rows || PW->getRHS()->getType()->getElementType() != ElemKind::FloatTy)
      continue;
    auto *Q3 = llvm::dyn_cast_or_null<QuantizeInst>(next(Kinded::Kind::QuantizeInstKind));
    if (!Q3 || org(Q3->getSrc()) != org(PW->getDest()))
      continue;
    auto *I2 = llvm::dyn_cast_or_null<InsertTensorInst>(next(Kinded::Kind::InsertTensorInstKind));
    if (!I2 || org(I2->getSrc()) != org(Q3->getDest()) || I2->getCount() != cols)
      continue;
    auto *V3 = llvm::dyn_cast_or_null<ElementDivInst>(next(Kinded::Kind::ElementDivInstKind));
    if (!V3 || org(V3->getLHS()) != org(E1->getDest()) || org(V3->getRHS()) != org(I2->getDest()))
      continue;
    auto *D4 = llvm::dyn_cast_or_null<DequantizeInst>(next(Kinded::Kind::DequantizeInstKind));
    if (!D4 || org(D4->getSrc()) != org(V3->getDest()))
      continue;
    auto *MU = llvm::dyn_cast_or_null<ElementMulInst>(next(Kinded::Kind::ElementMulInstKind));
    if (!MU)
      continue;
    const bool muswap = org(MU->getRHS()) == org(D4->getDest());
    const Value *W = muswap ? MU->getLHS() : MU->getRHS();
    if (org(muswap ? MU->getRHS() : MU->getLHS()) != org(D4->getDest()) || !isConstantWeight(W) ||
        W->size() != rows * cols || W->getType()->getElementType() != ElemKind::FloatTy)
      continue;
    auto *Q4 = llvm::dyn_cast_or_null<QuantizeInst>(next(Kinded::Kind::QuantizeInstKind));
    if (!Q4 || org(Q4->getSrc()) != org(MU->getDest()))
      continue;
    auto *A2 = llvm::dyn_cast_or_null<ElementAddInst>(next(Kinded::Kind::ElementAddInstKind));
    if (!A2)
      continue;
    const bool a2swap = org(A2->getRHS()) == org(Q4->getDest());
    const Value *qA2 = a2swap ? A2->getRHS() : A2->getLHS(), *B = a2swap ? A2->getLHS() : A2->getRHS();
    if (org(qA2) != org(Q4->getDest()) || !isConstantWeight(B) || B->size() != rows * cols ||
        A2->getDest()->size() != rows * cols)
      continue;
    // x and the divisors / exponent stay valid up to A2: no interleaved deallocation or splat of them
    const Value *ox = org(x);
    bool ok = true;
    for (const Instruction *J : inter)
      for (unsigned q = 0, e = J->getNumOperands(); q < e; q++) {
        const Value *o = org(J->getOperand(q).first);
        if (J->getKind() == Kinded::Kind::DeallocActivationInstKind &&
            (o == ox || o == org(V1->getRHS()) || o == org(V2->getRHS()) || o == org(PW->getRHS())))
          ok = false;
        if (J->getKind() == Kinded::Kind::SplatInstKind && o == ox)
          ok = false;
      }
    // the chain's own buffers: freed (or not used) after A2, except the output
    std::set<const Value *> w;
    for (const Instruction *J : mem)
      for (unsigned q = 0, e = J->getNumOperands(); q < e; q++)
        if (J->getOperand(q).second != OperandKind::In)
          w.insert(org(J->getOperand(q).first));
    w.erase(org(A2->getDest()));
    w.erase(ox);
    for (const Value *b : w) {
      for (size_t k = c + 1; k < L.size() && ok; k++) {
        bool t = false;
        for (unsigned q = 0, e = L[k]->getNumOperands(); q < e; q++)
          t |= org(L[k]->getOperand(q).first) == b;
        if (t) {
          ok = L[k]->getKind() == Kinded::Kind::DeallocActivationInstKind;
          break;
        }
      }
    }
    if (!ok)
      continue;
    LayerNormFusion f;
    f.x = x;
    f.out = A2->getDest();
    f.p1 = V1->getRHS();
    f.p2 = V2->getRHS();
    f.eps = eps;
    f.e = PW->getRHS();
    f.w = W;
    f.b = B;
    f.rows = rows;
    f.cols = cols;
    auto q = [](float s) { return "1/" + std::to_string(1 / s); };
    auto el = [](float s) { return "1.0/" + std::to_string(1 / s); };
    f.scales = {q(sc(D0->getSrc())), q(sc(Q1->getDest())),  el(sc(V1->getLHS())), el(sc(V1->getRHS())),
                el(sc(V1->getDest())), el(sc(E1->getLHS())), el(sc(E1->getRHS())), el(sc(E1->getDest())),
                q(sc(D2->getSrc())),  q(sc(Q2->getDest())),  el(sc(V2->getLHS())), el(sc(V2->getRHS())),
                el(sc(V2->getDest())), el(sc(vA1)),           el(sc(eps)),          el(sc(A1->getDest())),
                q(sc(D3->getSrc())),  q(sc(Q3->getDest())),  el(sc(V3->getLHS())), el(sc(V3->getRHS())),
                el(sc(V3->getDest())), q(sc(D4->getSrc())),  q(sc(Q4->getDest())),  el(sc(qA2)),
                el(sc(B)),            el(sc(A2->getDest()))};
    ctx->layerNorms[A2] = f;
    for (const Instruction *J : mem)
      if (J != A2)
        ctx->layerNormAway.insert(J);
    i0 = c;
  }
}

static void emitLayerNormI8(const LayerNormFusion &f, std::string *bundle, VTASaveContext *ctx) {
  ctx->usesCpuOps = true;
  auto ptr = [&](const Value *v) { return cpuPtr(const_cast<Value *>(v), bundle, ctx); };
  std::string call = "  nestc_cpu::layerNormI8(" + ptr(f.out) + ", " + ptr(f.x) + ", " + std::to_string(f.rows) +
                     ", " + std::to_string(f.cols) + ", " + ptr(f.p1) + ", " + ptr(f.p2) + ", " + ptr(f.eps) + ", " +
                     ptr(f.e) + ", " + ptr(f.w) + ", " + ptr(f.b);
  for (const auto &s : f.scales)
    call += ", " + s;
  call += vtaCpuApprox ? ", 1" : ", 0";
  *bundle += "  // the layer norm in one (layerNormI8, -vta-fuse-layernorm)\n" + call + ");\n";
}

//#### JO added (2026-11-10): the int8 matmuls of the attention heads in one EVTA call (-vta-matmul-heads: matmul_evta_n),
//#### and Q K^T straight from the EVTA outputs of the query / key convs (-vta-attn-keep-layout: matmul_evta_qk; the convs
//#### write their outputs kept in the EVTA layout, the head transposes / slice copies are not emitted)
static llvm::cl::opt<bool> vtaMatMulHeads(
    "vta-matmul-heads", llvm::cl::desc("Run consecutive int8 matmuls of one shape (attention heads) in one EVTA call"),
    llvm::cl::init(true));
static llvm::cl::opt<bool> vtaAttnKeepLayout(
    "vta-attn-keep-layout",
    llvm::cl::desc("Q K^T from the EVTA outputs of the query / key convs (no transposes or copies of the heads)"),
    llvm::cl::init(true));

// the shift of an int8 MatMul that -vta-evta-matmul runs on the EVTA, else -1 (the conditions of saveGenericCpuInst)
static int evtaMatMulShift(const MatMulInst *C) {
  if (!vtaEvtaMatMul)
    return -1;
  for (const Value *v : {(const Value *)C->getDest(), (const Value *)C->getLHS(), (const Value *)C->getRHS()})
    if (v->getType()->getElementType() != ElemKind::Int8QTy || v->getType()->getOffset())
      return -1;
  const float ratio =
      C->getLHS()->getType()->getScale() * C->getRHS()->getType()->getScale() / C->getDest()->getType()->getScale();
  int e2 = 0;
  const float mant = std::frexp(ratio, &e2);
  const int shift = 1 - e2;
  const dim_t M = C->getLHS()->dims()[0];
  dim_t w = 1;
  for (dim_t q = 1; q * q <= M; q++)
    if (M % q == 0)
      w = q;
  return mant == 0.5f && shift >= 1 && shift <= 15 && w >= 7 ? shift : -1;
}

// v as its origin + an element offset through tensor views (contiguous views)
static std::pair<const Value *, size_t> viewOrigin(const Value *v) {
  size_t off = 0;
  while (auto *tv = llvm::dyn_cast<TensorViewInst>(v)) {
    auto sd = tv->getSrc()->dims();
    auto o = tv->getOffsets();
    size_t stride = 1;
    for (size_t i = sd.size(); i-- > 0;) {
      off += (i < o.size() ? o[i] : 0) * stride;
      stride *= sd[i];
    }
    v = tv->getSrc();
  }
  return {v, off};
}

static void findMatMulGroups(IRFunction *F, VTASaveContext *ctx) {
  if (!vtaMatMulHeads)
    return;
  std::vector<const Instruction *> L;
  std::map<const Value *, size_t> pos;
  for (const auto &I : F->getInstrs()) {
    pos[&I] = L.size();
    L.push_back(&I);
  }
  auto skippable = [](const Instruction *I) {
    return I->getKind() == Kinded::Kind::AllocActivationInstKind ||
           I->getKind() == Kinded::Kind::DeallocActivationInstKind ||
           I->getKind() == Kinded::Kind::TensorViewInstKind || I->getKind() == Kinded::Kind::TouchInstKind;
  };
  for (size_t i = 0; i < L.size(); i++) {
    auto *M0 = llvm::dyn_cast<MatMulInst>(L[i]);
    if (!M0)
      continue;
    const int shift = evtaMatMulShift(M0);
    if (shift < 0)
      continue;
    MatMulGroup g;
    g.mm.push_back(M0);
    g.shift = shift;
    std::set<const Value *> outs{viewOrigin(M0->getDest()).first};
    size_t j = i + 1;
    for (; j < L.size() && g.mm.size() < 8; j++) {
      if (skippable(L[j]))
        continue;
      auto *Mj = llvm::dyn_cast<MatMulInst>(L[j]);
      if (!Mj || evtaMatMulShift(Mj) != shift || Mj->getLHS()->dims() != M0->getLHS()->dims() ||
          Mj->getRHS()->dims() != M0->getRHS()->dims() || outs.count(viewOrigin(Mj->getLHS()).first) ||
          outs.count(viewOrigin(Mj->getRHS()).first))
        break;
      g.mm.push_back(Mj);
      outs.insert(viewOrigin(Mj->getDest()).first);
    }
    // every operand's buffer exists at the first matmul
    bool ok = g.mm.size() > 1;
    for (auto *Mm : g.mm)
      for (const Value *v : {(const Value *)Mm->getDest(), (const Value *)Mm->getLHS(), (const Value *)Mm->getRHS()}) {
        const Value *o = viewOrigin(v).first;
        ok &= llvm::isa<AllocActivationInst>(o) && pos.count(o) && pos[o] < i && !ctx->isPacked(o);
      }
    if (!ok)
      continue;
    (*ctx->getMatMulGroups())[M0] = g;
    for (size_t k = 1; k < g.mm.size(); k++)
      ctx->getMatMulAway()->insert(g.mm[k]);
    i = pos[g.mm.back()];
  }
}

// a group whose lhs / rhs are head slices of transposes of two EVTA convs: Q K^T from their outputs
static void findAttnKeepLayout(IRFunction *F, VTASaveContext *ctx) {
  if (!vtaAttnKeepLayout)
    return;
  std::vector<const Instruction *> L;
  for (const auto &I : F->getInstrs())
    L.push_back(&I);
  auto users = [&](const Value *o) {
    std::vector<const Instruction *> u;
    for (const Instruction *I : L) {
      if (I->getKind() == Kinded::Kind::TensorViewInstKind || I->getKind() == Kinded::Kind::DeallocActivationInstKind ||
          I == o)
        continue;
      for (unsigned q = 0, e = I->getNumOperands(); q < e; q++)
        if (getOrigin(const_cast<Value *>(I->getOperand(q).first)) == o) {
          u.push_back(I);
          break;
        }
    }
    return u;
  };
  // the head slice: origin s written only by a copy from (transpose dest + head * size); its transpose and conv
  struct Head {
    const CopyInst *copy = nullptr;
    const TransposeInst *tr = nullptr;
    const ConvolutionInst *conv = nullptr;
    size_t head = 0;
  };
  auto trace = [&](const Value *operand, const std::vector<unsigned> &shuffle, Head &h) {
    const Value *s = viewOrigin(operand).first;
    for (const Instruction *I : users(s))
      if (auto *c = llvm::dyn_cast<CopyInst>(I))
        if (viewOrigin(c->getDest()).first == s && viewOrigin(c->getDest()).second == 0)
          h.copy = c;
    if (!h.copy || operand->size() != h.copy->getDest()->size())
      return false;
    auto src = viewOrigin(h.copy->getSrc());
    for (const Instruction *I : users(src.first))
      if (auto *t = llvm::dyn_cast<TransposeInst>(I))
        if (getOrigin(t->getDest()) == src.first)
          h.tr = t;
    if (!h.tr || h.tr->getShuffle().vec() != shuffle || src.second % operand->size())
      return false;
    h.head = src.second / operand->size();
    const Value *co = viewOrigin(h.tr->getSrc()).first;
    if (viewOrigin(h.tr->getSrc()).second)
      return false;
    for (const Instruction *I : users(co))
      if (auto *cv = llvm::dyn_cast<ConvolutionInst>(I))
        if (getOrigin(cv->getDest()) == co)
          h.conv = cv;
    return h.conv != nullptr;
  };
  for (auto &it : *ctx->getMatMulGroups()) {
    MatMulGroup &g = it.second;
    const size_t H = g.mm.size(), M = g.mm[0]->getLHS()->dims()[0], dh = g.mm[0]->getLHS()->dims()[1];
    if (g.mm[0]->getRHS()->dims()[1] != M || dh % 16)
      continue;
    std::vector<Head> q(H), k(H);
    bool ok = true;
    for (size_t i = 0; i < H && ok; i++)
      ok = trace(g.mm[i]->getLHS(), {0, 2, 1, 3}, q[i]) && trace(g.mm[i]->getRHS(), {0, 2, 3, 1}, k[i]) &&
           q[i].head == i && k[i].head == i && q[i].conv == q[0].conv && k[i].conv == k[0].conv &&
           q[i].tr == q[0].tr && k[i].tr == k[0].tr;
    if (!ok)
      continue;
    const ConvolutionInst *cq = q[0].conv, *ck = k[0].conv;
    auto cd = cq->getDest()->dims();
    if (cd.size() != 4 || cd[0] != 1 || cd[1] * cd[2] != M || cd[3] != H * dh || ck->getDest()->dims() != cd ||
        cq->getDest()->getType()->getScale() != g.mm[0]->getLHS()->getType()->getScale() ||
        ck->getDest()->getType()->getScale() != g.mm[0]->getRHS()->getType()->getScale())
      continue;
    // the conv outputs, the transposes and the slices: used by nothing else
    const Value *oq = getOrigin(cq->getDest()), *ok2 = getOrigin(ck->getDest());
    auto only = [&](const Value *o, std::set<const Instruction *> allowed) {
      for (const Instruction *I : users(o))
        if (!allowed.count(I))
          return false;
      return true;
    };
    std::set<const Instruction *> qc, kc;
    for (size_t i = 0; i < H; i++) {
      qc.insert(q[i].copy);
      kc.insert(k[i].copy);
    }
    ok = only(oq, {cq, q[0].tr}) && only(ok2, {ck, k[0].tr}) && only(getOrigin(q[0].tr->getDest()), [&] {
           auto s = qc;
           s.insert(q[0].tr);
           return s;
         }()) && only(getOrigin(k[0].tr->getDest()), [&] {
           auto s = kc;
           s.insert(k[0].tr);
           return s;
         }());
    for (size_t i = 0; i < H && ok; i++)
      ok = only(viewOrigin(g.mm[i]->getLHS()).first, {q[i].copy, g.mm[i]}) &&
           only(viewOrigin(g.mm[i]->getRHS()).first, {k[i].copy, g.mm[i]});
    if (!ok)
      continue;
    g.qk = true;
    g.qbuf = oq;
    g.kbuf = ok2;
    // the conv outputs were last read by the transposes: their deallocations come before the matmul
    bool before = true;
    for (const Instruction *I : L) {
      if (I == g.mm[0])
        before = false;
      if (auto *d = llvm::dyn_cast<DeallocActivationInst>(I))
        if (before && (d->getSrc() == oq || d->getSrc() == ok2)) {
          ctx->getMatMulAway()->insert(d);
          g.freeAfter.push_back(d->getSrc());
        }
    }
    ctx->getPacked()->insert(oq);
    ctx->getPacked()->insert(cq->getDest());
    ctx->getPacked()->insert(ok2);
    ctx->getPacked()->insert(ck->getDest());
    ctx->getMatMulAway()->insert(q[0].tr);
    ctx->getMatMulAway()->insert(k[0].tr);
    for (size_t i = 0; i < H; i++) {
      ctx->getMatMulAway()->insert(q[i].copy);
      ctx->getMatMulAway()->insert(k[i].copy);
    }
  }
}

static void emitMatMulGroup(const MatMulGroup &g, std::string *bundle, VTASaveContext *ctx) {
  static int id = 0;
  const std::string n = std::to_string(id++), H = std::to_string(g.mm.size());
  auto ptr = [](const Value *v) {
    auto o = viewOrigin(v);
    return o.second ? "(" + o.first->getName().str() + " + " + std::to_string(o.second) + ")" : o.first->getName().str();
  };
  std::string c = "  int8_t *mmC" + n + "[] = {";
  for (size_t i = 0; i < g.mm.size(); i++)
    c += (i ? ", " : "") + ptr(g.mm[i]->getDest());
  *bundle += c + "};\n";
  const auto *M0 = g.mm[0];
  if (g.qk) {
    *bundle += "  // Q K^T of " + H + " heads from the EVTA outputs of the query / key convs (-vta-attn-keep-layout)\n";
    *bundle += "  matmul_evta_qk(" + g.qbuf->getName().str() + "_vbuf, " + g.kbuf->getName().str() + "_vbuf, mmC" + n +
               ", " + H + ", " + std::to_string(M0->getLHS()->dims()[0]) + ", " +
               std::to_string(M0->getLHS()->dims()[1]) + ", " + std::to_string(g.shift) +
               matMulCallTail(ctx, matMulSchedArgs(M0->getLHS()->dims()[0], M0->getLHS()->dims()[1],
                                                   M0->getLHS()->dims()[0])) + ");\n";
    for (const Value *v : g.freeAfter)
      *bundle += "  VTABufferFree(" + v->getName().str() + "_vbuf);\n";
    return;
  }
  std::string a = "  const int8_t *mmA" + n + "[] = {", b = "  const int8_t *mmB" + n + "[] = {";
  for (size_t i = 0; i < g.mm.size(); i++) {
    a += (i ? ", " : "") + ptr(g.mm[i]->getLHS());
    b += (i ? ", " : "") + ptr(g.mm[i]->getRHS());
  }
  *bundle += a + "};\n" + b + "};\n";
  *bundle += "  // " + H + " heads in one EVTA call (-vta-matmul-heads)\n";
  *bundle += "  matmul_evta_n(" + H + ", mmA" + n + ", mmB" + n + ", mmC" + n + ", " +
             std::to_string(M0->getLHS()->dims()[0]) + ", " + std::to_string(M0->getLHS()->dims()[1]) + ", " +
             std::to_string(M0->getRHS()->dims()[1]) + ", " + std::to_string(g.shift) +
             matMulCallTail(ctx, matMulSchedArgs(M0->getLHS()->dims()[0], M0->getLHS()->dims()[1],
                                                 M0->getRHS()->dims()[1])) + ");\n";
}

//#### JO added (2026-11-10): ", vt, tileH, tileW" of an attention matmul's EVTA conv (M rows as h x w over K -> N, both
//#### padded to 16) from VTASchedules.h, or "" (vtalib's default 1, 14, 14) when it has none
static std::string matMulSchedArgs(dim_t M, dim_t K, dim_t N) {
  dim_t w = 1;
  for (dim_t q = 1; q * q <= M; q++)
    if (M % q == 0)
      w = q;
  const int h = M / w, Kp = (K + 15) / 16 * 16, Np = (N + 15) / 16 * 16;
#include "VTASchedules.h"
  for (auto elem : convTune.ConvolutionTune_)
    if (elem.isMatch(1, h, w, Kp, Np, 1, 1, Kp, 1, 0))
      return ", " + std::to_string(elem.nVirtualThread_) + ", " + std::to_string(elem.tileHSize_) + ", " +
             std::to_string(elem.tileWSize_);
  return "";
}

//#### JO added (2026-11-10): ", vtaCmdH<idx>" and the schedule / EVTA index tail of a matmul_evta* call: a multi-EVTA copy
//#### (-index-multi-evta) names its own command handle and passes idEVTA (the vtalib default schedule 1, 14, 14 when the
//#### table has none); a single bundle keeps the old text
static std::string matMulCallTail(VTASaveContext *ctx, const std::string &sched) {
  const uint32_t idx = ctx->getIdxMultiEVTA();
  if (!idx)
    return ", vtaCmdH" + sched;
  return ", vtaCmdH" + std::to_string(idx) + (sched.empty() ? std::string(", 1, 14, 14") : sched) + ", " +
         std::to_string(idx);
}

static std::string vtaOptionsComment() {
  static const char *const names[] = {
      "vta-keep-layout",   "vta-fuse-residual", "vta-evta-maxpool", "vta-evta-fc",
      "vta-fuse-preprocess", "vta-conv0-pack",  "vta-fuse-pool",    "vta-evta-depthwise",
      "vta-round-shift",   "vta-pad-channels",  "vta-matmul-conv", "vta-fold-matmul-bias", "vta-evta-matmul", "vta-lut-elementwise", "vta-fuse-layernorm",
      "vta-matmul-heads", "vta-attn-keep-layout", "vta-cpu-approx",
      "enable-channelwise-depthwise", "enable-channelwise"};
  auto &opts = llvm::cl::getRegisteredOptions();
  std::string s = "// Generated by the NEST-C VTA backend with:";
  for (const char *name : names) {
    auto it = opts.find(name);
    if (it == opts.end())
      continue;
    const bool v = static_cast<llvm::cl::opt<bool> *>(it->second)->getValue();
    s += std::string("\n//   -") + name + "=" + (v ? "1" : "0");
  }
  return s + "\n";
}

void exportBundleEntry(llvm::StringRef outputDir, llvm::StringRef bundleName,
                       struct BundleSaveCtx &bundleCtx) {
  std::string outputFile = outputDir;
  if (outputFile[outputFile.size() - 1] != '/')
    outputFile.append("/");
  outputFile.append(bundleName);
  outputFile.append(".cpp");

  std::ofstream fos(outputFile.c_str(), std::ios::out);
  const std::string optionsComment = vtaOptionsComment();
  fos.write(optionsComment.c_str(), optionsComment.size());
  fos.write(VTA_SAVE_COMMON.c_str(), VTA_SAVE_COMMON.size());
  fos.write(bundleCtx.includeHeader.c_str(), bundleCtx.includeHeader.size());
  auto &symbolTable = bundleCtx.symbolTable;
  fos.write(symbolTable.c_str(), symbolTable.size());
  auto &bundleConfig = bundleCtx.bundleConfig;
  fos.write(bundleConfig.c_str(), bundleConfig.size());
  fos.write(bundleCtx.initConst.c_str(), bundleCtx.initConst.size());
  fos.write(bundleCtx.destroyConst.c_str(), bundleCtx.destroyConst.size());
  fos.write(bundleCtx.bundle.c_str(), bundleCtx.bundle.size());
  fos.close();
}

void exportVTARuntimeHeader(llvm::StringRef outputDir) {
  std::string vtaRuntimeHeaderFile = outputDir;
  vtaRuntimeHeaderFile.append("/");
  vtaRuntimeHeaderFile.append("VTARuntime");
  vtaRuntimeHeaderFile.append(".h");

  std::ofstream ros(vtaRuntimeHeaderFile.c_str(), std::ios::out);
  ros.write(VTA_RUNTIME_HEADER.c_str(), VTA_RUNTIME_HEADER.size());
  ros.close();
}

void exportBundleHeader(llvm::StringRef outputDir, llvm::StringRef bundleName,
                        llvm::StringRef mainEntryName, VTASaveContext &ctx) {
  std::string vtaBundleHeaderFile = outputDir;
  vtaBundleHeaderFile.append("/");
  vtaBundleHeaderFile.append(mainEntryName);
  vtaBundleHeaderFile.append(".h");

  std::ofstream bhos(vtaBundleHeaderFile.c_str(), std::ios::out);
  std::string bundleHeaderDefineName = "VTA_BUNDLE_";
  bundleHeaderDefineName.append(bundleName);
  std::transform(bundleHeaderDefineName.begin(), bundleHeaderDefineName.end(),
                 bundleHeaderDefineName.begin(), ::toupper);

  std::string bundleHeaderDefine = "#ifndef ";
  bundleHeaderDefine.append(bundleHeaderDefineName);
  bundleHeaderDefine.append("\n#define ");
  bundleHeaderDefine.append(bundleHeaderDefineName);
  bundleHeaderDefine.append("\n");

  bhos.write(bundleHeaderDefine.c_str(), bundleHeaderDefine.size());
  bhos.write(VTA_BUNDLE_HEADER_0.c_str(), VTA_BUNDLE_HEADER_0.size());
  std::string bundleHeader = "";
  bundleHeader.append("extern BundleConfig ");
  bundleHeader.append(bundleName);
  bundleHeader.append("_config;\n");
  bundleHeader.append("void ");
  bundleHeader.append(mainEntryName);
  bundleHeader.append("_load_module(uint8_t *constantWeight);\n");
  bundleHeader.append("void ");
  bundleHeader.append(mainEntryName);
  bundleHeader.append("_destroy_module();\n");
  bundleHeader.append("int ");
  bundleHeader.append(mainEntryName);
#ifdef NESTC_EVTA_PROFILE_AUTOTUNE
  bundleHeader.append(
      "(uint8_t *constantWeight, uint8_t *mutableWeight, uint8_t *activations, "
      "int input1, int input2, int input3);\n");
#else
  bundleHeader.append("(uint8_t *constantWeight, uint8_t *mutableWeight, "
                      "uint8_t *activations);\n");
#endif
  std::string bundleHeaderPHInfo = R"~(
// ---------------------------------------------------------------
//                          Bundle API
// ---------------------------------------------------------------
)~";
  bundleHeaderPHInfo.append("// Model name: \"");
  bundleHeaderPHInfo.append(bundleName);
  bundleHeaderPHInfo.append("\"\n");
  bundleHeaderPHInfo.append("// Total data size: ");
  bundleHeaderPHInfo.append(std::to_string(ctx.getMMemSize()));
  bundleHeaderPHInfo.append("(bytes)\n");
  bundleHeaderPHInfo.append("// Placeholders:\n");

  for (auto ste : *ctx.getSymbols()) {
    auto wgt = ste.wgt;
    assert(wgt);
    bundleHeaderPHInfo.append("//   Name: \"");
    bundleHeaderPHInfo.append(ste.name);
    bundleHeaderPHInfo.append("\"\n");
    bundleHeaderPHInfo.append("//   Size: ");
    bundleHeaderPHInfo.append(std::to_string(wgt->size()));
    bundleHeaderPHInfo.append(" (elements)\n");
    bundleHeaderPHInfo.append("//   Size: ");
    bundleHeaderPHInfo.append(std::to_string(wgt->getSizeInBytes()));
    bundleHeaderPHInfo.append(" (bytes)\n");
    bundleHeaderPHInfo.append("//   Offset: ");
    bundleHeaderPHInfo.append(std::to_string(ste.offset));
    bundleHeaderPHInfo.append(" (bytes)\n//\n");
  }
  bhos.write(bundleHeaderPHInfo.c_str(), bundleHeaderPHInfo.size());
  bhos.write(bundleHeader.c_str(), bundleHeader.size());
  bhos.write(VTA_BUNDLE_HEADER_1.c_str(), VTA_BUNDLE_HEADER_1.size());

  std::string bundleHeaderEndif = "#endif //";
  bundleHeaderEndif.append(bundleHeaderDefineName);
  bundleHeaderEndif.append("\n");
  bhos.write(bundleHeaderEndif.c_str(), bundleHeaderEndif.size());

  bhos.close();
}

}
using namespace vta_save;

void VTA::save(Function *F, llvm::StringRef outputDir,
               llvm::StringRef bundleName,
               llvm::StringRef mainEntryName) const {
  if (((idxMultiEVTA & 1) + ((idxMultiEVTA >> 1) & 1) +
       ((idxMultiEVTA >> 2) & 1) + ((idxMultiEVTA >> 3) & 1)) != 1) {
    llvm::errs() << "Not supported Multi-EVTA combination\n"
                    "Please use one EVTA at one time\n";
    std::exit(1);
  }

  auto IR = generateAndOptimizeIR(F, *this, shouldShareBuffers());
  VTASaveContext ctx(&IR->getVariableMap());

  for (int i = 0; i < 4; i++) {
    if (((idxMultiEVTA >> i) & 1) == 1) {
      ctx.setIdxMultiEVTA(i);
      break;
    }
  }

  std::string weightFileName = outputDir;
  weightFileName.append("/");
  weightFileName.append(bundleName);
  weightFileName.append(".weights.bin");
  std::ofstream wfos(weightFileName.c_str(), std::ios::binary);
  ctx.setWeightFileStream(&wfos);

  struct BundleSaveCtx bundleCtx;
  auto &bundle = bundleCtx.bundle;
  auto &initConst = bundleCtx.initConst;
  auto &destroyConst = bundleCtx.destroyConst;

  initBundleSave(bundleCtx, bundleName, mainEntryName);
  findPackedActivations(IR.get(), &ctx);
  findResidualFusions(IR.get(), &ctx);
  findEvtaMaxPools(IR.get(), &ctx);
  findInputPreprocess(IR.get(), &ctx);
  findPoolFusions(IR.get(), &ctx);
  findLutRegions(IR.get(), &ctx);
  findSoftmaxI8(IR.get(), &ctx);
  findLayerNorms(IR.get(), &ctx);
  findMatMulGroups(IR.get(), &ctx);
  findAttnKeepLayout(IR.get(), &ctx);

  for (const auto &I : IR->getInstrs()) {

#ifdef VTA_PROFILE
    if (I.getKind() != Kinded::Kind::DebugPrintInstKind) {
      bundle.append("  clock_t ");
      bundle.append(I.getName());
      bundle.append("_start = clock();\n");
    }
#endif

    bundle.append("\n  //Run ");
    bundle.append(I.getKindName());
    bundle.append(" : ");
    bundle.append(I.getName());
    bundle.append("\n");

    //#### JO added (2026-11-10): findMatMulGroups
    if (ctx.matMulAway.count(&I)) {
      bundle.append("  // done by the matmul of the heads (matmul_evta_n / matmul_evta_qk)\n");
      continue;
    }
    if (ctx.matMulGroups.count(&I)) {
      emitMatMulGroup(ctx.matMulGroups[&I], &bundle, &ctx);
      continue;
    }
    //#### JO added (2026-11-10): findLayerNorms
    if (ctx.layerNormAway.count(&I)) {
      bundle.append("  // done by the layer norm call below (layerNormI8)\n");
      continue;
    }
    if (ctx.layerNorms.count(&I)) {
      emitLayerNormI8(ctx.layerNorms[&I], &bundle, &ctx);
      continue;
    }
    //#### JO added (2026-11-09): findSoftmaxI8
    if (ctx.softmaxAway.count(&I)) {
      bundle.append("  // done at the dequantize before (softmaxI8)\n");
      continue;
    }
    if (ctx.softmaxAtDq.count(&I)) {
      emitSoftmaxI8(llvm::cast<DequantizeInst>(&I), ctx.softmaxAtDq[&I], &bundle, &ctx);
      continue;
    }
    //#### JO added (2026-11-09): an elementwise region as a table (findLutRegions)
    if (ctx.isLutAway(&I)) {
      bundle.append("  // done by the table of the elementwise region before (lut8)\n");
      continue;
    }
    {
      auto it = ctx.getLutRegions()->find(&I);
      if (it != ctx.getLutRegions()->end()) {
        emitLutRegion(it->second, &bundle, &ctx);
        continue;
      }
    }
    //#### JO added (2026-10-31): elementadd / relu done by the fused conv before them
    if (ctx.isFusedAway(&I)) {
      bundle.append("  // done by the EVTA conv before (convolution_wo_tr_res / convolution_wo_tr_pool)\n");
      continue;
    }
    //#### JO added (2026-11-02): quantize / elemsub / elemdiv / transpose done by the first conv's input copy
    if (ctx.isPreFused(&I)) {
      bundle.append("  // done in the input copy of the first conv (preprocess_nhwc2vtaio_zerofill or preprocess_conv0pack)\n");
      continue;
    }

    switch (I.getKind()) {
    case Kinded::Kind::ConvolutionInstKind: {
      auto I2 = llvm::cast<ConvolutionInst>(&I);
      //#### JO added (2026-10-31): the dest of the relu of a fused conv, allocated after the conv in the IR
      if (auto *fu = ctx.getFusion(I2))
        if (fu->hoistOut && ctx.getHoisted()->insert(fu->out).second)
          emitPackedAlloc(fu->out, &bundle);
      //#### JO added (2026-11-02): the dest of the max pool done by the first conv (findPoolFusions)
      if (auto *pf = ctx.getPoolFusion(I2))
        if (pf->hoistOut && ctx.getHoisted()->insert(pf->out).second)
          emitPackedAlloc(pf->out, &bundle);
      saveConvolutionInst(I2, &bundle, &initConst, &ctx);
      break;
    }
    case Kinded::Kind::BNNConvolutionInstKind: {
      auto I2 = llvm::cast<BNNConvolutionInst>(&I);
      saveBNNConvolutionInst(I2, &bundle, &initConst, &ctx, BNNWithScale);
      break;
    }
    //#### JO added (2026-11-07): -enable-channelwise-depthwise (vtalib cw_convolution on the CPU)
    case Kinded::Kind::ChannelwiseQuantizedConvolutionInstKind: {
      saveChannelwiseConvolutionInst(llvm::cast<ChannelwiseQuantizedConvolutionInst>(&I), &bundle, &ctx);
      break;
    }
    case Kinded::Kind::VTAConvolutionInstKind: {
      auto I2 = llvm::cast<VTAConvolutionInst>(&I);
      saveEVTAConvolutionInst(I2, &bundle, &initConst, &ctx);
      break;
    }
    case Kinded::Kind::AllocActivationInstKind: {
      auto I2 = llvm::cast<AllocActivationInst>(&I);
#ifndef VTA_MEMOPT_DISABLE
      //#### JO added (2026-10-31): an activation kept in the EVTA layout is a VTA buffer
      //#### JO changed (2026-10-31): not again if a fused conv emitted it before (ResidualFusion::hoistOut)
      if (ctx.getHoisted()->count(I2)) {
        bundle.append("  // allocated before the fused conv\n");
      } else if (ctx.isPacked(I2)) {
        emitPackedAlloc(I2, &bundle);
      } else {
        bundle.append("  int8_t *");
        bundle.append(I2->getName());
        bundle.append(" = (int8_t *)malloc(");
        bundle.append(std::to_string(I2->getSizeInBytes()));
        bundle.append(");\n");
      }
#endif
      // for not constant but represented as a constant value
      auto csyms = ctx.getConstantSymbols();
      csyms->push_back({I2->getName().data(), 0, I2->getSizeInBytes(), '1'});

      break;
    }
    case Kinded::Kind::DeallocActivationInstKind: {
#ifndef VTA_MEMOPT_DISABLE
      auto I2 = llvm::cast<DeallocActivationInst>(&I);
      if (ctx.isPacked(I2->getSrc())) {
        bundle.append("  VTABufferFree(");
        bundle.append(I2->getSrc()->getName());
        bundle.append("_vbuf);\n");
      } else {
        bundle.append("  free(");
        bundle.append(I2->getSrc()->getName());
        bundle.append(");\n");
      }
#endif
      break;
    }
    case Kinded::Kind::MaxPoolInstKind: {
      auto I2 = llvm::cast<MaxPoolInst>(&I);
      saveMaxPoolInst(I2, &bundle, &ctx);
      break;
    }
    case Kinded::Kind::QuantizeInstKind: {
      auto I2 = llvm::cast<QuantizeInst>(&I);
      saveQuantizeInst(I2, &bundle, &ctx);
      break;
    }
    case Kinded::Kind::TransposeInstKind: {
      auto I2 = llvm::cast<TransposeInst>(&I);
      auto srcDimSize = I2->getSrc()->dims().size();
      //#### JO added (2026-11-09): vtalib transpose / transpose_6dim move int8 elements of 4 / 6 dims; others (DeiT: float,
      //#### 3 dims) by the generic CPU transpose
      const bool i8 = I2->getSrc()->getType()->getElementSize() == 1;

      if (!i8 || (srcDimSize != 4 && srcDimSize != 6)) {
        saveGenericCpuInst(I, &bundle, &ctx);
      } else if (srcDimSize == 4) {
        saveTransposeInst(I2, &bundle, &ctx);
      } else if (srcDimSize == 6) {
        saveTranspose_6dim(I2, &bundle, &ctx);
      } else {
        //#### JO changed (2026-11-06): + the instruction
        llvm::errs() << "Not supported dimenstion in transpose: " << I2->getName() << " " << srcDimSize << " dims\n"
                        "Please use 4 dim or 6 dim\n";
        std::exit(1);
      }

      break;
    }
    case Kinded::Kind::SplatInstKind: {
      auto I2 = llvm::cast<SplatInst>(&I);
      //#### JO added (2026-11-09): a float splat (DeiT: the 0.5 exponent of the layer norm sqrt) fills floats
      if (I2->getDest()->getType()->getElementType() == ElemKind::FloatTy)
        saveGenericCpuInst(I, &bundle, &ctx);
      else
        saveSplatInst(I2, &bundle, &ctx);
      break;
    }
    case Kinded::Kind::ElementMaxInstKind: {
      auto I2 = llvm::cast<ElementMaxInst>(&I);
      saveElemMaxInst(I2, &bundle, &ctx);
      break;
    }
    case Kinded::Kind::ElementAddInstKind: {
      auto I2 = llvm::cast<ElementAddInst>(&I);
      saveElemAddInst(I2, &bundle, &ctx);
      break;
    }
    case Kinded::Kind::ElementSubInstKind: {
      auto I2 = llvm::cast<ElementSubInst>(&I);
      saveElemSubInst(I2, &bundle, &ctx);
      break;
    }
    case Kinded::Kind::ElementDivInstKind: {
      auto I2 = llvm::cast<ElementDivInst>(&I);
      saveElemDivInst(I2, &bundle, &ctx);
      break;
    }
    case Kinded::Kind::AvgPoolInstKind: {
      auto I2 = llvm::cast<AvgPoolInst>(&I);
      saveAvgPoolInst(I2, &bundle, &ctx);
      break;
    }
    case Kinded::Kind::TensorViewInstKind: {
      auto I2 = llvm::cast<TensorViewInst>(&I);
      saveTensorViewInst(I2, &bundle, &ctx);

      // for not constant but represented as a constant value
      auto csyms = ctx.getConstantSymbols();
      csyms->push_back({I2->getName().data(), 0, I2->getSizeInBytes(), '1'});
      break;
    }
    case Kinded::Kind::FullyConnectedInstKind: {
      auto I2 = llvm::cast<FullyConnectedInst>(&I);
      saveFullyConnectedInst(I2, &bundle, &ctx);
      break;
    }
    case Kinded::Kind::DequantizeInstKind: {
      auto I2 = llvm::cast<DequantizeInst>(&I);
      saveDequantizeInst(I2, &bundle, &ctx);
      break;
    }
    case Kinded::Kind::SoftMaxInstKind: {
      auto I2 = llvm::cast<SoftMaxInst>(&I);
      //#### JO added (2026-11-09): vtalib softmax steps rows by inDim0 (right for the 1 x classes softmax of the CNNs only);
      //#### more rows (DeiT attention: 594 x 198) by the generic CPU softmax
      if (I2->getSrc()->dims()[0] > 1)
        saveGenericCpuInst(I, &bundle, &ctx);
      else
        saveSoftMaxInst(I2, &bundle, &ctx);
      break;
    }
    case Kinded::Kind::ReluInstKind: {
      auto I2 = llvm::cast<ReluInst>(&I);
      saveReluInst(I2, &bundle, &ctx);
      break;
    }
    case Kinded::Kind::DebugPrintInstKind: {
      auto I2 = llvm::cast<DebugPrintInst>(&I);
      saveDebugPrintInst(I2, &bundle, &ctx);
      break;
    }
    case Kinded::Kind::ElementSignInstKind: {
      auto I2 = llvm::cast<ElementSignInst>(&I);
      saveElemSignInst(I2, &bundle, &ctx);
      break;
    }
    default:
      //#### JO changed (2026-11-09): the generic CPU ops (matmul, copies, layer norm / GELU pieces); others stop with the
      //#### instruction's name (llvm_unreachable was a segfault in the Release build)
      if (!saveGenericCpuInst(I, &bundle, &ctx)) {
        llvm::errs() << "VTASave: " << I.getKindName() << " " << I.getName() << " is an unhandled instruction\n";
        std::exit(1);
      }
    }
    //#### JO added (2026-10-31): a CPU op that wrote an activation kept in the EVTA layout flushes it
    //#### for the EVTA (non-coherent; the EVTA conv invalidates its output for the CPU)
    //#### JO changed (2026-11-08): + the depthwise channelwise conv (cw_convolution)
    if ((I.getKind() == Kinded::Kind::ElementAddInstKind ||
         I.getKind() == Kinded::Kind::ChannelwiseQuantizedConvolutionInstKind ||
         I.getKind() == Kinded::Kind::ReluInstKind ||
         I.getKind() == Kinded::Kind::MaxPoolInstKind ||
         I.getKind() == Kinded::Kind::AvgPoolInstKind) &&
        !ctx.isEvtaMaxPool(&I) && !ctx.isEvtaDepthwise(&I)) {
      for (const auto &op : I.getOperands()) {
        if (op.second != OperandKind::In && ctx.isPacked(op.first)) {
          bundle.append("  VTABufferFlush(");
          bundle.append(op.first->getName());
          bundle.append("_vbuf, 0, ");
          bundle.append(std::to_string(op.first->getSizeInBytes()));
          bundle.append(");\n");
        }
      }
    }
#ifdef VTA_PROFILE
    if (I.getKind() != Kinded::Kind::DebugPrintInstKind) {
      bundle.append("  clock_t ");
      bundle.append(I.getName());
      bundle.append("_end = clock();\n");
      bundle.append("  prof_out<<\"");
      bundle.append(I.getKindName());
      bundle.append(":");
      bundle.append(I.getName());
      bundle.append(" : \"<<");
      bundle.append("(double)(");
      bundle.append(I.getName());
      bundle.append("_end - ");
      bundle.append(I.getName());
      bundle.append("_start)/CLOCKS_PER_SEC*1000 << std::endl;\n");
    }
#endif
  }
#ifdef VTA_PROFILE
  bundle.append("  prof_out.close();\n");
#endif
  wfos.close();
  finalizeBundleEntrySave(bundleCtx, bundleName, mainEntryName, ctx);
  exportBundleEntry(outputDir, bundleName, bundleCtx);
  exportVTARuntimeHeader(outputDir);
  exportBundleHeader(outputDir, bundleName, mainEntryName, ctx);

  return;
}
