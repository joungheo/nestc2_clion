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
#include "glow/IR/Instrs.h"
#include "glow/Optimizer/IROptimizer/IROptimizer.h"
#include "llvm/Support/CommandLine.h"
#include <cmath>
#include <cstring>
#include <fstream>
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
    llvm::cl::init(false));

namespace vta_save{

//#### JO added (2026-10-31): an EVTA conv that also does the elementadd and the relu after it
struct ResidualFusion {
  const Value *residual; // the other elementadd operand
  const Value *out;      // relu dest: written by the fused call
  int convShl, resShl, sumShr;
  bool hoistOut;         // out is allocated after the conv: its allocation is emitted before the conv
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

  bool doRelu = Inst->getFusedActivation() == RELU;
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

  assert(biasScale == inScale * filterScale);

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

  assert(biasScale == inScale * filterScale);

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
  assert(tensorBias);
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

  #ifdef VTA_PROFILE
    bundle->append("  clock_t ");
    bundle->append(Inst->getName());
    bundle->append("_input_transpose_start = clock();\n");
  #endif

    bundle->append("  int8_t* ");
    bundle->append(Inst->getName());
    bundle->append("_input_transpose = (int8_t *)VTABufferAlloc(");
    //bundle->append(std::to_string(N*H*W*C));
    bundle->append(std::to_string(N*H*W*C));
    bundle->append(");\n");
    bundle->append("  transpose_nhwc2vtaio_zerofill(");
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
    bundle->append(fu ? fu->out->getName() : dest->getName());
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
  //#### JO changed (2026-10-31): conv + elementadd + relu (findResidualFusions)
  // bundle->append("  convolution_wo_tr(");
  auto *fu = ctx->getFusion(Inst);
  bundle->append(fu ? "  convolution_wo_tr_res(" : "  convolution_wo_tr(");
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
  if (fu) {
    bundle->append(", ");
    bundle->append(fu->residual->getName());
    bundle->append("_vbuf, ");
    bundle->append(std::to_string(fu->convShl));
    bundle->append(", ");
    bundle->append(std::to_string(fu->resShl));
    bundle->append(", ");
    bundle->append(std::to_string(fu->sumShr));
    bundle->append(", 1");
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
  bool doRelu = Inst->getFusedActivation()==RELU;
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
  bundle->append(");\n");
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

  if (Inst->getGroup() == 1 && scale > 1 && srcDims[3] % 16 == 0 &&
      filterDims[0] % 16 == 0 && biasScale == matMulScale && outOffset == 0 &&
      inOffset == 0 && filterOffset == 0 && biasOffset == 0 && shift >= 0) {
    return ConvPath::VTA;
  } else if (Inst->getGroup() == srcDims[3] && scale > 1 &&
             biasScale == matMulScale && outOffset == 0 && inOffset == 0 &&
             filterOffset == 0 && biasOffset == 0 && shift >= 0) {
    return ConvPath::DepthwiseCPU;
  //#### JO added (2026-10-29): a quantized 3-channel conv (the image conv0 when it is not skipped from
  //#### quantization) runs on EVTA with its input and filter zero-filled to 16 channels, the path of the
  //#### first BNN conv; it is plain int8 (convolution_wo_tr), so it does not need VTA_BNN
  } else if (Inst->getGroup() == 1 && scale > 1 && srcDims[3] == 3 &&
             filterDims[3] == 3 && filterDims[0] % 16 == 0 &&
             biasScale == matMulScale && outOffset == 0 && inOffset == 0 &&
             filterOffset == 0 && biasOffset == 0 && shift >= 0) {
    return ConvPath::FirstBNN;
  }
  return ConvPath::Other;
}

void saveConvolutionInst(const glow::ConvolutionInst *Inst, std::string *bundle,
                         std::string *initConst, VTASaveContext *ctx) {
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
//#### the scales (sD / sO = 2^-a, sR / sO = 2^-b, offsets 0), the relu with equal scales, O' not the conv input,
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
    if (tD->getOffset() || tR->getOffset() || tO->getOffset() || tO2->getOffset() ||
        tO->getScale() != tO2->getScale())
      continue;
    int eD, eR; // sD / sO = 2^eD, sR / sO = 2^eR
    if (!powerOfTwoExp(tD->getScale() / tO->getScale(), &eD) ||
        !powerOfTwoExp(tR->getScale() / tO->getScale(), &eR))
      continue;
    int m = std::max(0, std::max(-eD, -eR));
    ResidualFusion f = {R, O2, m + eD, m + eR, m, hoist};
    if (f.convShl > 8 || f.resShl > 6 || f.sumShr > 15)
      continue;
    (*ctx->getFusions())[C] = f;
    ctx->getFusedAway()->insert(E);
    ctx->getFusedAway()->insert(Rl);
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
                   (C->getFusedActivation() == RELU || ctx->getFusion(C));
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

void saveFullyConnectedInst(const glow::FullyConnectedInst *Inst,
                            std::string *bundle, VTASaveContext *ctx) {
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

  assert(biasScale == inScale * filterScale);

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
    else {
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
    else {
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
    } else {
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
    } else {
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

  initConst.append("\nvoid ");
  initConst.append(mainEntryName);
  initConst.append("_load_module(uint8_t *constantWeight){\n");
  // initConst.append("  xlnk_reset();\n");
  // initConst.append("  vtaCmdH = VTATLSCommandHandle();\n");

  for (auto ste : *ctx.getConstantSymbols()) {
    if (ste.kind == '0') {
      initConst.append("  ");
      initConst.append(ste.name);
      initConst.append(" = (int8_t *)VTABufferAlloc(");
      initConst.append(std::to_string(ste.size));
      initConst.append(");\n");
      initConst.append("  VTABufferCopy((int8_t *)(constantWeight + ");
      initConst.append(std::to_string(ste.offset));
      initConst.append("), 0, ");
      initConst.append(ste.name);
      initConst.append(", 0, ");
      initConst.append(std::to_string(ste.size));
      initConst.append(", 1);\n");
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
      destroyConst.append("VTABufferFree(");
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

void exportBundleEntry(llvm::StringRef outputDir, llvm::StringRef bundleName,
                       struct BundleSaveCtx &bundleCtx) {
  std::string outputFile = outputDir;
  if (outputFile[outputFile.size() - 1] != '/')
    outputFile.append("/");
  outputFile.append(bundleName);
  outputFile.append(".cpp");

  std::ofstream fos(outputFile.c_str(), std::ios::out);
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

    //#### JO added (2026-10-31): elementadd / relu done by the fused conv before them
    if (ctx.isFusedAway(&I)) {
      bundle.append("  // done by the EVTA conv before (convolution_wo_tr_res)\n");
      continue;
    }

    switch (I.getKind()) {
    case Kinded::Kind::ConvolutionInstKind: {
      auto I2 = llvm::cast<ConvolutionInst>(&I);
      //#### JO added (2026-10-31): the dest of the relu of a fused conv, allocated after the conv in the IR
      if (auto *fu = ctx.getFusion(I2))
        if (fu->hoistOut && ctx.getHoisted()->insert(fu->out).second)
          emitPackedAlloc(fu->out, &bundle);
      saveConvolutionInst(I2, &bundle, &initConst, &ctx);
      break;
    }
    case Kinded::Kind::BNNConvolutionInstKind: {
      auto I2 = llvm::cast<BNNConvolutionInst>(&I);
      saveBNNConvolutionInst(I2, &bundle, &initConst, &ctx, BNNWithScale);
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

      if (srcDimSize == 4) {
        saveTransposeInst(I2, &bundle, &ctx);
      } else if (srcDimSize == 6) {
        saveTranspose_6dim(I2, &bundle, &ctx);
      } else {
        llvm::errs() << "Not supported dimenstion in transpose\n"
                        "Please use 4 dim or 6 dim\n";
        std::exit(1);
      }

      break;
    }
    case Kinded::Kind::SplatInstKind: {
      auto I2 = llvm::cast<SplatInst>(&I);
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
      std::string msg = I.getKindName();
      msg.append(" is an unhandled instruction");

      llvm_unreachable(msg.c_str());
    }
    //#### JO added (2026-10-31): a CPU op that wrote an activation kept in the EVTA layout flushes it
    //#### for the EVTA (non-coherent; the EVTA conv invalidates its output for the CPU)
    if ((I.getKind() == Kinded::Kind::ElementAddInstKind ||
         I.getKind() == Kinded::Kind::ReluInstKind ||
         I.getKind() == Kinded::Kind::MaxPoolInstKind ||
         I.getKind() == Kinded::Kind::AvgPoolInstKind) &&
        !ctx.isEvtaMaxPool(&I)) {
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
