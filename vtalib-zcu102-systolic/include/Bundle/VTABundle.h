#ifndef VTALIB_EXAMPLE_VTABUNDLE_H
#define VTALIB_EXAMPLE_VTABUNDLE_H

#define dim_t uint64_t
#define unsigned_t uint32_t

#include <string>

void xlnk_reset();

int elemsign(int8_t* input, int8_t *output, unsigned_t size);
int convolution(int8_t* input, int8_t *kernel, int8_t *output, int N, int H, int W, int C, int KN, int KH, int KW, int pad_size,
                int stride_size, bool doRelu, bool doBias, uint8_t shift, int out_h, int out_w, int tile_h=14, int tile_w=14, uint32_t idEVTA = 0);

int convolution_wo_tr(int8_t* input, int8_t *kernel, int32_t *bias, int8_t *output, int N, int H, int W, int C, int KN, int KH, int KW, int pad_size,
                       int stride_size, bool doRelu, bool doBias, uint8_t shift, int out_h, int out_w, void* vtaCmdH, int nVirtualThread = 1, int tileHSize = 14, int tileWSize = 14, uint32_t idEVTA = 0);

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
#endif //VTALIB_EXAMPLE_VTABUNDLE_H
