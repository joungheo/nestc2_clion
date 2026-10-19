#include <iostream>
#include <ctime>
#include <cstring>
#include <cstdint>
#include <fstream>
#include <vector>
//#### JO added: std::fill in convolutionFloat
#include <algorithm>

#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <cassert>
#include "llvm/ADT/ArrayRef.h"
//#include "glow/Base/Tensor.h"
//#include "glow/Quantization/Base/Base.h"

#include "vta/vtalib/include/Bundle/VTABundle.h"

using namespace std;
//using namespace glow;
//#### JO changed: convolutionFloat was a single-threaded loop with the output channel outermost
//#### (Resnet18 conv0: 1547 ms on the ZCU102). Original kept below, rewritten version after it.
//int convolutionFloat(int8_t* input, int8_t *kernel, int8_t *bias, int8_t *output, dim_t N, dim_t H, dim_t W, dim_t C, dim_t KN, dim_t KH, dim_t KW, int pad_size,
//                     int stride_size, size_t group, size_t dilation, bool doRelu, bool doBias, int out_h, int out_w) {
//    dim_t inCperG = C / group;
//    dim_t outCperG = KN / group;
//
//    for (dim_t n = 0; n < N; n++) {
//        // For each group of input channels:
//        for (dim_t g = 0; g < group; g++) {
//
//            // For each output channel in the group:
//            for (dim_t d = g * outCperG; d < (g + 1) * outCperG; d++) {
//
//                // For each convolution 'jump' in the input tensor:
//                ssize_t x = -ssize_t(pad_size);
//                for (dim_t ax = 0; ax < out_h; x += stride_size, ax++) {
//                    ssize_t y = -ssize_t(pad_size);
//                    for (dim_t ay = 0; ay < out_w; y += stride_size, ay++) {
//
//                        // For each element in the convolution-filter:
//                        float sum = 0.0;
//                        for (dim_t fx = 0; fx < KH; fx++) {
//                            for (dim_t fy = 0; fy < KW; fy++) {
//                                int64_t ox = x + fx * dilation;
//                                int64_t oy = y + fy * dilation;
//
//                                // Ignore index access below zero (this is due to padding).
//                                if (ox < 0 || oy < 0 || ox >= ssize_t(H) ||
//                                    oy >= int64_t(W)) {
//                                    continue;
//                                }
//                                for (dim_t fd = 0; fd < inCperG; fd++) {
//
//                                    float F = *((float*)kernel+d*KH*KW*inCperG + fx*KW*inCperG + fy*inCperG + fd);
//                                    float I = *((float*)input+n*H*W*C + ox*W*C + oy*C+ (g*inCperG +fd));
//                                    // We represent the element multiplication with offset as
//                                    // (value - offset).
//                                    sum += F * I;
//                                }
//                            }
//                        }
//
//                        float B = *((float *)bias+d);
//
//                        // Add the bias.
//                        sum += B;
//
//                        if(doRelu && sum<0.0){
//                            sum = 0.0;
//                        }
//
//                        *((float *)output + n*out_h*out_w*KN + ax*out_w*KN + ay*KN + d) = sum;
//                    } // W
//                }   // H
//            }     // C
//        }       // G
//    }         // NS
//    return 0;
//}
int convolutionFloat(int8_t* input, int8_t *kernel, int8_t *bias, int8_t *output, dim_t N, dim_t H, dim_t W, dim_t C, dim_t KN, dim_t KH, dim_t KW, int pad_size,
                     int stride_size, size_t group, size_t dilation, bool doRelu, bool doBias, int out_h, int out_w) {
    // Same arithmetic as before (per output element: sum over fx, fy, fd in the same order, then + bias),
    // but output channels are innermost: the kernel is repacked to [fx][fy][fd][KN] so that the
    // d loop is contiguous (vectorizable), and output rows are spread over the OpenMP threads.
    const dim_t inCperG = C / group;
    const dim_t outCperG = KN / group;
    const float *in = (const float *)input;
    const float *ker = (const float *)kernel;
    const float *b = (const float *)bias;
    float *out = (float *)output;

    std::vector<float> kt(KH * KW * inCperG * KN);
    for (dim_t d = 0; d < KN; d++)
        for (dim_t k = 0; k < KH * KW * inCperG; k++)
            kt[k * KN + d] = ker[d * KH * KW * inCperG + k];

    const ssize_t rows = ssize_t(N) * out_h;
#pragma omp parallel
    {
        std::vector<float> acc(KN);
#pragma omp for schedule(static)
        for (ssize_t r = 0; r < rows; r++) {
            const dim_t n = r / out_h;
            const dim_t ax = r % out_h;
            const ssize_t x = ssize_t(ax) * stride_size - pad_size;
            for (dim_t ay = 0; ay < dim_t(out_w); ay++) {
                const ssize_t y = ssize_t(ay) * stride_size - pad_size;
                std::fill(acc.begin(), acc.end(), 0.0f);
                for (dim_t g = 0; g < group; g++) {
                    float *a = acc.data() + g * outCperG;
                    for (dim_t fx = 0; fx < KH; fx++) {
                        const ssize_t ox = x + ssize_t(fx * dilation);
                        if (ox < 0 || ox >= ssize_t(H)) continue;
                        for (dim_t fy = 0; fy < KW; fy++) {
                            const ssize_t oy = y + ssize_t(fy * dilation);
                            // Ignore index access below zero (this is due to padding).
                            if (oy < 0 || oy >= ssize_t(W)) continue;
                            const float *ip = in + n * H * W * C + ox * W * C + oy * C + g * inCperG;
                            const float *kp = kt.data() + ((fx * KW + fy) * inCperG) * KN + g * outCperG;
                            for (dim_t fd = 0; fd < inCperG; fd++) {
                                const float I = ip[fd];
                                const float *k = kp + fd * KN;
                                for (dim_t d = 0; d < outCperG; d++)
                                    a[d] += k[d] * I;
                            }
                        }
                    }
                }
                float *o = out + n * out_h * out_w * KN + ax * out_w * KN + ay * KN;
                for (dim_t d = 0; d < KN; d++) {
                    float sum = acc[d] + b[d];
                    if (doRelu && sum < 0.0) sum = 0.0;
                    o[d] = sum;
                }
            }
        }
    }
    return 0;
}


int cpuvtaconvolution(int8_t* input, float inputScale, int32_t inputOffset, int8_t *kernel, float kernelScale, int32_t kernelOffset,
                      int8_t *bias, float biasScale, int32_t biasOffset, int8_t *output, float outputScale, int32_t outputOffset, dim_t N, dim_t H, dim_t W, dim_t C, dim_t KN, dim_t KH, dim_t KW, int pad_size,
                      int stride_size, size_t group, size_t dilation, bool doRelu, bool doBias, int out_h, int out_w) {
    dim_t inCperG = C / group;
    dim_t outCperG = KN / group;

    float matMulScale = inputScale * kernelScale;
    float scale =  outputScale / matMulScale;
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

    for (dim_t n = 0; n < N; n++) {
        // For each group of input channels:
        for (dim_t g = 0; g < group; g++) {

            // For each output channel in the group:
            for (dim_t d = g * outCperG; d < (g + 1) * outCperG; d++) {

                // For each convolution 'jump' in the input tensor:
                ssize_t x = -ssize_t(pad_size);
                for (dim_t ax = 0; ax < out_h; x += stride_size, ax++) {
                    ssize_t y = -ssize_t(pad_size);
                    for (dim_t ay = 0; ay < out_w; y += stride_size, ay++) {

                        // For each element in the convolution-filter:
                        int32_t sum = 0;
                        for (dim_t fx = 0; fx < KH; fx++) {
                            for (dim_t fy = 0; fy < KW; fy++) {
                                int64_t ox = x + fx * dilation;
                                int64_t oy = y + fy * dilation;

                                // Ignore index access below zero (this is due to padding).
                                if (ox < 0 || oy < 0 || ox >= ssize_t(H) ||
                                    oy >= int64_t(W)) {
                                    continue;
                                }
                                for (dim_t fd = 0; fd < inCperG; fd++) {

                                    int32_t F = (int32_t)*(kernel+d*KH*KW*inCperG + fx*KW*inCperG + fy*inCperG + fd);
                                    int32_t I = (int32_t)*(input+n*H*W*C + ox*W*C + oy*C+ (g*inCperG +fd));
                                    // We represent the element multiplication with offset as
                                    // (value - offset).
                                    sum += (F) * (I);
                                }
                            }
                        }

                        // Scale the bias to match the scale of the matrix multiplication.
                        if(doBias){
                          int32_t B = *((int32_t *)bias+d);

                          // Add the bias.
                          sum += B;
                        }

                        if(doRelu && sum<0){
                            sum = 0;
                        }
                        // Scale the result back to the expected destination scale.
                        auto out32 = sum >> shift;
                        int8_t out8;
                        if(out32>127) out8=127;
                        else if(out32<-128) out8=-128;
                        else out8=(int8_t)out32;
                        *(output + n*out_h*out_w*KN + ax*out_w*KN + ay*KN + d) = out8;
                    } // W
                }   // H
            }     // C
        }       // G
    }         // NS
    return 0;

}


int cpunonvtaconvolution(int8_t* input, float inputScale, int32_t inputOffset, int8_t *kernel, float kernelScale, int32_t kernelOffset,
                      int8_t *bias, float biasScale, int32_t biasOffset, int8_t *output, float outputScale, int32_t outputOffset, dim_t N, dim_t H, dim_t W, dim_t C, dim_t KN, dim_t KH, dim_t KW, int pad_size,
                      int stride_size, size_t group, size_t dilation, bool doRelu, bool doBias, int out_h, int out_w) {
    dim_t inCperG = C / group;
    dim_t outCperG = KN / group;
    float matMulScale = inputScale * kernelScale;
    for (dim_t n = 0; n < N; n++) {
        // For each group of input channels:
        for (dim_t g = 0; g < group; g++) {

            // For each output channel in the group:
            for (dim_t d = g * outCperG; d < (g + 1) * outCperG; d++) {

                // For each convolution 'jump' in the input tensor:
                ssize_t x = -ssize_t(pad_size);
                for (dim_t ax = 0; ax < out_h; x += stride_size, ax++) {
                    ssize_t y = -ssize_t(pad_size);
                    for (dim_t ay = 0; ay < out_w; y += stride_size, ay++) {

                        // For each element in the convolution-filter:
                        int32_t sum = 0;
                        for (dim_t fx = 0; fx < KH; fx++) {
                            for (dim_t fy = 0; fy < KW; fy++) {
                                int64_t ox = x + fx * dilation;
                                int64_t oy = y + fy * dilation;

                                // Ignore index access below zero (this is due to padding).
                                if (ox < 0 || oy < 0 || ox >= ssize_t(H) ||
                                    oy >= int64_t(W)) {
                                    continue;
                                }
                                for (dim_t fd = 0; fd < inCperG; fd++) {

                                    int32_t F = (int32_t)*(kernel+d*KH*KW*inCperG + fx*KW*inCperG + fy*inCperG + fd);
                                    int32_t I = (int32_t)*(input+n*H*W*C + ox*W*C + oy*C+ (g*inCperG +fd));
                                    // We represent the element multiplication with offset as
                                    // (value - offset).
                                    sum += (F - kernelOffset) * (I - inputOffset);
                                }
                            }
                        }

                        // Scale the bias to match the scale of the matrix multiplication.
                        int32_t B =std::round(float(*((int32_t *)bias+d) - biasOffset) *
                                              (biasScale / matMulScale));

                        // Add the bias.
                        sum += B;

                        if(doRelu && sum<0){
                            sum = 0;
                        }
                        // Scale the result back to the expected destination scale.
                        int32_t out32 = std::round(float(sum) * (matMulScale / outputScale) + outputOffset);
                        int8_t out8;
                        if(out32>127) out8=127;
                        else if(out32<-128) out8=-128;
                        else out8=(int8_t)out32;
                        *(output + n*out_h*out_w*KN + ax*out_w*KN + ay*KN + d) = out8;
                    } // W
                }   // H
            }     // C
        }       // G
    }         // NS
    return 0;

}


int nonvtaconvolution(int8_t* input, float inputScale, int32_t inputOffset, int8_t *kernel, float kernelScale, int32_t kernelOffset,
                      int8_t *bias, float biasScale, int32_t biasOffset, int8_t *output, float outputScale, int32_t outputOffset, dim_t N, dim_t H, dim_t W, dim_t C, dim_t KN, dim_t KH, dim_t KW, int pad_size,
                      int stride_size, size_t group, size_t dilation, bool doRelu, bool doBias, int out_h, int out_w) {

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
      cpuvtaconvolution(input, inputScale, inputOffset, kernel, kernelScale, kernelOffset,
                        bias, biasScale, biasOffset, output, outputScale, outputOffset, N, H, W, C, KN, KH, KW, pad_size,
                        stride_size, group, dilation, doRelu, doBias, out_h, out_w);
  }
  else {
      cpunonvtaconvolution(input, inputScale, inputOffset, kernel, kernelScale, kernelOffset,
                        bias, 1.0/biasScale, biasOffset, output, outputScale, outputOffset, N, H, W, C, KN, KH, KW, pad_size,
                        stride_size, group, dilation, doRelu, doBias, out_h, out_w);
  }
  return 0;
}


//#### JO changed: cpuFullyConnected single-threaded -> OpenMP (bit-identical result); original kept below
//int cpuFullyConnected(int8_t* input, float inputScale, int32_t inputOffset, int8_t *kernel, float kernelScale, int32_t kernelOffset,
//                         int8_t *bias, float biasScale, int32_t biasOffset, int8_t *output, float outputScale, int32_t outputOffset,
//                      dim_t inputDim0, dim_t inputDim1, dim_t kernelDim0, dim_t kernelDim1, dim_t outputDim0, dim_t outputDim1, bool doBias) {
//
//
//    //dim_t inCperG = C / group;
//    //dim_t outCperG = KN / group
//
//    float matMulScale = inputScale * kernelScale;
//
//
//
//    for (dim_t i = 0; i < inputDim0; i++) {
//        for (dim_t j = 0; j < outputDim1; j++) {
//            int32_t sum = 0;
//            for (dim_t k = 0; k < inputDim1; k++) {
//                int32_t W = (int32_t)*(kernel+k*kernelDim1 + j);
//                int32_t A = (int32_t)*(input+i*inputDim1 + k);
//                sum += (W - kernelOffset) * (A - inputOffset);
//            }
//
//            // Scale the bias to match the scale of the matrix multiplication.
//            int32_t B =std::round(float(*((int32_t *)bias+j) - biasOffset) *
//                                  (biasScale / matMulScale));
//
//            // Add the bias.
//            sum += B;
//
//            // Scale the result back to the expected destination scale.
//
//            int32_t out32 = std::round(float(sum) * (matMulScale / outputScale) + outputOffset);
//            int8_t out8;
//            if(out32>127) out8=127;
//            else if(out32<-128) out8=-128;
//            else out8=(int8_t)out32;
//
//            *(output + i*outputDim1 + j) = out8;
//
//        }
//    }
//
//    return 0;
//
//}
int cpuFullyConnected(int8_t* input, float inputScale, int32_t inputOffset, int8_t *kernel, float kernelScale, int32_t kernelOffset,
                         int8_t *bias, float biasScale, int32_t biasOffset, int8_t *output, float outputScale, int32_t outputOffset,
                      dim_t inputDim0, dim_t inputDim1, dim_t kernelDim0, dim_t kernelDim1, dim_t outputDim0, dim_t outputDim1, bool doBias) {
    // Integer accumulation (order-independent, so results are identical); kernel rows are read
    // contiguously (j innermost) and output columns are split over the OpenMP threads.
    float matMulScale = inputScale * kernelScale;
    const ssize_t J = outputDim1;
    for (dim_t i = 0; i < inputDim0; i++) {
        const int8_t *a = input + i * inputDim1;
#pragma omp parallel
        {
            std::vector<int32_t> sum;
#pragma omp for schedule(static)
            for (ssize_t j0 = 0; j0 < J; j0 += 64) {
                const ssize_t nj = std::min<ssize_t>(64, J - j0);
                sum.assign(nj, 0);
                for (dim_t k = 0; k < inputDim1; k++) {
                    const int32_t A = (int32_t)a[k] - inputOffset;
                    const int8_t *w = kernel + k * kernelDim1 + j0;
                    for (ssize_t j = 0; j < nj; j++)
                        sum[j] += ((int32_t)w[j] - kernelOffset) * A;
                }
                for (ssize_t j = 0; j < nj; j++) {
                    // Scale the bias to match the scale of the matrix multiplication.
                    int32_t B = std::round(float(*((int32_t *)bias + j0 + j) - biasOffset) *
                                           (biasScale / matMulScale));
                    int32_t s = sum[j] + B;
                    // Scale the result back to the expected destination scale.
                    int32_t out32 = std::round(float(s) * (matMulScale / outputScale) + outputOffset);
                    int8_t out8;
                    if(out32>127) out8=127;
                    else if(out32<-128) out8=-128;
                    else out8=(int8_t)out32;
                    *(output + i*outputDim1 + j0 + j) = out8;
                }
            }
        }
    }
    return 0;
}

int bnn_cpunonvtaconvolution_scale(int8_t* input, float inputScale, int32_t inputOffset, int8_t *kernel, float kernelScale, int32_t kernelOffset,
                      int8_t *bias, float biasScale, int32_t biasOffset, int8_t *scalefactor, int8_t *output, int32_t outputShift, int32_t outputOffset, dim_t N, dim_t H, dim_t W, dim_t C, dim_t KN, dim_t KH, dim_t KW, int pad_size,
                      int stride_size, size_t group, size_t dilation, bool doRelu, bool doBias, int out_h, int out_w) {
    dim_t inCperG = C / group;
    dim_t outCperG = KN / group;
    float matMulScale = inputScale * kernelScale;
    for (dim_t n = 0; n < N; n++) {
        // For each group of input channels:
        for (dim_t g = 0; g < group; g++) {

            // For each output channel in the group:
            for (dim_t d = g * outCperG; d < (g + 1) * outCperG; d++) {

                // For each convolution 'jump' in the input tensor:
                ssize_t x = -ssize_t(pad_size);
                for (dim_t ax = 0; ax < out_h; x += stride_size, ax++) {
                    ssize_t y = -ssize_t(pad_size);
                    for (dim_t ay = 0; ay < out_w; y += stride_size, ay++) {

                        // For each element in the convolution-filter:
                        int32_t sum = 0;
                        for (dim_t fx = 0; fx < KH; fx++) {
                            for (dim_t fy = 0; fy < KW; fy++) {
                                int64_t ox = x + fx * dilation;
                                int64_t oy = y + fy * dilation;

                                // Ignore index access below zero (this is due to padding).
                                if (ox < 0 || oy < 0 || ox >= ssize_t(H) ||
                                    oy >= int64_t(W)) {
                                    continue;
                                }
                                for (dim_t fd = 0; fd < inCperG; fd++) {

                                    int32_t F = (int32_t)*(kernel+d*KH*KW*inCperG + fx*KW*inCperG + fy*inCperG + fd);
                                    int32_t I = (int32_t)*(input+n*H*W*C + ox*W*C + oy*C+ (g*inCperG +fd));
                                    //uint8_t I = (uint8_t)*(input+n*H*W*C + ox*W*C + oy*C+ (g*inCperG +fd));
                                    // We represent the element multiplication with offset as
                                    // (value - offset).
                                    sum += (F - kernelOffset) * (I - inputOffset);
                                }
                            }
                        }
                        int32_t S = (*((int32_t *)scalefactor + d));
                        // Scale the bias to match the scale of the matrix multiplication.
                        int32_t B =std::round(float(*((int32_t *)bias+d) - biasOffset) * (biasScale / matMulScale));
                        // Add the bias.
                        sum += B;
                        sum *= S;

                        int32_t out32 = int32_t(std::round(float(sum) / (1 << outputShift)));
                        // Scale the result back to the expected destination scale.
                        //int32_t out32 = std::round(float(sum) * (matMulScale / outputScale) + outputOffset);
                        int8_t out8;
                        if(out32>127) out8=127;
                        else if(out32<-128) out8=-128;
                        else out8=(int8_t)out32;
                        *(output + n*out_h*out_w*KN + ax*out_w*KN + ay*KN + d) = out8;
                    } // W
                }   // H
            }     // C
        }       // G
    }         // NS
    return 0;

}

int bnn_nonvtaconvolution_scale(int8_t* input, float inputScale, int32_t inputOffset, int8_t *kernel, float kernelScale, int32_t kernelOffset,
                      int8_t *bias, float biasScale, int32_t biasOffset, int8_t* scalefactor, int8_t *output, int32_t outputShift, int32_t outputOffset, dim_t N, dim_t H, dim_t W, dim_t C, dim_t KN, dim_t KH, dim_t KW, int pad_size,
                      int stride_size, size_t group, size_t dilation, bool doRelu, bool doBias, int out_h, int out_w) {

  //int32_t biasOffset = biasOffset;
  bnn_cpunonvtaconvolution_scale(input, inputScale, inputOffset, kernel, kernelScale, kernelOffset,
                    bias, biasScale, biasOffset, scalefactor, output, outputShift, outputOffset, N, H, W, C, KN, KH, KW, pad_size,
                    stride_size, group, dilation, doRelu, doBias, out_h, out_w);

  return 0;
}


int fullyconnected(int8_t* input, float inputScale, int32_t inputOffset, int8_t *kernel, float kernelScale, int32_t kernelOffset,
                   int8_t *bias, float biasScale, int32_t biasOffset, int8_t *output, float outputScale, int32_t outputOffset,
                   dim_t inputDim0, dim_t inputDim1, dim_t kernelDim0, dim_t kernelDim1, dim_t outputDim0, dim_t outputDim1, bool doBias) {

    cpuFullyConnected(input, inputScale, inputOffset, kernel, kernelScale, kernelOffset,
                         bias, biasScale, biasOffset, output, outputScale, outputOffset,
                         inputDim0, inputDim1, kernelDim0, kernelDim1, outputDim0, outputDim1, doBias);
  return 0;
}



//#### JO changed: maxpool single-threaded -> OpenMP (bit-identical result); original kept below
//int maxpool(int8_t* input, int8_t *output, dim_t N, dim_t H, dim_t W, dim_t C, unsigned_t KH, unsigned_t KW, unsigned_t pad_size,
//            unsigned_t stride_size){
//
//  dim_t OH = (H+2*pad_size-KH)/stride_size + 1;
//  dim_t OW = (W+2*pad_size-KW)/stride_size + 1;
//
//    // For each input in the batch:
//    for (dim_t n = 0; n < N; n++) {
//
//        // For each layer in the output tensor:
//        for (dim_t z = 0; z < C; z++) {
//            // For each convolution 'jump' in the input tensor:
//            dim_t x = -dim_t(pad_size);
//            for (dim_t ax = 0; ax < OH; x += stride_size, ax++) {
//                dim_t y = -dim_t(pad_size);
//                for (dim_t ay = 0; ay < OW; y += stride_size, ay++) {
//
//                    bool first = true;
//                    int8_t max_value = 0;
//                    dim_t argmaxNHWC = 0;
//
//                    for (dim_t fx = 0; fx < KH; fx++) {
//                        for (dim_t fy = 0; fy < KW; fy++) {
//                            dim_t ox = x + fx;
//                            dim_t oy = y + fy;
//
//                            // Ignore index access below zero (this is due to padding).
//                            if (ox < 0 || oy < 0 || ox >= ssize_t(H) ||
//                                oy >= ssize_t(W)) {
//                                continue;
//                            }
//
//                            int8_t val = *(input + n*H*W*C + ox*W*C + oy*C + z);
//                            if (first || (val >= max_value)) {
//                                first = false;
//                                max_value = val;
//                            }
//                        }
//                    }
//
//                    *(output + n*OH*OW*C + ax*OW*C + ay*C + z) = max_value;
//                } // W
//            }   // H
//        }     // C
//    }       // N
//
//
//  return 0;
//
//}
int maxpool(int8_t* input, int8_t *output, dim_t N, dim_t H, dim_t W, dim_t C, unsigned_t KH, unsigned_t KW, unsigned_t pad_size,
            unsigned_t stride_size){
  // Same result as before (max over the in-bounds window, 0 if the window is empty), with the
  // channel loop innermost (contiguous) and output rows split over the OpenMP threads.
  const ssize_t OH = (H+2*pad_size-KH)/stride_size + 1;
  const ssize_t OW = (W+2*pad_size-KW)/stride_size + 1;
  const ssize_t rows = ssize_t(N) * OH;
#pragma omp parallel for schedule(static)
  for (ssize_t r = 0; r < rows; r++) {
    const ssize_t n = r / OH, ax = r % OH;
    const ssize_t x = ax * stride_size - ssize_t(pad_size);
    for (ssize_t ay = 0; ay < OW; ay++) {
      const ssize_t y = ay * stride_size - ssize_t(pad_size);
      int8_t *o = output + n*OH*OW*C + ax*OW*C + ay*C;
      bool first = true;
      for (ssize_t fx = 0; fx < KH; fx++) {
        const ssize_t ox = x + fx;
        if (ox < 0 || ox >= ssize_t(H)) continue;
        for (ssize_t fy = 0; fy < KW; fy++) {
          const ssize_t oy = y + fy;
          // Ignore index access below zero (this is due to padding).
          if (oy < 0 || oy >= ssize_t(W)) continue;
          const int8_t *v = input + n*H*W*C + ox*W*C + oy*C;
          if (first) {
            for (dim_t z = 0; z < C; z++) o[z] = v[z];
            first = false;
          } else {
            for (dim_t z = 0; z < C; z++) o[z] = v[z] > o[z] ? v[z] : o[z];
          }
        }
      }
      if (first)
        for (dim_t z = 0; z < C; z++) o[z] = 0;
    }
  }
  return 0;
}

//#### JO changed: relu single-threaded -> OpenMP (bit-identical result); original kept below
//int relu(int8_t* input, int8_t *output, unsigned_t size, float inputScale, float outputScale){
//  for(int i = 0; i < size; i ++){
//    if(input[i]>0)
//      output[i]=input[i];
//    else
//      output[i]=0;
//    output[i] *= (int)(inputScale/outputScale);
//  }
//
//  return 0;
//}
int relu(int8_t* input, int8_t *output, unsigned_t size, float inputScale, float outputScale){
  const int mul = (int)(inputScale/outputScale);
  // chunks over the threads, plain simd loop inside (clang-8 does not vectorize int8 "parallel for simd")
  const ssize_t n = size, CH = 4096;
//#### JO changed: small sizes run inline without any OpenMP call (elemmax calls quantize per element:
//#### 1.6M parallel regions -> 15-100 s; an if() clause still enters the OpenMP runtime)
//#pragma omp parallel for schedule(static)
  auto chunk = [&](ssize_t b) {
    const ssize_t e = std::min(n, b + CH);
#pragma omp simd
    for (ssize_t i = b; i < e; i++) {
      int8_t v = input[i] > 0 ? input[i] : 0;
      v *= mul;
      output[i] = v;
    }
  };
  if (n <= CH) { chunk(0); return 0; }
#pragma omp parallel for schedule(static)
  for (ssize_t b = 0; b < n; b += CH) chunk(b);
  return 0;
}

int avgpool(int8_t* input, float inputScale, int32_t inputOffset, int8_t *output, float outputScale, int32_t outputOffset,
            dim_t inputDim0, dim_t inputDim1, dim_t inputDim2, dim_t inputDim3,
            dim_t outputDim0, dim_t outputDim1, dim_t outputDim2, dim_t outputDim3,
            unsigned_t KH, unsigned_t KW, unsigned_t pad_size, unsigned_t stride_size) {

    dim_t N = inputDim0;
    dim_t H = inputDim1;
    dim_t W = inputDim2;
    dim_t C = inputDim3;


    dim_t OH = (H+2*pad_size-KH)/stride_size + 1;
    dim_t OW = (W+2*pad_size-KW)/stride_size + 1;
    float filterArea = KH* KW;
    // For each input in the batch:
    for (dim_t n = 0; n < N; n++) {

        // For each layer in the output tensor:
        for (dim_t z = 0; z < C; z++) {
            // For each convolution 'jump' in the input tensor:
            dim_t x = -dim_t(pad_size);
            for (dim_t ax = 0; ax < OH; x += stride_size, ax++) {
                dim_t y = -dim_t(pad_size);
                for (dim_t ay = 0; ay < OW; y += stride_size, ay++) {

                    int32_t sum = 0;


                    for (dim_t fx = 0; fx < KH; fx++) {
                        for (dim_t fy = 0; fy < KW; fy++) {
                            dim_t ox = x + fx;
                            dim_t oy = y + fy;

                            // Ignore index access below zero (this is due to padding).
                            if (ox < 0 || oy < 0 || ox >= ssize_t(H) ||
                                oy >= ssize_t(W)) {
                                continue;
                            }

                            sum += *(input + n*H*W*C + ox*W*C + oy*C + z);
                        }
                    }

                    int32_t out32 = std::round(float(sum) * (inputScale/ outputScale / filterArea) +
                               outputOffset);
                    int8_t out8;
                    if(out32>127) out8=127;
                    else if(out32<-128) out8=-128;
                    else out8=(int8_t)out32;
                    *(output + n*OH*OW*C + ax*OW*C + ay*C + z) = out8;
                } // W
            }   // H
        }     // C
    }       // N




  return 0;
}

//#### JO changed: quantize single-threaded -> OpenMP (bit-identical result); original kept below
//int quantize(int8_t *input, int8_t *output, dim_t size, float scale, int32_t offset){
//  float *inputF = (float *)input;
//  for(int i = 0; i < size; i++){
//      float in = *(inputF + i);
//      int32_t out32 = std::round(in/scale + offset);
//      int8_t out8;
//      if(out32>127) out8=127;
//      else if(out32<-128) out8=-128;
//      else out8=(int8_t)out32;
//      *(output+i) = out8;
//  }
//  return 0;
//}
int quantize(int8_t *input, int8_t *output, dim_t size, float scale, int32_t offset){
  float *inputF = (float *)input;
  const ssize_t n = size, CH = 4096;
//#### JO changed: small sizes run inline without any OpenMP call (elemmax calls quantize per element:
//#### 1.6M parallel regions -> 15-100 s; an if() clause still enters the OpenMP runtime)
//#pragma omp parallel for schedule(static)
  auto chunk = [&](ssize_t b) {
    const ssize_t e = std::min(n, b + CH);
#pragma omp simd
    for (ssize_t i = b; i < e; i++) {
      float in = *(inputF + i);
      int32_t out32 = std::round(in/scale + offset);
      out32 = out32 > 127 ? 127 : (out32 < -128 ? -128 : out32);
      *(output+i) = (int8_t)out32;
    }
  };
  if (n <= CH) { chunk(0); return 0; }
#pragma omp parallel for schedule(static)
  for (ssize_t b = 0; b < n; b += CH) chunk(b);
  return 0;
}

int dequantize(int8_t *input, int8_t *output, dim_t size, float scale, int32_t offset){
  float* outputF = (float*)output;
    for(int i = 0; i < size; i++){
        float in = (float)*(input + i);
        float out = in*scale + offset;
        *(outputF+i) = out;
    }



  return 0;


}

int8_t* at(int8_t *input, dim_t dim0, dim_t dim1, dim_t dim2, dim_t dim3,
              dim_t loc0, dim_t loc1, dim_t loc2, dim_t loc3){
  dim_t index = loc0*dim1*dim2*dim3 + loc1*dim2*dim3 + loc2*dim3 + loc3;
  return (input + index);
}


int transpose(int8_t *input, int8_t *output, dim_t inDim0, dim_t inDim1, dim_t inDim2, dim_t inDim3,
              dim_t outDim0, dim_t outDim1, dim_t outDim2, dim_t outDim3,
              unsigned_t shf0, unsigned_t shf1, unsigned_t shf2, unsigned_t shf3) {
  dim_t i0, j0, k0, l0, *i1, *j1, *k1, *l1;

  switch(shf0) {
    case 0:
      i1 = &i0;
      break;
    case 1:
      i1 = &j0;
      break;
    case 2:
      i1 = &k0;
      break;
    case 3:
      i1 = &l0;
      break;
  }
  switch(shf1) {
    case 0:
      j1 = &i0;
      break;
    case 1:
      j1 = &j0;
      break;
    case 2:
      j1 = &k0;
      break;
    case 3:
      j1 = &l0;
      break;
  }
  switch(shf2) {
    case 0:
      k1 = &i0;
      break;
    case 1:
      k1 = &j0;
      break;
    case 2:
      k1 = &k0;
      break;
    case 3:
      k1 = &l0;
      break;
  }
  switch(shf3) {
    case 0:
      l1 = &i0;
      break;
    case 1:
      l1 = &j0;
      break;
    case 2:
      l1 = &k0;
      break;
    case 3:
      l1 = &l0;
      break;
  }

  for(i0 = 0; i0 < inDim0; i0++)
    for(j0 = 0; j0 < inDim1; j0++)
      for(k0 = 0; k0 < inDim2; k0++)
        for(l0 = 0; l0 < inDim3; l0++) {
          int8_t* dst = at(output, outDim0, outDim1, outDim2, outDim3, *i1, *j1, *k1, *l1);
          int8_t* src = at(input, inDim0, inDim1, inDim2, inDim3, i0, j0, k0, l0);
          *dst = *src;
        }
  return 0;
}
int8_t* at6dim(int8_t *input, dim_t dim0, dim_t dim1, dim_t dim2, dim_t dim3, dim_t dim4, dim_t dim5,
               dim_t loc0, dim_t loc1, dim_t loc2, dim_t loc3 , dim_t loc4 , dim_t loc5){
    dim_t index = loc0*dim1*dim2*dim3*dim4*dim5 +
            loc1*dim2*dim3*dim4*dim5 +
            loc2*dim3*dim4*dim5 +
            loc3*dim4*dim5 +
            loc4*dim5 +
            loc5;
    return (input + index);
}

int transpose_6dim(int8_t *input, int8_t *output, dim_t inDim0, dim_t inDim1, dim_t inDim2, dim_t inDim3, dim_t inDim4, dim_t inDim5,
                   dim_t outDim0, dim_t outDim1, dim_t outDim2, dim_t outDim3, dim_t outDim4, dim_t outDim5,
                   unsigned_t shf0, unsigned_t shf1, unsigned_t shf2, unsigned_t shf3, unsigned_t shf4, unsigned_t shf5) {
    dim_t i0, j0, k0, l0, m0, n0;
    dim_t *i1, *j1, *k1, *l1, *m1, *n1;

    switch (shf0) {
        case 0:
            i1 = &i0;
            break;
        case 1:
            i1 = &j0;
            break;
        case 2:
            i1 = &k0;
            break;
        case 3:
            i1 = &l0;
            break;
        case 4:
            i1 = &m0;
            break;
        case 5:
            i1 = &n0;
            break;
    }
    switch (shf1) {
        case 0:
            j1 = &i0;
            break;
        case 1:
            j1 = &j0;
            break;
        case 2:
            j1 = &k0;
            break;
        case 3:
            j1 = &l0;
            break;
        case 4:
            j1 = &m0;
            break;
        case 5:
            j1 = &n0;
            break;
    }
    switch (shf2) {
        case 0:
            k1 = &i0;
            break;
        case 1:
            k1 = &j0;
            break;
        case 2:
            k1 = &k0;
            break;
        case 3:
            k1 = &l0;
            break;
        case 4:
            k1 = &m0;
            break;
        case 5:
            k1 = &n0;
            break;
    }
    switch (shf3) {
        case 0:
            l1 = &i0;
            break;
        case 1:
            l1 = &j0;
            break;
        case 2:
            l1 = &k0;
            break;
        case 3:
            l1 = &l0;
            break;
        case 4:
            l1 = &m0;
            break;
        case 5:
            l1 = &n0;
            break;
    }
    switch (shf4) {
        case 0:
            m1 = &i0;
            break;
        case 1:
            m1 = &j0;
            break;
        case 2:
            m1 = &k0;
            break;
        case 3:
            m1 = &l0;
            break;
        case 4:
            m1 = &m0;
            break;
        case 5:
            m1 = &n0;
            break;
    }
    switch (shf5) {
        case 0:
            n1 = &i0;
            break;
        case 1:
            n1 = &j0;
            break;
        case 2:
            n1 = &k0;
            break;
        case 3:
            n1 = &l0;
            break;
        case 4:
            n1 = &m0;
            break;
        case 5:
            n1 = &n0;
            break;
    }

    for(i0 = 0; i0 < inDim0; i0++)
        for(j0 = 0; j0 < inDim1; j0++)
            for(k0 = 0; k0 < inDim2; k0++)
                for(l0 = 0; l0 < inDim3; l0++)
                    for(m0 = 0; m0 < inDim4; m0++)
                        for(n0 = 0; n0 < inDim5; n0++){
                            int8_t* dst = at6dim(output, outDim0, outDim1, outDim2, outDim3,
                                             outDim4, outDim5, *i1, *j1, *k1, *l1, *m1, *n1);
                            int8_t* src = at6dim(input, inDim0, inDim1, inDim2, inDim3,
                                                 inDim4, inDim5, i0, j0, k0, l0, m0, n0);
                            *dst = *src;
                        }
    return 0;
}

//#### JO changed: elemadd single-threaded -> OpenMP (bit-identical result); original kept below
//int elemadd(int8_t *input0, float input0Scale, int32_t input0Offset, int8_t *input1, float input1Scale, int32_t input1Offset,
//            int8_t *output, float destScale, int32_t destOffset, unsigned_t size) {
//  for (dim_t i = 0, e = size; i < e; i++) {
//    int32_t L = input0[i];
//    int32_t R = input1[i];
//
//    // We increase the size of the integer up to 16 bits to prevent overflow.
//    const float largeScale = float(1) / (1 << 15);
//    // Scale both sides from 8-bit to 16-bits.
//    int32_t L32 = std::round(float(L - input0Offset) * (input0Scale / largeScale));
//    int32_t R32 = std::round(float(R - input1Offset) * (input1Scale / largeScale));
//    int32_t sum32 = L32 + R32;
//    sum32 = std::round(float(sum32) * (largeScale / destScale) + destOffset);
//      int8_t out8;
//      if(sum32>127) out8=127;
//      else if(sum32<-128) out8=-128;
//      else out8=(int8_t)sum32;
//    output[i] = out8;
//  }
//  return 0;
//}
int elemadd(int8_t *input0, float input0Scale, int32_t input0Offset, int8_t *input1, float input1Scale, int32_t input1Offset,
            int8_t *output, float destScale, int32_t destOffset, unsigned_t size) {
  // We increase the size of the integer up to 16 bits to prevent overflow.
  const float largeScale = float(1) / (1 << 15);
  const float k0 = input0Scale / largeScale, k1 = input1Scale / largeScale, kd = largeScale / destScale;
  // chunks over the threads; simd: element-wise, so safe when output aliases an input (the bundles call it in place)
  const ssize_t n = size, CH = 4096;
//#### JO changed: small sizes run inline without any OpenMP call (elemmax calls quantize per element:
//#### 1.6M parallel regions -> 15-100 s; an if() clause still enters the OpenMP runtime)
//#pragma omp parallel for schedule(static)
  auto chunk = [&](ssize_t b) {
    const ssize_t e = std::min(n, b + CH);
#pragma omp simd
    for (ssize_t i = b; i < e; i++) {
      int32_t L = input0[i];
      int32_t R = input1[i];
      // Scale both sides from 8-bit to 16-bits.
      int32_t L32 = std::round(float(L - input0Offset) * k0);
      int32_t R32 = std::round(float(R - input1Offset) * k1);
      int32_t sum32 = L32 + R32;
      sum32 = std::round(float(sum32) * kd + destOffset);
      sum32 = sum32 > 127 ? 127 : (sum32 < -128 ? -128 : sum32);
      output[i] = (int8_t)sum32;
    }
  };
  if (n <= CH) { chunk(0); return 0; }
#pragma omp parallel for schedule(static)
  for (ssize_t b = 0; b < n; b += CH) chunk(b);
  return 0;
}

int elemadd_scale(int8_t *input0, int8_t *input1, int8_t *output, float destScale, int32_t size) {

  for (dim_t i = 0, e = size; i < e; i++) {
    int32_t L = input0[i];
    int32_t R = input1[i];
    // We increase the size of the integer up to 16 bits to prevent overflow.
    int32_t sum32 = (L + R);
    sum32 = std::round(float(sum32) * destScale);
    int8_t out8;
    if(sum32>127) out8=127;
    else if(sum32<-128) out8=-128;
    else out8=(int8_t)sum32;
    output[i] = out8;
  }
  return 0;
}



int elemsign(int8_t *input, int8_t *output, unsigned_t size) {

  for (size_t i = 0, e = size; i < e; i++) {
    output[i] = int(input[i] >= 0) - int(input[i] < 0);
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
    int32_t sum32 = std::round(l - r + destOffset);
      int8_t out8;
      if(sum32>127) out8=127;
      else if(sum32<-128) out8=-128;
      else out8=(int8_t)sum32;
      output[i] = out8;
  }
  return 0;
}

//#### JO changed: elemsub_f32 single-threaded -> OpenMP (bit-identical result); original kept below
//int elemsub_f32(int8_t *input0, float input0Scale, int32_t input0Offset, int8_t *input1, float input1Scale, int32_t input1Offset,
//            int8_t *output, float destScale, int32_t destOffset, unsigned_t size) {
//  for (dim_t i = 0, e = size; i < e; i++) {
//    float L = ((float*)input0)[i];
//    float R = ((float*)input1)[i];
//    ((float*)output)[i] = L - R;
//  }
//  return 0;
//}
int elemsub_f32(int8_t *input0, float input0Scale, int32_t input0Offset, int8_t *input1, float input1Scale, int32_t input1Offset,
            int8_t *output, float destScale, int32_t destOffset, unsigned_t size) {
//#### JO changed: small sizes run inline without any OpenMP call (elemmax calls quantize per element:
//#### 1.6M parallel regions -> 15-100 s; an if() clause still enters the OpenMP runtime)
//#pragma omp parallel for simd schedule(static)
  if (size <= 4096) {
    for (ssize_t i = 0; i < ssize_t(size); i++) {
      float L = ((float*)input0)[i];
      float R = ((float*)input1)[i];
      ((float*)output)[i] = L - R;
    }
    return 0;
  }
#pragma omp parallel for simd schedule(static)
  for (ssize_t i = 0; i < ssize_t(size); i++) {
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
    int32_t sum32 = std::round(l / r + destOffset);
      int8_t out8;
      if(sum32>127) out8=127;
      else if(sum32<-128) out8=-128;
      else out8=(int8_t)sum32;
      output[i] = out8;
  }
  return 0;
}

//#### JO changed: elemdiv_f32 single-threaded -> OpenMP (bit-identical result); original kept below
//int elemdiv_f32(int8_t *input0, float input0Scale, int32_t input0Offset, int8_t *input1, float input1Scale, int32_t input1Offset,
//            int8_t *output, float destScale, int32_t destOffset, unsigned_t size) {
//  for (dim_t i = 0, e = size; i < e; i++) {
//    float L = ((float*)input0)[i];
//    float R = ((float*)input1)[i];
//    ((float*)output)[i] = L / R;
//  }
//  return 0;
//}
int elemdiv_f32(int8_t *input0, float input0Scale, int32_t input0Offset, int8_t *input1, float input1Scale, int32_t input1Offset,
            int8_t *output, float destScale, int32_t destOffset, unsigned_t size) {
//#### JO changed: small sizes run inline without any OpenMP call (elemmax calls quantize per element:
//#### 1.6M parallel regions -> 15-100 s; an if() clause still enters the OpenMP runtime)
//#pragma omp parallel for simd schedule(static)
  if (size <= 4096) {
    for (ssize_t i = 0; i < ssize_t(size); i++) {
      float L = ((float*)input0)[i];
      float R = ((float*)input1)[i];
      ((float*)output)[i] = L / R;
    }
    return 0;
  }
#pragma omp parallel for simd schedule(static)
  for (ssize_t i = 0; i < ssize_t(size); i++) {
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

  for (size_t i = 0, e = size; i < e; i++) {
    float lf, rf;
    int8_t l, r;

    dequantize(&input0[i],(int8_t *)&lf, 1, input0Scale, input0Offset);
    dequantize(&input1[i],(int8_t *)&rf, 1, input1Scale, input1Offset);

    quantize((int8_t*)&lf, &l, 1, destScale, destOffset);
    quantize((int8_t*)&rf, &r, 1, destScale, destOffset);

      output[i] = std::max(l, r);
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


//#### JO changed: transpose_nhwc2vtaio single-threaded -> OpenMP (bit-identical result); original kept below
//void transpose_nhwc2vtaio(int8_t* input, int8_t* output, int N, int H, int W, int C){
//  int8_t *IN_NHWC = input;
//
//  int8_t *IN_NCHW = (int8_t *)malloc(N*C*H*W);
//  for (int n = 0; n < N; n++) {
//    for (int h = 0; h < H; h++) {
//      for (int w = 0; w < W; w++) {
//        for (int c = 0; c < C; c++) {
//          *(IN_NCHW + n*C*H*W + c*H*W + h*W + w)=*(IN_NHWC + n*H*W*C + h*W*C + w*C + c);
//        }
//      }
//    }
//  }
//
//  // reshape input NCHW -> N{C//16}16HW
//  int8_t *in_NC_16HW = IN_NCHW;
//
//
//  for (int n = 0; n < N; n++) {
//    for (int c_ = 0; c_ < C / 16; c_++) {
//      for (int h = 0; h < H; h++) {
//        for (int w = 0; w < W; w++) {
//          for (int c = 0; c < 16; c++) {
//            *(output + n*(C/16)*H*W*16 + c_*H*W*16 + h*W*16 + w*16 + c) =
//                *(in_NC_16HW + n*(C/16)*16*H*W + c_*16*H*W + c*H*W + h*W + w);
//          }
//        }
//      }
//    }
//  }
//
//  free(IN_NCHW);
//}
void transpose_nhwc2vtaio(int8_t* input, int8_t* output, int N, int H, int W, int C){
  // NHWC -> N{C/16}HW16 in one pass (was NHWC -> NCHW in a malloc'ed buffer, then -> N{C/16}HW16).
  const int CB = C / 16;
  const int rows = N * CB * H;
#pragma omp parallel for schedule(static)
  for (int r = 0; r < rows; r++) {
    const int n = r / (CB * H), c_ = (r / H) % CB, h = r % H;
    const int8_t *in = input + n*H*W*C + h*W*C + c_*16;
    int8_t *out = output + n*CB*H*W*16 + c_*H*W*16 + h*W*16;
    for (int w = 0; w < W; w++)
      for (int c = 0; c < 16; c++)
        out[w*16 + c] = in[w*C + c];
  }
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


//#### JO changed: transpose_vtaio2nhwc single-threaded -> OpenMP (bit-identical result); original kept below
//void transpose_vtaio2nhwc(int8_t* input, int8_t* output, int N, int H, int W, int C){
//  for (int b = 0; b < N; b++) {
//    for (int f = 0; f < C; f++) {
//      for (int i = 0; i < H; i++) {    //h
//        for (int j = 0; j < W; j++) {    //w
//
//          int out_ch = f / 16;
//          int oo = f % 16;
//          auto value =*(input + (out_ch * H * W * 16 + i * W * 16 + j * 16 + oo));
//          *(output + b * H * W * C + i * W * C + j * C + f) = value;
//        }
//      }
//    }
//  }
//}
void transpose_vtaio2nhwc(int8_t* input, int8_t* output, int N, int H, int W, int C){
  // Same mapping as before (including the input index not depending on the batch b),
  // walked with the channel innermost so the NHWC output is written contiguously.
  const int rows = N * H;
#pragma omp parallel for schedule(static)
  for (int r = 0; r < rows; r++) {
    const int b = r / H, i = r % H;
    for (int j = 0; j < W; j++) {
      int8_t *out = output + b * H * W * C + i * W * C + j * C;
      for (int f = 0; f < C; f++)
        out[f] = *(input + ((f / 16) * H * W * 16 + i * W * 16 + j * 16 + f % 16));
    }
  }
}

void transpose_nhwc2vtaio_pack(int8_t* input, int8_t* output, int N, int H, int W, int C){
  int8_t *IN_NHWC = input;
  //int8_t *NHWC_PACK = (int8_t *)calloc(N*C*H*W, sizeof(int8_t));
  int8_t *NHWC_PACK = (int8_t *)malloc(N*C*H*W);
  memset(NHWC_PACK, 0, N*H*W*C);

  for (int n = 0; n < N; ++n) {
    for (int h = 0; h < H; ++h) {
      for (int w = 0; w < W; ++w) {
        for (int c = 0; c < C; ++c) {
          for(int bw = 0; bw < 8; ++bw){
            *(NHWC_PACK + n*H*W*C + h*W*C + w*C + c) +=
                (int(*(IN_NHWC + n*H*W*(C*8) + h*W*(C*8) + w*(C*8) + (c * 8 + bw)) > 0) << bw);
          }
        }
      }
    }
  }

  int8_t *in_PACK_NHWC = NHWC_PACK;
  int8_t *IN_NCHW = (int8_t *)malloc(N*C*H*W);

  for (int n = 0; n < N; n++) {
    for (int h = 0; h < H; h++) {
      for (int w = 0; w < W; w++) {
        for (int c = 0; c < C; c++) {
          *(IN_NCHW + n*C*H*W + c*H*W + h*W + w) =
                  *(in_PACK_NHWC + n*H*W*C + h*W*C + w*C + c);
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

  free(NHWC_PACK);
  free(IN_NCHW);
}

void transpose_nhwc2vtaio_pack_zerofill(int8_t* input, int8_t* output, int N, int H, int W, int C){

  int8_t *IN_NHWC = input;
  int8_t *NHWC_PACK = (int8_t *)malloc(N*C*H*W);
  memset(NHWC_PACK, 0, N*H*W*C);

  for (int n = 0; n < N; ++n) {
    for (int h = 0; h < H; ++h) {
      for (int w = 0; w < W; ++w) {
        for (int c = 0; c < C; ++c) {
          for(int bw = 0; bw < 8; ++bw){
            *(NHWC_PACK + n*H*W*C + h*W*C + w*C + c) +=
                (int(*(IN_NHWC + n*H*W*(C*8) + h*W*(C*8) + w*(C*8) + (c * 8 + bw)) > 0) << bw);
          }
        }
      }
    }
  }

  int8_t *in_PACK_NHWC = NHWC_PACK;
  int8_t *IN_NCHW = (int8_t *)malloc(N*C*H*W);

  for (int n = 0; n < N; n++) {
    for (int h = 0; h < H; h++) {
      for (int w = 0; w < W; w++) {
        for (int c = 0; c < C; c++) {
          *(IN_NCHW + n*C*H*W + c*H*W + h*W + w) =
                  *(in_PACK_NHWC + n*H*W*C + h*W*C + w*C + c);
        }
      }
    }
  }


  int8_t *NEW_DATA = (int8_t *)malloc(N*16*H*W);
  memset(NEW_DATA, 0, N*16*H*W);
  for (int k = 0; k < N*C*H*W; k++) {
    *(NEW_DATA + k) = *(IN_NCHW + k);
  }


  // reshape input NCHW -> N{C//16}16HW
  int8_t *in_NC_16HW = NEW_DATA;

  for (int n = 0; n < N; n++) {
    for (int c_ = 0; c_ < 16 / 16; c_++) {
      for (int h = 0; h < H; h++) {
        for (int w = 0; w < W; w++) {
          for (int c = 0; c < 16; c++) {
            *(output + n*(16/16)*H*W*16 + c_*H*W*16 + h*W*16 + w*16 + c) =
                *(in_NC_16HW + n*(16/16)*16*H*W + c_*16*H*W + c*H*W + h*W + w);
          }
        }
      }
    }
  }

  free(NHWC_PACK);
  free(IN_NCHW);
  free(NEW_DATA);
}

void transpose_nhwc2vtaio_zerofill(int8_t* input, int8_t* output, int N, int H, int W, int C){
  int8_t *IN_NHWC = input;

  int8_t *IN_NCHW = (int8_t *)malloc(N*C*H*W);
  memset(IN_NCHW, 0, N*C*H*W);

  for (int n = 0; n < N; n++) {
    for (int h = 0; h < H; h++) {
      for (int w = 0; w < W; w++) {
        for (int c = 0; c < 3; c++) {
          *(IN_NCHW + n*3*H*W + c*H*W + h*W + w)=*(IN_NHWC + n*H*W*3 + h*W*3 + w*3 + c);
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
