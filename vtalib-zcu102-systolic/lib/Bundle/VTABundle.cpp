#include <cstdint>
#include <vector>
#include <fstream>
#include <iostream>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <cassert>
#include "llvm/ADT/ArrayRef.h"

#if(!VTALIB_ONLY)
#if(NESTC_USE_VTASIM)
#include "runtime.h"
#else
#include "vta/vtalib/include/zcu102/vta/runtime.h"
#endif
#include "vta/vtalib/include/Bundle/VTABundle.h"
#else
#if(NESTC_USE_VTASIM)
#include "simulator/vta/runtime.h"
#else
#include "zcu102/vta/runtime.h"
#endif
#include "Bundle/VTABundle.h"
#endif

#define RESET_IOCTL _IOWR('X', 101, unsigned long)

using namespace std;

#define MIN_W_SIZE 7
#define MIN_H_SIZE 7
#define OUT_TILE_SIZE_W 14
#define OUT_TILE_SIZE_H 14

//#define NESTC_EVTA_PROFILE_AUTOTUNE
#ifdef NESTC_EVTA_PROFILE_AUTOTUNE
uint32_t f0 = 0;
uint32_t f1 = 0;
uint32_t f2 = 0;
uint32_t f3 = 0;
uint32_t f4 = 0;
uint32_t f5 = 0;
uint32_t f6 = 0;
uint32_t f7 = 0;
uint32_t f8 = 0;
uint32_t f9 = 0;
uint32_t f10 = 0;
uint32_t f11 = 0;
uint32_t f12 = 0;
uint32_t f13 = 0;
uint32_t f14 = 0;
uint32_t f15 = 0;
uint32_t f16 = 0;
uint32_t f17 = 0;
uint32_t b0 = 0;
#endif



void xlnk_reset() {
#if(!(NESTC_USE_VTASIM==1))
  int xlnkfd = open("/dev/xlnk", O_RDWR | O_CLOEXEC);
  if (xlnkfd < 0) {
    printf("Reset failed - could not open device: %d\n", xlnkfd);
    return;
  }
  if (ioctl(xlnkfd, RESET_IOCTL, 0) < 0) {
    printf("Reset failed - IOCTL failed: %d\n", errno);
  }
  close(xlnkfd);
#endif
}




struct ResetAcc {
//  VTACommandHandle *cmdH;
  uint32_t base_;
  uint32_t size_;
  bool operator==(ResetAcc &op) {
    if (base_ == op.base_ && size_ == op.size_)
      return true;
    return false;
  }
};

int reset_acc(void *param,VTACommandHandle cmdh) {
  ResetAcc *p = (ResetAcc *) (param);
  VTAUopLoopBegin(cmdh, p->size_, 1, 0, 0);
  VTAUopPush(cmdh, 0, 1, p->base_, 0, 0, 0, 0, 0);
  VTAUopLoopEnd(cmdh);
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

//#### JO changed (2026-10-27, systolic GEMM, docs/vta_synth_notes.md §8.4): weight-stationary uop order.
//#### The same weight block is used for all tile_h x tile_w output pixels in a row (was tile_h cycles), so a
//#### systolic GEMM that needs 16 cycles to load a weight block can hide the load. The result is the same
//#### (only the order of the integer accumulation changes); on the current GEMM the time is the same.
//#### Old order: it_out = x, it_in = kx, uops = ky x kernelIdx x y.
//#### New order: it_out = kernelIdx, it_in = kx, uops = ky x y x x (KH x tile_h x tile_w uops).
//#### Uop kernels larger than a quarter of the uop buffer (two virtual threads keep one each) keep the old order.
static int gemmop_mxn_old(struct gemmop_mxn *ctx, VTACommandHandle cmdh) {
  VTAUopLoopBegin(cmdh, ctx->tile_size_w_, 1, ctx->stride_w_, 0);
  VTAUopLoopBegin(cmdh, ctx->kernel_w_, 0, 1, 1);

  for (int dx = 0; dx < ctx->kernel_h_; dx++) {
    for (int kernelIdx = 0; kernelIdx < ctx->n_kernel_; kernelIdx++) {
      for (int inTileHIdx = 0; inTileHIdx < ctx->tile_size_h_; inTileHIdx++) {
        auto outTileHIdx = inTileHIdx + (kernelIdx * ctx->tile_size_h_);
        VTAUopPush(cmdh, 0, 0, ctx->output_base_addr_ + outTileHIdx * ctx->tile_size_w_,
                   ctx->input_base_addr_ + (inTileHIdx * ctx->stride_h_ + dx) * (ctx->in_tile_size_w_),
                   ctx->weight_base_addr_ + dx * ctx->kernel_w_ + kernelIdx * (ctx->kernel_w_ * ctx->kernel_h_), 0, 0,
                   0);
      }
    }
  }

  VTAUopLoopEnd(cmdh);
  VTAUopLoopEnd(cmdh);

  return 0;
}

int gemmop_mxn(void *param,VTACommandHandle cmdh ) {

  struct gemmop_mxn *ctx = (struct gemmop_mxn *) (param);
  if (ctx->kernel_h_ * ctx->tile_size_h_ * ctx->tile_size_w_ > VTA_UOP_BUFF_DEPTH / 4)
    return gemmop_mxn_old(ctx, cmdh);
  VTAUopLoopBegin(cmdh, ctx->n_kernel_, ctx->tile_size_h_ * ctx->tile_size_w_, 0,
                  ctx->kernel_w_ * ctx->kernel_h_);
  VTAUopLoopBegin(cmdh, ctx->kernel_w_, 0, 1, 1);

  for (int dx = 0; dx < ctx->kernel_h_; dx++) {
    for (int inTileHIdx = 0; inTileHIdx < ctx->tile_size_h_; inTileHIdx++) {
      for (int outTileWIdx = 0; outTileWIdx < ctx->tile_size_w_; outTileWIdx++) {
        VTAUopPush(cmdh, 0, 0, ctx->output_base_addr_ + inTileHIdx * ctx->tile_size_w_ + outTileWIdx,
                   ctx->input_base_addr_ + (inTileHIdx * ctx->stride_h_ + dx) * (ctx->in_tile_size_w_) +
                       outTileWIdx * ctx->stride_w_,
                   ctx->weight_base_addr_ + dx * ctx->kernel_w_, 0, 0, 0);
      }
    }
  }

  VTAUopLoopEnd(cmdh);
  VTAUopLoopEnd(cmdh);

  return 0;
}

//#### JO changed (2026-10-27, systolic GEMM): weight-stationary order as gemmop_mxn, same number of uops.
//#### Old order: it_out = x, it_in = kernelIdx, uops = y. New order: it_out = kernelIdx, it_in = x, uops = y.
int gemmop_1x1(void *param,VTACommandHandle cmdh) {

  struct gemmop_mxn *ctx = (struct gemmop_mxn *) (param);
  // VTAUopLoopBegin(cmdh, ctx->tile_size_w_, 1, ctx->stride_w_, 0);
  // VTAUopLoopBegin(cmdh, ctx->n_kernel_, ctx->tile_size_w_ * ctx->tile_size_h_, 0, 1);
  VTAUopLoopBegin(cmdh, ctx->n_kernel_, ctx->tile_size_w_ * ctx->tile_size_h_, 0, 1);
  VTAUopLoopBegin(cmdh, ctx->tile_size_w_, 1, ctx->stride_w_, 0);

  for (int dx = 0; dx < ctx->kernel_h_; dx++) {
    for (int inTileHIdx = 0; inTileHIdx < ctx->tile_size_h_; inTileHIdx++) {
      auto outTileHIdx = inTileHIdx;
      VTAUopPush(cmdh, 0, 0, ctx->output_base_addr_ + outTileHIdx * ctx->tile_size_w_,
                 ctx->input_base_addr_ + (inTileHIdx * ctx->stride_h_ + dx) * (ctx->in_tile_size_w_),
                 ctx->weight_base_addr_ + dx * ctx->kernel_w_, 0, 0,
                 0);
    }

  }

  VTAUopLoopEnd(cmdh);
  VTAUopLoopEnd(cmdh);

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
int alu_shr(void *param,VTACommandHandle cmdh) {
  ShiftContext *p = (ShiftContext *) (param);
  VTAUopLoopBegin(cmdh, p->size_, 1, 1, 0);
  VTAUopPush(cmdh, 1, 0, p->base_, p->base_, 0, 3, 1, p->shift_);
  VTAUopLoopEnd(cmdh);
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

int alu_add(void *param,VTACommandHandle cmdh) {
  AddContext *p = (AddContext *) (param);
  VTAUopLoopBegin(cmdh, p->iter_, p->size_, 1, 0);
  VTAUopLoopBegin(cmdh, p->size_, 1, 0, 0);

  VTAUopPush(cmdh, 1, 0, p->dst_, p->src_, 0, 2, 0, 0);

  VTAUopLoopEnd(cmdh);
  VTAUopLoopEnd(cmdh);
  return 0;
}
int alu_max(void *param,VTACommandHandle cmdh) {
  ResetAcc *p = (ResetAcc *) (param);
  VTAUopLoopBegin(cmdh, p->size_, 1, 1, 0);
  VTAUopPush(cmdh, 1, 0, p->base_, p->base_, 0, 1, 1, -128);
  VTAUopLoopEnd(cmdh);
  return 0;
}

int alu_relu(void *param,VTACommandHandle cmdh) {
  ResetAcc *p = (ResetAcc *) (param);
  VTAUopLoopBegin(cmdh, p->size_, 1, 1, 0);
  VTAUopPush(cmdh, 1, 0, p->base_, p->base_, 0, 1, 1, 0);
  VTAUopLoopEnd(cmdh);
  return 0;
}

int alu_min(void *param,VTACommandHandle cmdh) {
  ResetAcc *p = (ResetAcc *) (param);
  VTAUopLoopBegin(cmdh, p->size_, 1, 1, 0);
  VTAUopPush(cmdh, 1, 0, p->base_, p->base_, 0, 0, 1, 127);
  VTAUopLoopEnd(cmdh);
  return 0;
}

struct MulContext {
  uint32_t dst_;
  uint32_t src_;
  uint32_t size_;
  uint32_t iter_;
  bool operator==(MulContext &op){
    if(dst_==op.dst_ &&
       src_ == op.src_ &&
       size_ == op.size_ &&
       iter_ == op.iter_)
      return true;
    return false;
  }
};

int alu_mul(void *param,VTACommandHandle cmdh) {
  MulContext *p = (MulContext *) (param);
  VTAUopLoopBegin(cmdh, p->iter_, p->size_, 1, 0);
  VTAUopLoopBegin(cmdh, p->size_, 1, 0, 0);

  VTAUopPush(cmdh, 1, 0, p->dst_, p->src_, 0, 4, 0, 0);

  VTAUopLoopEnd(cmdh);
  VTAUopLoopEnd(cmdh);
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

struct XPGemmUOpHandle{
  struct gemmop_mxn strGemm;
  void *uopHandle_xp_gemm[2];
  XPGemmUOpHandle(){
    uopHandle_xp_gemm[0] = nullptr;
  }
};

std::vector<XPGemmUOpHandle*> vXPGemmUOpHandle[4];


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

struct MulUopHandle{
  struct MulContext strMul;
  void *uopHandle_mul[2];
  MulUopHandle(){
    uopHandle_mul[0] = nullptr;
  }
};

std::vector<MulUopHandle*> vMulUopHandle[4];

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
  for(;nFilterinLoop>0;nFilterinLoop--){
    if((KN/16) % (nFilterinLoop*nVirtualThread) ==0) break;
  }
#ifdef NESTC_EVTA_PROFILE_AUTOTUNE
  f7 = nFilterinLoop;
  f8 = KN / nFilterinLoop / nVirtualThread / 16;
#endif
    uint32_t outDummyH = 0;
    uint32_t inDummyH = 0;
    uint32_t outDummyW = 0;
    uint32_t inDummyW = 0;

  for (uint32_t filterIdx = 0; filterIdx < KN / nFilterinLoop / nVirtualThread / 16; filterIdx++) {

    uint32_t inputSramPtr = 0;
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
#ifdef NESTC_EVTA_PROFILE_AUTOTUNE
    if(b0==0){
      f9 = outDummyH;
      f11 = resizedOutTileH;
      f12 = resizedInTileH;
    }
    else{
      f10 = outDummyH;
      f13 = resizedOutTileH;
      f14 = resizedInTileH;
    }
#endif
    // Push operations for resetting Acc memory
    for (uint32_t threadIdx = 0; threadIdx < nVirtualThread; threadIdx++) {
      // acc buffer reset
      uint32_t outputSramBaseAddr = threadIdx * ACC_MEM_ENTRY / nVirtualThread;
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

    // Push operations for loading input and weight then gemming
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

      uint32_t inputSramBaseAddr = 0;
      if(inputSramPtr==0){
        inputSramPtr = INP_MEM_ENTRY / 2;
        inputSramBaseAddr = inputSramPtr;
      }
      else
        inputSramPtr = 0;

      for (uint32_t threadIdx = 0; threadIdx < nVirtualThread; threadIdx++) {
        uint32_t filter_offset =
            filterIdx * nVirtualThread * nFilterinLoop * (numInputChannel / 16) * KH * KW +
            threadIdx * nFilterinLoop * (numInputChannel / 16) * KH * KW +
            channelIdx * KH * KW;

        uint32_t weightSramBaseAddr =
            threadIdx * (WGT_MEM_ENTRY / nVirtualThread);
        VTADepPop(vtaCmdH, 2, 1);
        if(threadIdx==0) {
#ifdef NESTC_EVTA_PROFILE_AUTOTUNE
          f15 ++;
#endif
          VTALoadBuffer2D(vtaCmdH, vta_input_buf, src_offset, x_size, y_size,
                          inputWStride, x_pad_before, y_pad_before,
                          resizedInTileW - x_pad_before - x_size,
                          resizedInTileH - y_pad_before - y_size,
                          inputSramBaseAddr, // index
                          2);                // type
        }
        VTALoadBuffer2D(vtaCmdH, vta_filter_buf, filter_offset, KH * KW,
                        nFilterinLoop, KH * KW * numInputChannel / 16, 0, 0, 0,
                        0, weightSramBaseAddr, 1);
        VTADepPush(vtaCmdH, 1, 2);
      }
      for (uint32_t threadIdx = 0; threadIdx < nVirtualThread; threadIdx++) {

        uint32_t outputSramBaseAddr =
            threadIdx * ACC_MEM_ENTRY / nVirtualThread;
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

        if(KH>1)
          VTAPushGEMMOp(vtaCmdH, uopHandle_gemm, &gemmop_mxn, &gemm_ctx, 0);
        else
          VTAPushGEMMOp(vtaCmdH, uopHandle_gemm, &gemmop_1x1, &gemm_ctx, 0);
#ifdef NESTC_EVTA_PROFILE_AUTOTUNE
        f16 ++;
#endif

        VTADepPush(vtaCmdH, 2, 1);
      }
    }

    for (uint32_t threadIdx = 0; threadIdx < nVirtualThread; threadIdx++) {
      VTADepPop(vtaCmdH, 2, 1);
    }

    // Push operations for adding bias, shifting and relu
    for (uint32_t threadIdx = 0; threadIdx < nVirtualThread; threadIdx++) {

      uint32_t outputSramBaseAddr = threadIdx * ACC_MEM_ENTRY / nVirtualThread;
      uint32_t inputSramBaseAddr = threadIdx * (INP_MEM_ENTRY / nVirtualThread);
      uint32_t weightSramBaseAddr =
          threadIdx * (WGT_MEM_ENTRY / nVirtualThread);
      if(bias) {
        uint32_t sram_offset = outputSramBaseAddr + resizedOutTileH *
                                                    resizedOutTileW *
                                                    nFilterinLoop;
        VTALoadBuffer2D(vtaCmdH, (int32_t *)vta_bias_buf,
                        filterIdx * nVirtualThread * nFilterinLoop + threadIdx * nFilterinLoop,
                        nFilterinLoop, 1, 16 * nFilterinLoop, 0, 0, 0, 0,
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

    // Push operations for storing the results
    for (uint32_t threadIdx = 0; threadIdx < nVirtualThread; threadIdx++) {

      uint32_t outputSramBaseAddr = threadIdx * ACC_MEM_ENTRY / nVirtualThread;
      VTADepPop(vtaCmdH, 2, 3);

      if (outDummyW == 0) {
        for (uint32_t fIdx = 0; fIdx < nFilterinLoop; fIdx++) {
#ifdef NESTC_EVTA_PROFILE_AUTOTUNE
          f17 ++;
#endif
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
#ifdef NESTC_EVTA_PROFILE_AUTOTUNE
          f17 ++;
#endif
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


void conv_kernel_scale(uint32_t sizeOutTileH,
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
                 void *vta_mul_buf,
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
  for(;nFilterinLoop>0;nFilterinLoop--){
    if((KN/16) % (nFilterinLoop*nVirtualThread) ==0) break;
  }
    uint32_t outDummyH = 0;
    uint32_t inDummyH = 0;
    uint32_t outDummyW = 0;
    uint32_t inDummyW = 0;

  for (uint32_t filterIdx = 0; filterIdx < KN / nFilterinLoop / nVirtualThread / 16; filterIdx++) {

    uint32_t inputSramPtr = 0;
    if (resizedOutTileH < 4) {
      outDummyH = 4 - resizedOutTileH;
      resizedOutTileH = 4;
      resizedInTileH = (resizedOutTileH - 1) * stride_size + KH;
      inDummyH = resizedInTileH - sizeInTileH;
    }

    if (resizedOutTileW < 4) {
      outDummyW = 4 - resizedOutTileW;
      resizedOutTileW = 4;
      resizedInTileW = (resizedOutTileW - 1) * stride_size + KW;
      inDummyW = resizedInTileW - sizeInTileW;
    }
    // Push operations for resetting Acc memory
    for (uint32_t threadIdx = 0; threadIdx < nVirtualThread; threadIdx++) {
      // acc buffer reset
      uint32_t outputSramBaseAddr = threadIdx * ACC_MEM_ENTRY / nVirtualThread;
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

    // Push operations for loading input and weight then gemming
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

      uint32_t inputSramBaseAddr = 0;
      if(inputSramPtr==0){
        inputSramPtr = INP_MEM_ENTRY / 2;
        inputSramBaseAddr = inputSramPtr;
      }
      else
        inputSramPtr = 0;

      for (uint32_t threadIdx = 0; threadIdx < nVirtualThread; threadIdx++) {
        uint32_t filter_offset =
            filterIdx * nVirtualThread * nFilterinLoop * (numInputChannel / 16) * KH * KW +
            threadIdx * nFilterinLoop * (numInputChannel / 16) * KH * KW +
            channelIdx * KH * KW;

        uint32_t weightSramBaseAddr =
            threadIdx * (WGT_MEM_ENTRY / nVirtualThread);
        VTADepPop(vtaCmdH, 2, 1);
        if(threadIdx==0) {
          VTALoadBuffer2D(vtaCmdH, vta_input_buf, src_offset, x_size, y_size,
                          inputWStride, x_pad_before, y_pad_before,
                          resizedInTileW - x_pad_before - x_size,
                          resizedInTileH - y_pad_before - y_size,
                          inputSramBaseAddr, // index
                          2);                // type
        }
        VTALoadBuffer2D(vtaCmdH, vta_filter_buf, filter_offset, KH * KW,
                        nFilterinLoop, KH * KW * numInputChannel / 16, 0, 0, 0,
                        0, weightSramBaseAddr, 1);
        VTADepPush(vtaCmdH, 1, 2);
      }
      for (uint32_t threadIdx = 0; threadIdx < nVirtualThread; threadIdx++) {

        uint32_t outputSramBaseAddr =
            threadIdx * ACC_MEM_ENTRY / nVirtualThread;
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

        if(KH>1)
          VTAPushGEMMOp(vtaCmdH, uopHandle_gemm, &gemmop_mxn, &gemm_ctx, 0);
        else
          VTAPushGEMMOp(vtaCmdH, uopHandle_gemm, &gemmop_1x1, &gemm_ctx, 0);
        VTADepPush(vtaCmdH, 2, 1);
      }
    }

    for (uint32_t threadIdx = 0; threadIdx < nVirtualThread; threadIdx++) {
      VTADepPop(vtaCmdH, 2, 1);
    }

    // Push operations for adding bias, shifting and relu
    for (uint32_t threadIdx = 0; threadIdx < nVirtualThread; threadIdx++) {

      uint32_t outputSramBaseAddr = threadIdx * ACC_MEM_ENTRY / nVirtualThread;
      uint32_t inputSramBaseAddr = threadIdx * (INP_MEM_ENTRY / nVirtualThread);
      uint32_t weightSramBaseAddr =
          threadIdx * (WGT_MEM_ENTRY / nVirtualThread);
      if(bias) {
        uint32_t sram_offset = outputSramBaseAddr + resizedOutTileH *
                                                    resizedOutTileW *
                                                    nFilterinLoop;
        VTALoadBuffer2D(vtaCmdH, (int32_t *)vta_bias_buf,
                        filterIdx * nVirtualThread * nFilterinLoop + threadIdx * nFilterinLoop,
                        nFilterinLoop, 1, 16 * nFilterinLoop, 0, 0, 0, 0,
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

      uint32_t sram_offset = outputSramBaseAddr + resizedOutTileH *
                                                  resizedOutTileW *
                                                  nFilterinLoop;
      VTALoadBuffer2D(vtaCmdH, (int32_t *)vta_mul_buf,
                      filterIdx * nVirtualThread * nFilterinLoop + threadIdx * nFilterinLoop,
                      nFilterinLoop, 1, 16 * nFilterinLoop, 0, 0, 0, 0,
                      sram_offset, // index
                      3);          // type

      MulContext mul_ctx;
      mul_ctx.dst_ = outputSramBaseAddr;
      mul_ctx.size_ = resizedOutTileH * resizedOutTileW;
      mul_ctx.src_ = sram_offset;
      mul_ctx.iter_ = nFilterinLoop;

      void **uopHandle_mul;
      bool isFound = false;
      for(auto mulUOpHandle : vMulUopHandle[idEVTA]){
        if(mulUOpHandle->strMul==mul_ctx){
          uopHandle_mul = mulUOpHandle->uopHandle_mul;
          isFound = true;
          break;
        }
      }
      if(!isFound){
        auto newMulHandle = new MulUopHandle();
        newMulHandle->strMul = mul_ctx;
        uopHandle_mul = newMulHandle->uopHandle_mul;
        vMulUopHandle[idEVTA].push_back(newMulHandle);
      }
      VTAPushALUOp(vtaCmdH, uopHandle_mul, &alu_mul, &mul_ctx, 0);


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

    // Push operations for storing the results
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


void xp_conv_kernel(uint32_t sizeOutTileH,
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
                 void *vta_mul_buf,
                 VTACommandHandle& vtaCmdH,
                 uint32_t idEVTA = 0,
                 uint32_t nVirtualThread = 1,
                 bool domul=false) {

  // Test
  uint32_t resizedOutTileH = (sizeInTileH-KH) / stride_size + 1;
  uint32_t resizedOutTileW = (sizeInTileW-KW) / stride_size + 1;
  uint32_t resizedInTileH =  (resizedOutTileH - 1) * stride_size + KH;
  uint32_t resizedInTileW =  (resizedOutTileW - 1) * stride_size + KW;


  uint32_t ACC_MEM_ENTRY = 2048;
  uint32_t INP_MEM_ENTRY = 2048;
  uint32_t WGT_MEM_ENTRY = 1024;

  uint32_t nFilterinLoop = KN / nVirtualThread / 16 ;
  nFilterinLoop = min(nFilterinLoop, ACC_MEM_ENTRY/ nVirtualThread / resizedOutTileH / resizedOutTileW);
  for(;nFilterinLoop>0;nFilterinLoop--){
    if((KN/16) % (nFilterinLoop*nVirtualThread) ==0) break;
  }
#ifdef NESTC_EVTA_PROFILE_AUTOTUNE
  f7 = nFilterinLoop;
  f8 = KN / nFilterinLoop / nVirtualThread / 16;
#endif

  for (uint32_t filterIdx = 0; filterIdx < KN / nFilterinLoop / nVirtualThread / 16; filterIdx++) {
    uint32_t outDummyH = 0;
    uint32_t inDummyH = 0;
    uint32_t outDummyW = 0;
    uint32_t inDummyW = 0;

    uint32_t inputSramPtr = 0;
    if (resizedOutTileH < 4) {
      outDummyH = 4 - resizedOutTileH;
      resizedOutTileH = 4;
      resizedInTileH = (resizedOutTileH - 1) * stride_size + KH;
      inDummyH = resizedInTileH - sizeInTileH;
    }

    if (resizedOutTileW < 4) {
      outDummyW = 4 - resizedOutTileW;
      resizedOutTileW = 4;
      resizedInTileW = (resizedOutTileW - 1) * stride_size + KW;
      inDummyW = resizedInTileW - sizeInTileW;
    }
#ifdef NESTC_EVTA_PROFILE_AUTOTUNE
    if(b0==0){
      f9 = outDummyH;
      f11 = resizedOutTileH;
      f12 = resizedInTileH;
    }
    else{
      f10 = outDummyH;
      f13 = resizedOutTileH;
      f14 = resizedInTileH;
    }
#endif
    // Push operations for resetting Acc memory
    for (uint32_t threadIdx = 0; threadIdx < nVirtualThread; threadIdx++) {
      // acc buffer reset
      uint32_t outputSramBaseAddr = threadIdx * ACC_MEM_ENTRY / nVirtualThread;
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

      VTAPushXPGEMMOp(vtaCmdH, uopHandle_reset, &reset_acc, &reset, 0);
      VTADepPush(vtaCmdH, 2, 1);
    }

    uint32_t inputWOffset = (int)(outputWOffset * stride_size - pad_size) < 0
                            ? 0
                            : outputWOffset * stride_size - pad_size;
    uint32_t inputHOffset = (int)(outputHOffset * stride_size - pad_size) < 0
                            ? 0
                            : outputHOffset * stride_size - pad_size;

    // Push operations for loading input and weight then gemming
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

      uint32_t inputSramBaseAddr = 0;
      if(inputSramPtr==0){
        inputSramPtr = INP_MEM_ENTRY / 2;
        inputSramBaseAddr = inputSramPtr;
      }
      else
        inputSramPtr = 0;

      for (uint32_t threadIdx = 0; threadIdx < nVirtualThread; threadIdx++) {
        uint32_t filter_offset =
            filterIdx * nVirtualThread * nFilterinLoop * (numInputChannel / 16) * KH * KW +
            threadIdx * nFilterinLoop * (numInputChannel / 16) * KH * KW +
            channelIdx * KH * KW;

        uint32_t weightSramBaseAddr =
            threadIdx * (WGT_MEM_ENTRY / nVirtualThread);
        VTADepPop(vtaCmdH, 2, 1);
        if(threadIdx==0) {
#ifdef NESTC_EVTA_PROFILE_AUTOTUNE
          f15 ++;
#endif
          VTALoadBuffer2D(vtaCmdH, vta_input_buf, src_offset, x_size, y_size,
                          inputWStride, x_pad_before, y_pad_before,
                          resizedInTileW - x_pad_before - x_size,
                          resizedInTileH - y_pad_before - y_size,
                          inputSramBaseAddr, // index
                          2);                // type
        }
        VTALoadBuffer2D(vtaCmdH, vta_filter_buf, filter_offset, KH * KW,
                        nFilterinLoop, KH * KW * numInputChannel / 16, 0, 0, 0,
                        0, weightSramBaseAddr, 1);
        VTADepPush(vtaCmdH, 1, 2);
      }
      for (uint32_t threadIdx = 0; threadIdx < nVirtualThread; threadIdx++) {

        uint32_t outputSramBaseAddr =
            threadIdx * ACC_MEM_ENTRY / nVirtualThread;
        uint32_t weightSramBaseAddr =
            threadIdx * (WGT_MEM_ENTRY / nVirtualThread);
        VTADepPop(vtaCmdH, 1, 2);
        struct gemmop_mxn xp_gemm_ctx;
        xp_gemm_ctx.tile_size_h_ = resizedOutTileH;
        xp_gemm_ctx.tile_size_w_ = resizedOutTileW;
        xp_gemm_ctx.src_factor_ = resizedInTileW;
        xp_gemm_ctx.in_tile_size_h_ = resizedInTileH;
        xp_gemm_ctx.in_tile_size_w_ = resizedInTileW;
        xp_gemm_ctx.kernel_h_ = KH;
        xp_gemm_ctx.kernel_w_ = KW;
        xp_gemm_ctx.stride_h_ = stride_size;
        xp_gemm_ctx.stride_w_ = stride_size;
        xp_gemm_ctx.n_kernel_ = nFilterinLoop;
        xp_gemm_ctx.input_base_addr_ = inputSramBaseAddr;
        xp_gemm_ctx.output_base_addr_ = outputSramBaseAddr;
        xp_gemm_ctx.weight_base_addr_ = weightSramBaseAddr;

        void **uopHandle_xp_gemm;

        bool isFound = false;
        for(auto xp_gemmUOpHandle : vXPGemmUOpHandle[idEVTA]){
          if(xp_gemmUOpHandle->strGemm==xp_gemm_ctx){
            uopHandle_xp_gemm = xp_gemmUOpHandle->uopHandle_xp_gemm;
            isFound = true;
            break;
          }
        }
        if(!isFound){
          auto newHandle = new XPGemmUOpHandle();
          newHandle->strGemm = xp_gemm_ctx;
          uopHandle_xp_gemm = newHandle->uopHandle_xp_gemm;
          vXPGemmUOpHandle[idEVTA].push_back(newHandle);
        }

        if(KH>1)
          VTAPushXPGEMMOp(vtaCmdH, uopHandle_xp_gemm, &gemmop_mxn, &xp_gemm_ctx, 0);
        else
          VTAPushXPGEMMOp(vtaCmdH, uopHandle_xp_gemm, &gemmop_1x1, &xp_gemm_ctx, 0);
#ifdef NESTC_EVTA_PROFILE_AUTOTUNE
        f16 ++;
#endif

        VTADepPush(vtaCmdH, 2, 1);
      }
    }

    for (uint32_t threadIdx = 0; threadIdx < nVirtualThread; threadIdx++) {
      VTADepPop(vtaCmdH, 2, 1);
    }

    // Push operations for adding bias, shifting and relu
    for (uint32_t threadIdx = 0; threadIdx < nVirtualThread; threadIdx++) {

      uint32_t outputSramBaseAddr = threadIdx * ACC_MEM_ENTRY / nVirtualThread;
      uint32_t inputSramBaseAddr = threadIdx * (INP_MEM_ENTRY / nVirtualThread);
      uint32_t weightSramBaseAddr =
          threadIdx * (WGT_MEM_ENTRY / nVirtualThread);

      if(domul) {
        uint32_t sram_offset = outputSramBaseAddr + resizedOutTileH *
                                                    resizedOutTileW *
                                                    nFilterinLoop;
        VTALoadBuffer2D(vtaCmdH, (int32_t *)vta_mul_buf,
                        filterIdx * nVirtualThread * nFilterinLoop + threadIdx * nFilterinLoop,
                        nFilterinLoop, 1, 16 * nFilterinLoop, 0, 0, 0, 0,
                        sram_offset, // index
                        3);          // type

        MulContext mul_ctx;
        mul_ctx.dst_ = outputSramBaseAddr;
        mul_ctx.size_ = resizedOutTileH * resizedOutTileW;
        mul_ctx.src_ = sram_offset;
        mul_ctx.iter_ = nFilterinLoop;


        void **uopHandle_mul;

        bool isFound = false;
        for(auto mulUOpHandle : vMulUopHandle[idEVTA]){
          if(mulUOpHandle->strMul==mul_ctx){
            uopHandle_mul = mulUOpHandle->uopHandle_mul;
            isFound = true;
            break;
          }
        }
        if(!isFound){
          auto newMulHandle = new MulUopHandle();
          newMulHandle->strMul = mul_ctx;
          uopHandle_mul = newMulHandle->uopHandle_mul;
          vMulUopHandle[idEVTA].push_back(newMulHandle);
        }

        VTAPushALUOp(vtaCmdH, uopHandle_mul, &alu_mul, &mul_ctx, 0);
        ///////////////////////////////////////////////////////////////////////////
      }

      if(bias) {
        uint32_t sram_offset = outputSramBaseAddr + resizedOutTileH *
                                                    resizedOutTileW *
                                                    nFilterinLoop;
        VTALoadBuffer2D(vtaCmdH, (int32_t *)vta_bias_buf,
                        filterIdx * nVirtualThread * nFilterinLoop + threadIdx * nFilterinLoop,
                        nFilterinLoop, 1, 16 * nFilterinLoop, 0, 0, 0, 0,
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

    // Push operations for storing the results
    for (uint32_t threadIdx = 0; threadIdx < nVirtualThread; threadIdx++) {

      uint32_t outputSramBaseAddr = threadIdx * ACC_MEM_ENTRY / nVirtualThread;
      VTADepPop(vtaCmdH, 2, 3);

      if (outDummyW == 0) {
        for (uint32_t fIdx = 0; fIdx < nFilterinLoop; fIdx++) {
#ifdef NESTC_EVTA_PROFILE_AUTOTUNE
          f17 ++;
#endif
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
        for (int hIdx = 0; hIdx < sizeOutTileH * nFilterinLoop; hIdx++) {
#ifdef NESTC_EVTA_PROFILE_AUTOTUNE
          f17 ++;
#endif
          VTAStoreBuffer2D(vtaCmdH, outputSramBaseAddr + hIdx * resizedOutTileW,
                           4, vta_output_buf,
                           (filterIdx * nVirtualThread + threadIdx) *
                           nFilterinLoop * out_w * out_h +
                           (outputHOffset + hIdx) * out_w + outputWOffset,
                           sizeOutTileW, 1, out_w);
        }
      }
      VTADepPush(vtaCmdH, 3, 2);
      VTADepPop(vtaCmdH, 3, 2);
    }
  }
}

void xp_conv_kernel_scale(uint32_t sizeOutTileH,
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
                 void *vta_mul_buf,
                 VTACommandHandle& vtaCmdH,
                 uint32_t idEVTA = 0,
                 uint32_t nVirtualThread = 1,
                 bool domul=true) {

  // Test
  uint32_t resizedOutTileH = (sizeInTileH-KH) / stride_size + 1;
  uint32_t resizedOutTileW = (sizeInTileW-KW) / stride_size + 1;
  uint32_t resizedInTileH =  (resizedOutTileH - 1) * stride_size + KH;
  uint32_t resizedInTileW =  (resizedOutTileW - 1) * stride_size + KW;


  uint32_t ACC_MEM_ENTRY = 2048;
  uint32_t INP_MEM_ENTRY = 2048;
  uint32_t WGT_MEM_ENTRY = 1024;

  uint32_t nFilterinLoop = KN / nVirtualThread / 16 ;
  nFilterinLoop = min(nFilterinLoop, ACC_MEM_ENTRY/ nVirtualThread / resizedOutTileH / resizedOutTileW);
  for(;nFilterinLoop>0;nFilterinLoop--){
    if((KN/16) % (nFilterinLoop*nVirtualThread) ==0) break;
  }
#ifdef NESTC_EVTA_PROFILE_AUTOTUNE
  f7 = nFilterinLoop;
  f8 = KN / nFilterinLoop / nVirtualThread / 16;
#endif

  for (uint32_t filterIdx = 0; filterIdx < KN / nFilterinLoop / nVirtualThread / 16; filterIdx++) {
    uint32_t outDummyH = 0;
    uint32_t inDummyH = 0;
    uint32_t outDummyW = 0;
    uint32_t inDummyW = 0;

    uint32_t inputSramPtr = 0;
    if (resizedOutTileH < 4) {
      outDummyH = 4 - resizedOutTileH;
      resizedOutTileH = 4;
      resizedInTileH = (resizedOutTileH - 1) * stride_size + KH;
      inDummyH = resizedInTileH - sizeInTileH;
    }

    if (resizedOutTileW < 4) {
      outDummyW = 4 - resizedOutTileW;
      resizedOutTileW = 4;
      resizedInTileW = (resizedOutTileW - 1) * stride_size + KW;
      inDummyW = resizedInTileW - sizeInTileW;
    }
#ifdef NESTC_EVTA_PROFILE_AUTOTUNE
    if(b0==0){
      f9 = outDummyH;
      f11 = resizedOutTileH;
      f12 = resizedInTileH;
    }
    else{
      f10 = outDummyH;
      f13 = resizedOutTileH;
      f14 = resizedInTileH;
    }
#endif
    // Push operations for resetting Acc memory
    for (uint32_t threadIdx = 0; threadIdx < nVirtualThread; threadIdx++) {
      // acc buffer reset
      uint32_t outputSramBaseAddr = threadIdx * ACC_MEM_ENTRY / nVirtualThread;
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

      VTAPushXPGEMMOp(vtaCmdH, uopHandle_reset, &reset_acc, &reset, 0);
      VTADepPush(vtaCmdH, 2, 1);
    }

    uint32_t inputWOffset = (int)(outputWOffset * stride_size - pad_size) < 0
                            ? 0
                            : outputWOffset * stride_size - pad_size;
    uint32_t inputHOffset = (int)(outputHOffset * stride_size - pad_size) < 0
                            ? 0
                            : outputHOffset * stride_size - pad_size;

    // Push operations for loading input and weight then gemming
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

      uint32_t inputSramBaseAddr = 0;
      if(inputSramPtr==0){
        inputSramPtr = INP_MEM_ENTRY / 2;
        inputSramBaseAddr = inputSramPtr;
      }
      else
        inputSramPtr = 0;

      for (uint32_t threadIdx = 0; threadIdx < nVirtualThread; threadIdx++) {
        uint32_t filter_offset =
            filterIdx * nVirtualThread * nFilterinLoop * (numInputChannel / 16) * KH * KW +
            threadIdx * nFilterinLoop * (numInputChannel / 16) * KH * KW +
            channelIdx * KH * KW;

        uint32_t weightSramBaseAddr =
            threadIdx * (WGT_MEM_ENTRY / nVirtualThread);
        VTADepPop(vtaCmdH, 2, 1);
        if(threadIdx==0) {
#ifdef NESTC_EVTA_PROFILE_AUTOTUNE
          f15 ++;
#endif
          VTALoadBuffer2D(vtaCmdH, vta_input_buf, src_offset, x_size, y_size,
                          inputWStride, x_pad_before, y_pad_before,
                          resizedInTileW - x_pad_before - x_size,
                          resizedInTileH - y_pad_before - y_size,
                          inputSramBaseAddr, // index
                          2);                // type
        }
        VTALoadBuffer2D(vtaCmdH, vta_filter_buf, filter_offset, KH * KW,
                        nFilterinLoop, KH * KW * numInputChannel / 16, 0, 0, 0,
                        0, weightSramBaseAddr, 1);
        VTADepPush(vtaCmdH, 1, 2);
      }
      for (uint32_t threadIdx = 0; threadIdx < nVirtualThread; threadIdx++) {

        uint32_t outputSramBaseAddr =
            threadIdx * ACC_MEM_ENTRY / nVirtualThread;
        uint32_t weightSramBaseAddr =
            threadIdx * (WGT_MEM_ENTRY / nVirtualThread);
        VTADepPop(vtaCmdH, 1, 2);
        struct gemmop_mxn xp_gemm_ctx;
        xp_gemm_ctx.tile_size_h_ = resizedOutTileH;
        xp_gemm_ctx.tile_size_w_ = resizedOutTileW;
        xp_gemm_ctx.src_factor_ = resizedInTileW;
        xp_gemm_ctx.in_tile_size_h_ = resizedInTileH;
        xp_gemm_ctx.in_tile_size_w_ = resizedInTileW;
        xp_gemm_ctx.kernel_h_ = KH;
        xp_gemm_ctx.kernel_w_ = KW;
        xp_gemm_ctx.stride_h_ = stride_size;
        xp_gemm_ctx.stride_w_ = stride_size;
        xp_gemm_ctx.n_kernel_ = nFilterinLoop;
        xp_gemm_ctx.input_base_addr_ = inputSramBaseAddr;
        xp_gemm_ctx.output_base_addr_ = outputSramBaseAddr;
        xp_gemm_ctx.weight_base_addr_ = weightSramBaseAddr;

        void **uopHandle_xp_gemm;

        bool isFound = false;
        for(auto xp_gemmUOpHandle : vXPGemmUOpHandle[idEVTA]){
          if(xp_gemmUOpHandle->strGemm==xp_gemm_ctx){
            uopHandle_xp_gemm = xp_gemmUOpHandle->uopHandle_xp_gemm;
            isFound = true;
            break;
          }
        }
        if(!isFound){
          auto newHandle = new XPGemmUOpHandle();
          newHandle->strGemm = xp_gemm_ctx;
          uopHandle_xp_gemm = newHandle->uopHandle_xp_gemm;
          vXPGemmUOpHandle[idEVTA].push_back(newHandle);
        }

        if(KH>1)
          VTAPushXPGEMMOp(vtaCmdH, uopHandle_xp_gemm, &gemmop_mxn, &xp_gemm_ctx, 0);
        else
          VTAPushXPGEMMOp(vtaCmdH, uopHandle_xp_gemm, &gemmop_1x1, &xp_gemm_ctx, 0);
#ifdef NESTC_EVTA_PROFILE_AUTOTUNE
        f16 ++;
#endif

        VTADepPush(vtaCmdH, 2, 1);
      }
    }

    for (uint32_t threadIdx = 0; threadIdx < nVirtualThread; threadIdx++) {
      VTADepPop(vtaCmdH, 2, 1);
    }

    // Push operations for adding bias, shifting and relu
    for (uint32_t threadIdx = 0; threadIdx < nVirtualThread; threadIdx++) {

      uint32_t outputSramBaseAddr = threadIdx * ACC_MEM_ENTRY / nVirtualThread;
      uint32_t inputSramBaseAddr = threadIdx * (INP_MEM_ENTRY / nVirtualThread);
      uint32_t weightSramBaseAddr =
          threadIdx * (WGT_MEM_ENTRY / nVirtualThread);

      if(bias) {
        uint32_t sram_offset = outputSramBaseAddr + resizedOutTileH *
                                                    resizedOutTileW *
                                                    nFilterinLoop;
        VTALoadBuffer2D(vtaCmdH, (int32_t *)vta_bias_buf,
                        filterIdx * nVirtualThread * nFilterinLoop + threadIdx * nFilterinLoop,
                        nFilterinLoop, 1, 16 * nFilterinLoop, 0, 0, 0, 0,
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

      if(domul) {
        uint32_t sram_offset = outputSramBaseAddr + resizedOutTileH *
                                                    resizedOutTileW *
                                                    nFilterinLoop;
        VTALoadBuffer2D(vtaCmdH, (int32_t *)vta_mul_buf,
                        filterIdx * nVirtualThread * nFilterinLoop + threadIdx * nFilterinLoop,
                        nFilterinLoop, 1, 16 * nFilterinLoop, 0, 0, 0, 0,
                        sram_offset, // index
                        3);          // type

        MulContext mul_ctx;
        mul_ctx.dst_ = outputSramBaseAddr;
        mul_ctx.size_ = resizedOutTileH * resizedOutTileW;
        mul_ctx.src_ = sram_offset;
        mul_ctx.iter_ = nFilterinLoop;


        void **uopHandle_mul;

        bool isFound = false;
        for(auto mulUOpHandle : vMulUopHandle[idEVTA]){
          if(mulUOpHandle->strMul==mul_ctx){
            uopHandle_mul = mulUOpHandle->uopHandle_mul;
            isFound = true;
            break;
          }
        }
        if(!isFound){
          auto newMulHandle = new MulUopHandle();
          newMulHandle->strMul = mul_ctx;
          uopHandle_mul = newMulHandle->uopHandle_mul;
          vMulUopHandle[idEVTA].push_back(newMulHandle);
        }

        VTAPushALUOp(vtaCmdH, uopHandle_mul, &alu_mul, &mul_ctx, 0);
        ///////////////////////////////////////////////////////////////////////////
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

    // Push operations for storing the results
    for (uint32_t threadIdx = 0; threadIdx < nVirtualThread; threadIdx++) {

      uint32_t outputSramBaseAddr = threadIdx * ACC_MEM_ENTRY / nVirtualThread;
      VTADepPop(vtaCmdH, 2, 3);

      if (outDummyW == 0) {
        for (uint32_t fIdx = 0; fIdx < nFilterinLoop; fIdx++) {
#ifdef NESTC_EVTA_PROFILE_AUTOTUNE
          f17 ++;
#endif
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
        for (int hIdx = 0; hIdx < sizeOutTileH * nFilterinLoop; hIdx++) {
#ifdef NESTC_EVTA_PROFILE_AUTOTUNE
          f17 ++;
#endif
          VTAStoreBuffer2D(vtaCmdH, outputSramBaseAddr + hIdx * resizedOutTileW,
                           4, vta_output_buf,
                           (filterIdx * nVirtualThread + threadIdx) *
                           nFilterinLoop * out_w * out_h +
                           (outputHOffset + hIdx) * out_w + outputWOffset,
                           sizeOutTileW, 1, out_w);
        }
      }
      VTADepPush(vtaCmdH, 3, 2);
      VTADepPop(vtaCmdH, 3, 2);
    }
  }
}

int convolution(int8_t* input, int8_t *kernel, int8_t *output, int N, int H, int W, int C, int KN, int KH, int KW, int pad_size,
                int stride_size, bool doRelu, bool doBias, uint8_t shift, int out_h, int out_w, int tile_h, int tile_w, uint32_t idEVTA) {
  uint32_t nVirtualThread = 1;


  int8_t *IN_NHWC = input;


  //transpose input NHWC -> NCHW
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

  //transpose input N{C//16}16HW -> N{C//16}HW16
  int8_t *in_NC_HW16 = (int8_t *)malloc(N * (C / 16) * H * W * 16);
  for (int n = 0; n < N; n++) {
    for (int c_ = 0; c_ < C / 16; c_++) {
      for (int h = 0; h < H; h++) {
        for (int w = 0; w < W; w++) {
          for (int c = 0; c < 16; c++) {
            *(in_NC_HW16 + n*(C/16)*H*W*16 + c_*H*W*16 + h*W*16 + w*16 + c) =
                *(in_NC_16HW + n*(C/16)*16*H*W + c_*16*H*W + c*H*W + h*W + w);
          }
        }
      }
    }
  }

  // filter NHWC
  int8_t *filter_NHWC = kernel;

  // reshape filter NHWC -> {N//16}16HW{C//16}16
  assert(KN % 16 == 0);

  int8_t* filter_N_16HWC_16 = filter_NHWC;

  // transpose filter {N//16}16HW{C//16}16 =>  {N//16}{C//16}HW1616
  int8_t* filter_N_C_HW1616 = (int8_t*) malloc ((KN / 16) *(C / 16)*KH*KW*16*16);
  for (int n_ = 0; n_ < KN / 16; n_++) {
    for (int c_ = 0; c_ < C / 16; c_++) {
      for (int h = 0; h < KH; h++) {
        for (int w = 0; w < KW; w++) {
          for (int n = 0; n < 16; n++) {
            for (int c = 0; c < 16; c++) {
              *(filter_N_C_HW1616 + n_*(C/16)*KH*KW*16*16 + c_ *KH*KW*16*16 + h*KW*16*16 + w*16*16 + n*16 + c) =
                  *(filter_N_16HWC_16 + n_*16*KH*KW*C + n*KH*KW*C + h*KW*C + w*C + c_*16 + c);
            }
          }
        }
      }
    }
  }
  int32_t *bias;
  if(doBias) {
    auto locBias = KN * KW * KH * C;
    assert(locBias % 4 == 0);
    locBias /= 4;

    bias = (int32_t *) kernel + locBias;
  }
  int8_t *out_VTA = (int8_t *) malloc(out_h * out_w * KN);



  // VTA DRAM alloc
  // vta's input buffer, as the same size of in_NC_HW16[N][C/16][H][W][16];
  void *vta_input_buf = VTABufferAlloc(N * C * H * W);
  // vta's filter buffer, as the same size of filter_N_C_HW1616[KN/16][C/16][KH][KW][16][16];
  void *vta_filter_buf = VTABufferAlloc(KN * KH * KW * C);
  void *vta_output_buf = VTABufferAlloc(out_h * out_w * KN);
  void *vta_bias_buf;
  if(doBias) {
    vta_bias_buf = VTABufferAlloc(KN * 4);
  }
  // VTA Buffer copy
  VTABufferCopy(in_NC_HW16, 0, vta_input_buf, 0, N * C * H * W, 1);
  VTABufferCopy(filter_N_C_HW1616, 0, vta_filter_buf, 0, KN * KH * KW * C, 1);

  if(doBias) {
    VTABufferCopy(bias, 0, vta_bias_buf, 0, KN * 4, 1);
  }

  uint32_t nTileW, nTileH;
  uint32_t sizeOutTileW = tile_w;
  uint32_t sizeOutTileH = tile_h;

  uint32_t sizeInTileW = (sizeOutTileW - 1) * stride_size + KW;
  uint32_t sizeInTileH = (sizeOutTileH - 1) * stride_size + KH;

  int isRemainTileW = (out_w % sizeOutTileW) ? 1 : 0;
  int isRemainTileH = (out_h % sizeOutTileH) ? 1 : 0;
  nTileW = out_w / sizeOutTileW + isRemainTileW;
  nTileH = out_h / sizeOutTileH + isRemainTileH;


  VTACommandHandle vtaCmdH{nullptr};
  vtaCmdH = VTATLSCommandHandle(idEVTA);
  //VTASetDebugMode(vtaCmdH, VTA_DEBUG_DUMP_INSN | VTA_DEBUG_DUMP_UOP);
  VTASetDebugMode(vtaCmdH, 0);

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
                    idEVTA,
                    nVirtualThread
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
                    idEVTA,
                    nVirtualThread
        );

      }
    }
  }
  VTASynchronize(vtaCmdH, 1 << 31);


  VTABufferCopy(vta_output_buf, 0, out_VTA, 0, out_h * out_w * KN, 2);


#ifdef VTA_DEBUG_MODE

  int8_t* dbg_input = (int8_t* )VTABufferGetVirtAddr(vta_input_buf);
  int8_t* dbg_kernel = (int8_t* )VTABufferGetVirtAddr(vta_filter_buf);
  int8_t* dbg_bias = (int8_t* )VTABufferGetVirtAddr(vta_bias_buf);

  {
    std::ofstream fos("input.bin", std::ios::binary);
    int16_t data16 = 0;
    for (size_t i = 0, e = N * C * H * W; i < e; i++) {
      auto data = dbg_input[i];
      if (i % 2 == 0) {
        data16 = 0xff & data;
      } else {
        data16 = data16 | data << 8;
        fos.write((const char *)&data16, 2);
      }
    }
    fos.close();
  }

  {
    std::ofstream fos("kernel.bin", std::ios::binary);
    int16_t data16 = 0;
    for (size_t i = 0, e = KN * KH * KW * C; i < e; i++) {
      auto data = dbg_kernel[i];
      if (i % 2 == 0) {
        data16 = 0xff & data;
      } else {
        data16 = data16 | data << 8;
        fos.write((const char *)&data16, 2);
      }
    }
    fos.close();
  }


  {
    if(doBias) {
      std::ofstream fos("bias.bin", std::ios::binary);
      int16_t data16 = 0;
      for (size_t i = 0, e = KN * 4; i < e; i++) {
        auto data = dbg_bias[i];
        if (i % 2 == 0) {
          data16 = 0xff & data;
        } else {
          data16 = data16 | data << 8;
          fos.write((const char *)&data16, 2);
        }
      }
      fos.close();
    }
  }

  int8_t *transposed_output = (int8_t *)malloc(KN * out_h * out_w);
  transpose_vtaio2nhwc((int8_t *)VTABufferGetVirtAddr(vta_output_buf),
                       transposed_output, N, out_h, out_w, KN);

  {
    std::ofstream fos("output.bin", std::ios::binary);
    int16_t data16 = 0;
    for (size_t i = 0, e = out_h * out_w * KN; i < e; i++) {
      auto data = transposed_output[i];
      if (i % 2 == 0) {
        data16 = 0xff & data;
      } else {
        data16 = data16 | data << 8;
        fos.write((const char *)&data16, 2);
      }
    }
    fos.close();
  }

#ifndef VTA_RUN_ON_AARCH64
  float nonvtascale = 1.0;
  for (int i = 0; i < shift; i++) {
    nonvtascale *= 2;
  }
  int8_t *temp_output = (int8_t *)malloc(out_h * out_w * KN);
  int8_t *nhwc_input = (int8_t *)malloc( N * C * H * W);
  int8_t *nhwc_weight = (int8_t *)malloc(  KN * KH * KW * C);
  transpose_vtaio2nhwc(dbg_input, nhwc_input, N, H, W, C);
  transpose_vtak2nhwc(dbg_kernel, nhwc_weight, KN, KH, KW, C);

  cpuvtaconvolution(nhwc_input, 1.0, 0,
                    nhwc_weight, 1.0, 0,
                    dbg_bias, 1.0, 0,
                    temp_output, nonvtascale, 0, N, H, W, C, KN, KH, KW,
                    pad_size, stride_size, 1, 1, doRelu, doBias, out_h, out_w);
  {
    {
      std::ofstream fos("output2.bin", std::ios::binary);
      int16_t data16 = 0;
      for (size_t i = 0, e = out_h * out_w * KN; i < e; i++) {
        auto data = temp_output[i];
        if (i % 2 == 0) {
          data16 = 0xff & data;
        } else {
          data16 = data16 | data << 8;
          fos.write((const char *)&data16, 2);
        }
      }
      fos.close();
    }
  }
  for(int h=0; h<out_h; h++){
    for(int w=0; w<out_w; w++){
      for(int c=0; c<KN; c++){
        uint32_t pos = h*out_w*KN + w*KN + c;
        assert(temp_output[pos]==transposed_output[pos]);
      }
    }
  }

  free(temp_output);
  free(nhwc_input);
  free(nhwc_weight);
#endif
  free(transposed_output);
#endif

  // end case
  VTABufferFree(vta_input_buf);
  VTABufferFree(vta_output_buf);
  VTABufferFree(vta_filter_buf);
  if(doBias) {
    VTABufferFree(vta_bias_buf);
  }








  for (int b = 0; b < N; b++) {
    for (int f = 0; f < KN; f++) {
      for (int i = 0; i < out_h; i++) {    //h
        for (int j = 0; j < out_w; j++) {    //w

          int out_ch = f / 16;
          int oo = f % 16;
          auto value =*(out_VTA + (out_ch * out_h * out_w * 16 + i * out_w * 16 + j * 16 + oo));
          *(output + b * out_h * out_w * KN + i * out_w * KN + j * KN + f) = value;
        }
      }
    }
  }

  free(out_VTA);

  free(IN_NCHW);
  free(in_NC_HW16);
  free(filter_N_C_HW1616);

  VTARuntimeShutdown(idEVTA);

  vGemmUOpHandle[idEVTA].clear();
  vAddUOpHandle[idEVTA].clear();
  vResetUopHandle[idEVTA].clear();
  vReluUopHandle[idEVTA].clear();
  vMaxUopHandle[idEVTA].clear();
  vMinUopHandle[idEVTA].clear();
  vShiftUopHandle[idEVTA].clear();
  return 0;
}

int convolution_wo_tr_wo_ch(int8_t* input, int8_t *kernel, int32_t *bias, int8_t *output, int N, int H, int W, int C, int KN, int KH, int KW, int pad_size,
                            int stride_size, bool doRelu, bool doBias, uint8_t shift, int out_h, int out_w, uint32_t idEVTA) {

  int tileWSize = 14;
  int tileHSize = 14;
  int nVirtualThread = 1;
  int8_t *in_NC_HW16 = input;
  int8_t *filter_N_C_HW1616 = kernel;
#ifdef VTA_DEBUG_MODE
  clock_t conv_stage0_start = clock();
#endif


  void *vta_input_buf = VTABufferAlloc(N * C * H * W);
  // vta's filter buffer, as the same size of filter_N_C_HW1616[KN/16][C/16][KH][KW][16][16];
  void *vta_filter_buf = VTABufferAlloc(KN * KH * KW * C);
  void *vta_output_buf = VTABufferAlloc(out_h * out_w * KN);
  void *vta_bias_buf;
  if(doBias) {
    vta_bias_buf = VTABufferAlloc(KN * 4);
  }
  VTABufferCopy(input, 0, vta_input_buf, 0, N * C * H * W, 1);
  VTABufferCopy(kernel, 0, vta_filter_buf, 0, KN * KH * KW * C, 1);

  if(doBias) {
    VTABufferCopy(bias, 0, vta_bias_buf, 0, KN * 4, 1);
  }


#ifdef VTA_DEBUG_MODE
  int8_t *dbg_input = (int8_t *)VTABufferGetVirtAddr(vta_input_buf);
  int8_t *dbg_kernel = (int8_t *)VTABufferGetVirtAddr(vta_filter_buf);
  int8_t *dbg_bias = (int8_t *)VTABufferGetVirtAddr(vta_bias_buf);

  {
    std::ofstream fos("input.bin", std::ios::binary);
    int16_t data16 = 0;
    for (size_t i = 0, e = N * C * H * W; i < e; i++) {
      auto data = dbg_input[i];
      if (i % 2 == 0) {
        data16 = 0xff & data;
      } else {
        data16 = data16 | data << 8;
        fos.write((const char *)&data16, 2);
      }
    }
    fos.close();
  }

  {
    std::ofstream fos("kernel.bin", std::ios::binary);
    int16_t data16 = 0;
    for (size_t i = 0, e = KN * KH * KW * C; i < e; i++) {
      auto data = dbg_kernel[i];
      if (i % 2 == 0) {
        data16 = 0xff & data;
      } else {
        data16 = data16 | data << 8;
        fos.write((const char *)&data16, 2);
      }
    }
    fos.close();
  }

  {
    if (doBias) {
      std::ofstream fos("bias.bin", std::ios::binary);
      int16_t data16 = 0;
      for (size_t i = 0, e = KN * 4; i < e; i++) {
        auto data = dbg_bias[i];
        if (i % 2 == 0) {
          data16 = 0xff & data;
        } else {
          data16 = data16 | data << 8;
          fos.write((const char *)&data16, 2);
        }
      }
      fos.close();
    }
  }

  clock_t conv_stage0_end = clock();
#endif
  uint32_t nTileW, nTileH;
  uint32_t sizeOutTileW = tileWSize;
  uint32_t sizeOutTileH = tileHSize;

  uint32_t sizeInTileW = (sizeOutTileW - 1) * stride_size + KW;
  uint32_t sizeInTileH = (sizeOutTileH - 1) * stride_size + KH;

  int isRemainTileW = (out_w % sizeOutTileW) ? 1 : 0;
  int isRemainTileH = (out_h % sizeOutTileH) ? 1 : 0;
  nTileW = out_w / sizeOutTileW + isRemainTileW;
  nTileH = out_h / sizeOutTileH + isRemainTileH;

  VTACommandHandle vtaCmdH{nullptr};
  vtaCmdH = VTATLSCommandHandle(idEVTA);
  //VTASetDebugMode(vtaCmdH, VTA_DEBUG_DUMP_INSN | VTA_DEBUG_DUMP_UOP);
  VTASetDebugMode(vtaCmdH, 0);
#ifdef VTA_DEBUG_MODE
  clock_t conv_stage1_end = clock();
#endif
  for (int tileHIdx = 0; tileHIdx < nTileH; tileHIdx++) {
    for (int tileWIdx = 0; tileWIdx < nTileW; tileWIdx++) {
      uint32_t x_pad_before = tileWIdx == 0 ? pad_size : 0;
      uint32_t x_pad_after = tileWIdx == (nTileW - 1) ? pad_size : 0;
      uint32_t y_pad_before = tileHIdx == 0 ? pad_size : 0;
      uint32_t y_pad_after = tileHIdx == (nTileH - 1) ? pad_size : 0;
      uint32_t outputWOffset = tileWIdx * sizeOutTileW;
      uint32_t outputHOffset = tileHIdx * sizeOutTileH;

      uint32_t resizedOutTileH = sizeOutTileH;
      uint32_t resizedInTileH = sizeInTileH;
      if (tileHIdx == nTileH - 1) {
        resizedOutTileH = out_h - tileHIdx * sizeOutTileH;
        resizedInTileH = (resizedOutTileH - 1) * stride_size + KH;
      }

      if ((tileWIdx != nTileW - 1)) {
        conv_kernel(resizedOutTileH, sizeOutTileW, pad_size, stride_size,
                    x_pad_before, x_pad_after, y_pad_before, y_pad_after,
                    outputWOffset, outputHOffset, resizedInTileH, sizeInTileW,
                    C, H, W, vta_input_buf, KN, KH, KW, vta_filter_buf, out_h,
                    out_w, vta_output_buf, doRelu, shift, doBias, vta_bias_buf,
                    vtaCmdH, idEVTA, nVirtualThread);
      } else {
        uint32_t sizeOutTileBoundaryW = out_w - tileWIdx * sizeOutTileW;
        uint32_t sizeInTileBoundaryW =
            (sizeOutTileBoundaryW - 1) * stride_size + KW;
        conv_kernel(resizedOutTileH, sizeOutTileBoundaryW, pad_size,
                    stride_size, x_pad_before, x_pad_after, y_pad_before,
                    y_pad_after, outputWOffset, outputHOffset, resizedInTileH,
                    sizeInTileBoundaryW, C, H, W, vta_input_buf, KN, KH, KW,
                    vta_filter_buf, out_h, out_w, vta_output_buf, doRelu, shift,
                    doBias, vta_bias_buf, vtaCmdH, idEVTA, nVirtualThread);
      }
    }
  }

#ifdef VTA_DEBUG_MODE
  clock_t conv_stage2_end = clock();
#endif
  VTASynchronize(vtaCmdH, 1 << 31);

  VTABufferCopy(vta_output_buf, 0, output, 0, out_h * out_w * KN, 2);




#ifdef VTA_DEBUG_MODE
  {
    clock_t conv_stage3_end = clock();
    std::cout<< "conv_stage0 : "<<((double)(conv_stage0_end - conv_stage0_start)/CLOCKS_PER_SEC*1000)<<std::endl;
    std::cout<< "conv_stage1    : "<<((double)(conv_stage1_end - conv_stage0_end)/CLOCKS_PER_SEC*1000)<<std::endl;
    std::cout<< "conv_stage2 :    "<<((double)(conv_stage2_end - conv_stage1_end)/CLOCKS_PER_SEC*1000)<<std::endl;
    std::cout<<"conv_stage3(sync) : "<<((double)(conv_stage3_end - conv_stage2_end)/CLOCKS_PER_SEC*1000)<<std::endl;


    int8_t *transposed_output = (int8_t *)malloc(KN * out_h * out_w);
    transpose_vtaio2nhwc((int8_t *)VTABufferGetVirtAddr(vta_output_buf),
                         transposed_output, N, out_h, out_w, KN);

    {
      std::ofstream fos("output.bin", std::ios::binary);
      int16_t data16 = 0;
      for (size_t i = 0, e = out_h * out_w * KN; i < e; i++) {
        auto data = transposed_output[i];
        if (i % 2 == 0) {
          data16 = 0xff & data;
        } else {
          data16 = data16 | data << 8;
          fos.write((const char *)&data16, 2);
        }
      }
      fos.close();
    }

#ifndef VTA_RUN_ON_AARCH64
    float nonvtascale = 1.0;
    for (int i = 0; i < shift; i++) {
      nonvtascale *= 2;
    }
    int8_t *temp_output = (int8_t *)malloc(out_h * out_w * KN);
    int8_t *nhwc_input = (int8_t *)malloc( N * C * H * W);
    int8_t *nhwc_weight = (int8_t *)malloc(  KN * KH * KW * C);
    transpose_vtaio2nhwc(dbg_input, nhwc_input, N, H, W, C);
    transpose_vtak2nhwc(dbg_kernel, nhwc_weight, KN, KH, KW, C);

    cpuvtaconvolution(nhwc_input, 1.0, 0,
                      nhwc_weight, 1.0, 0,
                      dbg_bias, 1.0, 0,
                      temp_output, nonvtascale, 0, N, H, W, C, KN, KH, KW,
                      pad_size, stride_size, 1, 1, doRelu, doBias, out_h, out_w);
    {
      {
        std::ofstream fos("output2.bin", std::ios::binary);
        int16_t data16 = 0;
        for (size_t i = 0, e = out_h * out_w * KN; i < e; i++) {
          auto data = temp_output[i];
          if (i % 2 == 0) {
            data16 = 0xff & data;
          } else {
            data16 = data16 | data << 8;
            fos.write((const char *)&data16, 2);
          }
        }
        fos.close();
      }
    }

    for(int h=0; h<out_h; h++){
      for(int w=0; w<out_w; w++){
        for(int c=0; c<KN; c++){
          uint32_t pos = h*out_w*KN + w*KN + c;
          assert(temp_output[pos]==transposed_output[pos]);
        }
      }
    }

    free(temp_output);
    free(nhwc_input);
    free(nhwc_weight);
#endif
    free(transposed_output);
  }
#endif
  return 0;
}

int convolution_wo_tr(int8_t* input, int8_t *kernel, int32_t *bias, int8_t *output, int N, int H, int W, int C, int KN, int KH, int KW, int pad_size,
                      int stride_size, bool doRelu, bool doBias, uint8_t shift, int out_h, int out_w, void* vtaCmdH, int nVirtualThread, int tileHSize, int tileWSize, uint32_t idEVTA) {

  int8_t *in_NC_HW16 = input;
  int8_t *filter_N_C_HW1616 = kernel;
#ifdef VTA_DEBUG_MODE
  clock_t conv_stage0_start = clock();
#endif

  void *vta_input_buf = input;
  void *vta_filter_buf = kernel;
  void *vta_output_buf = output;
  void *vta_bias_buf = bias;

#ifdef VTA_DEBUG_MODE
  int8_t *dbg_input = (int8_t *)VTABufferGetVirtAddr(vta_input_buf);
  int8_t *dbg_kernel = (int8_t *)VTABufferGetVirtAddr(vta_filter_buf);
  int8_t *dbg_bias = (int8_t *)VTABufferGetVirtAddr(vta_bias_buf);

  {
    std::ofstream fos("input.bin", std::ios::binary);
    int16_t data16 = 0;
    for (size_t i = 0, e = N * C * H * W; i < e; i++) {
      auto data = dbg_input[i];
      if (i % 2 == 0) {
        data16 = 0xff & data;
      } else {
        data16 = data16 | data << 8;
        fos.write((const char *)&data16, 2);
      }
    }
    fos.close();
  }

  {
    std::ofstream fos("kernel.bin", std::ios::binary);
    int16_t data16 = 0;
    for (size_t i = 0, e = KN * KH * KW * C; i < e; i++) {
      auto data = dbg_kernel[i];
      if (i % 2 == 0) {
        data16 = 0xff & data;
      } else {
        data16 = data16 | data << 8;
        fos.write((const char *)&data16, 2);
      }
    }
    fos.close();
  }

  {
    if (doBias) {
      std::ofstream fos("bias.bin", std::ios::binary);
      int16_t data16 = 0;
      for (size_t i = 0, e = KN * 4; i < e; i++) {
        auto data = dbg_bias[i];
        if (i % 2 == 0) {
          data16 = 0xff & data;
        } else {
          data16 = data16 | data << 8;
          fos.write((const char *)&data16, 2);
        }
      }
      fos.close();
    }
  }

  clock_t conv_stage0_end = clock();
#endif
  uint32_t nTileW, nTileH;
  uint32_t sizeOutTileW = tileWSize;
  uint32_t sizeOutTileH = tileHSize;

  uint32_t sizeInTileW = (sizeOutTileW - 1) * stride_size + KW;
  uint32_t sizeInTileH = (sizeOutTileH - 1) * stride_size + KH;

  int isRemainTileW = (out_w % sizeOutTileW) ? 1 : 0;
  int isRemainTileH = (out_h % sizeOutTileH) ? 1 : 0;
  nTileW = out_w / sizeOutTileW + isRemainTileW;
  nTileH = out_h / sizeOutTileH + isRemainTileH;

#ifdef NESTC_EVTA_PROFILE_AUTOTUNE
  f0 = sizeOutTileW;
  f1 = sizeOutTileH;
  f2 = sizeInTileW;
  f3 = sizeInTileH;
  f4 = nTileW;
  f5 = nTileH;

  f6 = 0;
  f7 = 0;
  f8 = 0;
  f9 = 0;
  f10 = 0;
  f11 = 0;
  f12 = 0;
  f13 = 0;
  f14 = 0;
  f15 = 0;
  f16 = 0;
  f17 = 0;
  b0 = 0;

#endif

  //VTASetDebugMode(vtaCmdH, VTA_DEBUG_DUMP_INSN | VTA_DEBUG_DUMP_UOP);
  VTASetDebugMode(vtaCmdH, 0);
#ifdef VTA_DEBUG_MODE
  clock_t conv_stage1_end = clock();
#endif
  for (int tileHIdx = 0; tileHIdx < nTileH; tileHIdx++) {
    for (int tileWIdx = 0; tileWIdx < nTileW; tileWIdx++) {
      uint32_t x_pad_before = tileWIdx == 0 ? pad_size : 0;
      uint32_t x_pad_after = tileWIdx == (nTileW - 1) ? pad_size : 0;
      uint32_t y_pad_before = tileHIdx == 0 ? pad_size : 0;
      uint32_t y_pad_after = tileHIdx == (nTileH - 1) ? pad_size : 0;
      uint32_t outputWOffset = tileWIdx * sizeOutTileW;
      uint32_t outputHOffset = tileHIdx * sizeOutTileH;

      uint32_t resizedOutTileH = sizeOutTileH;
      uint32_t resizedInTileH = sizeInTileH;
      if (tileHIdx == nTileH - 1) {
        resizedOutTileH = out_h - tileHIdx * sizeOutTileH;
        resizedInTileH = (resizedOutTileH - 1) * stride_size + KH;
      }

      if ((tileWIdx != nTileW - 1)) {
#ifdef NESTC_EVTA_PROFILE_AUTOTUNE
        b0 = 0;
#endif
        conv_kernel(resizedOutTileH, sizeOutTileW, pad_size, stride_size,
                    x_pad_before, x_pad_after, y_pad_before, y_pad_after,
                    outputWOffset, outputHOffset, resizedInTileH, sizeInTileW,
                    C, H, W, vta_input_buf, KN, KH, KW, vta_filter_buf, out_h,
                    out_w, vta_output_buf, doRelu, shift, doBias, vta_bias_buf,
                    vtaCmdH, idEVTA, nVirtualThread);
      } else {
        uint32_t sizeOutTileBoundaryW = out_w - tileWIdx * sizeOutTileW;
#ifdef NESTC_EVTA_PROFILE_AUTOTUNE
        b0 = 1;
        f6 = sizeOutTileBoundaryW;
#endif
        uint32_t sizeInTileBoundaryW =
            (sizeOutTileBoundaryW - 1) * stride_size + KW;
        conv_kernel(resizedOutTileH, sizeOutTileBoundaryW, pad_size,
                    stride_size, x_pad_before, x_pad_after, y_pad_before,
                    y_pad_after, outputWOffset, outputHOffset, resizedInTileH,
                    sizeInTileBoundaryW, C, H, W, vta_input_buf, KN, KH, KW,
                    vta_filter_buf, out_h, out_w, vta_output_buf, doRelu, shift,
                    doBias, vta_bias_buf, vtaCmdH, idEVTA, nVirtualThread);
      }
    }
  }

#ifdef VTA_DEBUG_MODE
  clock_t conv_stage2_end = clock();
#endif
  VTASynchronize(vtaCmdH, 1 << 31);






#ifdef VTA_DEBUG_MODE
  {
    clock_t conv_stage3_end = clock();
    std::cout<< "conv_stage0 : "<<((double)(conv_stage0_end - conv_stage0_start)/CLOCKS_PER_SEC*1000)<<std::endl;
    std::cout<< "conv_stage1    : "<<((double)(conv_stage1_end - conv_stage0_end)/CLOCKS_PER_SEC*1000)<<std::endl;
    std::cout<< "conv_stage2 :    "<<((double)(conv_stage2_end - conv_stage1_end)/CLOCKS_PER_SEC*1000)<<std::endl;
    std::cout<<"conv_stage3(sync) : "<<((double)(conv_stage3_end - conv_stage2_end)/CLOCKS_PER_SEC*1000)<<std::endl;


    int8_t *transposed_output = (int8_t *)malloc(KN * out_h * out_w);
    transpose_vtaio2nhwc((int8_t *)VTABufferGetVirtAddr(vta_output_buf),
                         transposed_output, N, out_h, out_w, KN);

    {
      std::ofstream fos("output.bin", std::ios::binary);
      int16_t data16 = 0;
      for (size_t i = 0, e = out_h * out_w * KN; i < e; i++) {
        auto data = transposed_output[i];
        if (i % 2 == 0) {
          data16 = 0xff & data;
        } else {
          data16 = data16 | data << 8;
          fos.write((const char *)&data16, 2);
        }
      }
      fos.close();
    }

#ifdef VTA_VALIDATION
    float nonvtascale = 1.0;
    for (int i = 0; i < shift; i++) {
      nonvtascale *= 2;
    }
    int8_t *temp_output = (int8_t *)malloc(out_h * out_w * KN);
    int8_t *nhwc_input = (int8_t *)malloc( N * C * H * W);
    int8_t *nhwc_weight = (int8_t *)malloc(  KN * KH * KW * C);
    transpose_vtaio2nhwc(dbg_input, nhwc_input, N, H, W, C);
    transpose_vtak2nhwc(dbg_kernel, nhwc_weight, KN, KH, KW, C);

    cpuvtaconvolution(nhwc_input, 1.0, 0,
                      nhwc_weight, 1.0, 0,
                      dbg_bias, 1.0, 0,
                      temp_output, nonvtascale, 0, N, H, W, C, KN, KH, KW,
                      pad_size, stride_size, 1, 1, doRelu, doBias, out_h, out_w);
    {
      {
        std::ofstream fos("output2.bin", std::ios::binary);
        int16_t data16 = 0;
        for (size_t i = 0, e = out_h * out_w * KN; i < e; i++) {
          auto data = temp_output[i];
          if (i % 2 == 0) {
            data16 = 0xff & data;
          } else {
            data16 = data16 | data << 8;
            fos.write((const char *)&data16, 2);
          }
        }
        fos.close();
      }
    }

    for(int h=0; h<out_h; h++){
      for(int w=0; w<out_w; w++){
        for(int c=0; c<KN; c++){
          uint32_t pos = h*out_w*KN + w*KN + c;
          assert(temp_output[pos]==transposed_output[pos]);
        }
      }
    }


    free(temp_output);
    free(nhwc_input);
    free(nhwc_weight);
#endif
    free(transposed_output);
  }
#endif


#ifdef NESTC_EVTA_PROFILE_AUTOTUNE
    std::cout<<f0<<","<<f1<<","<<f2<<","<<f3<<","<<f4<<","<<f5<<","<<f6<<","<<f7<<","<<f8<<","<<f9<<","<<f10<<","<<f11<<","<<f12<<","<<f13<<","<<f14<<","<<f15<<","<<f16<<","<<f17<<endl;
#endif
  return 0;
}


int convolution_wo_tr_scale(int8_t* input, int8_t *kernel, int32_t *bias, int32_t *scalingfactor, int8_t *output, int N, int H, int W, int C, int KN, int KH, int KW, int pad_size,
                      int stride_size, bool doRelu, bool doBias, uint8_t shift, int out_h, int out_w, void* vtaCmdH, int nVirtualThread, int tileHSize, int tileWSize, uint32_t idEVTA) {

  int8_t *in_NC_HW16 = input;
  int8_t *filter_N_C_HW1616 = kernel;
  void *vta_input_buf = input;
  void *vta_filter_buf = kernel;
  void *vta_output_buf = output;
  void *vta_bias_buf = bias;
  void *vta_scale_buf = scalingfactor;

  uint32_t nTileW, nTileH;
  uint32_t sizeOutTileW = tileWSize;
  uint32_t sizeOutTileH = tileHSize;

  uint32_t sizeInTileW = (sizeOutTileW - 1) * stride_size + KW;
  uint32_t sizeInTileH = (sizeOutTileH - 1) * stride_size + KH;

  int isRemainTileW = (out_w % sizeOutTileW) ? 1 : 0;
  int isRemainTileH = (out_h % sizeOutTileH) ? 1 : 0;
  nTileW = out_w / sizeOutTileW + isRemainTileW;
  nTileH = out_h / sizeOutTileH + isRemainTileH;



  //VTASetDebugMode(vtaCmdH, VTA_DEBUG_DUMP_INSN | VTA_DEBUG_DUMP_UOP);
  VTASetDebugMode(vtaCmdH, 0);
  for (int tileHIdx = 0; tileHIdx < nTileH; tileHIdx++) {
    for (int tileWIdx = 0; tileWIdx < nTileW; tileWIdx++) {
      uint32_t x_pad_before = tileWIdx == 0 ? pad_size : 0;
      uint32_t x_pad_after = tileWIdx == (nTileW - 1) ? pad_size : 0;
      uint32_t y_pad_before = tileHIdx == 0 ? pad_size : 0;
      uint32_t y_pad_after = tileHIdx == (nTileH - 1) ? pad_size : 0;
      uint32_t outputWOffset = tileWIdx * sizeOutTileW;
      uint32_t outputHOffset = tileHIdx * sizeOutTileH;

      uint32_t resizedOutTileH = sizeOutTileH;
      uint32_t resizedInTileH = sizeInTileH;
      if (tileHIdx == nTileH - 1) {
        resizedOutTileH = out_h - tileHIdx * sizeOutTileH;
        resizedInTileH = (resizedOutTileH - 1) * stride_size + KH;
      }

      if ((tileWIdx != nTileW - 1)) {
        conv_kernel_scale(resizedOutTileH, sizeOutTileW, pad_size, stride_size,
                    x_pad_before, x_pad_after, y_pad_before, y_pad_after,
                    outputWOffset, outputHOffset, resizedInTileH, sizeInTileW,
                    C, H, W, vta_input_buf, KN, KH, KW, vta_filter_buf, out_h,
                    out_w, vta_output_buf, doRelu, shift, doBias, vta_bias_buf, vta_scale_buf,
                    vtaCmdH, idEVTA, nVirtualThread);
      } else {
        uint32_t sizeOutTileBoundaryW = out_w - tileWIdx * sizeOutTileW;
        uint32_t sizeInTileBoundaryW =
            (sizeOutTileBoundaryW - 1) * stride_size + KW;
        conv_kernel_scale(resizedOutTileH, sizeOutTileBoundaryW, pad_size,
                    stride_size, x_pad_before, x_pad_after, y_pad_before,
                    y_pad_after, outputWOffset, outputHOffset, resizedInTileH,
                    sizeInTileBoundaryW, C, H, W, vta_input_buf, KN, KH, KW,
                    vta_filter_buf, out_h, out_w, vta_output_buf, doRelu, shift,
                    doBias, vta_bias_buf, vta_scale_buf, vtaCmdH, idEVTA, nVirtualThread);
      }
    }
  }

  VTASynchronize(vtaCmdH, 1 << 31);

  return 0;
}



int xp_convolution_wo_tr(int8_t* input, int8_t *kernel, int32_t *bias, int32_t *scalingfactor, int8_t *output, int N, int H, int W, int C, int KN, int KH, int KW, int pad_size,
                      int stride_size, bool doRelu, bool doBias, uint8_t shift, int out_h, int out_w, void* vtaCmdH, int nVirtualThread, int tileHSize,
                      int tileWSize, uint32_t idEVTA, bool domul) {

  int8_t *in_NC_HW16 = input;
  int8_t *filter_N_C_HW1616 = kernel;
  void *vta_input_buf = input;
  void *vta_filter_buf = kernel;
  void *vta_output_buf = output;
  void *vta_bias_buf = bias;

  int8_t *temp_mul_buf = (int8_t *)VTABufferAlloc(KN * 4);
  if(!domul) {
    int32_t *mul_buf = (int32_t *)VTABufferGetVirtAddr(temp_mul_buf);
    for (int kn = 0; kn < KN; kn++) {
      *(mul_buf + kn) = 64;
    }
    scalingfactor = (int32_t *)temp_mul_buf;
  }

	void *vta_scale_buf = scalingfactor;

  int32_t* scale_buf = (int32_t *)VTABufferGetVirtAddr(vta_scale_buf);

	uint32_t nTileW, nTileH;
  uint32_t sizeOutTileW = tileWSize;
  uint32_t sizeOutTileH = tileHSize;

  uint32_t sizeInTileW = (sizeOutTileW - 1) * stride_size + KW;
  uint32_t sizeInTileH = (sizeOutTileH - 1) * stride_size + KH;

  int isRemainTileW = (out_w % sizeOutTileW) ? 1 : 0;
  int isRemainTileH = (out_h % sizeOutTileH) ? 1 : 0;
  nTileW = out_w / sizeOutTileW + isRemainTileW;
  nTileH = out_h / sizeOutTileH + isRemainTileH;


  //VTASetDebugMode(vtaCmdH, VTA_DEBUG_DUMP_INSN | VTA_DEBUG_DUMP_UOP);
  VTASetDebugMode(vtaCmdH, 0);
  for (int tileHIdx = 0; tileHIdx < nTileH; tileHIdx++) {
    for (int tileWIdx = 0; tileWIdx < nTileW; tileWIdx++) {
      uint32_t x_pad_before = tileWIdx == 0 ? pad_size : 0;
      uint32_t x_pad_after = tileWIdx == (nTileW - 1) ? pad_size : 0;
      uint32_t y_pad_before = tileHIdx == 0 ? pad_size : 0;
      uint32_t y_pad_after = tileHIdx == (nTileH - 1) ? pad_size : 0;
      uint32_t outputWOffset = tileWIdx * sizeOutTileW;
      uint32_t outputHOffset = tileHIdx * sizeOutTileH;

      uint32_t resizedOutTileH = sizeOutTileH;
      uint32_t resizedInTileH = sizeInTileH;
      if (tileHIdx == nTileH - 1) {
        resizedOutTileH = out_h - tileHIdx * sizeOutTileH;
        resizedInTileH = (resizedOutTileH - 1) * stride_size + KH;
      }

      if ((tileWIdx != nTileW - 1)) {
        xp_conv_kernel(resizedOutTileH, sizeOutTileW, pad_size, stride_size,
                  x_pad_before, x_pad_after, y_pad_before, y_pad_after,
                  outputWOffset, outputHOffset, resizedInTileH, sizeInTileW,
                  C, H, W, vta_input_buf, KN, KH, KW, vta_filter_buf, out_h,
                  out_w, vta_output_buf, doRelu, shift, doBias, vta_bias_buf,
                  (int32_t *)vta_scale_buf, vtaCmdH, idEVTA, nVirtualThread, true);
      }
      else {
        uint32_t sizeOutTileBoundaryW = out_w - tileWIdx * sizeOutTileW;
        uint32_t sizeInTileBoundaryW =
            (sizeOutTileBoundaryW - 1) * stride_size + KW;
        xp_conv_kernel(resizedOutTileH, sizeOutTileW, pad_size, stride_size,
                  x_pad_before, x_pad_after, y_pad_before, y_pad_after,
                  outputWOffset, outputHOffset, resizedInTileH, sizeInTileW,
                  C, H, W, vta_input_buf, KN, KH, KW, vta_filter_buf, out_h,
                  out_w, vta_output_buf, doRelu, shift, doBias, vta_bias_buf,
                  (int32_t *)vta_scale_buf, vtaCmdH, idEVTA, nVirtualThread, true);
      }
    }
  }
  VTABufferFree(temp_mul_buf);
  VTASynchronize(vtaCmdH, 1 << 31);

  return 0;
}


int xp_convolution_wo_tr_scale(int8_t* input, int8_t *kernel, int32_t *bias, int32_t *scalingfactor, int8_t *output, int N, int H, int W, int C, int KN, int KH, int KW, int pad_size,
                      int stride_size, bool doRelu, bool doBias, uint8_t shift, int out_h, int out_w, void* vtaCmdH, int nVirtualThread, int tileHSize,
                      int tileWSize, uint32_t idEVTA, bool domul) {

  int8_t *in_NC_HW16 = input;
  int8_t *filter_N_C_HW1616 = kernel;
  void *vta_input_buf = input;
  void *vta_filter_buf = kernel;
  void *vta_output_buf = output;
  void *vta_bias_buf = bias;
	void *vta_scale_buf = scalingfactor;

	uint32_t nTileW, nTileH;
  uint32_t sizeOutTileW = tileWSize;
  uint32_t sizeOutTileH = tileHSize;

  uint32_t sizeInTileW = (sizeOutTileW - 1) * stride_size + KW;
  uint32_t sizeInTileH = (sizeOutTileH - 1) * stride_size + KH;

  int isRemainTileW = (out_w % sizeOutTileW) ? 1 : 0;
  int isRemainTileH = (out_h % sizeOutTileH) ? 1 : 0;
  nTileW = out_w / sizeOutTileW + isRemainTileW;
  nTileH = out_h / sizeOutTileH + isRemainTileH;


  //VTASetDebugMode(vtaCmdH, VTA_DEBUG_DUMP_INSN | VTA_DEBUG_DUMP_UOP);
  VTASetDebugMode(vtaCmdH, 0);
  for (int tileHIdx = 0; tileHIdx < nTileH; tileHIdx++) {
    for (int tileWIdx = 0; tileWIdx < nTileW; tileWIdx++) {
      uint32_t x_pad_before = tileWIdx == 0 ? pad_size : 0;
      uint32_t x_pad_after = tileWIdx == (nTileW - 1) ? pad_size : 0;
      uint32_t y_pad_before = tileHIdx == 0 ? pad_size : 0;
      uint32_t y_pad_after = tileHIdx == (nTileH - 1) ? pad_size : 0;
      uint32_t outputWOffset = tileWIdx * sizeOutTileW;
      uint32_t outputHOffset = tileHIdx * sizeOutTileH;

      uint32_t resizedOutTileH = sizeOutTileH;
      uint32_t resizedInTileH = sizeInTileH;
      if (tileHIdx == nTileH - 1) {
        resizedOutTileH = out_h - tileHIdx * sizeOutTileH;
        resizedInTileH = (resizedOutTileH - 1) * stride_size + KH;
      }

      if ((tileWIdx != nTileW - 1)) {
        xp_conv_kernel_scale(resizedOutTileH, sizeOutTileW, pad_size, stride_size,
                  x_pad_before, x_pad_after, y_pad_before, y_pad_after,
                  outputWOffset, outputHOffset, resizedInTileH, sizeInTileW,
                  C, H, W, vta_input_buf, KN, KH, KW, vta_filter_buf, out_h,
                  out_w, vta_output_buf, doRelu, shift, doBias, vta_bias_buf,
                  vta_scale_buf, vtaCmdH, idEVTA, nVirtualThread, true);
      }
      else {
        uint32_t sizeOutTileBoundaryW = out_w - tileWIdx * sizeOutTileW;
        uint32_t sizeInTileBoundaryW =
            (sizeOutTileBoundaryW - 1) * stride_size + KW;
        xp_conv_kernel_scale(resizedOutTileH, sizeOutTileW, pad_size, stride_size,
                  x_pad_before, x_pad_after, y_pad_before, y_pad_after,
                  outputWOffset, outputHOffset, resizedInTileH, sizeInTileW,
                  C, H, W, vta_input_buf, KN, KH, KW, vta_filter_buf, out_h,
                  out_w, vta_output_buf, doRelu, shift, doBias, vta_bias_buf,
                  vta_scale_buf, vtaCmdH, idEVTA, nVirtualThread, true);
      }
    }
  }
  VTASynchronize(vtaCmdH, 1 << 31);

  return 0;
}

// #### add 260907, JO
// #### add __attribute__((weak)) to avoid the collision with the strong LLVMSupport when using VTASIM, 260908, JO  
//namespace llvm {
//    __attribute__((weak)) int DisableABIBreakingChecks = 0;
//}