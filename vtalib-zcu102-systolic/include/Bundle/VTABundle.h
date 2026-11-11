#ifndef VTALIB_EXAMPLE_VTABUNDLE_H
#define VTALIB_EXAMPLE_VTABUNDLE_H

#define dim_t uint64_t
#define unsigned_t uint32_t

#include <string>

void xlnk_reset();
//#### JO added (2026-11-06): load_module's constant VTA buffers, shared by the bundle copies of a process (VTABundle.cpp)
void *VTAConstBufferAlloc(const void *src, size_t size);
void VTAConstBufferFree(void *buf);

int elemsign(int8_t* input, int8_t *output, unsigned_t size);
int convolution(int8_t* input, int8_t *kernel, int8_t *output, int N, int H, int W, int C, int KN, int KH, int KW, int pad_size,
                int stride_size, bool doRelu, bool doBias, uint8_t shift, int out_h, int out_w, int tile_h=14, int tile_w=14, uint32_t idEVTA = 0);

int convolution_wo_tr(int8_t* input, int8_t *kernel, int32_t *bias, int8_t *output, int N, int H, int W, int C, int KN, int KH, int KW, int pad_size,
                       int stride_size, bool doRelu, bool doBias, uint8_t shift, int out_h, int out_w, void* vtaCmdH, int nVirtualThread = 1, int tileHSize = 14, int tileWSize = 14, uint32_t idEVTA = 0);
//#### JO added (2026-11-09): int8 matmul with both operands written at run time, as an EVTA 1x1 conv (see VTABundle.cpp)
int matmul_evta(const int8_t *A, const int8_t *B, int8_t *C, int M, int K, int N, int shift, void *vtaCmdH,
                int nVirtualThread = 1, int tileHSize = 14, int tileWSize = 14, uint32_t idEVTA = 0);
//#### JO added (2026-11-10): the products of H heads in one EVTA call; Q K^T from the EVTA outputs of the query / key convs
int matmul_evta_n(int H, const int8_t *const *A, const int8_t *const *B, int8_t *const *C, int M, int K, int N, int shift,
                  void *vtaCmdH, int nVirtualThread = 1, int tileHSize = 14, int tileWSize = 14, uint32_t idEVTA = 0);
int matmul_evta_qk(void *qbuf, void *kbuf, int8_t *const *C, int H, int M, int dh, int shift, void *vtaCmdH,
                   int nVirtualThread = 1, int tileHSize = 14, int tileWSize = 14, uint32_t idEVTA = 0);
//#### JO added (2026-11-02): conv (with relu) + max pool 3x3 / stride 2 / pad 1 in one EVTA call (see VTABundle.cpp ConvPool)
int convolution_wo_tr_pool(int8_t* input, int8_t *kernel, int32_t *bias, int8_t *output, int N, int H, int W, int C, int KN, int KH, int KW, int pad_size,
                           int stride_size, bool doRelu, bool doBias, uint8_t shift, int out_h, int out_w, void* vtaCmdH,
                           int nVirtualThread, int tileHSize, int tileWSize, uint32_t idEVTA,
                           int8_t *pool_out, int poolK, int poolStride, int poolPad);
//#### JO added (2026-10-31): max pool on the EVTA, input >= 0, EVTA layout (see VTABundle.cpp)
int maxpool_evta(int8_t *input, int8_t *output, int N, int H, int W, int C, int KH, int KW, int pad_size,
                 int stride_size, void *vtaCmdH, uint32_t idEVTA = 0);
//#### JO added (2026-11-02): fully connected layer on the EVTA, the arguments of fullyconnected (see VTABundle.cpp)
int fullyconnected_evta(int8_t *input, float inputScale, int32_t inputOffset, int8_t *kernel, float kernelScale,
                        int32_t kernelOffset, int8_t *bias, float biasScale, int32_t biasOffset, int8_t *output,
                        float outputScale, int32_t outputOffset, dim_t inputDim0, dim_t inputDim1, dim_t kernelDim0,
                        dim_t kernelDim1, dim_t outputDim0, dim_t outputDim1, bool doBias, void *vtaCmdH,
                        uint32_t idEVTA = 0);
//#### JO added (2026-11-02): the same with the weights and bias in the EVTA layout at compile time (VTA buffers)
int fullyconnected_evta_packed(int8_t *input, void *wgt, void *bias, int8_t *output, int IN, int OUT, int shift,
                               void *vtaCmdH, uint32_t idEVTA = 0);
//#### JO added (2026-11-08): depthwise conv on the EVTA (diagonal weights per tap, per-channel shift; see VTABundle.cpp)
int dw_convolution_evta(int8_t *input, void *kernel, void *bias, void *shift, int8_t *output, int N, int H, int W, int C,
                        int KH, int KW, int pad, int stride, int out_h, int out_w, int lo, int hi, void *vtaCmdH,
                        int nVirtualThread, int tileHSize, int tileWSize, uint32_t idEVTA = 0);
//#### JO added (2026-10-31): conv + elementadd (+ relu) in one EVTA call (power-of-two scales, see VTABundle.cpp)
int convolution_wo_tr_res(int8_t* input, int8_t *kernel, int32_t *bias, int8_t *output, int N, int H, int W, int C, int KN, int KH, int KW, int pad_size,
                          int stride_size, bool doRelu, bool doBias, uint8_t shift, int out_h, int out_w,
                          int8_t *residual, int convShl, int resShl, int sumShr, int sumRelu,
                          void* vtaCmdH, int nVirtualThread = 1, int tileHSize = 14, int tileWSize = 14, uint32_t idEVTA = 0);

int xp_convolution_wo_tr(int8_t* input, int8_t *kernel, int32_t *bias, int32_t *scalingfactor, int8_t *output, int N, int H, int W, int C, int KN, int KH, int KW, int pad_size, int stride_size, bool doRelu, bool doBias, uint8_t shift, int out_h, int out_w, void* vtaCmdH, int nVirtualThread = 1, int tileHSize = 14, int tileWSize = 14, uint32_t idEVTA = 0, bool mul = false);

int xp_convolution_wo_tr_scale(int8_t* input, int8_t *kernel, int32_t *bias, int32_t *scalingfactor, int8_t *output, int N, int H, int W, int C, int KN, int KH, int KW, int pad_size, int stride_size, bool doRelu, bool doBias, uint8_t shift, int out_h, int out_w, void* vtaCmdH, int nVirtualThread, int tileHSize, int tileWSize, uint32_t idEVTA, bool domul = true);

int convolution_wo_tr_scale(int8_t* input, int8_t *kernel, int32_t *bias, int32_t *scalingfactor, int8_t *output, int N, int H, int W, int C, int KN, int KH, int KW, int pad_size,
                      int stride_size, bool doRelu, bool doBias, uint8_t shift, int out_h, int out_w, void* vtaCmdH, int nVirtualThread, int tileHSize, int tileWSize, uint32_t idEVTA);


int convolution_wo_tr_wo_ch(int8_t* input, int8_t *kernel, int32_t *bias, int8_t *output, int N, int H, int W, int C, int KN, int KH, int KW, int pad_size,
                            int stride_size, bool doRelu, bool doBias, uint8_t shift, int out_h, int out_w, uint32_t idEVTA = 0);


int convolutionFloat(int8_t* input, int8_t *kernel, int8_t *bias, int8_t *output, dim_t N, dim_t H, dim_t W, dim_t C,
        dim_t KN, dim_t KH, dim_t KW, int pad_size, int stride_size, size_t group, size_t dilation,
        bool doRelu, bool doBias, int out_h, int out_w);

int bnn_nonvtaconvolution_scale(int8_t* input, float inputScale, int32_t inputOffset, int8_t *kernel, float kernelScale, int32_t kernelOffset, 
				int8_t *bias, float biasScale, int32_t biasOffset, int8_t* scalefactor, int8_t *output, int32_t outputShift, int32_t outputOffset, dim_t N, dim_t H, dim_t W, dim_t C, 
				dim_t KN, dim_t KH, dim_t KW, int pad_size, int stride_size, size_t group, size_t dilation, bool doRelu, bool doBias, int out_h, int out_w);

int nonvtaconvolution(int8_t* input, float inputScale, int32_t inputOffset, int8_t *kernel, float kernelScale, int32_t kernelOffset,
                      int8_t *bias, float biasScale, int32_t biasOffset, int8_t *output, float outputScale, int32_t outputOffset, dim_t N, dim_t H, dim_t W, dim_t C, dim_t KN, dim_t KH, dim_t KW, int pad_size,
                      int stride_size, size_t group, size_t dilation, bool doRelu, bool doBias, int out_h, int out_w);

//#### JO added (2026-11-07): the channelwise quantized conv (VTASave saveChannelwiseConvolutionInst; the depthwise convs with
//#### -enable-channelwise-depthwise): out = clip(round(float(sum + bias[d]) * mult[d]), lo, hi) per output channel d, NHWC;
//#### kernel [KH][KW][C] for a depthwise conv (group = C = KN), else [KN][KH][KW][C/group]; pads top / left (the taps past
//#### the bottom / right are skipped)
//#### JO changed (2026-11-08): inPacked / outPacked: the input / output in the EVTA layout N{C/16}HW16 (depthwise, C % 16 = 0)
int cw_convolution(const int8_t *input, const int8_t *kernel, const int32_t *bias, const float *mult, int8_t *output,
                   int N, int H, int W, int C, int KN, int KH, int KW, int padT, int padL, int strideH, int strideW,
                   int dilH, int dilW, int group, int outH, int outW, int lo, int hi, int inPacked = 0,
                   int outPacked = 0);

int nonvtaconvolution_f32(float* input, float inputScale, int32_t inputOffset, int8_t *kernel, float kernelScale, int32_t kernelOffset,
                      float *bias, float biasScale, int32_t biasOffset, float *output, float outputScale, int32_t outputOffset, dim_t N, dim_t H, dim_t W, dim_t C, dim_t KN, dim_t KH, dim_t KW, int pad_size,
                      int stride_size, size_t group, size_t dilation, bool doRelu, bool doBias, int out_h, int out_w);


int cpuvtaconvolution(int8_t* input, float inputScale, int32_t inputOffset, int8_t *kernel, float kernelScale, int32_t kernelOffset,
                      int8_t *bias, float biasScale, int32_t biasOffset, int8_t *output, float outputScale, int32_t outputOffset, dim_t N, dim_t H, dim_t W, dim_t C, dim_t KN, dim_t KH, dim_t KW, int pad_size,
                      int stride_size, size_t group, size_t dilation, bool doRelu, bool doBias, int out_h, int out_w);


int fullyconnected(int8_t* input, float inputScale, int32_t inputOffset, int8_t *kernel, float kernelScale, int32_t kernelOffset,
                   int8_t *bias, float biasScale, int32_t biasOffset, int8_t *output, float outputScale, int32_t outputOffset,
                   dim_t inputDim0, dim_t inputDim1, dim_t kernelDim0, dim_t kernelDim1, dim_t outputDim0, dim_t outputDim1, bool doBias);

int maxpool(int8_t* input, int8_t *output, uint64_t N, uint64_t H, uint64_t W, uint64_t C, uint32_t KH, uint32_t KW, uint32_t pad_size,
            uint32_t stride_size);

int avgpool(int8_t* input, float inputScale, int32_t inputOffset, int8_t *output, float outputScale, int32_t outputOffset,
            dim_t inputDim0, dim_t inputDim1, dim_t inputDim2, dim_t inputDim3,
            dim_t outputDim0, dim_t outputDim1, dim_t outputDim2, dim_t outputDim3,
            unsigned_t KH, unsigned_t KW, unsigned_t pad_size, unsigned_t stride_size);

int relu(int8_t* input, int8_t *output, unsigned_t size, float inputScale=1.0, float outputScale=1.0);

int quantize(int8_t *input, int8_t *output, uint64_t size, float scale, int32_t offset);

int dequantize(int8_t *input, int8_t *output, uint64_t size, float scale, int32_t offset);

int typecast(int8_t *input, int8_t *output, uint64_t size, float scale, int32_t offset);

int transpose(int8_t *input, int8_t *output, dim_t inDim0, dim_t inDim1, dim_t inDim2, dim_t inDim3,
              dim_t outDim0, dim_t outDim1, dim_t outDim2, dim_t outDim3,
              unsigned_t shf0, unsigned_t shf1, unsigned_t shf2, unsigned_t shf3);

int transpose_6dim(int8_t *input, int8_t *output, dim_t inDim0, dim_t inDim1, dim_t inDim2, dim_t inDim3, dim_t inDim4, dim_t inDim5,
                   dim_t outDim0, dim_t outDim1, dim_t outDim2, dim_t outDim3, dim_t outDim4, dim_t outDim5,
                   unsigned_t shf0, unsigned_t shf1, unsigned_t shf2, unsigned_t shf3, unsigned_t shf4, unsigned_t shf5);

int8_t* at6dim(int8_t *input, dim_t dim0, dim_t dim1, dim_t dim2, dim_t dim3, dim_t dim4, dim_t dim5,
               dim_t loc0, dim_t loc1, dim_t loc2, dim_t loc3 , dim_t loc4 , dim_t loc5);

int elemadd(int8_t *input0, float input0Scale, int32_t input0Offset, int8_t *input1, float input1Scale, int32_t input1Offset,
            int8_t *output, float destScale, int32_t destOffset, unsigned_t size);

int elemadd_relu(int8_t *input0, float input0Scale, int32_t input0Offset, int8_t *input1, float input1Scale, int32_t input1Offset,
            int8_t *output, float destScale, int32_t destOffset, unsigned_t size);

int elemadd_scale(int8_t *input0, int8_t *input1, int8_t *output, float destScale, int32_t size);

int elemsub(int8_t *input0, float input0Scale, int32_t input0Offset, int8_t *input1, float input1Scale, int32_t input1Offset,
            int8_t *output, float destScale, int32_t destOffset, unsigned_t size);

int elemsub_f32(int8_t *input0, float input0Scale, int32_t input0Offset, int8_t *input1, float input1Scale, int32_t input1Offset,
            int8_t *output, float destScale, int32_t destOffset, unsigned_t size);


int elemdiv(int8_t *input0, float input0Scale, int32_t input0Offset, int8_t *input1, float input1Scale, int32_t input1Offset,
            int8_t *output, float destScale, int32_t destOffset, unsigned_t size);

int elemdiv_f32(int8_t *input0, float input0Scale, int32_t input0Offset, int8_t *input1, float input1Scale, int32_t input1Offset,
            int8_t *output, float destScale, int32_t destOffset, unsigned_t size);

int splat(int8_t *input, unsigned_t size, int8_t value);

int elemmax(int8_t *input0, float input0Scale, int32_t input0Offset, int8_t *input1, float input1Scale, int32_t input1Offset,
             int8_t *output, float destScale, int32_t destOffset, unsigned_t size);

int softmax(int8_t *input, int8_t *output, dim_t inDim0, dim_t inDim1,
            dim_t outDim0, dim_t outDim1);

void debugprint(int8_t *input, unsigned_t size, std::string filename, bool isInt8 );

void transpose_nhwc2vtaio(int8_t* input, int8_t* output, int N, int H, int W, int C);

void transpose_nhwc2vtak(int8_t* input, int8_t* output, int KN, int KH, int KW, int C);

void transpose_vtaio2nhwc(int8_t* input, int8_t* output, int N, int H, int W, int C);

void transpose_vtak2nhwc(int8_t* input, int8_t* output, int KN, int KH, int KW, int C);

void transpose_nhwc2vtaio_pack(int8_t* input, int8_t* output, int N, int H, int W, int C);

void transpose_nhwc2vtaio_pack_zerofill(int8_t* input, int8_t* output, int N, int H, int W, int C);

void transpose_nhwc2vtaio_zerofill(int8_t* input, int8_t* output, int N, int H, int W, int C);
//#### JO added (2026-11-02): quantize + per-channel table + NHWC/NCHW -> zero-filled EVTA layout (CPUBundle_generic.cpp)
void preprocess_nhwc2vtaio_zerofill(const float *input, int8_t *output, int N, int H, int W, int C, int CO, int nchw,
                                    float scale, int32_t offset, const int8_t *lut);
//#### JO added (2026-11-02): packed input of a first conv with few channels (CPUBundle_generic.cpp, VTASave.cpp -vta-conv0-pack)
//#### JO changed (2026-11-07): + padEnd, the bottom / right pad (-1: pad = top / left; TF SAME stride 2: 0 and 1)
void conv0pack_nhwc2vtaio(const int8_t *input, int8_t *output, int N, int H, int W, int C, int KH, int KW, int stride,
                          int pad, int CO, int padEnd = -1);
void preprocess_conv0pack(const float *input, int8_t *output, int N, int H, int W, int C, int nchw, float scale,
                          int32_t offset, const int8_t *lut, int KH, int KW, int stride, int pad, int CO,
                          int padEnd = -1);
void preprocess_conv0pack_rows(const float *input, int8_t *output, int N, int H, int W, int C, int nchw, float scale,
                               int32_t offset, const int8_t *lut, int KH, int KW, int stride, int pad, int CO, int q0,
                               int q1, int padEnd = -1);
//#### JO added (2026-11-05): preprocess_conv0pack into the VTA buffer buf (a VTABufferAlloc handle), flushed for the EVTA,
//#### for the EVTA conv that reads buf next (VTASave.cpp). vtalib-zcu102-systolic writes the packed rows while that conv
//#### runs (VTABundle.cpp InputStream); vta/vtalib writes them at once.
//#### JO added (2026-11-06): upper clip of the relu of the following convs (ReLU6: round(6 / output scale)), 127 = plain relu
void VTASetActivationClip(int hi);
//#### JO added (2026-11-11, ALU extension): int8 -> int8 table on the output of the plain EVTA convs (nullptr = off;
//#### an "_alux" bitstream, or the simulator); see VTABundle.cpp
//#### JO changed (2026-11-12): T indexed by the byte (T[(uint8_t) v]); on the CPU after the conv without the extended
//#### ALU (VTAHasAluExt). VTA_OUTPUT_LUT tells a bundle that this vtalib has it (vta/vtalib does not).
#define VTA_OUTPUT_LUT 1
void VTASetOutputLut(const int8_t *T);
bool VTAHasAluExt();
//#### JO added (2026-11-12, ALU extension): the integer layer norm (nestc_cpu::layerNormI8, NESTC_LN_INT=1) as an EVTA ALU
//#### program; 0 = not run (no extended ALU, unsupported shape): the caller runs the CPU function. See VTABundle.cpp
int layernorm_evta(int8_t *out, const int8_t *x, int rows, int cols, const float *w, const int8_t *b, float a2b, float a2d,
                   void *vtaCmdH, uint32_t idEVTA = 0);
void preprocess_conv0pack_stream(const float *input, void *buf, int N, int H, int W, int C, int nchw, float scale,
                                 int32_t offset, const int8_t *lut, int KH, int KW, int stride, int pad, int CO,
                                 int padEnd = -1);
#endif //VTALIB_EXAMPLE_VTABUNDLE_H
