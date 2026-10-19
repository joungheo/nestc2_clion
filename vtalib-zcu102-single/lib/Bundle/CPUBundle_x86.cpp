#include <iostream>
#include <ctime>
#include <cstring>
#include <cstdint>
#include <fstream>
#include <vector>

#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <cassert>
#include "llvm/ADT/ArrayRef.h"
#include "glow/Base/Tensor.h"
#include "glow/Quantization/Base/Base.h"

#include "vta/vtalib/include/Bundle/VTABundle.h"

using namespace std;
using namespace glow;

void fwdConvolutionInstFloatImpl(
        Tensor *inV, Tensor *outV, Tensor *filterV, Tensor *biasV,
        llvm::ArrayRef<unsigned_t> kernelSizes, llvm::ArrayRef<unsigned_t> strides,
        llvm::ArrayRef<unsigned_t> pads, size_t group, size_t dilation, bool doRelu) {

  auto inW = inV->getHandle<float>();
  auto outW = outV->getHandle<float>();
  auto filterW = filterV->getHandle<float>();
  auto biasW = biasV->getHandle<float>();

  ShapeNHWC odim(outW.dims());
  ShapeNHWC idim(inW.dims());
  ShapeHW kdim(kernelSizes);
  ShapeHW sdim(strides);

  assert(idim.c % group == 0 && "Input channels must be divisible by group.");
  assert(odim.c % group == 0 && "Output channels must be divisible by group.");
  dim_t inCperG = idim.c / group;
  dim_t outCperG = odim.c / group;

  PaddingTLBR pdim(pads);

  // For each input in the batch:
  for (dim_t n = 0; n < idim.n; n++) {

    // For each group of input channels:
    for (dim_t g = 0; g < group; g++) {

      // For each output channel in the group:
      for (dim_t d = g * outCperG; d < (g + 1) * outCperG; d++) {

        // For each convolution 'jump' in the input tensor:
        ssize_t x = -ssize_t(pdim.top);
        for (dim_t ax = 0; ax < odim.h; x += sdim.height, ax++) {
          ssize_t y = -ssize_t(pdim.left);
          for (dim_t ay = 0; ay < odim.w; y += sdim.width, ay++) {

            // For each element in the convolution-filter:
            float sum = 0;
            for (dim_t fx = 0; fx < kdim.height; fx++) {
              for (dim_t fy = 0; fy < kdim.width; fy++) {
                sdim_t ox = x + fx * dilation;
                sdim_t oy = y + fy * dilation;

                // Ignore index access below zero (this is due to padding).
                if (ox < 0 || oy < 0 || ox >= ssize_t(idim.h) ||
                    oy >= ssize_t(idim.w)) {
                  continue;
                }
                for (dim_t fd = 0; fd < inCperG; fd++) {
                  sum += float(
                          filterW.at({d, fx, fy, fd}) *
                          inW.at({n, (dim_t)ox, (dim_t)oy, g * inCperG + fd}));
                }
              }
            }

            sum += float(biasW.at({d}));
            if(doRelu && sum<0.0){
              sum = 0.0;
            }
            outW.at({n, ax, ay, d}) = sum;
          } // W
        }   // H
      }     // C
    }       // G
  }         // N
}


int convolutionFloat(int8_t* input, int8_t *kernel, int8_t *bias, int8_t *output, dim_t N, dim_t H, dim_t W, dim_t C, dim_t KN, dim_t KH, dim_t KW, int pad_size,
                     int stride_size, size_t group, size_t dilation, bool doRelu, bool doBias, int out_h, int out_w) {
  llvm::SmallVector<dim_t, 4> inputDims = {N,H,W,C};
  Type inputType(ElemKind::FloatTy, inputDims);
  Tensor inV((void*)input, &inputType);

  llvm::SmallVector<dim_t, 4> outputDims = {N,(dim_t)out_h,(dim_t)out_w,KN};
  Type outputType(ElemKind::FloatTy, outputDims);
  Tensor outV((void*)output, &outputType);

  llvm::SmallVector<dim_t, 4> kernelDims = {KN,KH,KW,C};
  Type kernelType(ElemKind::FloatTy, kernelDims);
  Tensor filterV((void*)kernel, &kernelType);

  Tensor* biasV;
  if(doBias) {
    llvm::SmallVector<dim_t, 1> biasDims = {KN};
    Type biasType(ElemKind::FloatTy, biasDims);
    biasV = new Tensor((void *) bias, &biasType);
  }
  else{
    biasV = new Tensor(ElemKind::FloatTy, {KN});
    biasV->zero();
  }

  std::array<unsigned_t,2> kernelS = {(unsigned_t)KH, (unsigned_t)KW};
  llvm::ArrayRef<unsigned_t> kernelSizes(kernelS);

  std::array<unsigned_t,2> strideS = {(unsigned_t)stride_size, (unsigned_t)stride_size};
  llvm::ArrayRef<unsigned_t> strides(strideS);

  std::array<unsigned_t,4> padS = {(unsigned_t)pad_size, (unsigned_t)pad_size, (unsigned_t)pad_size, (unsigned_t)pad_size};
  llvm::ArrayRef<unsigned_t> pads(padS);
  fwdConvolutionInstFloatImpl(
          &inV, &outV, &filterV, biasV,
          kernelSizes, strides,
          pads, group, dilation, doRelu);
  return 0;
}


void fwdVTAConvolutionInstQuantizedImpl(
        Tensor *inV, Tensor *outV, Tensor *filterV, Tensor *biasV,
        llvm::ArrayRef<unsigned_t> kernelSizes, llvm::ArrayRef<unsigned_t> strides,
        llvm::ArrayRef<unsigned_t> pads, size_t group, size_t dilation, bool doRelu) {
  auto inW = inV->getHandle<int8_t>();
  auto outW = outV->getHandle<int8_t>();
  auto filterW = filterV->getHandle<int8_t>();
  auto biasW = biasV->getHandle<int32_t>();

  ShapeNHWC odim(outW.dims());
  ShapeNHWC idim(inW.dims());
  ShapeHW kdim(kernelSizes);
  ShapeHW sdim(strides);

  assert(idim.c % group == 0 && "Input channels must be divisible by group.");
  assert(odim.c % group == 0 && "Output channels must be divisible by group.");
  dim_t inCperG = idim.c / group;
  dim_t outCperG = odim.c / group;

  PaddingTLBR pdim(pads);
  auto outTy = outV->getType();
  auto inTy = inV->getType();
  auto filterTy = filterV->getType();
  auto biasTy = biasV->getType();

  float outScale = outTy.getScale();
  float inScale = inTy.getScale();
  float filterScale = filterTy.getScale();
  float biasScale = biasTy.getScale();
  filterScale = 1/filterScale;
  inScale = 1/inScale;
  biasScale = 1/biasScale;
  outScale = 1/outScale;

  // Calculate the scale of the values that come out of the matrix
  // multiplication part of the calculation.
  float matMulScale = inScale * filterScale;
  float scale =  matMulScale / outScale;
  float tempScale = 1.0;
  assert(scale > 1);
  uint32_t shift = 0;
  {
    while(tempScale<scale)
    {
      tempScale *= 2;
      shift++;
    }
  }

  // For each input in the batch:
  for (dim_t n = 0; n < idim.n; n++) {
    // For each group of input channels:
    for (dim_t g = 0; g < group; g++) {

      // For each output channel in the group:
      for (dim_t d = g * outCperG; d < (g + 1) * outCperG; d++) {

        // For each convolution 'jump' in the input tensor:
        ssize_t x = -ssize_t(pdim.top);
        for (dim_t ax = 0; ax < odim.h; x += sdim.height, ax++) {
          ssize_t y = -ssize_t(pdim.left);
          for (dim_t ay = 0; ay < odim.w; y += sdim.width, ay++) {

            // For each element in the convolution-filter:
            int32_t sum = 0;
            for (dim_t fx = 0; fx < kdim.height; fx++) {
              for (dim_t fy = 0; fy < kdim.width; fy++) {
                sdim_t ox = x + fx * dilation;
                sdim_t oy = y + fy * dilation;

                // Ignore index access below zero (this is due to padding).
                if (ox < 0 || oy < 0 || ox >= ssize_t(idim.h) ||
                    oy >= sdim_t(idim.w)) {
                  continue;
                }
                for (dim_t fd = 0; fd < inCperG; fd++) {

                  int32_t F = filterW.at({d, fx, fy, fd});
                  int32_t I =
                          inW.at({n, (dim_t)ox, (dim_t)oy, g * inCperG + fd});
                  // We represent the element multiplication with offset as
                  // (value - offset).
                  sum += (F) * (I);
                }
              }
            }

            // Scale the bias to match the scale of the matrix multiplication.

            int32_t B =biasW.at({d});

            // Add the bias.
            sum += B;
            if(doRelu && sum<0) sum = 0;
            // Scale the result back to the expected destination scale.

            outW.at({n, ax, ay, d}) = quantization::clip<int32_t, int8_t>(sum >> shift);

          } // W
        }   // H
      }     // C
    }       // G
  }         // N
}

void fwdConvolutionInstQuantizedImpl(
        Tensor *inV, Tensor *outV, Tensor *filterV, Tensor *biasV,
        llvm::ArrayRef<unsigned_t> kernelSizes, llvm::ArrayRef<unsigned_t> strides,
        llvm::ArrayRef<unsigned_t> pads, size_t group, size_t dilation, bool doRelu) {
  auto inW = inV->getHandle<int8_t>();
  auto outW = outV->getHandle<int8_t>();
  auto filterW = filterV->getHandle<int8_t>();
  auto biasW = biasV->getHandle<int32_t>();

  ShapeNHWC odim(outW.dims());
  ShapeNHWC idim(inW.dims());
  ShapeHW kdim(kernelSizes);
  ShapeHW sdim(strides);

  assert(idim.c % group == 0 && "Input channels must be divisible by group.");
  assert(odim.c % group == 0 && "Output channels must be divisible by group.");
  dim_t inCperG = idim.c / group;
  dim_t outCperG = odim.c / group;

  PaddingTLBR pdim(pads);
  auto outTy = outV->getType();
  auto inTy = inV->getType();
  auto filterTy = filterV->getType();
  auto biasTy = biasV->getType();

  int32_t outOffset = outTy.getOffset();
  int32_t inOffset = inTy.getOffset();
  int32_t filterOffset = filterTy.getOffset();
  int32_t biasOffset = biasTy.getOffset();

  float outScale = outTy.getScale();
  float inScale = inTy.getScale();
  float filterScale = filterTy.getScale();
  float biasScale = biasTy.getScale();

  // Calculate the scale of the values that come out of the matrix
  // multiplication part of the calculation.
  float matMulScale = inScale * filterScale;

  // For each input in the batch:
  for (dim_t n = 0; n < idim.n; n++) {
    // For each group of input channels:
    for (dim_t g = 0; g < group; g++) {

      // For each output channel in the group:
      for (dim_t d = g * outCperG; d < (g + 1) * outCperG; d++) {

        // For each convolution 'jump' in the input tensor:
        ssize_t x = -ssize_t(pdim.top);
        for (dim_t ax = 0; ax < odim.h; x += sdim.height, ax++) {
          ssize_t y = -ssize_t(pdim.left);
          for (dim_t ay = 0; ay < odim.w; y += sdim.width, ay++) {

            // For each element in the convolution-filter:
            int32_t sum = 0;
            for (dim_t fx = 0; fx < kdim.height; fx++) {
              for (dim_t fy = 0; fy < kdim.width; fy++) {
                sdim_t ox = x + fx * dilation;
                sdim_t oy = y + fy * dilation;

                // Ignore index access below zero (this is due to padding).
                if (ox < 0 || oy < 0 || ox >= ssize_t(idim.h) ||
                    oy >= sdim_t(idim.w)) {
                  continue;
                }
                for (dim_t fd = 0; fd < inCperG; fd++) {

                  int32_t F = filterW.at({d, fx, fy, fd});
                  int32_t I =
                          inW.at({n, (dim_t)ox, (dim_t)oy, g * inCperG + fd});
                  // We represent the element multiplication with offset as
                  // (value - offset).
                  sum += (F - filterOffset) * (I - inOffset);
                }
              }
            }

            // Scale the bias to match the scale of the matrix multiplication.

            int32_t B =std::round(float(biasW.at({d}) - biasOffset) *
                                  (biasScale / matMulScale));

            // Add the bias.
            sum += B;

            if(doRelu && sum<0){
              sum = 0;
            }
            // Scale the result back to the expected destination scale.

            outW.at({n, ax, ay, d}) = quantization::clip<int32_t, int8_t>(
                    std::round(float(sum) * (matMulScale / outScale) + outOffset));
          } // W
        }   // H
      }     // C
    }       // G
  }         // NS
}

int nonvtaconvolution(int8_t* input, float inputScale, int32_t inputOffset, int8_t *kernel, float kernelScale, int32_t kernelOffset,
                      int8_t *bias, float biasScale, int32_t biasOffset, int8_t *output, float outputScale, int32_t outputOffset, dim_t N, dim_t H, dim_t W, dim_t C, dim_t KN, dim_t KH, dim_t KW, int pad_size,
                      int stride_size, size_t group, size_t dilation, bool doRelu, bool doBias, int out_h, int out_w) {
  llvm::SmallVector<dim_t, 4> inputDims = {N,H,W,C};
  Type inputType(ElemKind::Int8QTy, inputDims, inputScale, inputOffset);
  Tensor inV((void*)input, &inputType);

  llvm::SmallVector<dim_t, 4> outputDims = {N,(dim_t)out_h,(dim_t)out_w,KN};
  Type outputType(ElemKind::Int8QTy, outputDims, outputScale, outputOffset);
  Tensor outV((void*)output, &outputType);

  llvm::SmallVector<dim_t, 4> kernelDims = {KN,KH,KW,C};
  Type kernelType(ElemKind::Int8QTy, kernelDims, kernelScale, kernelOffset);
  Tensor filterV((void*)kernel, &kernelType);

  Tensor* biasV;
  if(doBias) {
    llvm::SmallVector<dim_t, 1> biasDims = {KN};
    Type biasType(ElemKind::Int32QTy, biasDims, biasScale, biasOffset);
    biasV = new Tensor((void *) bias, &biasType);
  }
  else{
    biasV = new Tensor(ElemKind::Int32QTy, {KN}, 1.0, 0);
    biasV->zero();
  }

  std::array<unsigned_t,2> kernelS = {(unsigned_t)KH, (unsigned_t)KW};
  llvm::ArrayRef<unsigned_t> kernelSizes(kernelS);

  std::array<unsigned_t,2> strideS = {(unsigned_t)stride_size, (unsigned_t)stride_size};
  llvm::ArrayRef<unsigned_t> strides(strideS);

  std::array<unsigned_t,4> padS = {(unsigned_t)pad_size, (unsigned_t)pad_size, (unsigned_t)pad_size, (unsigned_t)pad_size};
  llvm::ArrayRef<unsigned_t> pads(padS);






  float filterScale = kernelScale;
  filterScale = 1/filterScale;
  float inScale = inputScale;
  inScale = 1/inScale;
  //float biasScale = biasScale;
  biasScale = 1/biasScale;
  float matMulScale = inScale * filterScale;
  float outScale = outputScale;
  outScale = 1/outScale;
  float scale =  matMulScale / outScale;
  float tempScale = 1.0;
  assert(scale > 1);
  uint32_t shift = 0;
  {
    while(tempScale<scale)
    {
      tempScale *= 2;
      shift++;
    }
  }

  int32_t outOffset = outputOffset;
  int32_t inOffset = inputOffset;
  int32_t filterOffset = kernelOffset;
  //int32_t biasOffset = biasOffset;
  if(C%16 ==0 && KN%16 ==0 && tempScale==scale && biasScale==matMulScale && outOffset ==0 && inOffset ==0 && filterOffset ==0 && biasOffset ==0) {
    fwdVTAConvolutionInstQuantizedImpl(
        &inV, &outV, &filterV, biasV,
        kernelSizes, strides,
        pads, group, dilation, doRelu);
  }
  else {
    fwdConvolutionInstQuantizedImpl(
        &inV, &outV, &filterV, biasV,
        kernelSizes, strides,
        pads, group, dilation, doRelu);
  }
  return 0;
}


int cpuvtaconvolution(int8_t* input, float inputScale, int32_t inputOffset, int8_t *kernel, float kernelScale, int32_t kernelOffset,
                      int8_t *bias, float biasScale, int32_t biasOffset, int8_t *output, float outputScale, int32_t outputOffset, dim_t N, dim_t H, dim_t W, dim_t C, dim_t KN, dim_t KH, dim_t KW, int pad_size,
                      int stride_size, size_t group, size_t dilation, bool doRelu, bool doBias, int out_h, int out_w) {
  llvm::SmallVector<dim_t, 4> inputDims = {N,H,W,C};
  Type inputType(ElemKind::Int8QTy, inputDims, inputScale, inputOffset);
  Tensor inV((void*)input, &inputType);

  llvm::SmallVector<dim_t, 4> outputDims = {N,(dim_t)out_h,(dim_t)out_w,KN};
  Type outputType(ElemKind::Int8QTy, outputDims, outputScale, outputOffset);
  Tensor outV((void*)output, &outputType);

  llvm::SmallVector<dim_t, 4> kernelDims = {KN,KH,KW,C};
  Type kernelType(ElemKind::Int8QTy, kernelDims, kernelScale, kernelOffset);
  Tensor filterV((void*)kernel, &kernelType);

  Tensor* biasV;
  if(doBias) {
    llvm::SmallVector<dim_t, 1> biasDims = {KN};
    Type biasType(ElemKind::Int32QTy, biasDims, biasScale, biasOffset);
    biasV = new Tensor((void *) bias, &biasType);
  }
  else{
    biasV = new Tensor(ElemKind::Int32QTy, {KN}, 1.0, 0);
    biasV->zero();
  }

    std::array<unsigned_t,2> kernelS = {(unsigned_t)KH, (unsigned_t)KW};
  llvm::ArrayRef<unsigned_t> kernelSizes(kernelS);

  std::array<unsigned_t,2> strideS = {(unsigned_t)stride_size, (unsigned_t)stride_size};
  llvm::ArrayRef<unsigned_t> strides(strideS);

  std::array<unsigned_t,4> padS = {(unsigned_t)pad_size, (unsigned_t)pad_size, (unsigned_t)pad_size, (unsigned_t)pad_size};
  llvm::ArrayRef<unsigned_t> pads(padS);






  float filterScale = kernelScale;
  filterScale = 1/filterScale;
  float inScale = inputScale;
  inScale = 1/inScale;
  //float biasScale = biasScale;
  biasScale = 1/biasScale;
  float matMulScale = inScale * filterScale;
  float outScale = outputScale;
  outScale = 1/outScale;
  float scale =  matMulScale / outScale;
  float tempScale = 1.0;
  assert(scale > 1);
  uint32_t shift = 0;
  {
    while(tempScale<scale)
    {
      tempScale *= 2;
      shift++;
    }
  }

  int32_t outOffset = outputOffset;
  int32_t inOffset = inputOffset;
  int32_t filterOffset = kernelOffset;
  //int32_t biasOffset = biasOffset;
  if(C%16 ==0 && KN%16 ==0 && tempScale==scale && biasScale==matMulScale && outOffset ==0 && inOffset ==0 && filterOffset ==0 && biasOffset ==0) {
    fwdVTAConvolutionInstQuantizedImpl(
            &inV, &outV, &filterV, biasV,
            kernelSizes, strides,
            pads, group, dilation, doRelu);
  }
  else {
    assert(0=="This is not a VTA Convolution");
  }
  return 0;
}


template <typename ElemTy, typename AccumulatorTy, typename BiasElemTy>
void fwdFullyConnectedInstQuantizedImpl(Tensor *inV, Tensor *outV, Tensor *weightsV, Tensor *biasV){
  //const glow::FullyConnectedInst *I) {

  auto inW = inV->getHandle<ElemTy>();
  auto outW = outV->getHandle<ElemTy>();
  auto weightsW = weightsV->getHandle<ElemTy>();
  auto biasW = biasV->getHandle<BiasElemTy>();

  auto inTy = inW.getType();
  auto weightsTy = weightsW.getType();
  auto biasTy = biasW.getType();
  auto outTy = outW.getType();

  int32_t inOffset = inTy.getOffset();
  int32_t weightsOffset = weightsTy.getOffset();
  int32_t biasOffset = biasTy.getOffset();
  int32_t outOffset = outTy.getOffset();

  float outScale = outTy.getScale();
  float weightsScale = weightsTy.getScale();
  float biasScale = biasTy.getScale();
  float inScale = inTy.getScale();

  ShapeHW idim(inW.dims());
  ShapeHW odim(outW.dims());

  // Calculate the scale of the values that come out of the matrix
  // multiplication part of the calculation.
  float matMulScale = weightsScale * inScale;

  outW.clear(0);

  for (dim_t i = 0; i < idim.height; i++) {
    for (dim_t j = 0; j < odim.width; j++) {
      AccumulatorTy sum = 0;
      for (dim_t k = 0; k < idim.width; k++) {
        AccumulatorTy W = weightsW.at({k, j});
        AccumulatorTy A = inW.at({i, k});
        sum += (W - weightsOffset) * (A - inOffset);
      }

      // Scale the bias to match the scale of the matrix multiplication.
      AccumulatorTy B = std::round(float(biasW.at({j}) - biasOffset) *
                                   (biasScale / matMulScale));

      // Add the bias.
      sum += B;

      // Scale the result back to the expected destination scale.
      outW.at({i, j}) = quantization::clip<AccumulatorTy, ElemTy>(
              std::round(float(sum) * (matMulScale / outScale)) + outOffset);
    }
  }
}

int fullyconnected(int8_t* input, float inputScale, int32_t inputOffset, int8_t *kernel, float kernelScale, int32_t kernelOffset,
                   int8_t *bias, float biasScale, int32_t biasOffset, int8_t *output, float outputScale, int32_t outputOffset,
                   dim_t inputDim0, dim_t inputDim1, dim_t kernelDim0, dim_t kernelDim1, dim_t outputDim0, dim_t outputDim1, bool doBias) {
  llvm::SmallVector<dim_t, 2> inputDims = {inputDim0,inputDim1};
  Type inputType(ElemKind::Int8QTy, inputDims, inputScale, inputOffset);
  Tensor inV((void*)input, &inputType);

  llvm::SmallVector<dim_t, 2> outputDims = {outputDim0,outputDim1};
  Type outputType(ElemKind::Int8QTy, outputDims, outputScale, outputOffset);
  Tensor outV((void*)output, &outputType);

  llvm::SmallVector<dim_t, 2> kernelDims = {kernelDim0, kernelDim1};
  Type kernelType(ElemKind::Int8QTy, kernelDims, kernelScale, kernelOffset);
  Tensor filterV((void*)kernel, &kernelType);

  Tensor* biasV;
  if(doBias) {
    llvm::SmallVector<dim_t, 1> biasDims = {kernelDim1};
    Type biasType(ElemKind::Int32QTy, biasDims, biasScale, biasOffset);
    biasV = new Tensor((void *) bias, &biasType);
  }
  else{
    biasV = new Tensor(ElemKind::Int32QTy, {kernelDim1}, 1.0, 0);
    biasV->zero();
  }
  fwdFullyConnectedInstQuantizedImpl<int8_t, int32_t, int32_t>(
          &inV, &outV, &filterV, biasV);

  return 0;
}



template <class T>
void fwdMaxPool(Tensor *inW, Tensor *outW,
                llvm::ArrayRef<unsigned_t> kernelSizes,
                llvm::ArrayRef<unsigned_t> strides,
                llvm::ArrayRef<unsigned_t> pads) {
  ShapeNHWC odim(outW->dims());
  ShapeNHWC idim(inW->dims());
  Handle<T> inHandle = inW->getHandle<T>();
  Handle<T> outHandle = outW->getHandle<T>();
  PaddingTLBR pdim(pads);
  ShapeHW kdim(kernelSizes);
  ShapeHW sdim(strides);

  // For each input in the batch:
  for (dim_t n = 0; n < odim.n; n++) {

    // For each layer in the output tensor:
    for (dim_t z = 0; z < idim.c; z++) {
      // For each convolution 'jump' in the input tensor:
      sdim_t x = -sdim_t(pdim.top);
      for (dim_t ax = 0; ax < odim.h; x += sdim.height, ax++) {
        sdim_t y = -sdim_t(pdim.left);
        for (dim_t ay = 0; ay < odim.w; y += sdim.width, ay++) {

          bool first = true;
          T max_value = 0;
          dim_t argmaxNHWC = 0;

          for (dim_t fx = 0; fx < kdim.height; fx++) {
            for (dim_t fy = 0; fy < kdim.width; fy++) {
              sdim_t ox = x + fx;
              sdim_t oy = y + fy;

              // Ignore index access below zero (this is due to padding).
              if (ox < 0 || oy < 0 || ox >= ssize_t(idim.h) ||
                  oy >= ssize_t(idim.w)) {
                continue;
              }

              T val = inHandle.at({n, (dim_t)ox, (dim_t)oy, z});
              if (first || (val >= max_value)) {
                first = false;
                max_value = val;
              }
            }
          }
          outHandle.at({n, ax, ay, z}) = max_value;

        } // W
      }   // H
    }     // C
  }       // N
}



int maxpool(int8_t* input, int8_t *output, dim_t N, dim_t H, dim_t W, dim_t C, unsigned_t KH, unsigned_t KW, unsigned_t pad_size,
            unsigned_t stride_size){

  std::array<unsigned_t,4> padsS = {pad_size, pad_size, pad_size, pad_size};
  llvm::ArrayRef<unsigned_t> padShape(padsS);

  std::array<unsigned_t,2> kernelS = {KH, KW};
  llvm::ArrayRef<unsigned_t> kernelShape(kernelS);

  std::array<unsigned_t,2> strideS = {stride_size, stride_size};
  llvm::ArrayRef<unsigned_t> strideShape(strideS);

  llvm::SmallVector<dim_t, 4> inputDims = {N, H, W, C};
  Type inputType(ElemKind::Int8QTy, inputDims, 1.0, 0);
  Tensor inputs((void*)input, &inputType);


  dim_t OH = (H+2*pad_size-KH)/stride_size + 1;
  dim_t OW = (W+2*pad_size-KW)/stride_size + 1;

  llvm::SmallVector<dim_t, 4> outputDims = {N, OH, OW, C};
  Type outputType(ElemKind::Int8QTy, outputDims, 1.0, 0);
  Tensor outputs((void*)output, &outputType);

  fwdMaxPool<int8_t>(&inputs, &outputs, kernelShape, strideShape, padShape);
  return 0;

}

int relu(int8_t* input, int8_t *output, unsigned_t size, float inputScale, float outputScale){
  for(int i = 0; i < size; i ++){
    if(input[i]>0)
      output[i]=input[i];
    else
      output[i]=0;
    output[i] *= (int)(inputScale/outputScale);
  }

  return 0;
}

void fwdAvgPoolInstI8Impl(Tensor *inW, Tensor *outW,
                          llvm::ArrayRef<unsigned_t> kernelSizes,
                          llvm::ArrayRef<unsigned_t> strides,
                          llvm::ArrayRef<unsigned_t> pads) {
  ShapeNHWC odim(outW->dims());
  ShapeNHWC idim(inW->dims());

  PaddingTLBR pdim(pads);
  ShapeHW kdim(kernelSizes);
  ShapeHW sdim(strides);

  Handle<int8_t> inHandle = inW->getHandle<int8_t>();
  Handle<int8_t> outHandle = outW->getHandle<int8_t>();

  // Implement the avg pooling operation as defined here:
  // https://arxiv.org/abs/1312.4400
  float filterArea = kdim.height * kdim.width;

  TensorQuantizationParams inQP{inW->getType().getScale(),
                                inW->getType().getOffset()};
  TensorQuantizationParams outQP{outW->getType().getScale(),
                                 outW->getType().getOffset()};

  // For each input in the batch:
  for (dim_t n = 0; n < odim.n; n++) {
    // For each layer in the output tensor:
    for (dim_t z = 0; z < idim.c; z++) {
      // For each convolution 'jump' in the input tensor:
      ssize_t x = -ssize_t(pdim.top);
      for (dim_t ax = 0; ax < odim.h; x += sdim.height, ax++) {
        ssize_t y = -ssize_t(pdim.left);
        for (dim_t ay = 0; ay < odim.w; y += sdim.width, ay++) {
          int32_t sum = 0;

          for (dim_t fx = 0; fx < kdim.height; fx++) {
            for (dim_t fy = 0; fy < kdim.width; fy++) {
              sdim_t ox = x + fx;
              sdim_t oy = y + fy;

              // Ignore index access below zero (this is due to padding).
              if (ox < 0 || oy < 0 || ox >= ssize_t(idim.h) ||
                  oy >= ssize_t(idim.w)) {
                continue;
              }

              sum += inHandle.at({n, (dim_t)ox, (dim_t)oy, z}) - inQP.offset;
            }
          }
          // Instead of dividing by filterArea, just change scale.
          outHandle.at({n, ax, ay, z}) = quantization::clip<int32_t, int8_t>(
                  std::round(float(sum) * (inQP.scale / outQP.scale / filterArea) +
                             outQP.offset));
        } // W
      }   // H
    }     // C
  }       // N

  return;
}


int avgpool(int8_t* input, float inputScale, int32_t inputOffset, int8_t *output, float outputScale, int32_t outputOffset,
            dim_t inputDim0, dim_t inputDim1, dim_t inputDim2, dim_t inputDim3,
            dim_t outputDim0, dim_t outputDim1, dim_t outputDim2, dim_t outputDim3,
            unsigned_t KH, unsigned_t KW, unsigned_t pad_size, unsigned_t stride_size) {
  std::array<unsigned_t,4> padsS = {pad_size, pad_size, pad_size, pad_size};
  llvm::ArrayRef<unsigned_t> padShape(padsS);

  std::array<unsigned_t,2> kernelS = {KH, KW};
  llvm::ArrayRef<unsigned_t> kernelShape(kernelS);

  std::array<unsigned_t,2> strideS = {stride_size, stride_size};
  llvm::ArrayRef<unsigned_t> strideShape(strideS);

  llvm::SmallVector<dim_t, 4> inputDims = {inputDim0, inputDim1, inputDim2, inputDim3};
  Type inputType(ElemKind::Int8QTy, inputDims, inputScale, inputOffset);
  Tensor inputs((void*)input, &inputType);

  llvm::SmallVector<dim_t, 4> outputDims = {outputDim0, outputDim1, outputDim2, outputDim3};
  Type outputType(ElemKind::Int8QTy, outputDims, outputScale, outputOffset);
  Tensor outputs((void*)output, &outputType);

  fwdAvgPoolInstI8Impl(&inputs, &outputs, kernelShape, strideShape, padShape);
  return 0;
}

int quantize(int8_t *input, int8_t *output, dim_t size, float scale, int32_t offset){
  llvm::SmallVector<dim_t, 1> inputDims = {size};
  Type inputType(ElemKind::FloatTy, inputDims);
  Tensor inputs((void*)input, &inputType);


  llvm::SmallVector<dim_t, 1> outputDims = {size};
  Type outputType(ElemKind::Int8QTy, outputDims, scale, offset);
  Tensor outputs((void*)output, &outputType);

  auto destTy = outputs.getType();
  Tensor qTensor = quantization::quantizeTensor(
          inputs, {destTy.getScale(), destTy.getOffset()},
          destTy.getElementType());
  outputs.assign(&qTensor);
  return 0;
}

int dequantize(int8_t *input, int8_t *output, dim_t size, float scale, int32_t offset){
  llvm::SmallVector<dim_t, 1> inputDims = {size};
  Type inputType(ElemKind::Int8QTy, inputDims, scale, offset);
  Tensor inputs((void*)input, &inputType);


  llvm::SmallVector<dim_t, 1> outputDims = {size};
  Type outputType(ElemKind::FloatTy, outputDims);
  Tensor outputs((void*)output, &outputType);

  auto destTy = outputs.getType();
  Tensor qTensor = quantization::dequantizeTensor(
          inputs, destTy.getElementType());
  outputs.assign(&qTensor);
  return 0;
}


int transpose(int8_t *input, int8_t *output, dim_t inDim0, dim_t inDim1, dim_t inDim2, dim_t inDim3,
              dim_t outDim0, dim_t outDim1, dim_t outDim2, dim_t outDim3,
              unsigned_t shf0, unsigned_t shf1, unsigned_t shf2, unsigned_t shf3) {

  llvm::SmallVector<dim_t, 4> inputDims = {inDim0, inDim1, inDim2, inDim3};
  Type inputType(ElemKind::Int8QTy, inputDims, 1.0, 0);
  Tensor inputs((void*)input, &inputType);

  llvm::SmallVector<dim_t, 4> outputDims = {outDim0, outDim1, outDim2, outDim3};
  Type outputType(ElemKind::Int8QTy, outputDims, 1.0, 0);
  Tensor outputs((void*)output, &outputType);

  std::array<unsigned_t, 4> T{{shf0, shf1, shf2, shf3}};
  llvm::ArrayRef<unsigned_t> shuffle(T);

  assert(inputs.size() == outputs.size() && "Invalid tensor dimensions");

  inputs.transpose(&outputs, shuffle);

  return 0;
}

int elemadd(int8_t *input0, float input0Scale, int32_t input0Offset, int8_t *input1, float input1Scale, int32_t input1Offset,
            int8_t *output, float destScale, int32_t destOffset, unsigned_t size) {
  for (dim_t i = 0, e = size; i < e; i++) {
    int32_t L = input0[i];
    int32_t R = input1[i];

    // We increase the size of the integer up to 16 bits to prevent overflow.
    const float largeScale = float(1) / (1 << 15);
    // Scale both sides from 8-bit to 16-bits.
    int32_t L32 = std::round(float(L - input0Offset) * (input0Scale / largeScale));
    int32_t R32 = std::round(float(R - input1Offset) * (input1Scale / largeScale));
    int32_t sum32 = L32 + R32;
    sum32 = std::round(float(sum32) * (largeScale / destScale) + destOffset);
    output[i] = quantization::clip<int32_t, int8_t>(sum32);
  }
  return 0;
}


int elemsub(int8_t *input0, float input0Scale, int32_t input0Offset, int8_t *input1, float input1Scale, int32_t input1Offset,
            int8_t *output, float destScale, int32_t destOffset, unsigned_t size) {
  for (dim_t i = 0, e = size; i < e; i++) {
    int32_t L = input0[i];
    int32_t R = input1[i];

    float l = (input0Scale / destScale) * float(L - input0Offset);
    float r = (input1Scale / destScale) * float(R - input1Offset);
    int32_t q = std::round(l - r + destOffset);
    output[i] = quantization::clip<int32_t, int8_t>(q);
  }
  return 0;
}

int elemsub_f32(int8_t *input0, float input0Scale, int32_t input0Offset, int8_t *input1, float input1Scale, int32_t input1Offset,
            int8_t *output, float destScale, int32_t destOffset, unsigned_t size) {
  for (dim_t i = 0, e = size; i < e; i++) {
    float L = ((float*)input0)[i];
    float R = ((float*)input1)[i];
    ((float*)output)[i] = L - R;
  }
  return 0;
}


int elemdiv(int8_t *input0, float input0Scale, int32_t input0Offset, int8_t *input1, float input1Scale, int32_t input1Offset,
            int8_t *output, float destScale, int32_t destOffset, unsigned_t size) {
  for (dim_t i = 0, e = size; i < e; i++) {
    int32_t L = input0[i];
    int32_t R = input1[i];

    float l = (input0Scale) * float(L - input0Offset);
    float r = input1Scale * destScale * float(R - input1Offset);
    int32_t q = std::round(l / r + destOffset);
    output[i] = quantization::clip<int32_t, int8_t>(q);
  }
  return 0;
}

int elemdiv_f32(int8_t *input0, float input0Scale, int32_t input0Offset, int8_t *input1, float input1Scale, int32_t input1Offset,
            int8_t *output, float destScale, int32_t destOffset, unsigned_t size) {
  for (dim_t i = 0, e = size; i < e; i++) {
    float L = ((float*)input0)[i];
    float R = ((float*)input1)[i];
    ((float*)output)[i] = L / R;
  }
  return 0;
}

int splat(int8_t *output, unsigned_t size, int8_t value) {
  for( int i = 0; i<size; i++ ){
    output[i]=value;
  }
  return 0;
}


int elemmax(int8_t *input0, float input0Scale, int32_t input0Offset, int8_t *input1, float input1Scale, int32_t input1Offset,
            int8_t *output, float destScale, int32_t destOffset, unsigned_t size) {
  TensorQuantizationParams lhsQ{input0Scale, input0Offset};
  TensorQuantizationParams rhsQ{input1Scale, input1Offset};
  TensorQuantizationParams destQ{destScale, destOffset};

  for (size_t i = 0, e = size; i < e; i++) {
    // Convert both sides to the destination scale and perform a regular
    // comparison.
    int8_t L = quantization::quantize(
            quantization::dequantize(input0[i], lhsQ), destQ);
    int8_t R = quantization::quantize(
            quantization::dequantize(input1[i], rhsQ), destQ);
    output[i] = std::max(L, R);
  }
  return 0;
}

int softmax(int8_t *input, int8_t *output, dim_t inDim0, dim_t inDim1,
            dim_t outDim0, dim_t outDim1){
  float *inputP = (float *)input;
  float *outputP = (float *)output;
  for (dim_t n = 0; n < inDim0; n++) {
    // Find Max.
    float max = inputP[n * inDim0];
    for (dim_t i = 1; i < inDim1; i++) {
      max = std::max(max, inputP[n * inDim0 + i]);
    }

    // Compute exp.
    float sum = 0;
    for (dim_t i = 0; i < inDim1; i++) {
      float e = std::exp(inputP[n * inDim0 + i] - max);
      sum += e;
      outputP[n*inDim0 +i] = float(e);
    }
    // Normalize the output.
    for (dim_t i = 0; i < inDim1; i++) {
      outputP[n*inDim0 +i] =
          outputP[n*inDim0 +i] / sum;
    }
  }

  return 0;
}


void debugprint(int8_t *input, unsigned_t size, std::string filename, bool isInt8 ) {
  std::ofstream fos(filename, std::ios::binary);

  if(isInt8) {
    int16_t data16 = 0;
    for (size_t i = 0, e = size; i < e; i++) {
      auto data = input[i];
      if (i % 2 == 0) {
        data16 = 0xff & data;
      } else {
        data16 = data16 | data << 8;
        fos.write((const char *) &data16, 2);
      }
    }
  }
  else{
    int32_t* input_32 = (int32_t*) input;
    for (size_t i = 0, e = size; i < e; i++) {
      auto data = input_32[i];
      fos.write((const char *)&data, 4);
    }
  }
  fos.close();
}


void transpose_nhwc2vtaio(int8_t* input, int8_t* output, int N, int H, int W, int C){
  int8_t *IN_NHWC = input;

  int8_t *IN_NCHW = (int8_t *)malloc(N*C*H*W);
  for (int n = 0; n < N; n++) {
    for (int h = 0; h < H; h++) {
      for (int w = 0; w < W; w++) {
        for (int c = 0; c < C; c++) {
          *(IN_NCHW + n*C*H*W + c*H*W + h*W + w)=*(IN_NHWC + n*H*W*C + h*W*C + w*C + c);
        }
      }
    }
  }

  // reshape input NCHW -> N{C//16}16HW
  int8_t *in_NC_16HW = IN_NCHW;


  for (int n = 0; n < N; n++) {
    for (int c_ = 0; c_ < C / 16; c_++) {
      for (int h = 0; h < H; h++) {
        for (int w = 0; w < W; w++) {
          for (int c = 0; c < 16; c++) {
            *(output + n*(C/16)*H*W*16 + c_*H*W*16 + h*W*16 + w*16 + c) =
                *(in_NC_16HW + n*(C/16)*16*H*W + c_*16*H*W + c*H*W + h*W + w);
          }
        }
      }
    }
  }

  free(IN_NCHW);
}

void transpose_nhwc2vtak(int8_t* input, int8_t* output, int KN, int KH, int KW, int C){
  // filter NHWC
  int8_t *filter_NHWC = input;

  int8_t* filter_N_16HWC_16 = filter_NHWC;
  // transpose filter {N//16}16HW{C//16}16 =>  {N//16}{C//16}HW1616
  for (int n_ = 0; n_ < KN / 16; n_++) {
    for (int c_ = 0; c_ < C / 16; c_++) {
      for (int h = 0; h < KH; h++) {
        for (int w = 0; w < KW; w++) {
          for (int n = 0; n < 16; n++) {
            for (int c = 0; c < 16; c++) {
              *(output + n_*(C/16)*KH*KW*16*16 + c_ *KH*KW*16*16 + h*KW*16*16 + w*16*16 + n*16 + c) =
                  *(filter_N_16HWC_16 + n_*16*KH*KW*C + n*KH*KW*C + h*KW*C + w*C + c_*16 + c);
            }
          }
        }
      }
    }
  }
}

void transpose_vtak2nhwc(int8_t* input, int8_t* output, int KN, int KH, int KW, int C){
  for (int n_ = 0; n_ < KN / 16; n_++) {
    for (int c_ = 0; c_ < C / 16; c_++) {
      for (int h = 0; h < KH; h++) {
        for (int w = 0; w < KW; w++) {
          for (int n = 0; n < 16; n++) {
            for (int c = 0; c < 16; c++) {
              *(output + n_*16*KH*KW*C + n*KH*KW*C + h*KW*C + w*C + c_*16 + c)
                  = *(input + n_*(C/16)*KH*KW*16*16 + c_ *KH*KW*16*16 + h*KW*16*16 + w*16*16 + n*16 + c);
            }
          }
        }
      }
    }
  }
}


void transpose_vtaio2nhwc(int8_t* input, int8_t* output, int N, int H, int W, int C){
  for (int b = 0; b < N; b++) {
    for (int f = 0; f < C; f++) {
      for (int i = 0; i < H; i++) {    //h
        for (int j = 0; j < W; j++) {    //w

          int out_ch = f / 16;
          int oo = f % 16;
          auto value =*(input + (out_ch * H * W * 16 + i * W * 16 + j * 16 + oo));
          *(output + b * H * W * C + i * W * C + j * C + f) = value;
        }
      }
    }
  }
}