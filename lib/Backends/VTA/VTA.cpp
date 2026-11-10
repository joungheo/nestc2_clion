/**
 * Copyright (c) Glow Contributors. See CONTRIBUTORS file.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 * Modifications copyright (C) 2022 <ETRI/Yongin Kwon>
 */

#include "VTA.h"
#include <cmath>
#include <cstring>
#include "VTAFunction.h"

#include "VTACodeGen/VTASaver.h"
#include "glow/Backend/BackendUtils.h"
#include "glow/CodeGen/MemoryAllocator.h"
#include "glow/Flags/Flags.h"
#include "glow/Graph/Graph.h"
#include "glow/Graph/Nodes.h"
#include "glow/IR/IR.h"
#include "glow/IR/Instrs.h"
#include "glow/Optimizer/IROptimizer/IROptimizer.h"
#include "llvm/Support/CommandLine.h"

#include <cstring>
#include <set>
#include <unordered_set>

using namespace glow;
using llvm::dyn_cast;

//#### JO added (2026-11-07): widen int8 activations whose channel count is not a multiple of 16 (MobileNetV2: 24) to the
//#### next multiple when every producer and user is a group-1 conv (constant filter and bias) or an Add of such
//#### activations: the producing convs get zero filter rows and zero bias, the using convs zero filter columns, so the
//#### extra channels stay 0 (offsets are 0 in the symmetric schemas) and the convs take the EVTA path (padChannelsTo16)
static llvm::cl::opt<bool> vtaPadChannels(
    "vta-pad-channels",
    llvm::cl::desc("Widen conv / add activations with C % 16 != 0 to a multiple of 16 channels (zero weights) so "
                   "that their convs run on the EVTA"),
    llvm::cl::init(true));

//#### JO added (2026-11-09): MatMul in int8 (DeiT: the linear layers and the attention products); a MatMul with a
//#### constant right-hand side K x N and K, N multiples of 16 becomes a 1 x 1 conv (matMulToConv) over its M rows laid
//#### out as an H x W image (H * W = M, W the largest divisor <= sqrt(M)), so it runs on the EVTA; the others run on
//#### the CPU in int8 (VTASave nestc_cpu::matmulI8)
static llvm::cl::opt<bool> vtaMatMulConv(
    "vta-matmul-conv",
    llvm::cl::desc("Quantize MatMul to int8 and run a MatMul with a constant right-hand side as an EVTA 1x1 conv"),
    llvm::cl::init(true));

//#### JO added (2026-11-09): the Add of a constant bias (one row repeated) after a MatMul that becomes a conv (DeiT Linear):
//#### folded into the conv's int32 bias when the int8 bias times its scale ratio is an integer; the conv then writes the
//#### Add's type (one rounding instead of the conv's and the Add's)
//#### JO added (2026-11-10): with a non-power-of-two quantization schema (-quantization-schema=symmetric) the EVTA's shift-only
//#### requantize still works when in * w / out is a power of two, not each scale: every int8 Convolution / FullyConnected /
//#### MatMul gets its output scale rounded up to in * w * 2^k (the other tensors keep their free scales; the layout-only
//#### users after it (Reshape / Transpose / Slice / Concat with constant other inputs, requantized) take the same scale).
//#### No effect on a power-of-two graph. DeiT ImageNetV2 (PC emulation, imagenet_eval/fakequant.py --ratio out):
//#### all power-of-two 58.34 % -> 60.51 %, free scales 60.66 % (docs/deit_optimization.md 6.6)
static llvm::cl::opt<bool> vtaPow2Ratio(
    "vta-pow2-ratio",
    llvm::cl::desc("Round the output scale of int8 convs / FCs / matmuls up so that in * w / out is a power of two "
                   "(for -quantization-schema=symmetric)"),
    llvm::cl::init(true));

static llvm::cl::opt<bool> vtaFoldMatMulBias(
    "vta-fold-matmul-bias",
    llvm::cl::desc("Fold the constant bias Add after a MatMul run as an EVTA conv into the conv bias"),
    llvm::cl::init(true));

Expected<std::unique_ptr<CompiledFunction>>
VTA::compile(Function *F, const BackendOptions &opts) const {
  TraceInfo traceInfo = buildManualTraceInfo(F);
  auto IR = generateAndOptimizeIR(F, *this, shouldShareBuffers());

  if (!opts.backendSpecificOpts.empty()) {
    parseBackendSpecificOptions(opts);
  }

  if (opts.autoInstrument) {
    autoInstrument(traceInfo, IR.get());
  }

  std::unique_ptr<CompiledFunction> compiledFunc;
  if (opts.collectConstants) {
    compiledFunc = compileIR(std::move(IR));
  } else {
    compiledFunc = compileIRWithoutConstants(std::move(IR));
  }

  compiledFunc->setTraceInfo(std::move(traceInfo));
  return Expected<std::unique_ptr<CompiledFunction>>(std::move(compiledFunc));
}

std::unique_ptr<CompiledFunction>
VTA::compileIR(std::unique_ptr<IRFunction> IR) const {
  auto *mod = IR->getParent();
  auto function = compileIRWithoutConstants(std::move(IR));
  auto IFunction = static_cast<VTAFunction *>(function.get());
  IFunction->collectConstants(mod);
  return function;
}

std::unique_ptr<CompiledFunction>
VTA::compileIRWithoutConstants(std::unique_ptr<IRFunction> IR) const {
  MemoryAllocator constantWeightsAllocator("ConstantWeights", 0);
  MemoryAllocator placeholderWeightsAllocator("PlaceholderWeights", 0);
  MemoryAllocator activationsAllocator("Activations", 0);
  runtime::RuntimeBundle bundle = runtime::RuntimeBundle::create(
      *IR, constantWeightsAllocator, placeholderWeightsAllocator,
      activationsAllocator);
  auto compiledFunction =
      glow::make_unique<VTAFunction>(std::move(IR), std::move(bundle));
  compiledFunction->setIRInstructionProcessingHandler(
      getIRInstructionProcessingHandler());
  return compiledFunction;
}

bool VTA::isOpSupported(const NodeInfo &NI) const {
  switch (NI.getKind()) {
  case Kinded::Kind::AddNodeKind:
  case Kinded::Kind::SubNodeKind:
  case Kinded::Kind::DivNodeKind:
  case Kinded::Kind::ReluNodeKind:
  //#### JO added (2026-11-06): Clip in int8, so that the quantizer keeps it quantized and it fuses into the conv before it
  case Kinded::Kind::ClipNodeKind:
    return NI.allInputsAndOutputsHaveSameElemKind(
        {ElemKind::FloatTy, ElemKind::Float16Ty, ElemKind::BFloat16Ty,
         ElemKind::Int8QTy, ElemKind::Int32ITy, ElemKind::Int64ITy});

  //#### JO added (2026-11-09): -vta-matmul-conv (float MatMul stays unsupported as before: it has no lowering)
  case Kinded::Kind::MatMulNodeKind:
    return vtaMatMulConv && NI.allInputsAndOutputsHaveSameElemKind({ElemKind::Int8QTy});

  case Kinded::Kind::AvgPoolNodeKind:
    return NI.allInputsAndOutputsHaveSameElemKind(
        {ElemKind::Int32ITy, ElemKind::FloatTy, ElemKind::Float16Ty,
         ElemKind::BFloat16Ty, ElemKind::Int8QTy});

  case Kinded::Kind::FullyConnectedNodeKind:
    if (!NI.getInTy(FullyConnectedNode::InputIdx)->isQuantizedType()) {
      return NI.allInputsAndOutputsHaveSameElemKind(
          {ElemKind::FloatTy, ElemKind::Float16Ty, ElemKind::BFloat16Ty});
    }
    return (NI.allInputsAndOutputsHaveSameElemKind(
                {ElemKind::Int8QTy}, {FullyConnectedNode::BiasIdx}) &&
            (NI.getInElemTy(FullyConnectedNode::BiasIdx) == ElemKind::Int8QTy ||
             NI.getInElemTy(FullyConnectedNode::BiasIdx) ==
                 ElemKind::Int32QTy ||
             NI.getInElemTy(FullyConnectedNode::BiasIdx) ==
                 ElemKind::FloatTy)) ||
           (NI.allInputsAndOutputsHaveSameElemKind(
                {ElemKind::Int16QTy}, {FullyConnectedNode::BiasIdx}) &&
            (NI.getInElemTy(FullyConnectedNode::BiasIdx) ==
                 ElemKind::Int16QTy ||
             NI.getInElemTy(FullyConnectedNode::BiasIdx) ==
                 ElemKind::Int32QTy));

  case Kinded::Kind::MaxPoolNodeKind:
    return NI.allInputsAndOutputsHaveSameElemKind(
               {ElemKind::FloatTy, ElemKind::Float16Ty, ElemKind::BFloat16Ty,
                ElemKind::Int8QTy},
               {}, {MaxPoolNode::ArgmaxIdx}) &&
           (NI.getOutElemTy(MaxPoolNode::ArgmaxIdx) == ElemKind::Int64ITy);

  //#### JO added (2026-11-10): Slice / Concat in int8 too (data movement: the DeiT attention heads), so that the quantizer
  //#### keeps the transposes between the Linear convs and the attention matmuls in int8 (VTASave: generic extract / insert)
  case Kinded::Kind::SliceNodeKind:
  case Kinded::Kind::ConcatNodeKind:
  case Kinded::Kind::SplatNodeKind:
  case Kinded::Kind::InsertTensorNodeKind:
    return NI.allInputsAndOutputsHaveSameElemKind(
        {ElemKind::FloatTy, ElemKind::Float16Ty, ElemKind::BFloat16Ty,
         ElemKind::Int8QTy, ElemKind::Int16QTy, ElemKind::Int32ITy,
         ElemKind::Int64ITy, ElemKind::BoolTy});

  case Kinded::Kind::SignNodeKind:
    return NI.allInputsAndOutputsHaveSameElemKind(
        {ElemKind::FloatTy, ElemKind::Int8QTy});

  //#### JO added (2026-11-07): int8 with per-channel filter scales (the depthwise convs with -enable-channelwise-depthwise;
  //#### VTASave saveChannelwiseConvolutionInst, vtalib cw_convolution on the CPU)
  case Kinded::Kind::ChannelwiseQuantizedConvolutionNodeKind:
    return NI.getInElemTy(ChannelwiseQuantizedConvolutionNode::InputIdx) == ElemKind::Int8QTy &&
           NI.getInElemTy(ChannelwiseQuantizedConvolutionNode::FilterIdx) == ElemKind::Int8QTy &&
           NI.getOutElemTy(ChannelwiseQuantizedConvolutionNode::ResultIdx) == ElemKind::Int8QTy &&
           (NI.getInElemTy(ChannelwiseQuantizedConvolutionNode::BiasIdx) == ElemKind::Int32QTy ||
            NI.getInElemTy(ChannelwiseQuantizedConvolutionNode::BiasIdx) == ElemKind::FloatTy);

  case Kinded::Kind::ConvolutionNodeKind:
  case Kinded::Kind::VTAConvolutionNodeKind:
    if (!NI.getInTy(ConvolutionNode::InputIdx)->isQuantizedType()) {
      return NI.allInputsAndOutputsHaveSameElemKind(
          {ElemKind::FloatTy, ElemKind::Float16Ty, ElemKind::BFloat16Ty});
    }
    return (NI.allInputsAndOutputsHaveSameElemKind(
                {ElemKind::Int8QTy}, {ConvolutionNode::BiasIdx}) &&
            (NI.getInElemTy(ConvolutionNode::BiasIdx) == ElemKind::Int8QTy ||
             NI.getInElemTy(ConvolutionNode::BiasIdx) == ElemKind::Int32QTy)) ||
           (NI.allInputsAndOutputsHaveSameElemKind(
                {ElemKind::Int16QTy}, {ConvolutionNode::BiasIdx}) &&
            (NI.getInElemTy(ConvolutionNode::BiasIdx) == ElemKind::Int16QTy ||
             NI.getInElemTy(ConvolutionNode::BiasIdx) == ElemKind::Int32QTy));

   case Kinded::Kind::BNNConvolutionNodeKind:
      if (!NI.getInTy(BNNConvolutionNode::InputIdx)->isQuantizedType()) {
          return NI.allInputsAndOutputsHaveSameElemKind(
                  {ElemKind::FloatTy, ElemKind::Float16Ty});
      }
      return (NI.allInputsAndOutputsHaveSameElemKind(
              {ElemKind::Int8QTy}, {BNNConvolutionNode::BiasIdx, BNNConvolutionNode::ScalingfactorIdx}) &&
              (NI.getInElemTy(BNNConvolutionNode::BiasIdx) == ElemKind::Int8QTy ||
               NI.getInElemTy(BNNConvolutionNode::BiasIdx) == ElemKind::Int32QTy)) ||
             (NI.allInputsAndOutputsHaveSameElemKind(
                     {ElemKind::Int16QTy}, {BNNConvolutionNode::BiasIdx, BNNConvolutionNode::ScalingfactorIdx}) &&
              (NI.getInElemTy(BNNConvolutionNode::BiasIdx) == ElemKind::Int16QTy ||
               NI.getInElemTy(BNNConvolutionNode::BiasIdx) == ElemKind::Int32QTy));

  case Kinded::Kind::QuantizeNodeKind:
    return ((NI.getInElemTy(QuantizeNode::InputIdx) == ElemKind::FloatTy) ||
            (NI.getInElemTy(QuantizeNode::InputIdx) == ElemKind::Float16Ty) ||
            (NI.getInElemTy(QuantizeNode::InputIdx) == ElemKind::BFloat16Ty)) &&
           ((NI.getOutElemTy(QuantizeNode::ResultIdx) == ElemKind::Int8QTy) ||
            (NI.getOutElemTy(QuantizeNode::ResultIdx) == ElemKind::UInt8QTy) ||
            (NI.getOutElemTy(QuantizeNode::ResultIdx) == ElemKind::Int16QTy) ||
            (NI.getOutElemTy(QuantizeNode::ResultIdx) == ElemKind::Int32QTy));

  case Kinded::Kind::DequantizeNodeKind:
    return ((NI.getInElemTy(DequantizeNode::InputIdx) == ElemKind::Int8QTy) ||
            (NI.getInElemTy(DequantizeNode::InputIdx) == ElemKind::UInt8QTy) ||
            (NI.getInElemTy(DequantizeNode::InputIdx) == ElemKind::Int16QTy) ||
            (NI.getInElemTy(DequantizeNode::InputIdx) == ElemKind::Int32QTy) ||
            (NI.getInElemTy(DequantizeNode::InputIdx) ==
             ElemKind::UInt8FusedQTy)) &&
           ((NI.getOutElemTy(DequantizeNode::ResultIdx) == ElemKind::FloatTy) ||
            (NI.getOutElemTy(DequantizeNode::ResultIdx) ==
             ElemKind::Float16Ty) ||
            (NI.getOutElemTy(DequantizeNode::ResultIdx) ==
             ElemKind::BFloat16Ty));

  // We just clip 64 to 32 SelectedIdx silently with the SoftMax
  // SelectedIdx in case dim_t is 32b.
  case Kinded::Kind::SoftMaxNodeKind:
    return NI.allInputsAndOutputsHaveSameElemKind(
               {ElemKind::FloatTy, ElemKind::Float16Ty, ElemKind::BFloat16Ty},
               {SoftMaxNode::SelectedIdx}) &&
           (NI.getInElemTy(SoftMaxNode::SelectedIdx) == ElemKind::Int32ITy ||
            NI.getInElemTy(SoftMaxNode::SelectedIdx) == ElemKind::Int64ITy);

  case Kinded::Kind::TransposeNodeKind:
  case Kinded::Kind::ReshapeNodeKind:
    // These work regardless of the underlying type.
    return true;

  default:
    return false;
  }
}

/// Use template meta-programming to check if typename ClassName contains
/// has_getLayout() method. Below generates a struct named has_getLayout that
/// looks for said method.
CLASS_CONTAINS_METHOD(getLayout)

template <typename T, std::enable_if_t<
                          !has_getLayout<T, ConvolutionLayout>::value, int> = 0>
static bool checkLayout(const T &I) {
  (void)I;
  return true;
}

template <typename T,
          std::enable_if_t<has_getLayout<T, ConvolutionLayout>::value, int> = 0>
static bool checkLayout(const T &I) {
  if (I.getLayout() != NHWC && I.getLayout() != VTA_LAYOUT) {
    report("VTA supports only NHWC and VTA_LAYOUT");
    return false;
  }
  return true;
}

static bool checkLayoutForNode(const Node &N) {
#define DEF_NODE(CLASS, NAME)                                                  \
  case Kinded::Kind::CLASS##Kind: {                                            \
    const CLASS *CI = llvm::cast<CLASS>(&N);                                   \
    return checkLayout(*CI);                                                   \
    break;                                                                     \
  }
  switch (N.getKind()) {
    DEF_NODE(ConvolutionNode, Convolution)
    DEF_NODE(MaxPoolNode, MaxPool)
    DEF_NODE(AvgPoolNode, AvgPool)
    DEF_NODE(FullyConnectedNode, FullyConnected)
    DEF_NODE(SoftMaxNode, SoftMax)
    DEF_NODE(AddNode, Add)
    DEF_NODE(SubNode, Sub)
    DEF_NODE(DivNode, Div)
    DEF_NODE(MaxNode, Max)
    DEF_NODE(SignNode, Sign)
    DEF_NODE(ReluNode, Relu)
    DEF_NODE(ReshapeNode, Reshape)
    DEF_NODE(TransposeNode, Transpose)
    DEF_NODE(InsertTensorNode, InsertTensor)
    DEF_NODE(SplatNode, Splat)
    DEF_NODE(QuantizeNode, Quantize)
    DEF_NODE(DequantizeNode, Dequantize)
    DEF_NODE(VTAConvolutionNode, VTAConvolution)
#undef DEF_NODE
  default:
    // llvm_unreachable("Invalid instruction.");
    break;
  }
  return true;
}

bool VTA::verify(const Function &F, bool verbose) const {
  if (!F.verify(this)) {
    return false;
  }
  // if (!checkAllNodesSupported(F, verbose)) {
  //   return false;
  // }
  for (const Node &N : F.getNodes()) {
    if (!checkLayoutForNode(N)) {
      return false;
    }
    //#### JO changed (2026-11-06): + CLIP (ReLU6)
    //#### JO changed (2026-11-07): + the channelwise quantized conv with RELU / CLIP
    if (!(N.getKind() == Kinded::Kind::ConvolutionNodeKind &&
          (llvm::cast<ConvolutionNode>(&N)->getFusedActivation() == FusedActivation::RELU ||
           llvm::cast<ConvolutionNode>(&N)->getFusedActivation() == FusedActivation::CLIP)) &&
        !(N.getKind() == Kinded::Kind::ChannelwiseQuantizedConvolutionNodeKind &&
          (llvm::cast<ChannelwiseQuantizedConvolutionNode>(&N)->getFusedActivation() == FusedActivation::RELU ||
           llvm::cast<ChannelwiseQuantizedConvolutionNode>(&N)->getFusedActivation() == FusedActivation::CLIP)) &&
        !(N.getKind() == Kinded::Kind::VTAConvolutionNodeKind &&
          llvm::cast<VTAConvolutionNode>(&N)->getFusedActivation() ==
              FusedActivation::RELU) &&
        !checkNoFusionForNode(N)) {
      return false;
    }
  }
  return true;
}

static bool checkLayoutForInstr(const Instruction &I) {
#define DEF_INSTR(CLASS, NAME)                                                 \
  case Kinded::Kind::CLASS##Kind: {                                            \
    const CLASS *CI = llvm::cast<CLASS>(&I);                                   \
    return checkLayout(*CI);                                                   \
    break;                                                                     \
  }
#define DEF_BACKEND_SPECIFIC_INSTR(CLASS, NAME)                                \
  case Kinded::Kind::CLASS##Kind: {                                            \
    const CLASS *CI = llvm::cast<CLASS>(&I);                                   \
    return checkLayout(*CI);                                                   \
    break;                                                                     \
  }
  switch (I.getKind()) {
    DEF_INSTR(AllocActivationInst, allocactivation)
    DEF_INSTR(TensorViewInst, tensorview)
    DEF_INSTR(DeallocActivationInst, deallocactivation)
    DEF_INSTR(ConvolutionInst, convolution)
    DEF_INSTR(MaxPoolInst, maxpool)
    DEF_INSTR(AvgPoolInst, avgpool)
    DEF_INSTR(FullyConnectedInst, fullyconnected)
    DEF_INSTR(SoftMaxInst, softmax)
    DEF_INSTR(ElementAddInst, elementadd)
    DEF_INSTR(ElementSubInst, elementsub)
    DEF_INSTR(ElementDivInst, elementdiv)
    DEF_INSTR(ElementMaxInst, elementmax)
    DEF_INSTR(ElementSignInst, elementsign)
    DEF_INSTR(ReluInst, relu)
    DEF_INSTR(TransposeInst, transpose)
    DEF_INSTR(SplatInst, splat)
    DEF_INSTR(InsertTensorInst, inserttensor)
    DEF_INSTR(DebugPrintInst, debugprint)
    DEF_INSTR(QuantizeInst, quantize)
    DEF_INSTR(DequantizeInst, dequantize)
    DEF_BACKEND_SPECIFIC_INSTR(VTAConvolutionInst, vtaconvolution)
#undef DEF_INSTR
#undef DEF_BACKEND_SPECIFIC_INSTR
  default:
    // llvm_unreachable("Invalid instruction.");
    break;
  }
  return true;
}

bool VTA::verify(const IRFunction &IR) const {
  for (const auto &I : IR.getInstrs()) {
    // Only support convolution+relu fusions for now.
    //#### JO changed (2026-11-06): + CLIP (ReLU6)
    //#### JO changed (2026-11-07): + the channelwise quantized conv with RELU / CLIP
    if (!(I.getKind() == Kinded::Kind::ConvolutionInstKind &&
          (llvm::cast<ConvolutionInst>(&I)->getFusedActivation() == FusedActivation::RELU ||
           llvm::cast<ConvolutionInst>(&I)->getFusedActivation() == FusedActivation::CLIP)) &&
        !(I.getKind() == Kinded::Kind::ChannelwiseQuantizedConvolutionInstKind &&
          (llvm::cast<ChannelwiseQuantizedConvolutionInst>(&I)->getFusedActivation() == FusedActivation::RELU ||
           llvm::cast<ChannelwiseQuantizedConvolutionInst>(&I)->getFusedActivation() == FusedActivation::CLIP)) &&
        !(I.getKind() == Kinded::Kind::VTAConvolutionInstKind &&
          llvm::cast<VTAConvolutionInst>(&I)->getFusedActivation() ==
              FusedActivation::RELU) &&
        !checkNoFusionForInstr(I)) {
      return false;
    }
    if (!checkLayoutForInstr(I)) {
      return false;
    }
  }
  return true;
}

bool VTA::shouldLower(const Node *N) const {
  return !isOpSupported(NodeInfo(*N));
}

/// Quantize the float \p bias for the given FullyConnectedNode as int32 using
/// \p inputScale, weight \p scales and \p offset=0. \returns false if the bias
/// was already quantized and thus no change was made and true otherwise.
static bool quantizeFCFloatBias(Function *F,
                                FullyConnectedNode &fullyConnected) {
  if (fullyConnected.getBias().getType()->isQuantizedType() ||
      (!fullyConnected.getWeights().getType()->isQuantizedType())) {
    return false;
  }
  assert(fullyConnected.getBias().getElementType() == ElemKind::FloatTy &&
         "Bias type must be a float in order to quantize it.");
  Constant *biasC =
      llvm::dyn_cast<Constant>(fullyConnected.getBias().getNode());
  assert(biasC && "bias input to FullyConnectedNode must be Constant in order "
                  "to quantize the bias");
  const auto &biasUnquantizedH = biasC->getPayload().getHandle<float>();
  // biasQuantizedT is Int32QTy
  const float inputScale = fullyConnected.getInput().getType()->getScale();
  const float weigthScale = fullyConnected.getWeights().getType()->getScale();
  const float scale = inputScale * weigthScale;
  auto biasQuantizedT = Tensor(ElemKind::Int32QTy, biasUnquantizedH.dims(),
                               /* scale */ scale, /* offset */ 0);
  auto biasQuantizedH = biasQuantizedT.getHandle<int32_t>();
  TensorQuantizationParams tqp;
  tqp.scale = scale;
  tqp.offset = 0;
  for (dim_t i = 0; i < biasQuantizedH.size(); ++i) {
    biasQuantizedH.raw(i) =
        quantization::quantize<int32_t>(biasUnquantizedH.raw(i), tqp);
  }
  auto biasQuantizedC = F->getParent()->createConstant(
      biasC->getName(), std::move(biasQuantizedT));
  auto newFullyConnectedNode = F->createFullyConnected(
      fullyConnected.getName(), fullyConnected.getInput(),
      fullyConnected.getWeights(), biasQuantizedC,
      fullyConnected.getResult().getType(), /* axis doens't matter */ 1);
  fullyConnected.getResult().replaceAllUsesOfWith(newFullyConnectedNode);
  return true;
}

/// Quantize the float \p bias for the given RowwiseQuantizedFullyConnectedNode
/// as int32. \returns false if the bias was already quantized and thus no
/// change was made and true otherwise.
static bool quantizeRQFCFloatBias(Function *F,
                                  RowwiseQuantizedFullyConnectedNode &rqfc) {
  if (rqfc.getBias().getType()->isQuantizedType() ||
      (!rqfc.getWeights().getType()->isQuantizedType())) {
    return false;
  }
  assert(rqfc.getBias().getElementType() == ElemKind::FloatTy &&
         "Bias type must be a float in order to quantize it.");
  Constant *biasC = llvm::dyn_cast<Constant>(rqfc.getBias().getNode());
  assert(biasC && "bias input to RowwiseQuantizedFullyConnectedNode must be a "
                  "Constant in order to quantize the bias");

  auto TQPs = getTensorQuantizationParams(
      biasC->getPayload(), quantization::Schema::Asymmetric, ElemKind::Int32QTy,
      0, biasC->dims()[0]);

  DCHECK_EQ(TQPs.size(), 1) << "Should only be one dimension to quantize on";

  auto biasQuantizedT = quantization::quantizeTensor(
      biasC->getPayload(), TQPs[0], ElemKind::Int32QTy);

  auto biasQuantizedC = F->getParent()->createConstant(
      biasC->getName(), std::move(biasQuantizedT));

  Constant *weights = llvm::dyn_cast<Constant>(rqfc.getWeights().getNode());
  Constant *scales = llvm::dyn_cast<Constant>(rqfc.getScales().getNode());
  Constant *offsets = llvm::dyn_cast<Constant>(rqfc.getOffsets().getNode());

  DCHECK(weights);
  DCHECK(scales);
  DCHECK(offsets);

  auto newRQFC = F->createRowwiseQuantizedFullyConnected(
      rqfc.getName(), rqfc.getInput(), weights, scales, offsets, biasQuantizedC,
      rqfc.getResult().getType());
  rqfc.getResult().replaceAllUsesOfWith(newRQFC);
  return true;
}

/// This function performs the channelwise quantization for the bias operand of
/// a ChannelwiseQuantizedConvolutionNode \p channelwiseConv from function \p F.
/// The quantization is done only if the bias is float. \returns false if the
/// bias was already quantized and thus no change was made and true otherwise.
static bool channelwiseQuantizeFloatBias(
    Function *F, ChannelwiseQuantizedConvolutionNode &channelwiseConv) {

  // If bias is already quantized then quit.
  if (channelwiseConv.getBias().getType()->isQuantizedType()) {
    return false;
  }

  DCHECK(channelwiseConv.getBias().getElementType() == ElemKind::FloatTy)
      << "Bias type must be a float in order to quantize it!";

  Constant *biasC =
      llvm::dyn_cast<Constant>(channelwiseConv.getBias().getNode());
  DCHECK(biasC)
      << "Bias input to ChannelwiseQuantizedConvolutionNode must be a Constant "
         "in order to quantize the bias!";

  Constant *filterScalesC =
      llvm::dyn_cast<Constant>(channelwiseConv.getFilterScales().getNode());
  DCHECK(filterScalesC)
      << "Filter scales input to ChannelwiseQuantizedConvolutionNode must be a "
         "Constant in order to quantize the bias!";

  // Create new constants for Bias, BiasScales and BiasOffsets operands.
  Constant *biasCQ = F->getParent()->createConstant(
      ElemKind::Int32QTy, biasC->getType()->dims(), 1.0, 0, biasC->getName());
  Constant *biasScalesC = F->getParent()->createConstant(
      ElemKind::FloatTy, biasC->getType()->dims(), "biasScales");
  Constant *biasOffsetsC = F->getParent()->createConstant(
      ElemKind::Int32ITy, biasC->getType()->dims(), "biasOffsets");

  // Quantize the bias operand manually from FloatTy to Int32QTy using the
  // quantization parameters biasScales[i] = inputScale * filterScales[i] and
  // biasOffsets[i] = 0.
  float inputScale = channelwiseConv.getInput().getType()->getScale();
  const auto &filterScalesH = filterScalesC->getPayload().getHandle<float>();
  const auto &biasH = biasC->getPayload().getHandle<float>();
  auto biasQH = biasCQ->getPayload().getHandle<int32_t>();
  auto biasScalesH = biasScalesC->getPayload().getHandle<float>();
  auto biasOffsetsH = biasOffsetsC->getPayload().getHandle<int32_t>();
  for (dim_t idx = 0, idxEnd = biasC->getType()->size(); idx < idxEnd; ++idx) {
    TensorQuantizationParams biasTQP;
    biasTQP.scale = inputScale * filterScalesH.raw(idx);
    biasTQP.offset = 0;
    biasQH.raw(idx) = quantization::quantize<int32_t>(biasH.raw(idx), biasTQP);
    biasScalesH.raw(idx) = biasTQP.scale;
    biasOffsetsH.raw(idx) = biasTQP.offset;
  }

  // Create new ChannelwiseQuantizedConvolutionNode with quantized bias
  // and explicit bias scales and offsets.
  auto *newChannelwiseConv = F->createChannelwiseQuantizedConv(
      channelwiseConv.getName(), channelwiseConv.getInput(),
      channelwiseConv.getFilter(), biasCQ, channelwiseConv.getFilterScales(),
      channelwiseConv.getFilterOffsets(), biasScalesC, biasOffsetsC,
      channelwiseConv.getResult().getType(), channelwiseConv.getKernels(),
      channelwiseConv.getStrides(), channelwiseConv.getPads(),
      channelwiseConv.getGroup(), channelwiseConv.getDilation());
  channelwiseConv.getResult().replaceAllUsesOfWith(newChannelwiseConv);
  return true;
}

static Node *optimizeVTAConv(ConvolutionNode *CN, Function *F) {
  auto depth = CN->getFilter().dims()[0];
  auto channel = CN->getFilter().dims()[3];

  auto *M = F->getParent();
  auto group = CN->getGroup();
  auto ActivationNV = CN->getFusedActivation();

  // Make sure that the depth group is divisible by 16 to perform the
  // transformation.
  if (((depth / group) % 16) != 0) {
    return nullptr;
  }

  // Make sure that the channel group is divisible by 16 to perform the
  // transformation.
  if (((channel / group) % 16) != 0) {
    return nullptr;
  }

  Constant *filter = dyn_cast<Constant>(CN->getFilter());
  auto input = CN->getInput();
  auto output = CN->getResult();

  if (!filter || filter->getNumUsers() != 1) {
    // Can't mutate the filter.
    return nullptr;
  }

  // We only support Int8 for EVTA.
  if (filter->getElementType() != ElemKind::Int8QTy) {
    return nullptr;
  }

  // This optimization is not supported with Dilation currently.
  if (CN->getDilation() != llvm::ArrayRef<unsigned_t>({1, 1})) {
    return nullptr;
  }

  // This optimization only support single group size.
  if (CN->getGroup() != 1) {
    return nullptr;
  }

  // Create a new constant filter with the layout [Nm, Cm, H, W, Ns, Cs];
  TypeRef filterTy = filter->getType();
  TypeRef inputTy = input.getType();
  TypeRef outputTy = output.getType();

  auto dims = filterTy->dims();
  auto dims_input = inputTy->dims();
  auto dims_output = outputTy->dims();
  assert(dims.size() == 4 && "Invalid filter size");
  auto *filter8 = M->createConstant(
      filterTy->getElementType(),
      {dims[0] / 16, dims[3] / 16, dims[1], dims[2], 16, 16},
      filterTy->getScale(), filterTy->getOffset(), filter->getName());
  ShapeVTAKernel fdim(filter8->dims());

  auto F8H = filter8->getHandle<int8_t>();
  auto FH = filter->getHandle<int8_t>();

  // Transpose the weights into the following format [Nm, Cm, H, W, Ns, Cs]
  for (dim_t c0 = 0; c0 < dims[0]; c0++)
    for (dim_t c1 = 0; c1 < dims[1]; c1++)
      for (dim_t c2 = 0; c2 < dims[2]; c2++)
        for (dim_t c3 = 0; c3 < dims[3]; c3++) {
          F8H.at({c0 / 16, c3 / 16, c1, c2, c0 % 16, c3 % 16}) =
              FH.at({c0, c1, c2, c3});
        }

  // 4 dim -> 6dim and do reshape and transpose.
  std::array<dim_t, 6> reshapeInputS{
      {dims_input[0], 1, dims_input[1], dims_input[2],
       (dim_t)ceil(dims_input[3] / (double)16), 16}};
  llvm::ArrayRef<dim_t> reshapeInputDim(reshapeInputS);
  std::array<unsigned_t, 6> transposeInputS{{0, 4, 2, 3, 1, 5}};
  llvm::ArrayRef<unsigned_t> transposeInputDim(transposeInputS);
  auto *reshapeInput =
      F->createReshape("reshapeInput", CN->getInput(), reshapeInputDim);
  auto *transposeInput =
      F->createTranspose("transposeInput", reshapeInput, transposeInputDim);

  // output tensor of VTAConv
  std::array<dim_t, 6> outputS{{dims_output[0],
                                (dim_t)ceil(dims_output[3] / (double)16),
                                dims_output[1], dims_output[2], 1, 16}};
  llvm::ArrayRef<dim_t> newOutputDim(outputS);

  TypeRef newOT = F->getParent()->uniqueType(
      output.getElementType(), newOutputDim, output.getType()->getScale(),
      output.getType()->getOffset());
  VTAConvolutionNode *conv = F->createVTAConv(
      CN->getName(), transposeInput, filter8, CN->getBias(), newOT,
      CN->getKernels(), CN->getStrides(), CN->getPads(), group);
  if (ActivationNV == FusedActivation::RELU) {
    conv->setFusedActivation(FusedActivation::RELU);
  }

  // output node
  std::array<unsigned_t, 6> transposeOOutputS{{0, 4, 2, 3, 1, 5}};
  llvm::ArrayRef<unsigned_t> transposeOutputDim(transposeOOutputS);

  auto *transposeOutput =
      F->createTranspose("transposeOutput", conv, transposeOutputDim);
  auto *reshapeOutput =
      F->createReshape("reshapeOutput", transposeOutput, dims_output);

  return reshapeOutput;
}

//#### JO added (2026-11-07): -vta-pad-channels (above)
static bool isWidenableConv(const ConvolutionNode *CN) {
  return CN->getGroup() == 1 && CN->getResult().getType()->getElementType() == ElemKind::Int8QTy &&
         CN->getResult().getType()->getOffset() == 0 && llvm::isa<Constant>(CN->getFilter()) &&
         llvm::isa<Constant>(CN->getBias()) && CN->getFilter().getType()->getOffset() == 0 &&
         CN->getInput().getType()->getOffset() == 0;
}

static bool padChannelsTo16(Function *F) {
  auto pad16 = [](dim_t c) { return (c + 15) / 16 * 16; };
  std::unordered_set<Node *> S;
  for (auto &N : F->getNodes()) {
    if (N.getNumResults() != 1)
      continue;
    auto ty = N.getNthResult(0).getType();
    if (ty->dims().size() != 4 || ty->getElementType() != ElemKind::Int8QTy || ty->getOffset() != 0 ||
        ty->dims()[3] % 16 == 0)
      continue;
    if (auto *CN = dyn_cast<ConvolutionNode>(&N)) {
      if (isWidenableConv(CN))
        S.insert(&N);
    } else if (llvm::isa<AddNode>(&N)) {
      S.insert(&N);
    }
  }
  for (bool changed = true; changed;) {
    changed = false;
    for (auto it = S.begin(); it != S.end();) {
      Node *N = *it;
      bool ok = true;
      if (auto *A = dyn_cast<AddNode>(N))
        ok = S.count(A->getLHS().getNode()) && S.count(A->getRHS().getNode());
      for (auto &U : N->getUsers()) {
        Node *u = U.getUser();
        if (auto *CN = dyn_cast<ConvolutionNode>(u))
          ok = ok && isWidenableConv(CN) && CN->getInput().getNode() == N && CN->getFilter().getNode() != N &&
               CN->getBias().getNode() != N;
        else
          ok = ok && llvm::isa<AddNode>(u) && S.count(u);
      }
      if (!ok) {
        it = S.erase(it);
        changed = true;
      } else {
        ++it;
      }
    }
  }
  if (S.empty())
    return false;

  auto *M = F->getParent();
  for (auto &N : F->getNodes()) {
    auto *CN = dyn_cast<ConvolutionNode>(&N);
    if (!CN)
      continue;
    const bool in = S.count(CN->getInput().getNode()), out = S.count(CN);
    if (!in && !out)
      continue;
    auto *filter = llvm::cast<Constant>(CN->getFilter());
    auto fd = filter->dims(); // [KN, KH, KW, C]
    const dim_t KN = out ? pad16(fd[0]) : fd[0], C = in ? pad16(fd[3]) : fd[3];
    auto *nf = M->createConstant(M->uniqueTypeWithNewShape(filter->getType(), {KN, fd[1], fd[2], C}),
                                 filter->getName().str() + "_pad16");
    nf->getPayloadMutable().zero();
    auto hs = filter->getPayload().getHandle<int8_t>();
    auto hd = nf->getPayloadMutable().getHandle<int8_t>();
    for (dim_t k = 0; k < fd[0]; k++)
      for (dim_t y = 0; y < fd[1]; y++)
        for (dim_t x = 0; x < fd[2]; x++)
          for (dim_t c = 0; c < fd[3]; c++)
            hd.at({k, y, x, c}) = hs.at({k, y, x, c});
    CN->setNthInput(ConvolutionNode::FilterIdx, nf);
    if (out) {
      auto *bias = llvm::cast<Constant>(CN->getBias());
      auto *nb = M->createConstant(M->uniqueTypeWithNewShape(bias->getType(), {KN}), bias->getName().str() + "_pad16");
      nb->getPayloadMutable().zero();
      memcpy(nb->getPayloadMutable().getUnsafePtr(), bias->getPayload().getUnsafePtr(),
             bias->getPayload().getSizeInBytes());
      CN->setNthInput(ConvolutionNode::BiasIdx, nb);
      auto rd = CN->getResult().dims();
      CN->getResult().setType(M->uniqueTypeWithNewShape(CN->getResult().getType(), {rd[0], rd[1], rd[2], KN}));
    }
  }
  for (auto &N : F->getNodes())
    if (auto *A = dyn_cast<AddNode>(&N))
      if (S.count(A)) {
        auto rd = A->getResult().dims();
        A->getResult().setType(
            M->uniqueTypeWithNewShape(A->getResult().getType(), {rd[0], rd[1], rd[2], pad16(rd[3])}));
      }
  return true;
}

//#### JO added (2026-11-09): -vta-matmul-conv (above). The conv has a zero int32 bias at the scale lhs * rhs and the
//#### MatMul's output type, so getConvPath (VTASave) takes the EVTA path; the reshapes are tensor views in the IR.
static bool matMulToConv(Function *F) {
  auto *M = F->getParent();
  std::vector<MatMulNode *> mms;
  for (auto &N : F->getNodes())
    if (auto *MM = dyn_cast<MatMulNode>(&N))
      mms.push_back(MM);
  bool changed = false;
  for (auto *MM : mms) {
    auto *W = dyn_cast<Constant>(MM->getRHS().getNode());
    auto lTy = MM->getLHS().getType(), oTy = MM->getResult().getType();
    if (!W || W->getElementType() != ElemKind::Int8QTy || lTy->getElementType() != ElemKind::Int8QTy ||
        oTy->getElementType() != ElemKind::Int8QTy || lTy->getOffset() || W->getType()->getOffset() ||
        oTy->getOffset())
      continue;
    const dim_t rows = lTy->dims()[0], K = lTy->dims()[1], N = W->dims()[1];
    if (K % 16 || N % 16)
      continue;
    dim_t w = 1;
    for (dim_t d = 1; d * d <= rows; d++)
      if (rows % d == 0)
        w = d;
    const dim_t h = rows / w;
    const std::string name = MM->getName().str();
    // -vta-fold-matmul-bias: MatMul -> (Reshape ->) Add(constant bias with every row the same)
    AddNode *biasAdd = nullptr;
    std::vector<int32_t> foldedBias;
    if (vtaFoldMatMulBias && MM->getResult().getNumUsers() == 1) {
      NodeValue cur = MM->getResult();
      Node *u = (*cur.getUsers().begin()).getUser();
      if (auto *R = dyn_cast<ReshapeNode>(u)) {
        cur = R->getResult();
        u = cur.getNumUsers() == 1 ? (*cur.getUsers().begin()).getUser() : nullptr;
      }
      auto *A = u ? dyn_cast<AddNode>(u) : nullptr;
      if (A && (A->getLHS() == cur || A->getRHS() == cur)) {
        auto *Cb = dyn_cast<Constant>((A->getLHS() == cur ? A->getRHS() : A->getLHS()).getNode());
        auto aTy = A->getResult().getType();
        if (Cb && Cb->getElementType() == ElemKind::Int8QTy && !Cb->getType()->getOffset() &&
            aTy->getElementType() == ElemKind::Int8QTy && !aTy->getOffset() && Cb->getType()->size() == rows * N) {
          const int8_t *p = reinterpret_cast<const int8_t *>(Cb->getPayload().getUnsafePtr());
          bool same = true;
          for (dim_t r = 1; r < rows && same; r++)
            same = !memcmp(p, p + r * N, N);
          const double k = (double)Cb->getType()->getScale() / ((double)lTy->getScale() * W->getType()->getScale());
          //#### JO changed (2026-11-10): a non-integer k (non-power-of-two scales) rounds the int32 bias (error < 0.5 at the
          //#### scale lhs * rhs, far below the output step)
          // if (same && k >= 1 && k <= (1 << 22) && k == std::round(k)) {
          if (same && k > 0 && k <= (1 << 22)) {
            for (dim_t n = 0; n < N; n++)
              foldedBias.push_back((int32_t)std::lround((double)p[n] * k));
            biasAdd = A;
          }
        }
      }
    }
    auto *filter = M->createConstant(ElemKind::Int8QTy, {N, 1, 1, K}, W->getType()->getScale(), 0,
                                     W->getName().str() + "_conv1x1");
    auto hs = W->getPayload().getHandle<int8_t>();
    auto hd = filter->getPayloadMutable().getHandle<int8_t>();
    for (dim_t n = 0; n < N; n++)
      for (dim_t k = 0; k < K; k++)
        hd.at({n, 0, 0, k}) = hs.at({k, n});
    auto *bias = M->createConstant(ElemKind::Int32QTy, {N}, lTy->getScale() * W->getType()->getScale(), 0,
                                   name + "_conv1x1_bias");
    bias->getPayloadMutable().zero();
    if (biasAdd) {
      auto hb = bias->getPayloadMutable().getHandle<int32_t>();
      for (dim_t n = 0; n < N; n++)
        hb.raw(n) = foldedBias[n];
    }
    auto *in = F->createReshape(name + "_conv1x1_in", MM->getLHS(), {1, h, w, K});
    const float outScale = biasAdd ? biasAdd->getResult().getType()->getScale() : oTy->getScale();
    auto convTy = M->uniqueType(ElemKind::Int8QTy, {1, h, w, N}, outScale, 0);
    auto *conv = F->createConv(name + "_conv1x1", in, filter, bias, convTy, {1, 1}, {1, 1}, {0, 0, 0, 0}, 1);
    if (biasAdd) {
      auto *out = F->createReshape(name + "_conv1x1_out", conv, biasAdd->getResult().dims());
      biasAdd->getResult().replaceAllUsesOfWith(out);
    } else {
      auto *out = F->createReshape(name + "_conv1x1_out", conv, {rows, N});
      MM->getResult().replaceAllUsesOfWith(out);
    }
    changed = true;
  }
  return changed;
}

//#### JO added (2026-11-10): -vta-pow2-ratio (above)
static bool isPow2Ratio(double r) {
  const double l = std::log2(r);
  return std::fabs(l - std::round(l)) < 1e-6;
}
static bool layoutOnly(const Node *u) {
  return llvm::isa<ReshapeNode>(u) || llvm::isa<TransposeNode>(u) || llvm::isa<SliceNode>(u);
}
static bool isScaledOp(const Node *n) {
  return llvm::isa<ConvolutionNode>(n) || llvm::isa<FullyConnectedNode>(n) || llvm::isa<MatMulNode>(n);
}
// can v's int8 type take another scale: its layout-only users follow it; a Concat when its other inputs are constants
// (requantized) or outputs of convs / FCs / matmuls with v's scale (the attention heads: rescaled with it)
static bool canRescale(NodeValue v, std::set<const Node *> &seen) {
  for (auto &use : v.getUsers()) {
    Node *u = use.getUser();
    if (!seen.insert(u).second)
      continue;
    if (layoutOnly(u)) {
      if (!canRescale(u->getNthResult(0), seen))
        return false;
    } else if (auto *C = llvm::dyn_cast<ConcatNode>(u)) {
      for (auto &in : C->getInputs()) {
        if (in == v || llvm::isa<Constant>(in.getNode()))
          continue;
        if (!isScaledOp(in.getNode()) || in.getType()->getScale() != v.getType()->getScale() ||
            !canRescale(in, seen))
          return false;
      }
      if (!canRescale(C->getResult(), seen))
        return false;
    } else if (llvm::isa<SaveNode>(u)) {
      return false;
    }
  }
  return true;
}
static void rescale(Function *F, NodeValue v, float s);
static void rescaleUsers(Function *F, NodeValue v, float s) {
  auto *M = F->getParent();
  std::vector<Node *> users;
  for (auto &use : v.getUsers())
    users.push_back(use.getUser());
  for (Node *u : users) {
    if (layoutOnly(u)) {
      if (u->getNthResult(0).getType()->getScale() != s)
        rescale(F, u->getNthResult(0), s);
    } else if (auto *C = llvm::dyn_cast<ConcatNode>(u)) {
      if (C->getResult().getType()->getScale() == s)
        continue;
      C->getResult().setType(M->uniqueType(ElemKind::Int8QTy, C->getResult().dims(), s, 0));
      for (unsigned i = 0, e = C->getInputs().size(); i < e; i++) {
        NodeValue in = C->getInputs()[i];
        if (in == v || in.getType()->getScale() == s)
          continue;
        if (auto *K = llvm::dyn_cast<Constant>(in.getNode())) {
          auto *K2 = M->createConstant(ElemKind::Int8QTy, K->dims(), s, 0, K->getName().str() + "_rescaled");
          auto hs = K->getPayload().getHandle<int8_t>();
          auto hd = K2->getPayloadMutable().getHandle<int8_t>();
          const float ks = K->getType()->getScale();
          for (dim_t j = 0, n = hs.size(); j < n; j++) {
            const long q = std::lround(hs.raw(j) * ks / s);
            hd.raw(j) = (int8_t)(q > 127 ? 127 : q < -128 ? -128 : q);
          }
          C->setNthInput(i, K2->getOutput());
        } else {
          rescale(F, in, s);
        }
      }
      rescaleUsers(F, C->getResult(), s);
    }
  }
}
static void rescale(Function *F, NodeValue v, float s) {
  v.setType(F->getParent()->uniqueType(ElemKind::Int8QTy, v.dims(), s, 0));
  rescaleUsers(F, v, s);
}
static bool pow2Ratios(Function *F) {
  bool changed = false, again = true;
  for (int iter = 0; again && iter < 1000; iter++) {
    again = false;
    for (auto &N : F->getNodes()) {
      NodeValue in, w, out;
      if (auto *C = llvm::dyn_cast<ConvolutionNode>(&N)) {
        in = C->getInput(), w = C->getFilter(), out = C->getResult();
      } else if (auto *FC = llvm::dyn_cast<FullyConnectedNode>(&N)) {
        in = FC->getInput(), w = FC->getWeights(), out = FC->getResult();
      } else if (auto *MM = llvm::dyn_cast<MatMulNode>(&N)) {
        in = MM->getLHS(), w = MM->getRHS(), out = MM->getResult();
      } else {
        continue;
      }
      auto q = [](NodeValue v) { return v.getElementType() == ElemKind::Int8QTy && !v.getType()->getOffset(); };
      if (!q(in) || !q(w) || !q(out))
        continue;
      const double iw = (double)in.getType()->getScale() * w.getType()->getScale();
      const double r = out.getType()->getScale() / iw;
      if (isPow2Ratio(r))
        continue;
      std::set<const Node *> seen;
      if (!canRescale(out, seen)) {
        llvm::errs() << "vta-pow2-ratio: cannot rescale the output of " << N.getName() << " (ratio " << r << ")\n";
        continue;
      }
      const float s = (float)std::ldexp(iw, (int)std::ceil(std::log2(r)));
      rescale(F, out, s);
      changed = again = true;
    }
  }
  return changed;
}

Expected<bool>
VTA::transformPostLowering(Function *F, CompilationContext &cctx,
                           const glow::runtime::DeviceInfo *devInfo) const {
  LOG_SCOPE(F->getLogContext(), "VTA::transformPostLowering")

  bool changed = false;
  //#### JO added (2026-11-07): -vta-pad-channels
  if (vtaPadChannels)
    changed |= padChannelsTo16(F);
  //#### JO added (2026-11-09): -vta-matmul-conv
  if (vtaMatMulConv)
    changed |= matMulToConv(F);
  //#### JO added (2026-11-10): -vta-pow2-ratio
  if (vtaPow2Ratio)
    changed |= pow2Ratios(F);
  for (auto &node : F->getNodes()) {
    if (auto *channelwiseConv =
            llvm::dyn_cast<ChannelwiseQuantizedConvolutionNode>(&node)) {
      changed |= channelwiseQuantizeFloatBias(F, *channelwiseConv);
    } else if (auto *fullyConnected =
                   llvm::dyn_cast<FullyConnectedNode>(&node)) {
      changed |= quantizeFCFloatBias(F, *fullyConnected);
    } else if (auto *rowwiseFC =
                   llvm::dyn_cast<RowwiseQuantizedFullyConnectedNode>(&node)) {
      changed |= quantizeRQFCFloatBias(F, *rowwiseFC);
    }
#ifdef NESTC_EVTA_GRAPH_OPT
    // Try to replace the generic convolution with vta-optimized version (6dim).
    if (auto *CN = dyn_cast<ConvolutionNode>(&node)) {
      if (Node *NCN = optimizeVTAConv(CN, F)) {
        CN->getResult().replaceAllUsesOfWith(NCN);
        changed = true;
        continue;
      }
    }
#endif
  }
  return changed;
}

void VTA::parseBackendSpecificOptions(const BackendOptions &opts) const {}

void VTA::save(Function *F, llvm::StringRef outputDir,
               llvm::StringRef bundleName, llvm::StringRef mainEntryName,
               unsigned idxMultiEVTA,
               bool BNNWithScale) {
  setIdxMultiEVTA(idxMultiEVTA);
  setBNNWithScale(BNNWithScale);
  save(F, outputDir, bundleName, mainEntryName);
}

Expected<double> VTA::estimateNodeCost(const glow::Node *node) const {
  // Using default cost from Partitioner which is 1.
  return 1.0;
}
