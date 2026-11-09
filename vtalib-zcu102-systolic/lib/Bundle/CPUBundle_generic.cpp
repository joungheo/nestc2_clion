#include <iostream>
#include <ctime>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <fstream>
#include <vector>
#ifdef _OPENMP
#include <omp.h>
#endif
#if defined(__aarch64__)
#include <arm_neon.h>
#endif
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


//#### JO added (2026-11-06): upper clip of a fused relu (ReLU6 = Clip(0, 6) -> round(6 / output scale)), set by
//#### VTASetActivationClip around the call (VTABundle.cpp); 127 = plain relu
extern thread_local int gEvtaClipHi;

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
                        //#### JO changed (2026-11-06): the relu's upper clip (gEvtaClipHi)
                        //if(out32>127) out8=127;
                        const int hi = doRelu ? gEvtaClipHi : 127;
                        if(out32>hi) out8=hi;
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
                        //#### JO changed (2026-11-06): the relu's upper clip (gEvtaClipHi)
                        //if(out32>127) out8=127;
                        const int hi = doRelu ? gEvtaClipHi : 127;
                        if(out32>hi) out8=hi;
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


//#### JO added (2026-11-07): the channelwise quantized conv, bit-exact with Glow's Interpreter (VTABundle.h). Depthwise:
//#### per output pixel 16 channels per NEON step (int8 products widened to int32, the float multiplier, round half away
//#### from zero as std::round, clip, narrow); output rows split over the OpenMP threads.
//#### JO changed (2026-11-08): the depthwise input / output NHWC or in the EVTA layout (inPacked / outPacked, VTASave
//#### findPackedActivations: no transposes around the depthwise convs between EVTA convs)
static inline int8_t cwOut(int32_t sum, float m, int lo, int hi) {
  int32_t v = (int32_t)std::round(float(sum) * m);
  return (int8_t)(v < lo ? lo : v > hi ? hi : v);
}

int cw_convolution(const int8_t *input, const int8_t *kernel, const int32_t *bias, const float *mult, int8_t *output,
                   int N, int H, int W, int C, int KN, int KH, int KW, int padT, int padL, int strideH, int strideW,
                   int dilH, int dilW, int group, int outH, int outW, int lo, int hi, int inPacked, int outPacked) {
  const bool dw = group == C && KN == C;
  const int CG = C / group, KG = KN / group;
  assert((!inPacked && !outPacked) || (dw && C % 16 == 0));
  // element (y, x, c) at y * rs + x * ps + (c / 16) * bs + c % 16: NHWC rs = W * C, ps = C, bs = 16; EVTA layout
  // rs = W * 16, ps = 16, bs = H * W * 16
  const size_t irs = inPacked ? (size_t)W * 16 : (size_t)W * C, ips = inPacked ? 16 : C,
               ibs = inPacked ? (size_t)H * W * 16 : 16;
  const size_t ors = outPacked ? (size_t)outW * 16 : (size_t)outW * KN, ops = outPacked ? 16 : KN,
               obs = outPacked ? (size_t)outH * outW * 16 : 16;
  const int CB16 = dw ? C / 16 : 0;
#if defined(__aarch64__)
  const int doneC = CB16 * 16;  // channels of the NEON loop
#else
  const int doneC = 0;
#endif
  for (int n = 0; n < N; n++) {
    const int8_t *in = input + (size_t)n * H * W * C;
    int8_t *out = output + (size_t)n * outH * outW * KN;
#if defined(__aarch64__)
    // depthwise, 16 channels per NEON step: 16-channel block outermost (a plane of the EVTA layout, read in order)
    if (dw && CB16) {
      const int32x4_t vlo = vdupq_n_s32(lo), vhi = vdupq_n_s32(hi);
#pragma omp parallel for collapse(2) schedule(static)
      for (int cb = 0; cb < CB16; cb++)
        for (int oy = 0; oy < outH; oy++) {
          const int c = cb * 16;
          const int8_t *ib = in + (size_t)cb * ibs;
          int8_t *ob = out + (size_t)cb * obs + oy * ors;
          // 3x3 with all rows inside: the 9 weights in registers and two output pixels at a time over the columns
          // whose taps are inside (8 independent accumulators); same sums and rounding as the general loop below,
          // which does the other columns
          int fastLo = 0, fastHi = 0;  // output columns [fastLo, fastHi) done here
          const int yi = oy * strideH - padT;
          if (KH == 3 && KW == 3 && yi >= 0 && yi + 2 * dilH < H) {
            fastLo = (padL + strideW - 1) / strideW;
            fastHi = outW;
            while (fastHi > fastLo && (fastHi - 1) * strideW - padL + 2 * dilW >= W)
              fastHi--;
            fastHi = fastLo + (fastHi - fastLo) / 2 * 2;
            int8x16_t wk[9];
            for (int t = 0; t < 9; t++)
              wk[t] = vld1q_s8(kernel + (size_t)t * C + c);
            const int32x4_t b0 = vld1q_s32(bias + c), b1 = vld1q_s32(bias + c + 4);
            const int32x4_t b2 = vld1q_s32(bias + c + 8), b3 = vld1q_s32(bias + c + 12);
            const float32x4_t m0 = vld1q_f32(mult + c), m1 = vld1q_f32(mult + c + 4);
            const float32x4_t m2 = vld1q_f32(mult + c + 8), m3 = vld1q_f32(mult + c + 12);
            auto store = [&](int32x4_t r0, int32x4_t r1, int32x4_t r2, int32x4_t r3, int8_t *dst) {
              r0 = vminq_s32(vmaxq_s32(vcvtaq_s32_f32(vmulq_f32(vcvtq_f32_s32(r0), m0)), vlo), vhi);
              r1 = vminq_s32(vmaxq_s32(vcvtaq_s32_f32(vmulq_f32(vcvtq_f32_s32(r1), m1)), vlo), vhi);
              r2 = vminq_s32(vmaxq_s32(vcvtaq_s32_f32(vmulq_f32(vcvtq_f32_s32(r2), m2)), vlo), vhi);
              r3 = vminq_s32(vmaxq_s32(vcvtaq_s32_f32(vmulq_f32(vcvtq_f32_s32(r3), m3)), vlo), vhi);
              const int16x8_t l = vcombine_s16(vqmovn_s32(r0), vqmovn_s32(r1));
              const int16x8_t h = vcombine_s16(vqmovn_s32(r2), vqmovn_s32(r3));
              vst1q_s8(dst, vcombine_s8(vqmovn_s16(l), vqmovn_s16(h)));
            };
            for (int px = fastLo; px < fastHi; px += 2) {
              const int8_t *p0 = ib + yi * irs + (size_t)(px * strideW - padL) * ips;
              const int8_t *p1 = p0 + (size_t)strideW * ips;
              int32x4_t a0 = b0, a1 = b1, a2 = b2, a3 = b3, e0 = b0, e1 = b1, e2 = b2, e3 = b3;
              for (int ky = 0; ky < 3; ky++)
                for (int kx = 0; kx < 3; kx++) {
                  const size_t off = (size_t)ky * dilH * irs + (size_t)kx * dilW * ips;
                  const int8x16_t w = wk[ky * 3 + kx];
                  const int8x16_t x = vld1q_s8(p0 + off), y = vld1q_s8(p1 + off);
                  const int16x8_t pl = vmull_s8(vget_low_s8(x), vget_low_s8(w)), ph = vmull_high_s8(x, w);
                  const int16x8_t ql = vmull_s8(vget_low_s8(y), vget_low_s8(w)), qh = vmull_high_s8(y, w);
                  a0 = vaddw_s16(a0, vget_low_s16(pl));
                  e0 = vaddw_s16(e0, vget_low_s16(ql));
                  a1 = vaddw_high_s16(a1, pl);
                  e1 = vaddw_high_s16(e1, ql);
                  a2 = vaddw_s16(a2, vget_low_s16(ph));
                  e2 = vaddw_s16(e2, vget_low_s16(qh));
                  a3 = vaddw_high_s16(a3, ph);
                  e3 = vaddw_high_s16(e3, qh);
                }
              store(a0, a1, a2, a3, ob + (size_t)px * ops);
              store(e0, e1, e2, e3, ob + (size_t)(px + 1) * ops);
            }
          }
          for (int ox = 0; ox < outW; ox++) {
            if (ox == fastLo && fastHi > fastLo) {
              ox = fastHi - 1;
              continue;
            }
            const int y0 = oy * strideH - padT, x0 = ox * strideW - padL;
            int32x4_t a0 = vld1q_s32(bias + c), a1 = vld1q_s32(bias + c + 4);
            int32x4_t a2 = vld1q_s32(bias + c + 8), a3 = vld1q_s32(bias + c + 12);
            for (int ky = 0; ky < KH; ky++) {
              const int iy = y0 + ky * dilH;
              if (iy < 0 || iy >= H)
                continue;
              for (int kx = 0; kx < KW; kx++) {
                const int ix = x0 + kx * dilW;
                if (ix < 0 || ix >= W)
                  continue;
                const int8x16_t x = vld1q_s8(ib + iy * irs + ix * ips);
                const int8x16_t w = vld1q_s8(kernel + ((size_t)ky * KW + kx) * C + c);
                const int16x8_t pl = vmull_s8(vget_low_s8(x), vget_low_s8(w));
                const int16x8_t ph = vmull_high_s8(x, w);
                a0 = vaddw_s16(a0, vget_low_s16(pl));
                a1 = vaddw_high_s16(a1, pl);
                a2 = vaddw_s16(a2, vget_low_s16(ph));
                a3 = vaddw_high_s16(a3, ph);
              }
            }
            int32x4_t r[4] = {a0, a1, a2, a3};
            for (int k = 0; k < 4; k++) {
              float32x4_t f = vmulq_f32(vcvtq_f32_s32(r[k]), vld1q_f32(mult + c + 4 * k));
              r[k] = vminq_s32(vmaxq_s32(vcvtaq_s32_f32(f), vlo), vhi);
            }
            const int16x8_t l = vcombine_s16(vqmovn_s32(r[0]), vqmovn_s32(r[1]));
            const int16x8_t h = vcombine_s16(vqmovn_s32(r[2]), vqmovn_s32(r[3]));
            vst1q_s8(ob + ox * ops, vcombine_s8(vqmovn_s16(l), vqmovn_s16(h)));
          }
        }
    }
#endif
    if (dw && doneC == C)
      continue;
#pragma omp parallel for schedule(static)
    for (int oy = 0; oy < outH; oy++) {
      for (int ox = 0; ox < outW; ox++) {
        const int y0 = oy * strideH - padT, x0 = ox * strideW - padL;
        if (dw) {
          int c = doneC;
          for (; c < C; c++) {
            int32_t sum = 0;
            for (int ky = 0; ky < KH; ky++) {
              const int iy = y0 + ky * dilH;
              if (iy < 0 || iy >= H)
                continue;
              for (int kx = 0; kx < KW; kx++) {
                const int ix = x0 + kx * dilW;
                if (ix < 0 || ix >= W)
                  continue;
                sum += (int32_t)in[(size_t)(c / 16) * ibs + iy * irs + ix * ips + c % 16] *
                       kernel[((size_t)ky * KW + kx) * C + c];
              }
            }
            out[(size_t)(c / 16) * obs + oy * ors + ox * ops + c % 16] = cwOut(sum + bias[c], mult[c], lo, hi);
          }
          continue;
        }
        int8_t *o = out + ((size_t)oy * outW + ox) * KN;
        for (int d = 0; d < KN; d++) {
          const int g = d / KG;
          int32_t sum = 0;
          for (int ky = 0; ky < KH; ky++) {
            const int iy = y0 + ky * dilH;
            if (iy < 0 || iy >= H)
              continue;
            for (int kx = 0; kx < KW; kx++) {
              const int ix = x0 + kx * dilW;
              if (ix < 0 || ix >= W)
                continue;
              const int8_t *ip = in + ((size_t)iy * W + ix) * C + g * CG;
              const int8_t *kp = kernel + (((size_t)d * KH + ky) * KW + kx) * CG;
              for (int k = 0; k < CG; k++)
                sum += (int32_t)ip[k] * kp[k];
            }
          }
          o[d] = cwOut(sum + bias[d], mult[d], lo, hi);
        }
      }
    }
  }
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
//#### JO changed (2026-11-01): cpuFullyConnected reads the kernel rows in order (the kernel is not in the cache after the
//#### convs: Resnet18 dense 512 x 1000 = 512 KB, the 64-byte pieces 1000 bytes apart did not prefetch). Each thread takes
//#### a contiguous range of outputs; per FC_KB kernel rows and 64 outputs the sums stay in 16 NEON registers
//#### (int8 x int8 -> int16 -> int32, exact when both offsets are 0, else scalar), the next FC_KB rows are prefetched.
//#### Bit-identical (integer sums). Board bench, cold cache: OMP 1 1.92 -> 0.69 ms, OMP 4 0.60 -> 0.23 ms. Previous version:
//int cpuFullyConnected(int8_t* input, float inputScale, int32_t inputOffset, int8_t *kernel, float kernelScale, int32_t kernelOffset,
//                         int8_t *bias, float biasScale, int32_t biasOffset, int8_t *output, float outputScale, int32_t outputOffset,
//                      dim_t inputDim0, dim_t inputDim1, dim_t kernelDim0, dim_t kernelDim1, dim_t outputDim0, dim_t outputDim1, bool doBias) {
//    // Integer accumulation (order-independent, so results are identical); kernel rows are read
//    // contiguously (j innermost) and output columns are split over the OpenMP threads.
//    float matMulScale = inputScale * kernelScale;
//    const ssize_t J = outputDim1;
//    for (dim_t i = 0; i < inputDim0; i++) {
//        const int8_t *a = input + i * inputDim1;
//#pragma omp parallel
//        {
//            std::vector<int32_t> sum;
//#pragma omp for schedule(static)
//            for (ssize_t j0 = 0; j0 < J; j0 += 64) {
//                const ssize_t nj = std::min<ssize_t>(64, J - j0);
//                sum.assign(nj, 0);
//                for (dim_t k = 0; k < inputDim1; k++) {
//                    const int32_t A = (int32_t)a[k] - inputOffset;
//                    const int8_t *w = kernel + k * kernelDim1 + j0;
//                    for (ssize_t j = 0; j < nj; j++)
//                        sum[j] += ((int32_t)w[j] - kernelOffset) * A;
//                }
//                for (ssize_t j = 0; j < nj; j++) {
//                    // Scale the bias to match the scale of the matrix multiplication.
//                    int32_t B = std::round(float(*((int32_t *)bias + j0 + j) - biasOffset) *
//                                           (biasScale / matMulScale));
//                    int32_t s = sum[j] + B;
//                    // Scale the result back to the expected destination scale.
//                    int32_t out32 = std::round(float(s) * (matMulScale / outputScale) + outputOffset);
//                    int8_t out8;
//                    if(out32>127) out8=127;
//                    else if(out32<-128) out8=-128;
//                    else out8=(int8_t)out32;
//                    *(output + i*outputDim1 + j0 + j) = out8;
//                }
//            }
//        }
//    }
//    return 0;
//}
static const ssize_t FC_KB = 8;
int cpuFullyConnected(int8_t* input, float inputScale, int32_t inputOffset, int8_t *kernel, float kernelScale, int32_t kernelOffset,
                         int8_t *bias, float biasScale, int32_t biasOffset, int8_t *output, float outputScale, int32_t outputOffset,
                      dim_t inputDim0, dim_t inputDim1, dim_t kernelDim0, dim_t kernelDim1, dim_t outputDim0, dim_t outputDim1, bool doBias) {
    float matMulScale = inputScale * kernelScale;
    const ssize_t J = outputDim1, K = inputDim1;
    for (dim_t i = 0; i < inputDim0; i++) {
        const int8_t *a = input + i * inputDim1;
#pragma omp parallel
        {
#ifdef _OPENMP
            const int nt = omp_get_num_threads(), t = omp_get_thread_num();
#else
            const int nt = 1, t = 0;
#endif
            const ssize_t per = ((J + nt - 1) / nt + 15) / 16 * 16;
            const ssize_t jb = std::min<ssize_t>(J, t * per), n = std::min<ssize_t>(J, jb + per) - jb;
            std::vector<int32_t> sum(n, 0);
            int32_t *s = sum.data();
#if defined(__aarch64__)
            const bool vec = inputOffset == 0 && kernelOffset == 0;
#else
            const bool vec = false;
#endif
            const ssize_t n64 = vec ? n / 64 * 64 : 0, n16 = vec ? n / 16 * 16 : 0;
            for (ssize_t k0 = 0; k0 < K; k0 += FC_KB) {
                const ssize_t k1 = std::min<ssize_t>(K, k0 + FC_KB);
                for (ssize_t k = k1; k < std::min<ssize_t>(K, k1 + FC_KB); k++)
                    for (ssize_t j = 0; j < n; j += 64)
                        __builtin_prefetch(kernel + k * kernelDim1 + jb + j);
#if defined(__aarch64__)
                for (ssize_t j = 0; j < n64; j += 64) {
                    int32x4_t c[16];
                    for (int q = 0; q < 16; q++) c[q] = vld1q_s32(s + j + 4 * q);
                    for (ssize_t k = k0; k < k1; k++) {
                        const int8_t *w = kernel + k * kernelDim1 + jb + j;
                        const int8x8_t av = vdup_n_s8(a[k]);
                        const int8x16_t avq = vcombine_s8(av, av);
                        for (int q = 0; q < 4; q++) {
                            const int8x16_t wv = vld1q_s8(w + 16 * q);
                            const int16x8_t lo = vmull_s8(vget_low_s8(wv), av), hi = vmull_high_s8(wv, avq);
                            c[4 * q] = vaddw_s16(c[4 * q], vget_low_s16(lo));
                            c[4 * q + 1] = vaddw_high_s16(c[4 * q + 1], lo);
                            c[4 * q + 2] = vaddw_s16(c[4 * q + 2], vget_low_s16(hi));
                            c[4 * q + 3] = vaddw_high_s16(c[4 * q + 3], hi);
                        }
                    }
                    for (int q = 0; q < 16; q++) vst1q_s32(s + j + 4 * q, c[q]);
                }
#endif
                for (ssize_t k = k0; k < k1; k++) {
                    const int8_t *w = kernel + k * kernelDim1 + jb;
#if defined(__aarch64__)
                    const int8x8_t av = vdup_n_s8(a[k]);
                    const int8x16_t avq = vcombine_s8(av, av);
                    for (ssize_t j = n64; j < n16; j += 16) {
                        const int8x16_t wv = vld1q_s8(w + j);
                        const int16x8_t lo = vmull_s8(vget_low_s8(wv), av), hi = vmull_high_s8(wv, avq);
                        vst1q_s32(s + j, vaddw_s16(vld1q_s32(s + j), vget_low_s16(lo)));
                        vst1q_s32(s + j + 4, vaddw_high_s16(vld1q_s32(s + j + 4), lo));
                        vst1q_s32(s + j + 8, vaddw_s16(vld1q_s32(s + j + 8), vget_low_s16(hi)));
                        vst1q_s32(s + j + 12, vaddw_high_s16(vld1q_s32(s + j + 12), hi));
                    }
#endif
                    const int32_t A = (int32_t)a[k] - inputOffset;
                    for (ssize_t j = n16; j < n; j++)
                        s[j] += ((int32_t)w[j] - kernelOffset) * A;
                }
            }
            for (ssize_t jj = 0; jj < n; jj++) {
                const ssize_t j = jb + jj;
                // Scale the bias to match the scale of the matrix multiplication.
                int32_t B = std::round(float(*((int32_t *)bias + j) - biasOffset) *
                                       (biasScale / matMulScale));
                int32_t sj = s[jj] + B;
                // Scale the result back to the expected destination scale.
                int32_t out32 = std::round(float(sj) * (matMulScale / outputScale) + outputOffset);
                int8_t out8;
                if(out32>127) out8=127;
                else if(out32<-128) out8=-128;
                else out8=(int8_t)out32;
                *(output + i*outputDim1 + j) = out8;
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
      //#### JO changed (2026-11-05): clip to 127 when the output scale is smaller (int8 v *= mul wrapped: 64 * 2 = -128)
      // int8_t v = input[i] > 0 ? input[i] : 0;
      // v *= mul;
      int v = input[i] > 0 ? input[i] * mul : 0;
      output[i] = (int8_t)(v > 127 ? 127 : v);
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
//#### JO added (2026-11-09): NEON paths of quantize / dequantize / elemadd / elemdiv for offsets 0 (the symmetric schemas),
//#### the same operations in the same order as the scalar loops: vcvtaq_s32_f32 rounds half away from zero as std::round
//#### and saturates, vqmovn clamps as the scalar clip, an int -> float conversion rounds to nearest in both, and with a
//#### zero offset an add or a fused multiply-add of the scalar code is the plain product. Results are bit-identical
//#### (DeiT-tiny: 16 / 20 / 21 / 7 ns -> about 1-2 ns per element).
#if defined(__aarch64__)
static inline bool isPow2f(float x) {
  int e;
  return x > 0.f && std::frexp(x, &e) == 0.5f;
}
static inline int8x16_t narrow16(int32x4_t a, int32x4_t b, int32x4_t c, int32x4_t d) {
  return vcombine_s8(vqmovn_s16(vcombine_s16(vqmovn_s32(a), vqmovn_s32(b))),
                     vqmovn_s16(vcombine_s16(vqmovn_s32(c), vqmovn_s32(d))));
}
static inline void widen16(int8x16_t v, int32x4_t *o) {
  const int16x8_t lo = vmovl_s8(vget_low_s8(v)), hi = vmovl_high_s8(v);
  o[0] = vmovl_s16(vget_low_s16(lo));
  o[1] = vmovl_high_s16(lo);
  o[2] = vmovl_s16(vget_low_s16(hi));
  o[3] = vmovl_high_s16(hi);
}
#endif
int quantize(int8_t *input, int8_t *output, dim_t size, float scale, int32_t offset){
  float *inputF = (float *)input;
  const ssize_t n = size, CH = 4096;
#if defined(__aarch64__)
  //#### JO added (2026-11-09): NEON (above); in / scale is in * (1 / scale) when 1 / scale is a power of two
  if (offset == 0) {
    const bool mul = isPow2f(1.f / scale) && 1.f / scale * scale == 1.f;
    const float32x4_t vs = vdupq_n_f32(mul ? 1.f / scale : scale);
    ssize_t i = 0;
    for (; i + 16 <= n; i += 16) {
      int32x4_t r[4];
      for (int j = 0; j < 4; j++) {
        const float32x4_t x = vld1q_f32(inputF + i + 4 * j);
        r[j] = vcvtaq_s32_f32(mul ? vmulq_f32(x, vs) : vdivq_f32(x, vs));
      }
      vst1q_s8(output + i, narrow16(r[0], r[1], r[2], r[3]));
    }
    for (; i < n; i++) {
      int32_t out32 = std::round(inputF[i] / scale);
      output[i] = (int8_t)(out32 > 127 ? 127 : (out32 < -128 ? -128 : out32));
    }
    return 0;
  }
#endif
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
#if defined(__aarch64__)
  //#### JO added (2026-11-09): NEON (above quantize)
  if (offset == 0) {
    const float32x4_t vs = vdupq_n_f32(scale);
    dim_t i = 0;
    for (; i + 16 <= size; i += 16) {
      int32x4_t v[4];
      widen16(vld1q_s8(input + i), v);
      for (int j = 0; j < 4; j++) vst1q_f32(outputF + i + 4 * j, vmulq_f32(vcvtq_f32_s32(v[j]), vs));
    }
    for (; i < size; i++) outputF[i] = (float)input[i] * scale;
    return 0;
  }
#endif
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


//#### JO added (2026-11-10): fast paths of the 4-dim int8 transpose (same result as the element loop below): runs of the
//#### innermost axis copied when it stays innermost, else 2-D transposes (NEON 16 x 16 byte blocks) for every index of
//#### the two other output axes. DeiT attention heads: 30 ms -> about 1 ms per inference.
// dst[i * ds + j] = src[j * ss + i], i < rows, j < cols
static void transpose2dI8(int8_t *dst, dim_t ds, const int8_t *src, dim_t ss, dim_t rows, dim_t cols) {
  dim_t i0 = 0;
#if defined(__aarch64__)
  for (; i0 + 16 <= rows; i0 += 16) {
    dim_t j0 = 0;
    for (; j0 + 16 <= cols; j0 += 16) {
      uint8x16_t R[16], P[16], Q[16], S[16];
      for (int j = 0; j < 16; j++)
        R[j] = vld1q_u8((const uint8_t *)src + (j0 + j) * ss + i0);
      for (int i = 0; i < 16; i += 2) {
        const uint8x16x2_t t = vtrnq_u8(R[i], R[i + 1]);
        P[i] = t.val[0];
        P[i + 1] = t.val[1];
      }
      for (int i = 0; i < 16; i += 4)
        for (int j = 0; j < 2; j++) {
          const uint16x8x2_t t = vtrnq_u16(vreinterpretq_u16_u8(P[i + j]), vreinterpretq_u16_u8(P[i + j + 2]));
          Q[i + j] = vreinterpretq_u8_u16(t.val[0]);
          Q[i + j + 2] = vreinterpretq_u8_u16(t.val[1]);
        }
      for (int i = 0; i < 16; i += 8)
        for (int j = 0; j < 4; j++) {
          const uint32x4x2_t t = vtrnq_u32(vreinterpretq_u32_u8(Q[i + j]), vreinterpretq_u32_u8(Q[i + j + 4]));
          S[i + j] = vreinterpretq_u8_u32(t.val[0]);
          S[i + j + 4] = vreinterpretq_u8_u32(t.val[1]);
        }
      for (int j = 0; j < 8; j++) {
        vst1q_u8((uint8_t *)dst + (i0 + j) * ds + j0, vcombine_u8(vget_low_u8(S[j]), vget_low_u8(S[j + 8])));
        vst1q_u8((uint8_t *)dst + (i0 + j + 8) * ds + j0, vcombine_u8(vget_high_u8(S[j]), vget_high_u8(S[j + 8])));
      }
    }
    for (dim_t i = i0; i < i0 + 16; i++)
      for (dim_t j = j0; j < cols; j++)
        dst[i * ds + j] = src[j * ss + i];
  }
#endif
  for (dim_t i = i0; i < rows; i++)
    for (dim_t j = 0; j < cols; j++)
      dst[i * ds + j] = src[j * ss + i];
}

static bool transposeFast(int8_t *input, int8_t *output, const dim_t id[4], const dim_t od[4], const unsigned sh[4]) {
  unsigned seen = 0;
  for (int i = 0; i < 4; i++) {
    if (sh[i] > 3 || od[i] != id[sh[i]])
      return false;
    seen |= 1u << sh[i];
  }
  if (seen != 15)
    return false;
  const dim_t is[4] = {id[1] * id[2] * id[3], id[2] * id[3], id[3], 1};
  const dim_t os[4] = {od[1] * od[2] * od[3], od[2] * od[3], od[3], 1};
  if (sh[3] == 3) {
    const long n = (long)(od[0] * od[1] * od[2]);
    const dim_t run = od[3];
#pragma omp parallel for schedule(static)
    for (long r = 0; r < n; r++) {
      const dim_t a = r / (od[1] * od[2]), b = (r / od[2]) % od[1], c = r % od[2];
      memcpy(output + r * run, input + a * is[sh[0]] + b * is[sh[1]] + c * is[sh[2]], run);
    }
    return true;
  }
  const int k = sh[0] == 3 ? 0 : sh[1] == 3 ? 1 : 2;
  int p = -1, q = -1;
  for (int i = 0; i < 3; i++)
    if (i != k)
      (p < 0 ? p : q) = i;
  const long n = (long)(od[p] * od[q]);
#pragma omp parallel for schedule(static)
  for (long r = 0; r < n; r++) {
    const dim_t u = r / od[q], v = r % od[q];
    transpose2dI8(output + u * os[p] + v * os[q], os[k], input + u * is[sh[p]] + v * is[sh[q]], is[sh[3]], od[k],
                  od[3]);
  }
  return true;
}

int transpose(int8_t *input, int8_t *output, dim_t inDim0, dim_t inDim1, dim_t inDim2, dim_t inDim3,
              dim_t outDim0, dim_t outDim1, dim_t outDim2, dim_t outDim3,
              unsigned_t shf0, unsigned_t shf1, unsigned_t shf2, unsigned_t shf3) {
  {
    const dim_t id[4] = {inDim0, inDim1, inDim2, inDim3}, od[4] = {outDim0, outDim1, outDim2, outDim3};
    const unsigned sh[4] = {(unsigned)shf0, (unsigned)shf1, (unsigned)shf2, (unsigned)shf3};
    if (transposeFast(input, output, id, od, sh))
      return 0;
  }
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
#if defined(__aarch64__)
  //#### JO added (2026-11-09): NEON (above quantize)
  if (input0Offset == 0 && input1Offset == 0 && destOffset == 0) {
    const float32x4_t v0 = vdupq_n_f32(k0), v1 = vdupq_n_f32(k1), vd = vdupq_n_f32(kd);
    const ssize_t n = size;
    ssize_t i = 0;
    for (; i + 16 <= n; i += 16) {
      int32x4_t L[4], R[4], S[4];
      widen16(vld1q_s8(input0 + i), L);
      widen16(vld1q_s8(input1 + i), R);
      for (int j = 0; j < 4; j++) {
        const int32x4_t l32 = vcvtaq_s32_f32(vmulq_f32(vcvtq_f32_s32(L[j]), v0));
        const int32x4_t r32 = vcvtaq_s32_f32(vmulq_f32(vcvtq_f32_s32(R[j]), v1));
        S[j] = vcvtaq_s32_f32(vmulq_f32(vcvtq_f32_s32(vaddq_s32(l32, r32)), vd));
      }
      vst1q_s8(output + i, narrow16(S[0], S[1], S[2], S[3]));
    }
    for (; i < n; i++) {
      int32_t L32 = std::round(float(input0[i]) * k0);
      int32_t R32 = std::round(float(input1[i]) * k1);
      int32_t sum32 = std::round(float(L32 + R32) * kd);
      output[i] = (int8_t)(sum32 > 127 ? 127 : (sum32 < -128 ? -128 : sum32));
    }
    return 0;
  }
#endif
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
#if defined(__aarch64__)
  //#### JO added (2026-11-09): NEON for zero offsets, the scalar expression below (l = k0 * L, r = k1 * R, round(l - r))
  if (input0Offset == 0 && input1Offset == 0 && destOffset == 0) {
    const float k0 = input0Scale / destScale, k1 = input1Scale / destScale;
    const float32x4_t v0 = vdupq_n_f32(k0), v1 = vdupq_n_f32(k1);
    dim_t i = 0;
    for (; i + 16 <= size; i += 16) {
      int32x4_t L[4], R[4], S[4];
      widen16(vld1q_s8(input0 + i), L);
      widen16(vld1q_s8(input1 + i), R);
      for (int j = 0; j < 4; j++)
        S[j] = vcvtaq_s32_f32(vsubq_f32(vmulq_f32(v0, vcvtq_f32_s32(L[j])), vmulq_f32(v1, vcvtq_f32_s32(R[j]))));
      vst1q_s8(output + i, narrow16(S[0], S[1], S[2], S[3]));
    }
    for (; i < size; i++) {
      const float l = k0 * float(input0[i]);
      const float r = k1 * float(input1[i]);
      int32_t sum32 = std::round(l - r);
      output[i] = (int8_t)(sum32 > 127 ? 127 : (sum32 < -128 ? -128 : sum32));
    }
    return 0;
  }
#endif
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
#if defined(__aarch64__)
  //#### JO added (2026-11-09): NEON (above quantize); r = (input1Scale * destScale) * R as the scalar expression
  if (input0Offset == 0 && input1Offset == 0 && destOffset == 0) {
    const float c1 = input1Scale * destScale;
    const float32x4_t v0 = vdupq_n_f32(input0Scale), v1 = vdupq_n_f32(c1);
    dim_t i = 0;
    for (; i + 16 <= size; i += 16) {
      int32x4_t L[4], R[4], S[4];
      widen16(vld1q_s8(input0 + i), L);
      widen16(vld1q_s8(input1 + i), R);
      for (int j = 0; j < 4; j++)
        S[j] = vcvtaq_s32_f32(vdivq_f32(vmulq_f32(v0, vcvtq_f32_s32(L[j])), vmulq_f32(v1, vcvtq_f32_s32(R[j]))));
      vst1q_s8(output + i, narrow16(S[0], S[1], S[2], S[3]));
    }
    for (; i < size; i++) {
      int32_t sum32 = std::round((input0Scale * float(input0[i])) / (c1 * float(input1[i])));
      output[i] = (int8_t)(sum32 > 127 ? 127 : (sum32 < -128 ? -128 : sum32));
    }
    return 0;
  }
#endif
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
  //#### JO changed (2026-10-31): 16-byte copies (the 16 channels of a block are contiguous in both layouts); the byte
  //#### loop took 1.75 ms for 56x56x64 on one A53 core
#pragma omp parallel for schedule(static)
  for (int r = 0; r < rows; r++) {
    const int n = r / (CB * H), c_ = (r / H) % CB, h = r % H;
    const int8_t *in = input + n*H*W*C + h*W*C + c_*16;
    int8_t *out = output + n*CB*H*W*16 + c_*H*W*16 + h*W*16;
    for (int w = 0; w < W; w++)
      memcpy(out + w*16, in + w*C, 16);
    // for (int w = 0; w < W; w++)
    //   for (int c = 0; c < 16; c++)
    //     out[w*16 + c] = in[w*C + c];
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
  //#### JO changed (2026-10-31): 16-byte copies per channel block (C % 16 channels of the last block: that many bytes);
  //#### the byte loop with f / 16, f % 16 took 3.3 ms for 56x56x64 on one A53 core
  const int rows = N * H, CB = (C + 15) / 16;
#pragma omp parallel for schedule(static)
  for (int r = 0; r < rows; r++) {
    const int b = r / H, i = r % H;
    for (int c_ = 0; c_ < CB; c_++) {
      const int nc = std::min(16, C - c_ * 16);
      const int8_t *in = input + c_ * H * W * 16 + i * W * 16;
      int8_t *out = output + b * H * W * C + i * W * C + c_ * 16;
      if (nc == 16)
        for (int j = 0; j < W; j++)
          memcpy(out + j * C, in + j * 16, 16);
      else
        for (int j = 0; j < W; j++)
          memcpy(out + j * C, in + j * 16, nc);
    }
    // for (int j = 0; j < W; j++) {
    //   int8_t *out = output + b * H * W * C + i * W * C + j * C;
    //   for (int f = 0; f < C; f++)
    //     out[f] = *(input + ((f / 16) * H * W * 16 + i * W * 16 + j * 16 + f % 16));
    // }
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

//#### JO changed (2026-11-01): one pass, 16 bytes per pixel (the 3 input channels, then zeros), OpenMP over rows; was
//#### memset + malloc'ed NCHW + byte-wise reshape (Resnet18 conv0 224x224x3 -> 16 channels: 6.8 ms with 4 threads).
//#### Same output for N = 1 (input NHWC with 3 channels, channels 3..C-1 zero; the old code mixed the batches for N > 1).
//#### Old version below as _old.
void transpose_nhwc2vtaio_zerofill(int8_t* input, int8_t* output, int N, int H, int W, int C){
  const int CB = C / 16;
  const int rows = N * H;
#pragma omp parallel for schedule(static)
  for (int r = 0; r < rows; r++) {
    const int n = r / H, h = r % H;
    const int8_t *in = input + ((size_t)n * H + h) * W * 3;
    int8_t *out = output + ((size_t)n * CB * H + h) * W * 16;
    for (int w = 0; w < W; w++) {
      int8_t px[16] = {0};
      px[0] = in[w * 3]; px[1] = in[w * 3 + 1]; px[2] = in[w * 3 + 2];
      memcpy(out + w * 16, px, 16);
    }
    for (int c_ = 1; c_ < CB; c_++)
      memset(out + (size_t)c_ * H * W * 16, 0, (size_t)W * 16);
  }
}

//#### JO added (2026-11-02): the input preprocessing in one pass (VTASave.cpp findInputPreprocess): quantize the float
//#### input as quantize() does (std::round(x / scale + offset), clipped to int8), map each channel's value through its
//#### 256-entry table (elemsub / elemdiv by per-channel constants, folded at compile time; nullptr = none) and write the
//#### EVTA layout N{CO/16}HW16 of the zero-filled first conv (channels C..CO-1 are 0, C <= 16). nchw: the input is
//#### NCHW (an NCHW -> NHWC transpose folded in), else NHWC.
static inline int8_t preprocessQuant1(float x, float scale, int32_t offset) {
  int32_t q = std::round(x / scale + offset);
  return (int8_t)(q > 127 ? 127 : (q < -128 ? -128 : q));
}
#if defined(__aarch64__)
// 16 values as preprocessQuant1 (x / scale, or x * (1 / scale) when that is exact: scale a power of two). vcvtaq rounds
// half away from zero like std::round, the saturating narrows clip to int8.
static inline int8x16_t preprocessQuant16(const float32x4_t x[4], float32x4_t vs, float32x4_t vo, bool mul) {
  int16x8_t h[2];
  for (int i = 0; i < 2; i++) {
    int32x4_t a = vcvtaq_s32_f32(vaddq_f32(mul ? vmulq_f32(x[2 * i], vs) : vdivq_f32(x[2 * i], vs), vo));
    int32x4_t b = vcvtaq_s32_f32(vaddq_f32(mul ? vmulq_f32(x[2 * i + 1], vs) : vdivq_f32(x[2 * i + 1], vs), vo));
    h[i] = vcombine_s16(vqmovn_s32(a), vqmovn_s32(b));
  }
  return vcombine_s8(vqmovn_s16(h[0]), vqmovn_s16(h[1]));
}
// lut[q + 128] for 16 values: 4 table lookups of 64 bytes (vqtbx keeps the lanes whose index is out of its table)
static inline int8x16_t preprocessLut16(int8x16_t q, const uint8x16x4_t t[4]) {
  uint8x16_t i = veorq_u8(vreinterpretq_u8_s8(q), vdupq_n_u8(0x80)), d = vdupq_n_u8(64);
  uint8x16_t r = vqtbl4q_u8(t[0], i);
  r = vqtbx4q_u8(r, t[1], i = vsubq_u8(i, d));
  r = vqtbx4q_u8(r, t[2], i = vsubq_u8(i, d));
  r = vqtbx4q_u8(r, t[3], vsubq_u8(i, d));
  return vreinterpretq_s8_u8(r);
}
#endif
void preprocess_nhwc2vtaio_zerofill(const float *input, int8_t *output, int N, int H, int W, int C, int CO, int nchw,
                                    float scale, int32_t offset, const int8_t *lut) {
  const int CB = CO / 16, rows = N * H;
#if defined(__aarch64__)
  int fe;
  const bool mul = std::frexp(scale, &fe) == 0.5f && std::isnormal(1 / scale);
  const float32x4_t vs = vdupq_n_f32(mul ? 1 / scale : scale), vo = vdupq_n_f32((float)offset);
  uint8x16x4_t tab[3][4];
  if (C == 3 && lut)
    for (int c = 0; c < 3; c++)
      for (int k = 0; k < 4; k++)
        for (int j = 0; j < 4; j++)
          tab[c][k].val[j] = vld1q_u8((const uint8_t *)lut + c * 256 + k * 64 + j * 16);
#endif
#pragma omp parallel for schedule(static)
  for (int r = 0; r < rows; r++) {
    const int n = r / H, h = r % H;
    int8_t *out = output + ((size_t)n * CB * H + h) * W * 16;
    int w0 = 0;
#if defined(__aarch64__)
    // 3 channels, 16 pixels at a time: quantize, table, then each pixel as one 32-bit word c0 c1 c2 0 followed by 12 zero
    // bytes (vst4q_u32 with three zero vectors)
    if (C == 3) {
      const uint32x4_t z = vdupq_n_u32(0);
      for (; w0 + 16 <= W; w0 += 16) {
        int8x16_t q[3];
        float32x4_t x[3][4];
        if (nchw) {
          for (int c = 0; c < 3; c++) {
            const float *src = input + (((size_t)n * 3 + c) * H + h) * W + w0;
            for (int k = 0; k < 4; k++)
              x[c][k] = vld1q_f32(src + 4 * k);
          }
        } else {
          const float *src = input + (((size_t)n * H + h) * W + w0) * 3;
          for (int k = 0; k < 4; k++) {
            float32x4x3_t v = vld3q_f32(src + 12 * k);
            x[0][k] = v.val[0], x[1][k] = v.val[1], x[2][k] = v.val[2];
          }
        }
        for (int c = 0; c < 3; c++) {
          q[c] = preprocessQuant16(x[c], vs, vo, mul);
          if (lut)
            q[c] = preprocessLut16(q[c], tab[c]);
        }
        uint8x16_t c0 = vreinterpretq_u8_s8(q[0]), c1 = vreinterpretq_u8_s8(q[1]), c2 = vreinterpretq_u8_s8(q[2]),
                   zb = vdupq_n_u8(0);
        uint16x8_t p01[2] = {vreinterpretq_u16_u8(vzip1q_u8(c0, c1)), vreinterpretq_u16_u8(vzip2q_u8(c0, c1))};
        uint16x8_t p2[2] = {vreinterpretq_u16_u8(vzip1q_u8(c2, zb)), vreinterpretq_u16_u8(vzip2q_u8(c2, zb))};
        for (int i = 0; i < 2; i++) {
          uint32x4x4_t a = {{vreinterpretq_u32_u16(vzip1q_u16(p01[i], p2[i])), z, z, z}};
          vst4q_u32((uint32_t *)(out + (size_t)(w0 + 8 * i) * 16), a);
          a.val[0] = vreinterpretq_u32_u16(vzip2q_u16(p01[i], p2[i]));
          vst4q_u32((uint32_t *)(out + (size_t)(w0 + 8 * i + 4) * 16), a);
        }
      }
    }
#endif
    for (int w = w0; w < W; w++) {
      int8_t px[16] = {0};
      for (int c = 0; c < C; c++) {
        int8_t v = preprocessQuant1(nchw ? input[(((size_t)n * C + c) * H + h) * W + w]
                                         : input[(((size_t)n * H + h) * W + w) * C + c],
                                    scale, offset);
        px[c] = lut ? lut[c * 256 + v + 128] : v;
      }
      memcpy(out + (size_t)w * 16, px, 16);
    }
    for (int c_ = 1; c_ < CB; c_++)
      memset(out + (size_t)c_ * H * W * 16, 0, (size_t)W * 16);
  }
}

//#### JO added (2026-11-02): packed input of a first conv with few channels (VTASave.cpp -vta-conv0-pack): the
//#### KH x KW / stride s conv over C channels (pad p) becomes a ceil(KH/s) x 1 / stride 1 conv without pad over CO channels,
//#### CO = s * KW * C rounded up to 16 (Resnet18 conv0 7x7/2 over 3 channels: 4x1 over 48 instead of 7x7 over 16 zero
//#### filled). Packed pixel (q, x), channel (dh * KW + j) * C + c = padded input (s * q + dh, s * x + j, c); the rest 0.
//#### Its size: H2 = out_h + ceil(KH/s) - 1 rows, W2 = out_w columns, EVTA layout N{CO/16}H2W2 16.
//#### JO changed (2026-11-07): padEnd = the bottom / right pad (-1: pad; TF SAME of a stride-2 conv: pad 0, padEnd 1)
struct Conv0PackDims {
  int s, KH2, H2, W2, PW, CB;
  Conv0PackDims(int H, int W, int KH, int KW, int stride, int pad, int CO, int padEnd = -1) {
    const int pe = padEnd < 0 ? pad : padEnd;
    s = stride;
    KH2 = (KH + s - 1) / s;
    H2 = (H + pad + pe - KH) / s + 1 + KH2 - 1;
    W2 = (W + pad + pe - KW) / s + 1;
    PW = s * (W2 - 1) + KW;   // columns of the padded input that the packed pixels read
    CB = CO / 16;
  }
};
//#### Packed row q reads only the padded rows s * q .. s * q + s - 1, which no other packed row reads: each thread
//#### writes those s rows (fill(h, dst): input row h, its first w1 = min(W, PW - pad) pixels, at dst) into its own
//#### buffer of s padded rows (the pad columns stay 0) and packs them at once (no padded image in memory). Board, Resnet18
//#### conv0 input, OMP 1: 2.18 -> 1.69 ms (zero-filled copy 1.56 ms; most of it is the quantize, docs/evta_runtime_compare.md 7.12).
//#### JO changed (2026-11-05): packed rows q0 .. q1 - 1 only (preprocess_conv0pack_rows: written while the EVTA conv runs)
template <class Fill>
static void conv0packRows(int8_t *output, int n, int H, int pad, const Conv0PackDims &d, int KW, int C, int CO,
                          Fill fill, int q0 = 0, int q1 = -1) {
  if (q1 < 0 || q1 > d.H2)
    q1 = d.H2;
  const int run = KW * C, used = d.s * run;
  const size_t rowBytes = (size_t)d.PW * C, plane = (size_t)d.H2 * d.W2 * 16;
#if defined(__aarch64__)
  // s <= 2, KW * C <= 32, CO <= 64 (Resnet conv0 7x7/2: 21 bytes per row, 48 channels): the 32 bytes at the pixel in
  // each of the s rows are one 64-byte table, each 16-channel block one vqtbl4q_u8 (index 255 = 0)
  const bool tbl = d.s <= 2 && run <= 32 && CO <= 64;
  uint8_t idx[64];
  for (int ch = 0; ch < 64; ch++)
    idx[ch] = ch < used ? (uint8_t)(ch / run * 32 + ch % run) : 255;
  uint8x16_t vi[4];
  for (int b = 0; b < 4; b++)
    vi[b] = vld1q_u8(idx + 16 * b);
#endif
#pragma omp parallel
  {
    // s padded rows, 32 bytes to spare for the 16- / 32-byte reads at the last pixel
    std::vector<int8_t> rows(d.s * rowBytes + 32, 0);
#pragma omp for schedule(static)
    for (int q = q0; q < q1; q++) {
      for (int dh = 0; dh < d.s; dh++) {
        int8_t *dst = rows.data() + dh * rowBytes;
        const int h = d.s * q + dh - pad;
        if (h < 0 || h >= H)
          memset(dst, 0, rowBytes);
        else
          fill(h, dst + (size_t)pad * C);
      }
      int8_t *out = output + (((size_t)n * d.CB * d.H2 + q) * d.W2) * 16;
#if defined(__aarch64__)
      if (tbl) {
        // everything the loop reads in locals: its byte stores may alias any memory, so d, vi[] would be reloaded
        const uint8_t *r0 = (const uint8_t *)rows.data(), *r1 = d.s == 2 ? r0 + rowBytes : r0;
        const int W2 = d.W2, CB = d.CB, step = d.s * C;
        const uint8x16_t i0 = vi[0], i1 = vi[1], i2 = vi[2], i3 = vi[3];
        uint8_t *o0 = (uint8_t *)out, *o1 = o0 + plane, *o2 = o1 + plane, *o3 = o2 + plane;
        for (int x = 0; x < W2; x++, r0 += step, r1 += step) {
          uint8x16x4_t t = {{vld1q_u8(r0), vld1q_u8(r0 + 16), vld1q_u8(r1), vld1q_u8(r1 + 16)}};
          vst1q_u8(o0 + 16 * x, vqtbl4q_u8(t, i0));
          if (CB > 1)
            vst1q_u8(o1 + 16 * x, vqtbl4q_u8(t, i1));
          if (CB > 2)
            vst1q_u8(o2 + 16 * x, vqtbl4q_u8(t, i2));
          if (CB > 3)
            vst1q_u8(o3 + 16 * x, vqtbl4q_u8(t, i3));
        }
        continue;
      }
#endif
      //#### JO changed (2026-11-09): CO up to 1024 (DeiT patch embedding 16x16 / 16 over 3 channels: 1x1 over 768), was 256
      int8_t v[1024];
      memset(v, 0, sizeof(v));
      for (int x = 0; x < d.W2; x++) {
        for (int dh = 0; dh < d.s; dh++)
          memcpy(v + dh * run, rows.data() + dh * rowBytes + (size_t)d.s * x * C, run);
        for (int b = 0; b < d.CB; b++)
          memcpy(out + b * plane + (size_t)x * 16, v + 16 * b, 16);
      }
    }
  }
}
void conv0pack_nhwc2vtaio(const int8_t *input, int8_t *output, int N, int H, int W, int C, int KH, int KW, int stride,
                          int pad, int CO, int padEnd) {
  const Conv0PackDims d(H, W, KH, KW, stride, pad, CO, padEnd);
  assert(CO <= 1024 && CO % 16 == 0 && stride * KW * C <= CO);
  const int w1 = std::min(W, d.PW - pad);
  for (int n = 0; n < N; n++)
    conv0packRows(output, n, H, pad, d, KW, C, CO, [&](int h, int8_t *dst) {
      memcpy(dst, input + ((size_t)n * H + h) * W * C, (size_t)w1 * C);
    });
}
// with the quantize / table of preprocess_nhwc2vtaio_zerofill (findInputPreprocess) written into the padded rows
void preprocess_conv0pack(const float *input, int8_t *output, int N, int H, int W, int C, int nchw, float scale,
                          int32_t offset, const int8_t *lut, int KH, int KW, int stride, int pad, int CO, int padEnd) {
  preprocess_conv0pack_rows(input, output, N, H, W, C, nchw, scale, offset, lut, KH, KW, stride, pad, CO, 0, -1, padEnd);
}
// packed rows q0 .. q1 - 1 (-1 = all) of every image
void preprocess_conv0pack_rows(const float *input, int8_t *output, int N, int H, int W, int C, int nchw, float scale,
                               int32_t offset, const int8_t *lut, int KH, int KW, int stride, int pad, int CO, int q0,
                               int q1, int padEnd) {
  const Conv0PackDims d(H, W, KH, KW, stride, pad, CO, padEnd);
  assert(CO <= 1024 && CO % 16 == 0 && stride * KW * C <= CO);
#if defined(__aarch64__)
  int fe;
  const bool mul = std::frexp(scale, &fe) == 0.5f && std::isnormal(1 / scale);
  const float32x4_t vs_ = vdupq_n_f32(mul ? 1 / scale : scale), vo_ = vdupq_n_f32((float)offset);
  uint8x16x4_t tab_[3][4];
  if (C == 3 && lut)
    for (int c = 0; c < 3; c++)
      for (int k = 0; k < 4; k++)
        for (int j = 0; j < 4; j++)
          tab_[c][k].val[j] = vld1q_u8((const uint8_t *)lut + c * 256 + k * 64 + j * 16);
#endif
  const int w1 = std::min(W, d.PW - pad);  // image columns the packed input reads
  for (int n = 0; n < N; n++)
    conv0packRows(output, n, H, pad, d, KW, C, CO, [&](int h, int8_t *row) {
      int w0 = 0;
#if defined(__aarch64__)
      if (C == 3) {
        // the constants in locals: the byte stores below may alias the captured ones (reloaded at every pixel block)
        const float32x4_t vs = vs_, vo = vo_;
        uint8x16x4_t tab[3][4];
        if (lut)
          memcpy(tab, tab_, sizeof(tab));
        for (; w0 + 16 <= w1; w0 += 16) {
          float32x4_t x[3][4];
          if (nchw) {
            for (int c = 0; c < 3; c++) {
              const float *src = input + (((size_t)n * 3 + c) * H + h) * W + w0;
              for (int k = 0; k < 4; k++)
                x[c][k] = vld1q_f32(src + 4 * k);
            }
          } else {
            const float *src = input + (((size_t)n * H + h) * W + w0) * 3;
            for (int k = 0; k < 4; k++) {
              float32x4x3_t v = vld3q_f32(src + 12 * k);
              x[0][k] = v.val[0], x[1][k] = v.val[1], x[2][k] = v.val[2];
            }
          }
          int8x16x3_t q;
          for (int c = 0; c < 3; c++) {
            q.val[c] = preprocessQuant16(x[c], vs, vo, mul);
            if (lut)
              q.val[c] = preprocessLut16(q.val[c], tab[c]);
          }
          vst3q_s8(row + (size_t)w0 * 3, q);
        }
      }
#endif
      for (int w = w0; w < w1; w++)
        for (int c = 0; c < C; c++) {
          int8_t v = preprocessQuant1(nchw ? input[(((size_t)n * C + c) * H + h) * W + w]
                                           : input[(((size_t)n * H + h) * W + w) * C + c],
                                      scale, offset);
          row[(size_t)w * C + c] = lut ? lut[c * 256 + v + 128] : v;
        }
    }, q0, q1);
}

void transpose_nhwc2vtaio_zerofill_old(int8_t* input, int8_t* output, int N, int H, int W, int C){
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
