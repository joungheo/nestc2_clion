#define MIN_W_SIZE 5
#define MIN_H_SIZE 5
#define OUT_TILE_SIZE_W 14
#define OUT_TILE_SIZE_H 14
#define NTHREAD 1
#include <iostream>
#include <cstring>
#include <cstdint>
#include <stdlib.h>
#include <unistd.h>
#include <assert.h>
#include <vta/runtime.h>

#include <time.h>
#include <vector>
#include <map>

using namespace std;

namespace sim_test_conv21 {
#define EVTA_ID 0

int getTileH(int H, int outH){
  int tileH = 0;
  int h = OUT_TILE_SIZE_H;
  while(H>=h){
    tileH++;
    h+= OUT_TILE_SIZE_H;
  }
  return tileH;
}


int getTileW(int W, int outW){
  int tileW = 0;
  int w = OUT_TILE_SIZE_W;
  while(W>=w){
    tileW++;
    w+= OUT_TILE_SIZE_W;
  }
  return tileW;
}


struct ResetAcc {
  uint32_t base_;
  uint32_t size_;
  bool operator==(ResetAcc &op) {
    if (base_ == op.base_ && size_ == op.size_)
      return true;
    return false;
  }
};

int reset_acc(void *param, VTACommandHandle cmdH) {
  ResetAcc *p = (ResetAcc *) (param);
  VTAUopLoopBegin(cmdH, p->size_, 1, 0, 0);
  VTAUopPush(cmdH, 0, 1, p->base_, 0, 0, 0, 0, 0);
  VTAUopLoopEnd(cmdH);
  return 0;
}

struct gemmop_mxn {
  uint32_t tile_size_h_;
  uint32_t tile_size_w_;
  uint32_t in_tile_size_h_;
  uint32_t in_tile_size_w_;
  uint32_t src_factor_;
  uint32_t kernel_h_;
  uint32_t kernel_w_;
  uint32_t stride_h_;
  uint32_t stride_w_;
  uint32_t n_kernel_;
  uint32_t input_base_addr_;
  uint32_t weight_base_addr_;
  uint32_t output_base_addr_;
  bool operator==(gemmop_mxn &op){
    if(tile_size_h_==op.tile_size_h_ &&
       tile_size_w_ == op.tile_size_w_ &&
       in_tile_size_h_ == op.in_tile_size_h_ &&
       in_tile_size_w_ == op.in_tile_size_w_ &&
       src_factor_ == op.src_factor_ &&
       kernel_h_ == op.kernel_h_ &&
       kernel_w_ == op.kernel_w_ &&
       stride_h_ == op.stride_h_ &&
       stride_w_ == op.stride_w_ &&
       n_kernel_ == op.n_kernel_ &&
       input_base_addr_ == op.input_base_addr_ &&
       weight_base_addr_ == op.weight_base_addr_ &&
       output_base_addr_ == op.output_base_addr_)
      return true;
    return false;
  }

};

int gemmop_mxn2(void *param,  VTACommandHandle cmdH) {

  struct gemmop_mxn *ctx = (struct gemmop_mxn *) (param);
  VTAUopLoopBegin(cmdH, ctx->tile_size_w_, 1, ctx->stride_w_, 0);
  VTAUopLoopBegin(cmdH, ctx->kernel_w_, 0, 1, 1);

  for (int dx = 0; dx < ctx->kernel_h_; dx++) {
    for (int kernelIdx = 0; kernelIdx < ctx->n_kernel_; kernelIdx++) {
      for (int inTileHIdx = 0; inTileHIdx < ctx->tile_size_h_; inTileHIdx++) {
        auto outTileHIdx = inTileHIdx + (kernelIdx * ctx->tile_size_h_);
        VTAUopPush(cmdH, 0, 0, outTileHIdx * ctx->tile_size_w_,
                   (inTileHIdx * ctx->stride_h_ + dx) * (ctx->in_tile_size_w_), dx * ctx->kernel_w_ + kernelIdx * (ctx->kernel_w_ * ctx->kernel_h_), 0, 0,
                   0);
      }
    }
  }
  VTAUopLoopEnd(cmdH);
  VTAUopLoopEnd(cmdH);

  return 0;
}

int gemmop_mxn(void *param,  VTACommandHandle cmdH) {

  struct gemmop_mxn *ctx = (struct gemmop_mxn *) (param);
  VTAUopLoopBegin(cmdH, ctx->tile_size_w_, 1, ctx->stride_w_, 0);
  VTAUopLoopBegin(cmdH, ctx->kernel_w_, 0, 1, 1);

  for (int dx = 0; dx < ctx->kernel_h_; dx++) {
    for (int kernelIdx = 0; kernelIdx < ctx->n_kernel_; kernelIdx++) {
      for (int inTileHIdx = 0; inTileHIdx < ctx->tile_size_h_; inTileHIdx++) {
        auto outTileHIdx = inTileHIdx + (kernelIdx * ctx->tile_size_h_);
        VTAUopPush(cmdH, 0, 0, ctx->output_base_addr_ + outTileHIdx * ctx->tile_size_w_,
                   ctx->input_base_addr_ + (inTileHIdx * ctx->stride_h_ + dx) * (ctx->in_tile_size_w_),
                   ctx->weight_base_addr_ + dx * ctx->kernel_w_ + kernelIdx * (ctx->kernel_w_ * ctx->kernel_h_), 0, 0,
                   0);
      }
    }
  }

  VTAUopLoopEnd(cmdH);
  VTAUopLoopEnd(cmdH);

  return 0;
}
// alu func
struct ShiftContext {
  uint32_t base_;
  uint32_t size_;
  uint8_t shift_;
  bool operator==(ShiftContext &op){
    if(base_==op.base_ &&
       size_ == op.size_ &&
       shift_ == op.shift_)
      return true;
    return false;
  }
};
int alu_shr(void *param,  VTACommandHandle cmdH) {
  ShiftContext *p = (ShiftContext *) (param);
  VTAUopLoopBegin(cmdH, p->size_, 1, 1, 0);
  VTAUopPush(cmdH, 1, 0, p->base_, p->base_, 0, 3, 1, p->shift_);
  VTAUopLoopEnd(cmdH);
  return 0;
}

struct AddContext {
  uint32_t dst_;
  uint32_t src_;
  uint32_t size_;
  uint32_t iter_;
  bool operator==(AddContext &op){
    if(dst_==op.dst_ &&
       src_ == op.src_ &&
       size_ == op.size_ &&
       iter_ == op.iter_)
      return true;
    return false;
  }
};

int alu_add(void *param,  VTACommandHandle cmdH) {
  AddContext *p = (AddContext *) (param);
  VTAUopLoopBegin(cmdH, p->iter_, p->size_, 1, 0);
  VTAUopLoopBegin(cmdH, p->size_, 1, 0, 0);

  VTAUopPush(cmdH, 1, 0, p->dst_, p->src_, 0, 2, 0, 0);

  VTAUopLoopEnd(cmdH);
  VTAUopLoopEnd(cmdH);
  return 0;
}
int alu_max(void *param,  VTACommandHandle cmdH) {
  ResetAcc *p = (ResetAcc *) (param);
  VTAUopLoopBegin(cmdH, p->size_, 1, 1, 0);
  VTAUopPush(cmdH, 1, 0, p->base_, p->base_, 0, 1, 1, -128);
  VTAUopLoopEnd(cmdH);
  return 0;
}

int alu_relu(void *param,  VTACommandHandle cmdH) {
  ResetAcc *p = (ResetAcc *) (param);
  VTAUopLoopBegin(cmdH, p->size_, 1, 1, 0);
  VTAUopPush(cmdH, 1, 0, p->base_, p->base_, 0, 1, 1, 0);
  VTAUopLoopEnd(cmdH);
  return 0;
}

int alu_min(void *param,  VTACommandHandle cmdH) {
  ResetAcc *p = (ResetAcc *) (param);
  VTAUopLoopBegin(cmdH, p->size_, 1, 1, 0);
  VTAUopPush(cmdH, 1, 0, p->base_, p->base_, 0, 0, 1, 127);
  VTAUopLoopEnd(cmdH);
  return 0;
}


struct GemmUOpHandle{
  struct gemmop_mxn strGemm;
  void *uopHandle_gemm[2];
  GemmUOpHandle(){
    uopHandle_gemm[0] = nullptr;
  }
};

std::vector<GemmUOpHandle*> vGemmUOpHandle[4];



struct AddUopHandle{
  struct AddContext strAdd;
  void *uopHandle_add[2];
  AddUopHandle(){
    uopHandle_add[0] = nullptr;
  }
};

std::vector<AddUopHandle*> vAddUOpHandle[4];

struct ResetUopHandle{
  struct ResetAcc strReset;
  void *uopHandle_reset[2];
  ResetUopHandle(){
    uopHandle_reset[0] = nullptr;
  }
};

std::vector<ResetUopHandle*> vResetUopHandle[4];

std::vector<ResetUopHandle*> vReluUopHandle[4];
std::vector<ResetUopHandle*> vMaxUopHandle[4];
std::vector<ResetUopHandle*> vMinUopHandle[4];


struct ShiftUopHandle{
  struct ShiftContext strShift;
  void *uopHandle_shift[2];
  ShiftUopHandle(){
    uopHandle_shift[0] = nullptr;
  }
};

std::vector<ShiftUopHandle*> vShiftUopHandle[4];



void conv_kernel(uint32_t sizeOutTileH,
                 uint32_t sizeOutTileW,
                 uint32_t pad_size,
                 uint32_t stride_size,
                 uint32_t x_pad_before,
                 uint32_t x_pad_after,
                 uint32_t y_pad_before,
                 uint32_t y_pad_after,
                 uint32_t outputWOffset,
                 uint32_t outputHOffset,
                 uint32_t sizeInTileH,
                 uint32_t sizeInTileW,
                 uint32_t numInputChannel,
                 uint32_t inputHStride,
                 uint32_t inputWStride,
                 void *vta_input_buf,
                 uint32_t KN,
                 uint32_t KH,
                 uint32_t KW,
                 void *vta_filter_buf,
                 uint32_t out_h,
                 uint32_t out_w,
                 void *vta_output_buf,
                 bool relu,
                 uint8_t shift,
                 bool bias,
                 void *vta_bias_buf,
                 VTACommandHandle& vtaCmdH,
                 uint32_t idEVTA = 0,
                 uint32_t nVirtualThread = 1) {

  uint32_t resizedOutTileH = (sizeInTileH-KH) / stride_size + 1;
  uint32_t resizedOutTileW = (sizeInTileW-KW) / stride_size + 1;
  uint32_t resizedInTileH =  (resizedOutTileH - 1) * stride_size + KH;
  uint32_t resizedInTileW =  (resizedOutTileW - 1) * stride_size + KW;

  uint32_t ACC_MEM_ENTRY = 2048;
  uint32_t INP_MEM_ENTRY = 2048;
  uint32_t WGT_MEM_ENTRY = 1024;

  uint32_t nFilterinLoop = KN / nVirtualThread / 16 ;
  nFilterinLoop = min(nFilterinLoop, ACC_MEM_ENTRY/ nVirtualThread / resizedOutTileH / resizedOutTileW);
  for(uint32_t i = 31; i>=0; i--){
    if(nFilterinLoop & 0x1 <<i) {
      nFilterinLoop = 0x1 <<i;
      break;
    }
  }

  uint32_t outDummyH = 0;
  uint32_t inDummyH = 0;
  uint32_t outDummyW = 0;
  uint32_t inDummyW = 0;
  for (uint32_t filterIdx = 0; filterIdx < KN / nFilterinLoop / nVirtualThread / 16; filterIdx++) {
    // for (uint32_t threadIdx = 0; threadIdx < nVirtualThread; threadIdx++) {



    if (resizedOutTileH < MIN_H_SIZE) {
      outDummyH = MIN_H_SIZE - resizedOutTileH;
      resizedOutTileH = MIN_H_SIZE;
      resizedInTileH = (resizedOutTileH - 1) * stride_size + KH;
      inDummyH = resizedInTileH - sizeInTileH;
    }

    if (resizedOutTileW < MIN_W_SIZE) {
      outDummyW = MIN_W_SIZE - resizedOutTileW;
      resizedOutTileW = MIN_W_SIZE;
      resizedInTileW = (resizedOutTileW - 1) * stride_size + KW;
      inDummyW = resizedInTileW - sizeInTileW;
    }

    for (uint32_t threadIdx = 0; threadIdx < nVirtualThread; threadIdx++) {
      // acc buffer reset

      uint32_t outputSramBaseAddr = threadIdx * ACC_MEM_ENTRY / nVirtualThread;
      uint32_t inputSramBaseAddr = threadIdx * (INP_MEM_ENTRY / nVirtualThread);
      uint32_t weightSramBaseAddr =
          threadIdx * (WGT_MEM_ENTRY / nVirtualThread);
      ResetAcc reset;
      reset.base_ = outputSramBaseAddr;
      reset.size_ = resizedOutTileH * resizedOutTileW * nFilterinLoop;

      void **uopHandle_reset;

      bool isFound = false;
      for(auto resetUOpHandle : vResetUopHandle[idEVTA]){
        if(resetUOpHandle->strReset==reset){
          uopHandle_reset = resetUOpHandle->uopHandle_reset;
          isFound = true;
          break;
        }
      }
      if(!isFound){
        auto newHandle = new ResetUopHandle();
        newHandle->strReset = reset;
        uopHandle_reset = newHandle->uopHandle_reset;
        vResetUopHandle[idEVTA].push_back(newHandle);
      }

      VTAPushGEMMOp(vtaCmdH, uopHandle_reset, &reset_acc, &reset, 0);
      VTADepPush(vtaCmdH, 2, 1);
    }

    uint32_t inputWOffset = (int)(outputWOffset * stride_size - pad_size) < 0
                            ? 0
                            : outputWOffset * stride_size - pad_size;
    uint32_t inputHOffset = (int)(outputHOffset * stride_size - pad_size) < 0
                            ? 0
                            : outputHOffset * stride_size - pad_size;

    for (uint32_t channelIdx = 0; channelIdx < numInputChannel / 16;
         channelIdx++) {
      uint32_t src_offset = channelIdx * inputHStride * inputWStride +
                            inputHOffset * inputWStride + inputWOffset;

      uint32_t y_size =
          (inputHStride - inputHOffset + y_pad_before) < resizedInTileH
          ? inputHStride - inputHOffset
          : resizedInTileH - y_pad_before;
      uint32_t x_size =
          (inputWStride - inputWOffset + x_pad_before) < resizedInTileW
          ? inputWStride - inputWOffset
          : resizedInTileW - x_pad_before;

      for (uint32_t threadIdx = 0; threadIdx < nVirtualThread; threadIdx++) {
        uint32_t filter_offset =
            filterIdx * nVirtualThread * nFilterinLoop * (numInputChannel / 16) * KH * KW +
            threadIdx * nFilterinLoop * (numInputChannel / 16) * KH * KW +
            channelIdx * KH * KW;
        uint32_t inputSramBaseAddr =
            threadIdx * (INP_MEM_ENTRY / nVirtualThread);
        uint32_t weightSramBaseAddr =
            threadIdx * (WGT_MEM_ENTRY / nVirtualThread);
        VTADepPop(vtaCmdH, 2, 1);
        VTALoadBuffer2D(vtaCmdH, vta_input_buf, src_offset, x_size, y_size,
                        inputWStride, x_pad_before, y_pad_before,
                        resizedInTileW - x_pad_before - x_size,
                        resizedInTileH - y_pad_before - y_size,
                        inputSramBaseAddr, // index
                        2);                // type

        VTALoadBuffer2D(vtaCmdH, vta_filter_buf, filter_offset, KH * KW,
                        nFilterinLoop, KH * KW * numInputChannel / 16, 0, 0, 0,
                        0, weightSramBaseAddr, 1);
        VTADepPush(vtaCmdH, 1, 2);
      }
      for (uint32_t threadIdx = 0; threadIdx < nVirtualThread; threadIdx++) {
        uint32_t outputSramBaseAddr =
            threadIdx * ACC_MEM_ENTRY / nVirtualThread;
        uint32_t inputSramBaseAddr =
            threadIdx * (INP_MEM_ENTRY / nVirtualThread);
        uint32_t weightSramBaseAddr =
            threadIdx * (WGT_MEM_ENTRY / nVirtualThread);
        VTADepPop(vtaCmdH, 1, 2);
        struct gemmop_mxn gemm_ctx;
        gemm_ctx.tile_size_h_ = resizedOutTileH;
        gemm_ctx.tile_size_w_ = resizedOutTileW;
        gemm_ctx.src_factor_ = resizedInTileW;
        gemm_ctx.in_tile_size_h_ = resizedInTileH;
        gemm_ctx.in_tile_size_w_ = resizedInTileW;
        gemm_ctx.kernel_h_ = KH;
        gemm_ctx.kernel_w_ = KW;
        gemm_ctx.stride_h_ = stride_size;
        gemm_ctx.stride_w_ = stride_size;
        gemm_ctx.n_kernel_ = nFilterinLoop;
        gemm_ctx.input_base_addr_ = inputSramBaseAddr;
        gemm_ctx.output_base_addr_ = outputSramBaseAddr;
        gemm_ctx.weight_base_addr_ = weightSramBaseAddr;

        void **uopHandle_gemm;

        bool isFound = false;
        for(auto gemmUOpHandle : vGemmUOpHandle[idEVTA]){
          if(gemmUOpHandle->strGemm==gemm_ctx){
            uopHandle_gemm = gemmUOpHandle->uopHandle_gemm;
            isFound = true;
            break;
          }
        }
        if(!isFound){
          auto newHandle = new GemmUOpHandle();
          newHandle->strGemm = gemm_ctx;
          uopHandle_gemm = newHandle->uopHandle_gemm;
          vGemmUOpHandle[idEVTA].push_back(newHandle);
        }


        VTAPushGEMMOp(vtaCmdH, uopHandle_gemm, &gemmop_mxn, &gemm_ctx, 0);
        VTADepPush(vtaCmdH, 2, 1);
      }
    }
    for (uint32_t threadIdx = 0; threadIdx < nVirtualThread; threadIdx++) {
      VTADepPop(vtaCmdH, 2, 1);
    }
    for (uint32_t threadIdx = 0; threadIdx < nVirtualThread; threadIdx++) {

      uint32_t outputSramBaseAddr = threadIdx * ACC_MEM_ENTRY / nVirtualThread;
      uint32_t inputSramBaseAddr = threadIdx * (INP_MEM_ENTRY / nVirtualThread);
      uint32_t weightSramBaseAddr =
          threadIdx * (WGT_MEM_ENTRY / nVirtualThread);
      if (bias) {
        uint32_t sram_offset = outputSramBaseAddr + resizedOutTileH *
                                                    resizedOutTileW *
                                                    nFilterinLoop;
        VTALoadBuffer2D(vtaCmdH, (int32_t *)vta_bias_buf,
                        filterIdx * nVirtualThread * nFilterinLoop + threadIdx * nFilterinLoop,
                        16 * nFilterinLoop, 1, 16 * nFilterinLoop, 0, 0, 0, 0,
                        sram_offset, // index
                        3);          // type

        AddContext add_ctx;
        add_ctx.dst_ = outputSramBaseAddr;
        add_ctx.size_ = resizedOutTileH * resizedOutTileW;
        add_ctx.src_ = sram_offset;
        add_ctx.iter_ = nFilterinLoop;


        void **uopHandle_add;

        bool isFound = false;
        for(auto addUOpHandle : vAddUOpHandle[idEVTA]){
          if(addUOpHandle->strAdd==add_ctx){
            uopHandle_add = addUOpHandle->uopHandle_add;
            isFound = true;
            break;
          }
        }
        if(!isFound){
          auto newHandle = new AddUopHandle();
          newHandle->strAdd = add_ctx;
          uopHandle_add = newHandle->uopHandle_add;
          vAddUOpHandle[idEVTA].push_back(newHandle);
        }

        VTAPushALUOp(vtaCmdH, uopHandle_add, &alu_add, &add_ctx, 0);
      }

      ShiftContext shift_ctx;
      shift_ctx.base_ = outputSramBaseAddr;
      shift_ctx.size_ = resizedOutTileH * resizedOutTileW * nFilterinLoop;
      shift_ctx.shift_ = shift;

      {
        void **uopHandle_shift;

        bool isFound = false;
        for (auto shiftUOpHandle : vShiftUopHandle[idEVTA]) {
          if (shiftUOpHandle->strShift == shift_ctx) {
            uopHandle_shift = shiftUOpHandle->uopHandle_shift;
            isFound = true;
            break;
          }
        }
        if (!isFound) {
          auto newHandle = new ShiftUopHandle();
          newHandle->strShift = shift_ctx;
          uopHandle_shift = newHandle->uopHandle_shift;
          vShiftUopHandle[idEVTA].push_back(newHandle);
        }

        VTAPushALUOp(vtaCmdH, uopHandle_shift, &alu_shr, &shift_ctx, 0);
      }

      {
        if (!relu) {
          ResetAcc alu_ctx;
          alu_ctx.base_ = outputSramBaseAddr;
          alu_ctx.size_ = resizedOutTileH * resizedOutTileW * nFilterinLoop;
          void **uopHandle_reset;

          bool isFound = false;
          for (auto resetUOpHandle : vMaxUopHandle[idEVTA]) {
            if (resetUOpHandle->strReset == alu_ctx) {
              uopHandle_reset = resetUOpHandle->uopHandle_reset;
              isFound = true;
              break;
            }
          }
          if (!isFound) {
            auto newHandle = new ResetUopHandle();
            newHandle->strReset = alu_ctx;
            uopHandle_reset = newHandle->uopHandle_reset;
            vMaxUopHandle[idEVTA].push_back(newHandle);
          }
          VTAPushALUOp(vtaCmdH, uopHandle_reset, &alu_max, &alu_ctx, 0);
        } else {
          ResetAcc alu_ctx;
          alu_ctx.base_ = outputSramBaseAddr;
          alu_ctx.size_ = resizedOutTileH * resizedOutTileW * nFilterinLoop;
          void **uopHandle_reset;

          bool isFound = false;
          for (auto resetUOpHandle : vReluUopHandle[idEVTA]) {
            if (resetUOpHandle->strReset == alu_ctx) {
              uopHandle_reset = resetUOpHandle->uopHandle_reset;
              isFound = true;
              break;
            }
          }
          if (!isFound) {
            auto newHandle = new ResetUopHandle();
            newHandle->strReset = alu_ctx;
            uopHandle_reset = newHandle->uopHandle_reset;
            vReluUopHandle[idEVTA].push_back(newHandle);
          }
          VTAPushALUOp(vtaCmdH, uopHandle_reset, &alu_relu, &alu_ctx, 0);
        }
      }
      {
        ResetAcc alu_ctx;
        alu_ctx.base_ = outputSramBaseAddr;
        alu_ctx.size_ = resizedOutTileH * resizedOutTileW * nFilterinLoop;
        void **uopHandle_reset;

        bool isFound = false;
        for (auto resetUOpHandle : vMinUopHandle[idEVTA]) {
          if (resetUOpHandle->strReset == alu_ctx) {
            uopHandle_reset = resetUOpHandle->uopHandle_reset;
            isFound = true;
            break;
          }
        }
        if (!isFound) {
          auto newHandle = new ResetUopHandle();
          newHandle->strReset = alu_ctx;
          uopHandle_reset = newHandle->uopHandle_reset;
          vMinUopHandle[idEVTA].push_back(newHandle);
        }
        VTAPushALUOp(vtaCmdH, uopHandle_reset, &alu_min, &alu_ctx, 0);
      }
      VTADepPush(vtaCmdH, 2, 3);
    }

    for (uint32_t threadIdx = 0; threadIdx < nVirtualThread; threadIdx++) {
      uint32_t outputSramBaseAddr = threadIdx * ACC_MEM_ENTRY / nVirtualThread;
      VTADepPop(vtaCmdH, 2, 3);

      if (outDummyW == 0) {
        for (uint32_t fIdx = 0; fIdx < nFilterinLoop; fIdx++) {

          VTAStoreBuffer2D(
              vtaCmdH,
              outputSramBaseAddr + fIdx * resizedOutTileH * resizedOutTileW, 4,
              vta_output_buf,
              (filterIdx * nVirtualThread + threadIdx) * nFilterinLoop * out_w *
              out_h +
              outputHOffset * out_w + outputWOffset + fIdx * out_w * out_h,
              sizeOutTileW, sizeOutTileH, out_w);
        }
      } else {
        for (int fIdx = 0; fIdx < nFilterinLoop; fIdx++) {
          for (int hIdx = 0; hIdx < sizeOutTileH; hIdx++) {
            VTAStoreBuffer2D(vtaCmdH, outputSramBaseAddr + (hIdx+fIdx*sizeOutTileH) * resizedOutTileW + fIdx * outDummyH * resizedOutTileH,
                             4, vta_output_buf,
                             ((filterIdx+fIdx) * nVirtualThread + threadIdx) *
                                 out_w * out_h +
                                 (outputHOffset + hIdx) * out_w + outputWOffset,
                             sizeOutTileW, 1, out_w);
          }
        }
      }
      VTADepPush(vtaCmdH, 3, 2);
      VTADepPop(vtaCmdH, 3, 2);
    }
  }
}

}

using namespace sim_test_conv21;
int conv21_test() {
  int is_diff = 0;
  srand(time(NULL));

  //N, H, W, C
  //N == 1
  //group == 1
  struct timeval tv_s, tv_e;

  int N = 1;
  int H = 56;
  int W = 56;
  int C = 64;
  int KN = 64;

  int KH = 3;
  int KW = 3;
  int pad_size = 1;
  int stride_size = 1;
  bool doRelu = false;
  uint8_t shift = 6;
  bool doBias = true;

  // input NCHW
  int8_t in_NCHW[N][C][H][W];

  for (int n = 0; n < N; n++) {
    for (int c = 0; c < C; c++) {
      for (int h = 0; h < H; h++) {
        for (int w = 0; w < W; w++) {
          in_NCHW[n][c][h][w] = (int8_t) (rand() % 5);
        }
      }
    }
  }

  //transpose input NCHW -> NHWC
  int8_t in_NHWC[N][H][W][C];
  for (int i = 0; i < N; i++) {
    for (int j = 0; j < H; j++) {
      for (int k = 0; k < W; k++) {
        for (int l = 0; l < C; l++) {
          in_NHWC[i][j][k][l] = in_NCHW[i][l][j][k];
        }
      }
    }
  }


  // reshape input NCHW -> N{C//16}16HW
  assert(C % 16 == 0);
  int8_t in_NC_16HW[N][C / 16][16][H][W];
  for (int i = 0; i < N; i++) {
    for (int j = 0; j < C / 16; j++) {
      for (int k = 0; k < 16; k++) {
        for (int l = 0; l < H; l++) {
          for (int o = 0; o < W; o++) {
            in_NC_16HW[i][j][k][l][o] =
                in_NCHW[i][j * 16 + k][l][o];
          }
        }
      }
    }
  }

  //transpose input N{C//16}16HW -> N{C//16}HW16
  int8_t in_NC_HW16[N][C / 16][H][W][16];
  for (int i = 0; i < N; i++) {
    for (int j = 0; j < C / 16; j++) {
      for (int k = 0; k < H; k++) {
        for (int l = 0; l < W; l++) {
          for (int o = 0; o < 16; o++) {
            in_NC_HW16[i][j][k][l][o] =
                in_NC_16HW[i][j][o][k][l];
          }
        }
      }
    }
  }




  // filter NHWC
  int8_t filter_NHWC[KN][KH][KW][C];

  for (int i = 0; i < KN; i++) {
    for (int j = 0; j < KH; j++) {
      for (int k = 0; k < KW; k++) {
        for (int l = 0; l < C; l++) {
          filter_NHWC[i][j][k][l] = (int8_t) (rand() % 20);
          if (rand() % 2 == 0) filter_NHWC[i][j][k][l] = filter_NHWC[i][j][k][l] * -1;
        }
      }
    }
  }


  // reshape filter NHWC -> {N//16}16HW{C//16}16
  assert(KN % 16 == 0);

  int8_t filter_N_16HWC_16[KN / 16][16][KH][KW][C / 16][16];
  for (int i = 0; i < KN / 16; i++) {
    for (int j = 0; j < 16; j++) {
      for (int k = 0; k < KH; k++) {
        for (int l = 0; l < KW; l++) {
          for (int o = 0; o < C / 16; o++) {
            for (int p = 0; p < 16; p++) {
              filter_N_16HWC_16[i][j][k][l][o][p] =
                  filter_NHWC[i * 16 + j][k][l][o * 16 + p];
            }
          }
        }
      }
    }
  }

  // transpose filter {N//16}16HW{C//16}16 =>  {N//16}{C//16}HW1616
  int8_t filter_N_C_HW1616[KN / 16][C / 16][KH][KW][16][16];
  for (int i = 0; i < KN / 16; i++) {
    for (int j = 0; j < C / 16; j++) {
      for (int k = 0; k < KH; k++) {
        for (int l = 0; l < KW; l++) {
          for (int o = 0; o < 16; o++) {
            for (int p = 0; p < 16; p++) {
              filter_N_C_HW1616[i][j][k][l][o][p] =
                  filter_N_16HWC_16[i][o][k][l][j][p];
            }
          }
        }
      }
    }
  }


  int32_t bias[KN];

  for (int i = 0; i < KN; i++) {
    bias[i] = (int8_t) (rand() % 30000);
    if (rand() % 2 == 0) bias[i] = bias[i] * -1;
    //bias[i] = (int8_t)(1);
    bias[i] = bias[i] << shift;
  }


  int out_h = (H + 2 * pad_size - KH) / stride_size + 1;
  int out_w = (W + 2 * pad_size - KW) / stride_size + 1;


  int8_t *out_VTA = (int8_t *) malloc(out_h * out_w * KN);


  // VTA DRAM alloc
  // vta's input buffer, as the same size of in_NC_HW16[N][C/16][H][W][16];
  void *vta_input_buf = VTABufferAlloc(N * C * H * W);
  // vta's filter buffer, as the same size of filter_N_C_HW1616[KN/16][C/16][KH][KW][16][16];
  void *vta_filter_buf = VTABufferAlloc(KN * KH * KW * C);
  void *vta_output_buf = VTABufferAlloc(out_h * out_w * KN);
  void *vta_bias_buf = VTABufferAlloc(KN*4);

  // VTA Buffer copy
  VTABufferCopy(in_NC_HW16, 0, vta_input_buf, 0, N * C * H * W, 1);
  VTABufferCopy(filter_N_C_HW1616, 0, vta_filter_buf, 0, KN * KH * KW * C, 1);
  VTABufferCopy(bias, 0, vta_bias_buf, 0, KN * 4, 1);
  uint32_t nTileW, nTileH;
  uint32_t sizeOutTileW = OUT_TILE_SIZE_W;
  uint32_t sizeOutTileH = OUT_TILE_SIZE_H;

  uint32_t sizeInTileW = (sizeOutTileW - 1) * stride_size + KW;
  uint32_t sizeInTileH = (sizeOutTileH - 1) * stride_size + KH;

  int isRemainTileW = (out_w % sizeOutTileW) ? 1 : 0;
  int isRemainTileH = (out_h % sizeOutTileH) ? 1 : 0;
  nTileW = out_w / sizeOutTileW + isRemainTileW;
  nTileH = out_h / sizeOutTileH + isRemainTileH;



  VTACommandHandle vtaCmdH{nullptr};
  vtaCmdH = VTATLSCommandHandle(EVTA_ID);
  VTASetDebugMode(vtaCmdH, VTA_DEBUG_DUMP_INSN | VTA_DEBUG_DUMP_UOP);

  for (int tileH = 0; tileH < nTileH; tileH++) {
    for (int tileW = 0; tileW < nTileW; tileW++) {
      uint32_t x_pad_before = tileW == 0 ? pad_size : 0;
      uint32_t x_pad_after = tileW == (nTileW - 1) ? pad_size : 0;
      uint32_t y_pad_before = tileH == 0 ? pad_size : 0;
      uint32_t y_pad_after = tileH == (nTileH - 1) ? pad_size : 0;
      uint32_t outputWOffset = tileW * sizeOutTileW;
      uint32_t outputHOffset = tileH * sizeOutTileH;

      uint32_t resizedOutTileH = sizeOutTileH;
      uint32_t resizedInTileH = sizeInTileH;
      if (tileH == nTileH-1) {
        resizedOutTileH = out_h - tileH * sizeOutTileH;
        resizedInTileH = (resizedOutTileH -1) * stride_size + KH;
      }

      if ((tileW != nTileW - 1)) {
        conv_kernel(resizedOutTileH,
                    sizeOutTileW,
                    pad_size,
                    stride_size,
                    x_pad_before,
                    x_pad_after,
                    y_pad_before,
                    y_pad_after,
                    outputWOffset,
                    outputHOffset,
                    resizedInTileH,
                    sizeInTileW,
                    C,
                    H,
                    W,
                    vta_input_buf,
                    KN,
                    KH,
                    KW,
                    vta_filter_buf,
                    out_h,
                    out_w,
                    vta_output_buf,
                    doRelu,
                    shift,
                    doBias,
                    vta_bias_buf,
                    vtaCmdH,
                    EVTA_ID,
                    NTHREAD

        );
      } else {
        uint32_t sizeOutTileBoundaryW = out_w - tileW * sizeOutTileW;
        uint32_t sizeInTileBoundaryW = (sizeOutTileBoundaryW - 1) * stride_size + KW ;

        conv_kernel(resizedOutTileH,
                    sizeOutTileBoundaryW,
                    pad_size,
                    stride_size,
                    x_pad_before,
                    x_pad_after,
                    y_pad_before,
                    y_pad_after,
                    outputWOffset,
                    outputHOffset,
                    resizedInTileH,
                    sizeInTileBoundaryW,
                    C,
                    H,
                    W,
                    vta_input_buf,
                    KN,
                    KH,
                    KW,
                    vta_filter_buf,
                    out_h,
                    out_w,
                    vta_output_buf,
                    doRelu,
                    shift,
                    doBias,
                    vta_bias_buf,
                    vtaCmdH,
                    EVTA_ID,
                    NTHREAD
        );
      }


    }
  }
  VTASynchronize(vtaCmdH, 1 << 31);
  VTARuntimeShutdown(EVTA_ID);
  VTABufferCopy(vta_output_buf, 0, out_VTA, 0, out_h * out_w * KN, 2);



  // end case
  VTABufferFree(vta_input_buf);
  VTABufferFree(vta_output_buf);
  VTABufferFree(vta_filter_buf);





  // reference output

  int new_height = H + pad_size * 2;
  int new_width = W + pad_size * 2;

  int8_t newin[N][new_height][new_width][C];
  for (int i = 0; i < N; i++) {
    for (int j = 0; j < new_height; j++) {
      for (int k = 0; k < new_width; k++) {
        for (int l = 0; l < C; l++) {
          newin[i][j][k][l] = 0;
        }

      }
    }
  }

  for (int b = 0; b < N; b++) {
    for (size_t c = 0; c < C; c++) {
      for (int h = 0; h < H; h++) {
        for (int w = 0; w < W; w++) {
          newin[b][pad_size + h][pad_size + w][c] = in_NHWC[b][h][w][c];
        }
      }
    }
  }

  int8_t result[N][out_h][out_w][KN];
  for (int i = 0; i < N; i++) {
    for (int j = 0; j < out_h; j++) {
      for (int k = 0; k < out_w; k++) {
        for (int l = 0; l < KN; l++) {
          result[i][j][k][l] = 0;
        }

      }
    }
  }

  int new_h_idx = 0, new_w_idx = 0;
  for (int b = 0; b < N; b++) {
    for (int fc = 0; fc < KN; fc++) {
      for (int i = 0; i < out_h; i++) {
        for (int j = 0; j < out_w; j++) {

          int32_t total = 0;
          for (int fh = 0; fh < KH; fh++) {
            for (int fw = 0; fw < KW; fw++) {
              for (size_t f = 0; f < C; f++) {

                total = total +
                    newin[b][new_h_idx + fh][new_w_idx + fw][f] * filter_NHWC[fc][fh][fw][f];
              }
            }
          }
          if(doBias) {
            total += bias[fc];
          }
          total = total >> shift;
          if(total > 127) total = 127;
          if(doRelu){
            if(total <0) total = 0;
          } else{
            if(total <-128) total = -128;
          }


          result[b][i][j][fc] = total;
          new_w_idx += stride_size;
        }
        new_h_idx += stride_size;
        new_w_idx = 0;
      }
      new_h_idx = 0;
    }
  }



  for (int b = 0; b < N; b++) {
    for (int f = 0; f < KN; f++) {
      for (int i = 0; i < out_h; i++) {    //h
        for (int j = 0; j < out_w; j++) {    //w
          int out_ch = f / 16;
          int oo = f % 16;

          int8_t v = *(out_VTA + (out_ch * out_h * out_w * 16 + i * out_w * 16 + j * 16 + oo));

          {
            if ((int8_t) v != result[b][i][j][f])
            {
              {
                printf("TILE : (%d, %d), CHW : %u|%u|%u  (%d, %d)\n",
                       getTileH(i, out_h),
                       getTileW(j, out_w),
                       f,
                       i,
                       j,
                       (int8_t) v,
                       result[b][i][j][f]);
                is_diff = -1;
              }
            }
          }
        }
      }
    }
  }


  free(out_VTA);

  printf("TEST COMPLETE\n");

  return is_diff;
}
