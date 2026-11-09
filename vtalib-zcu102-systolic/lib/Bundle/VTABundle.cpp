#include <atomic>
#include <cstdint>
#include <cmath>
#include <cstdlib>
#include <vector>
#include <fstream>
#include <iostream>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <cassert>
#include <cstring>
#include <map>
#include <mutex>
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



//#### JO added (2026-11-06): the constant VTA buffers of a bundle's load_module (weights, bias, tables; VTASave.cpp),
//#### one buffer for every request with the same bytes in the process: the multi-EVTA executables run one bundle copy
//#### per EVTA with the same weights (Resnet50 x 4 copies = 4 x 25.6 MB of the board's 128 MB CMA). The EVTAs only
//#### read them. Reference counted; EVTA_CONST_SHARE=0 gives every request its own buffer.
struct ConstBuffer {
  void *buf;
  size_t size;
  uint64_t hash;
  int refs;
};
static std::mutex constBufferMutex;
static std::vector<ConstBuffer> constBuffers;

void *VTAConstBufferAlloc(const void *src, size_t size) {
  static int share = -1;
  if (share < 0) {
    const char *e = getenv("EVTA_CONST_SHARE");
    share = !e || atoi(e) != 0;
  }
  uint64_t h = 1469598103934665603ull;  // FNV-1a
  for (size_t i = 0; share && i < size; i++)
    h = (h ^ ((const uint8_t *)src)[i]) * 1099511628211ull;
  std::lock_guard<std::mutex> lock(constBufferMutex);
  for (auto &c : constBuffers)
    if (share && c.size == size && c.hash == h && !memcmp(VTABufferGetVirtAddr(c.buf), src, size)) {
      c.refs++;
      return c.buf;
    }
  void *b = VTABufferAlloc(size);
  VTABufferCopy(src, 0, b, 0, size, 1);
  constBuffers.push_back({b, size, h, 1});
  return b;
}

void VTAConstBufferFree(void *buf) {
  std::lock_guard<std::mutex> lock(constBufferMutex);
  for (size_t i = 0; i < constBuffers.size(); i++)
    if (constBuffers[i].buf == buf) {
      if (--constBuffers[i].refs == 0) {
        VTABufferFree(buf);
        constBuffers.erase(constBuffers.begin() + i);
      }
      return;
    }
  VTABufferFree(buf);
}

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
  //#### JO added (2026-11-08, depthwise): inp entries between the input tiles of consecutive kernels (0: one input tile
  //#### for all n_kernel_ kernels, a dense conv; the depthwise conv has one input tile per 16-channel block)
  uint32_t in_kernel_factor_ = 0;
  bool operator==(gemmop_mxn &op){
    if(tile_size_h_==op.tile_size_h_ &&
       in_kernel_factor_ == op.in_kernel_factor_ &&
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
//#### hw_spec.h is not included here: the uop buffer depth comes from vtalib's -DVTA_LOG_UOP_BUFF_SIZE (64-bit uops).
#ifdef VTA_LOG_UOP_BUFF_SIZE
#define GEMM_UOP_BUFF_DEPTH ((1 << VTA_LOG_UOP_BUFF_SIZE) / 8)
#else
#define GEMM_UOP_BUFF_DEPTH 8192
#endif
//#### JO added (2026-11-06): the acc depth the same way (-DVTA_LOG_ACC_BUFF_SIZE, 16 x 32-bit entries): 4096 on the
//#### single-EVTA buffers, 2048 on the multi-EVTA ones (vtalib's conv code uses ACC_MEM_ENTRY = 2048 of it)
#ifdef VTA_LOG_ACC_BUFF_SIZE
#define ACC_BUFF_DEPTH ((1 << VTA_LOG_ACC_BUFF_SIZE) / 64)
#else
#define ACC_BUFF_DEPTH 4096
#endif
static int gemmop_mxn_old(struct gemmop_mxn *ctx, VTACommandHandle cmdh) {
  VTAUopLoopBegin(cmdh, ctx->tile_size_w_, 1, ctx->stride_w_, 0);
  VTAUopLoopBegin(cmdh, ctx->kernel_w_, 0, 1, 1);

  for (int dx = 0; dx < ctx->kernel_h_; dx++) {
    for (int kernelIdx = 0; kernelIdx < ctx->n_kernel_; kernelIdx++) {
      for (int inTileHIdx = 0; inTileHIdx < ctx->tile_size_h_; inTileHIdx++) {
        auto outTileHIdx = inTileHIdx + (kernelIdx * ctx->tile_size_h_);
        VTAUopPush(cmdh, 0, 0, ctx->output_base_addr_ + outTileHIdx * ctx->tile_size_w_,
                   ctx->input_base_addr_ + kernelIdx * ctx->in_kernel_factor_ +
                       (inTileHIdx * ctx->stride_h_ + dx) * (ctx->in_tile_size_w_),
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
  if (ctx->kernel_h_ * ctx->tile_size_h_ * ctx->tile_size_w_ > GEMM_UOP_BUFF_DEPTH / 4)
    return gemmop_mxn_old(ctx, cmdh);
  VTAUopLoopBegin(cmdh, ctx->n_kernel_, ctx->tile_size_h_ * ctx->tile_size_w_, ctx->in_kernel_factor_,
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


//#### JO added (2026-10-31): residual of a conv fused with the elementadd (+ relu) after it (convolution_wo_tr_res).
//#### After the conv's own requantization (bias, >> shift, clip to int8), still in the acc buffer:
//####   acc = conv << convShl                                  (ALU ADD acc, acc; convShl times)
//####   acc += residual * (2^resShl I)                         (residual tile LOADed into the inp buffer at the acc layout
//####                                                           of the output tile, GEMM with a scaled identity weight)
//####   acc = (acc + 2^(sumShr-1)) >> sumShr                   (ALU ADD imm, SHR)
//####   acc = min(max(acc, relu ? 0 : -128), 127)              (ALU MAX, MIN)
//#### = round(conv * 2^convShl + residual * 2^resShl) / 2^sumShr) as the CPU elemadd for power-of-two scales (rounding
//#### up at .5 where the CPU rounds away from zero: equal for a non-negative sum, and with relu for every sum).
//#### Only ALU MIN / MAX / ADD / SHR (2-bit alu_opcode) and GEMM; the identity weight goes to the last wgt entry and
//#### the residual to the inp buffer after the conv's GEMMs of the tile (load waits on them), so the schedule is unchanged.
struct ConvResidual {
  void *buf;          // residual, EVTA layout, the shape of the output
  void *idWgt;        // 2^k I, k = 0..RES_MAX_SHL, one wgt entry each
  uint32_t resShl;
  uint32_t convShl;
  uint32_t sumShr;
  bool relu;
  uint32_t reluShl;   //#### JO added (2026-11-05): relu output scale = sum scale / 2^reluShl (relu, then << reluShl, clip 127)
};
#define RES_MAX_SHL 6
#define RES_WGT_ENTRY 1023  // last entry of WGT_MEM_ENTRY (1024) in conv_kernel
// alu_opcode as the alu_* functions above use them (hw_spec.h VTA_ALU_OPCODE_*)
#define RES_ALU_MIN 0
#define RES_ALU_MAX 1
#define RES_ALU_ADD 2
#define RES_ALU_SHR 3

//#### JO added (2026-11-02): max pool 3x3 / stride 2 / pad 1 fused into a conv with relu (convolution_wo_tr_pool).
//#### The conv's output tiles (even th x tw, dividing the output) are computed with one more row above and one more column
//#### to the left (the halo of the pool window, recomputed; not at the image border, where it is the pad): after the
//#### requantize / relu the tile is in acc as [block][th(+1)][tw(+1)]. The max in two passes of ALU MAX, each a 2-level
//#### loop with one uop per output block of the thread (consecutive uops write different entries):
//####   rows: the pool's centre columns 2j + haloW of every tile row take the max with the columns left and right of them
//####         (2 instructions, in place; the left column of j = 0 is the pad at the image border, left out),
//####   cols: an area of th/2 x tw/2 entries per block after the bias entries, reset to 0, takes the max of the centre
//####         columns of rows 2i + haloH + dy - 1, dy = 0..2 (3 instructions; row -1 at the image border is the pad).
//#### The conv values are >= 0 (relu), so 0 plus the in-bounds max is the CPU's max. The pooled area is STOREd to the
//#### pool output; the conv output is not. (One instruction per window position and block took 2.7x longer, 2026-11-02.)
struct ConvPool {
  void *out;            // pool output, EVTA layout, KN x PH x PW
  uint32_t PH, PW;      // pool output size
  uint32_t py0, px0;    // this tile's first pooled pixel
  uint32_t pth, ptw;    // this tile's pooled pixels (conv tile / 2)
  bool haloH, haloW;    // the conv tile has the extra row above / column to the left
};
// acc[out + i * outRow + j * outCol + f * outF] = max(itself, acc[in + i * inRow + j * inCol + f * inF]),
// i < oh, j < ow, f < nF (one uop per f)
struct PoolMax2Context {
  uint32_t out_, in_, oh_, ow_, outRow_, outCol_, inRow_, inCol_, nF_, outF_, inF_;
  bool operator==(PoolMax2Context &op) {
    return out_ == op.out_ && in_ == op.in_ && oh_ == op.oh_ && ow_ == op.ow_ && outRow_ == op.outRow_ &&
           outCol_ == op.outCol_ && inRow_ == op.inRow_ && inCol_ == op.inCol_ && nF_ == op.nF_ &&
           outF_ == op.outF_ && inF_ == op.inF_;
  }
};
int alu_poolmax2(void *param, VTACommandHandle cmdh) {
  PoolMax2Context *p = (PoolMax2Context *)(param);
  VTAUopLoopBegin(cmdh, p->oh_, p->outRow_, p->inRow_, 0);
  VTAUopLoopBegin(cmdh, p->ow_, p->outCol_, p->inCol_, 0);
  for (uint32_t f = 0; f < p->nF_; f++)
    VTAUopPush(cmdh, 1, 0, p->out_ + f * p->outF_, p->in_ + f * p->inF_, 0, RES_ALU_MAX, 0, 0);
  VTAUopLoopEnd(cmdh);
  VTAUopLoopEnd(cmdh);
  return 0;
}
struct PoolMax2UopHandle {
  PoolMax2Context ctx;
  void *uopHandle[2];
  PoolMax2UopHandle() { uopHandle[0] = nullptr; }
};
std::vector<PoolMax2UopHandle *> vPoolMax2UopHandle[4];
static void pushPoolMax2(VTACommandHandle &cmdh, uint32_t idEVTA, const PoolMax2Context &c) {
  PoolMax2Context ctx = c;
  void **h = nullptr;
  for (auto u : vPoolMax2UopHandle[idEVTA])
    if (u->ctx == ctx) { h = u->uopHandle; break; }
  if (!h) {
    auto u = new PoolMax2UopHandle();
    u->ctx = ctx;
    h = u->uopHandle;
    vPoolMax2UopHandle[idEVTA].push_back(u);
  }
  VTAPushALUOp(cmdh, h, &alu_poolmax2, &ctx, 0);
}
static void pushReset(VTACommandHandle &cmdh, uint32_t idEVTA, uint32_t base, uint32_t size);

static void *getIdentityWeights(uint32_t idEVTA) {
  static void *idWgt[4] = {nullptr, nullptr, nullptr, nullptr};
  if (!idWgt[idEVTA]) {
    const size_t esz = 16 * 16;
    void *b = VTABufferAlloc(esz * (RES_MAX_SHL + 1));
    int8_t *p = (int8_t *)VTABufferGetVirtAddr(b);
    for (uint32_t k = 0; k <= RES_MAX_SHL; k++)
      for (int i = 0; i < 16; i++)
        for (int j = 0; j < 16; j++)
          p[k * esz + i * 16 + j] = i == j ? (int8_t)(1 << k) : 0;
    VTABufferFlush(b, 0, esz * (RES_MAX_SHL + 1));
    idWgt[idEVTA] = b;
  }
  return idWgt[idEVTA];
}

// one ALU op over size acc entries: acc[base + i] = op(acc[base + i], use_imm ? imm : acc[src + i])
struct AluContext {
  uint32_t base_;
  uint32_t src_;
  uint32_t size_;
  int opcode_;
  int useImm_;
  int imm_;
  bool operator==(AluContext &op) {
    return base_ == op.base_ && src_ == op.src_ && size_ == op.size_ && opcode_ == op.opcode_ &&
           useImm_ == op.useImm_ && imm_ == op.imm_;
  }
};
int alu_generic(void *param, VTACommandHandle cmdh) {
  AluContext *p = (AluContext *)(param);
  VTAUopLoopBegin(cmdh, p->size_, 1, 1, 0);
  VTAUopPush(cmdh, 1, 0, p->base_, p->src_, 0, p->opcode_, p->useImm_, p->imm_);
  VTAUopLoopEnd(cmdh);
  return 0;
}
struct AluUopHandle {
  AluContext ctx;
  void *uopHandle[2];
  AluUopHandle() { uopHandle[0] = nullptr; }
};
std::vector<AluUopHandle *> vAluUopHandle[4];

static void pushAlu(VTACommandHandle &cmdh, uint32_t idEVTA, uint32_t base, uint32_t src, uint32_t size, int opcode,
                    int useImm, int imm) {
  AluContext ctx = {base, src, size, opcode, useImm, imm};
  void **h = nullptr;
  for (auto u : vAluUopHandle[idEVTA])
    if (u->ctx == ctx) { h = u->uopHandle; break; }
  if (!h) {
    auto u = new AluUopHandle();
    u->ctx = ctx;
    h = u->uopHandle;
    vAluUopHandle[idEVTA].push_back(u);
  }
  VTAPushALUOp(cmdh, h, &alu_generic, &ctx, 0);
}

//#### JO added (2026-11-06): upper clip of a fused relu, e.g. ReLU6 = Clip(0, 6) -> round(6 / output scale); VTASave sets it
//#### around a conv with a fused Clip (VTASetActivationClip(hi) ... VTASetActivationClip(127)). conv_kernel puts it in the
//#### requantize (rq_hi) or an ALU MIN, the CPU convs (CPUBundle_generic.cpp) clip to it; 127 = a plain relu
thread_local int gEvtaClipHi = 127;
void VTASetActivationClip(int hi) { gEvtaClipHi = hi < 127 ? hi : 127; }

// acc[acc + i] += inp[inp + i] x wgt[wgt], i < size
struct ResGemmContext {
  uint32_t acc_;
  uint32_t inp_;
  uint32_t wgt_;
  uint32_t size_;
  bool operator==(ResGemmContext &op) {
    return acc_ == op.acc_ && inp_ == op.inp_ && wgt_ == op.wgt_ && size_ == op.size_;
  }
};
int gemm_res(void *param, VTACommandHandle cmdh) {
  ResGemmContext *p = (ResGemmContext *)(param);
  VTAUopLoopBegin(cmdh, p->size_, 1, 1, 0);
  VTAUopPush(cmdh, 0, 0, p->acc_, p->inp_, p->wgt_, 0, 0, 0);
  VTAUopLoopEnd(cmdh);
  return 0;
}
struct ResGemmUopHandle {
  ResGemmContext ctx;
  void *uopHandle[2];
  ResGemmUopHandle() { uopHandle[0] = nullptr; }
};
std::vector<ResGemmUopHandle *> vResGemmUopHandle[4];

static void pushResGemm(VTACommandHandle &cmdh, uint32_t idEVTA, uint32_t acc, uint32_t inp, uint32_t size) {
  ResGemmContext ctx = {acc, inp, RES_WGT_ENTRY, size};
  void **h = nullptr;
  for (auto u : vResGemmUopHandle[idEVTA])
    if (u->ctx == ctx) { h = u->uopHandle; break; }
  if (!h) {
    auto u = new ResGemmUopHandle();
    u->ctx = ctx;
    h = u->uopHandle;
    vResGemmUopHandle[idEVTA].push_back(u);
  }
  VTAPushGEMMOp(cmdh, h, &gemm_res, &ctx, 0);
}

//#### JO added (2026-11-04, bias reset): the conv bias as the start of the accumulation. The bias entries are LOADed into
//#### acc (bias + f, f < nF) before the reset GEMM, and the reset writes acc[base + f * T + p] = acc[bias + f] (GEMM
//#### bias_reset, VTAUopPush reset_out 2) instead of 0, so the ALU ADD of the bias after the GEMMs goes away (same sums,
//#### integer). Only on a bitstream that has it (gemm_sa sa_rmw.v, 2026-11-04): "bias" in the name of the bitstream the
//#### run scripts loaded (/tmp/evta_bitstream); EVTA_BIAS_RESET=0|1 overrides.
static bool evtaBiasReset() {
  static int on = -1;
  if (on < 0) {
    const char *e = getenv("EVTA_BIAS_RESET");
    if (e && *e) on = atoi(e) > 0;
    else {
      char name[256] = {0};
      FILE *f = fopen("/tmp/evta_bitstream", "r");
      //#### JO changed (2026-11-04): an "_rq" bitstream (requantize, below) has the bias reset too
      //on = f && fgets(name, sizeof(name), f) && strstr(name, "bias") && strncmp(name, "FAILED", 6);
      on = f && fgets(name, sizeof(name), f) && (strstr(name, "bias") || strstr(name, "_rq")) && strncmp(name, "FAILED", 6);
      if (f) fclose(f);
    }
  }
  return on;
}
//#### JO added (2026-11-04): declared here: this file includes vta/vtalib's runtime.h (not this copy's include/zcu102/vta/runtime.h,
//#### where it is declared); defined in this copy's lib/zcu102/api.cc
extern "C" void VTARequant(VTACommandHandle cmd, int en, int round, int shift, int lo, int hi);
//#### JO added (2026-11-04, requantize): the requantize of conv / fc outputs on the write to out (VTARequant, GEMM / ALU rq_*)
//#### instead of the ALU SHR / MAX / MIN (+ the rounding ADD); only on a bitstream that has it (gemm_sa sa_rmw.v, vta.cc
//#### alu(), 2026-11-04): "_rq" in the name of the bitstream the run scripts loaded (/tmp/evta_bitstream); EVTA_REQUANT=0|1
//#### overrides. Used by conv_kernel (plain conv: on its GEMMs; fused max pool: on the column MAX passes; fused residual: on
//#### the residual GEMM) and fc_evta_core (on its GEMMs).
static bool evtaRequant() {
  static int on = -1;
  if (on < 0) {
    const char *e = getenv("EVTA_REQUANT");
    if (e && *e) on = atoi(e) > 0;
    else {
      char name[256] = {0};
      FILE *f = fopen("/tmp/evta_bitstream", "r");
      on = f && fgets(name, sizeof(name), f) && strstr(name, "_rq") && strncmp(name, "FAILED", 6);
      if (f) fclose(f);
    }
  }
  return on;
}
//#### JO added (2026-11-05): smallest conv output tile (conv_kernel pads smaller tiles with dummy rows / columns), default
//#### MIN_H_SIZE / MIN_W_SIZE. The HLS GEMM (vta/vtalib) needs it: its acc read-modify-write has no hazard check and the same
//#### dst comes back every tile_h * tile_w uops (gemmop_mxn). The systolic GEMM forwards in-flight dsts (sa_rmw.v), so a
//#### smaller tile only saves the dummy GEMM / ALU work: 1 on a systolic bitstream (_sa_ in /tmp/evta_bitstream; VGG16 EVTA
//#### 13.3 -> 7.0 ms with nVirtualThread 2 on its 4 x 4 / 2 x 2 convs, dumps identical), else dflt. EVTA_MIN_TILE=<n> overrides it.
static bool evtaSystolic() {   // systolic GEMM bitstream loaded (_sa_ in /tmp/evta_bitstream)
  static int on = -1;
  if (on < 0) {
    char name[256] = {0};
    FILE *f = fopen("/tmp/evta_bitstream", "r");
    on = f && fgets(name, sizeof(name), f) && strstr(name, "_sa_") && strncmp(name, "FAILED", 6);
    if (f) fclose(f);
  }
  return on;
}
static uint32_t evtaMinTile(uint32_t dflt) {
  static int m = -1;
  if (m < 0) {
    const char *e = getenv("EVTA_MIN_TILE");
    m = e && *e ? std::max(1, atoi(e)) : evtaSystolic() ? 1 : 0;
  }
  return m ? m : dflt;
}
struct BiasResetContext {
  uint32_t base_, bias_, T_, nF_;
  bool operator==(BiasResetContext &op) { return base_ == op.base_ && bias_ == op.bias_ && T_ == op.T_ && nF_ == op.nF_; }
};
int bias_reset_acc(void *param, VTACommandHandle cmdh) {
  BiasResetContext *p = (BiasResetContext *)(param);
  VTAUopLoopBegin(cmdh, p->nF_, p->T_, 1, 0);
  VTAUopLoopBegin(cmdh, p->T_, 1, 0, 0);
  VTAUopPush(cmdh, 0, 2, p->base_, p->bias_, 0, 0, 0, 0);
  VTAUopLoopEnd(cmdh);
  VTAUopLoopEnd(cmdh);
  return 0;
}
struct BiasResetUopHandle {
  BiasResetContext ctx;
  void *uopHandle[2];
  BiasResetUopHandle() { uopHandle[0] = nullptr; }
};
std::vector<BiasResetUopHandle *> vBiasResetUopHandle[4];
static void pushBiasReset(VTACommandHandle &cmdh, uint32_t idEVTA, uint32_t base, uint32_t bias, uint32_t T, uint32_t nF) {
  BiasResetContext ctx = {base, bias, T, nF};
  void **h = nullptr;
  for (auto u : vBiasResetUopHandle[idEVTA])
    if (u->ctx == ctx) { h = u->uopHandle; break; }
  if (!h) {
    auto u = new BiasResetUopHandle();
    u->ctx = ctx;
    h = u->uopHandle;
    vBiasResetUopHandle[idEVTA].push_back(u);
  }
  VTAPushGEMMOp(cmdh, h, &bias_reset_acc, &ctx, 0);
}

//#### JO added (2026-11-08, docs/evta_runtime_compare.md 7.21): depthwise conv on the EVTA (dw_convolution_evta). One
//#### 16-channel block of the EVTA layout N{C/16}HW16 is a plane of H x W x 16, so the depthwise conv is a 16 -> 16 conv
//#### per block whose weights are diagonal (diag(w[c][ky][kx]), one wgt entry per tap, written by VTASave): conv_kernel
//#### with one input tile per output block (LOADed per block into the thread's inp half, gemmop_mxn in_kernel_factor_)
//#### and no loop over input blocks. The per-channel power-of-two scales are a per-channel shift: the shifts (int32, one
//#### acc entry of 16 per block, after the bias entries) are LOADed into acc and an ALU SHR with that vector operand
//#### (dst = the tile, src = the block's shift entry) shifts the sum; the round half up is 2^(shift - 1) in the bias
//#### (VTASave). The clip to [lo, hi] (relu6) by the requantize of that SHR (rq shift 0) or ALU MAX / MIN.
//#### 1/16 of the GEMM's MACs are used, but the A53's NEON (no int8 dot product) is slower still.
struct ConvDw {
  void *shift;   // int32 per channel, VTA buffer
  int lo, hi;
};
// acc[base + f * T + i] >>= acc[src + f], i < T, f < nF
struct DwShrContext {
  uint32_t base_, src_, T_, nF_;
  bool operator==(DwShrContext &op) { return base_ == op.base_ && src_ == op.src_ && T_ == op.T_ && nF_ == op.nF_; }
};
int alu_dw_shr(void *param, VTACommandHandle cmdh) {
  DwShrContext *p = (DwShrContext *)(param);
  VTAUopLoopBegin(cmdh, p->nF_, p->T_, 1, 0);
  VTAUopLoopBegin(cmdh, p->T_, 1, 0, 0);
  VTAUopPush(cmdh, 1, 0, p->base_, p->src_, 0, 3, 0, 0);
  VTAUopLoopEnd(cmdh);
  VTAUopLoopEnd(cmdh);
  return 0;
}
struct DwShrUopHandle {
  DwShrContext ctx;
  void *uopHandle[2];
  DwShrUopHandle() { uopHandle[0] = nullptr; }
};
std::vector<DwShrUopHandle *> vDwShrUopHandle[4];
static void pushDwShr(VTACommandHandle &cmdh, uint32_t idEVTA, uint32_t base, uint32_t src, uint32_t T, uint32_t nF) {
  DwShrContext ctx = {base, src, T, nF};
  void **h = nullptr;
  for (auto u : vDwShrUopHandle[idEVTA])
    if (u->ctx == ctx) { h = u->uopHandle; break; }
  if (!h) {
    auto u = new DwShrUopHandle();
    u->ctx = ctx;
    h = u->uopHandle;
    vDwShrUopHandle[idEVTA].push_back(u);
  }
  VTAPushALUOp(cmdh, h, &alu_dw_shr, &ctx, 0);
}

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
                 uint32_t nVirtualThread = 1,
                 const ConvResidual *res = nullptr,  //#### JO added (2026-10-31): res
                 const ConvPool *pool = nullptr,     //#### JO added (2026-11-02): pool
                 const ConvDw *dw = nullptr) {       //#### JO added (2026-11-08): depthwise

  uint32_t resizedOutTileH = (sizeInTileH-KH) / stride_size + 1;
  uint32_t resizedOutTileW = (sizeInTileW-KW) / stride_size + 1;
  uint32_t resizedInTileH =  (resizedOutTileH - 1) * stride_size + KH;
  uint32_t resizedInTileW =  (resizedOutTileW - 1) * stride_size + KW;


  uint32_t ACC_MEM_ENTRY = 2048;
  uint32_t INP_MEM_ENTRY = 2048;
  uint32_t WGT_MEM_ENTRY = 1024;

  //#### JO changed (2026-11-05): a thread's half of acc holds the tile padded to the minimum (padT) x nFilterinLoop entries
  //#### and the nFilterinLoop bias entries after them (not with the bias reset: the reset of thread 1 rewrites those rows) (was: the tile before the padding, no
  //#### bias entries; the bias LOAD of thread 0 overwrote the first rows of thread 1 when T x nFilterinLoop filled the half,
  //#### VGG16 conv0 nVirtualThread 2 / 16 x 32 on the HLS GEMM)
  const uint32_t padT = max(resizedOutTileH, evtaMinTile(MIN_H_SIZE)) * max(resizedOutTileW, evtaMinTile(MIN_W_SIZE));
  //#### JO changed (2026-11-06): with the bias reset too when the last thread's bias entries (its base + T x nFilterinLoop)
  //#### would lie past the physical acc: the multi-EVTA buffers have 2048 acc entries (the single ones 4096), VGG16 conv0
  //#### (nVirtualThread 2, 16 x 32, nFilterinLoop 2) on the multi4 systolic bitstream was wrong
  auto pickNF = [&](uint32_t rows) {
    uint32_t n = min(KN / nVirtualThread / 16, ACC_MEM_ENTRY / nVirtualThread / (padT + rows));
    for (; n > 0; n--)
      if ((KN / 16) % (n * nVirtualThread) == 0) break;
    return n;
  };
  uint32_t biasRows = bias && !evtaBiasReset() ? 1 : 0;
  uint32_t nFilterinLoop = pickNF(biasRows);
  if (bias && !biasRows &&
      (nVirtualThread - 1) * (ACC_MEM_ENTRY / nVirtualThread) + padT * nFilterinLoop + nFilterinLoop > ACC_BUFF_DEPTH)
    nFilterinLoop = pickNF(biasRows = 1);
  //#### JO added (2026-11-08, depthwise): per thread nF blocks: acc nF x (tile + bias + shift entry), inp nF input tiles,
  //#### wgt nF x KH x KW entries (dw_convolution_evta checked that one block fits)
  const uint32_t cBlocks = dw ? 1 : numInputChannel / 16;
  if (dw) {
    const uint32_t minH0 = evtaMinTile(MIN_H_SIZE), minW0 = evtaMinTile(MIN_W_SIZE);
    const uint32_t inTile0 = ((max(resizedOutTileH, minH0) - 1) * stride_size + KH) *
                             ((max(resizedOutTileW, minW0) - 1) * stride_size + KW);
    uint32_t n = min(KN / nVirtualThread / 16, ACC_MEM_ENTRY / nVirtualThread / (padT + 2));
    n = min(n, INP_MEM_ENTRY / nVirtualThread / inTile0);
    n = min(n, WGT_MEM_ENTRY / nVirtualThread / (KH * KW));
    for (; n > 0; n--)
      if ((KN / 16) % (n * nVirtualThread) == 0) break;
    nFilterinLoop = n;
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
    //#### JO changed (2026-11-05): evtaMinTile (was MIN_H_SIZE / MIN_W_SIZE)
    const uint32_t minH = evtaMinTile(MIN_H_SIZE), minW = evtaMinTile(MIN_W_SIZE);
    if (resizedOutTileH < minH) {
      outDummyH = minH - resizedOutTileH;
      resizedOutTileH = minH;
      resizedInTileH = (resizedOutTileH - 1) * stride_size + KH;
      inDummyH = resizedInTileH - sizeInTileH;
    }

    if (resizedOutTileW < minW) {
      outDummyW = minW - resizedOutTileW;
      resizedOutTileW = minW;
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
    //#### JO added (2026-11-04): bias reset (evtaBiasReset): the bias LOAD of the bias / shift / relu part below comes here
    const bool biasReset = bias && evtaBiasReset();
    //#### JO added (2026-11-04, requantize, evtaRequant): rqConv: the GEMMs of this tile (and the bias ADD without the bias
    //#### reset) write the requantized int8 (out = clip(acc >> shift, relu ? 0 : -128, 127), what SHR / MAX / MIN did);
    //#### rqPool: the column MAX passes of the fused pool write it (pool on the raw acc: max and requantize commute, the
    //#### pooled area starts at 0 = relu); rqRes: the residual GEMM writes it (round + sumShr + clip after the residual add)
    const bool rqOn = evtaRequant();
    const bool rqConv = rqOn && !res && !pool && !dw, rqPool = rqOn && pool && relu, rqRes = rqOn && res && !res->reluShl;
    //#### JO changed (2026-11-06): the relu's upper clip (gEvtaClipHi)
    // if (rqConv) VTARequant(vtaCmdH, 1, 0, shift, relu ? 0 : -128, 127);
    if (rqConv) VTARequant(vtaCmdH, 1, 0, shift, relu ? 0 : -128, relu ? gEvtaClipHi : 127);
    // Push operations for resetting Acc memory
    for (uint32_t threadIdx = 0; threadIdx < nVirtualThread; threadIdx++) {
      // acc buffer reset
      uint32_t outputSramBaseAddr = threadIdx * ACC_MEM_ENTRY / nVirtualThread;
      if (biasReset) {
        uint32_t T = resizedOutTileH * resizedOutTileW;
        uint32_t sram_offset = outputSramBaseAddr + T * nFilterinLoop;
        VTALoadBuffer2D(vtaCmdH, (int32_t *)vta_bias_buf,
                        filterIdx * nVirtualThread * nFilterinLoop + threadIdx * nFilterinLoop,
                        nFilterinLoop, 1, 16 * nFilterinLoop, 0, 0, 0, 0, sram_offset, 3);
        pushBiasReset(vtaCmdH, idEVTA, outputSramBaseAddr, sram_offset, T, nFilterinLoop);
        VTADepPush(vtaCmdH, 2, 1);
        continue;
      }
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
    for (uint32_t channelIdx = 0; channelIdx < cBlocks;
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
            filterIdx * nVirtualThread * nFilterinLoop * cBlocks * KH * KW +
            threadIdx * nFilterinLoop * cBlocks * KH * KW +
            channelIdx * KH * KW;

        uint32_t weightSramBaseAddr =
            threadIdx * (WGT_MEM_ENTRY / nVirtualThread);
        VTADepPop(vtaCmdH, 2, 1);
        //#### JO added (2026-11-08, depthwise): each output block's own input block, into the thread's inp half
        if (dw) {
          const uint32_t inTile = resizedInTileH * resizedInTileW;
          for (uint32_t fIdx = 0; fIdx < nFilterinLoop; fIdx++) {
            const uint32_t blk = (filterIdx * nVirtualThread + threadIdx) * nFilterinLoop + fIdx;
            VTALoadBuffer2D(vtaCmdH, vta_input_buf,
                            blk * inputHStride * inputWStride + inputHOffset * inputWStride + inputWOffset, x_size,
                            y_size, inputWStride, x_pad_before, y_pad_before, resizedInTileW - x_pad_before - x_size,
                            resizedInTileH - y_pad_before - y_size,
                            threadIdx * (INP_MEM_ENTRY / nVirtualThread) + fIdx * inTile, 2);
          }
        } else
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
                        nFilterinLoop, KH * KW * cBlocks, 0, 0, 0,
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
        if (dw) {   //#### JO added (2026-11-08)
          gemm_ctx.input_base_addr_ = threadIdx * (INP_MEM_ENTRY / nVirtualThread);
          gemm_ctx.in_kernel_factor_ = resizedInTileH * resizedInTileW;
        }

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

    //#### JO added (2026-10-31): residual of the fused elementadd (ConvResidual): the identity weight, then per thread the
    //#### residual tile into the inp buffer at the acc layout of the output tile (the addresses of the store below);
    //#### the pops above make these loads wait for the GEMMs of this tile
    if (res) {
      VTALoadBuffer2D(vtaCmdH, res->idWgt, res->resShl, 1, 1, 1, 0, 0, 0, 0, RES_WGT_ENTRY, 1);
      for (uint32_t threadIdx = 0; threadIdx < nVirtualThread; threadIdx++) {
        uint32_t resSramBaseAddr = threadIdx * (INP_MEM_ENTRY / nVirtualThread);
        if (outDummyW == 0) {
          for (uint32_t fIdx = 0; fIdx < nFilterinLoop; fIdx++)
            VTALoadBuffer2D(vtaCmdH, res->buf,
                            (filterIdx * nVirtualThread + threadIdx) * nFilterinLoop * out_w * out_h +
                                outputHOffset * out_w + outputWOffset + fIdx * out_w * out_h,
                            sizeOutTileW, sizeOutTileH, out_w, 0, 0, 0, 0,
                            resSramBaseAddr + fIdx * resizedOutTileH * resizedOutTileW, 2);
        } else {
          for (uint32_t fIdx = 0; fIdx < nFilterinLoop; fIdx++)
            for (uint32_t hIdx = 0; hIdx < sizeOutTileH; hIdx++)
              VTALoadBuffer2D(vtaCmdH, res->buf,
                              ((filterIdx * nVirtualThread + threadIdx) * nFilterinLoop + fIdx) * out_w * out_h +
                                  (outputHOffset + hIdx) * out_w + outputWOffset,
                              sizeOutTileW, 1, out_w, 0, 0, 0, 0,
                              resSramBaseAddr + (hIdx + fIdx * sizeOutTileH) * resizedOutTileW +
                                  fIdx * outDummyH * resizedOutTileW,
                              2);
        }
        VTADepPush(vtaCmdH, 1, 2);
      }
    }

    // Push operations for adding bias, shifting and relu
    for (uint32_t threadIdx = 0; threadIdx < nVirtualThread; threadIdx++) {

      uint32_t outputSramBaseAddr = threadIdx * ACC_MEM_ENTRY / nVirtualThread;
      uint32_t inputSramBaseAddr = threadIdx * (INP_MEM_ENTRY / nVirtualThread);
      uint32_t weightSramBaseAddr =
          threadIdx * (WGT_MEM_ENTRY / nVirtualThread);
      //#### JO changed (2026-11-04): not with the bias reset (the bias is already in acc)
      //if(bias) {
      if(bias && !biasReset) {
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

      //#### JO added (2026-11-08, depthwise): the per-channel shift (ALU SHR by the shift entries), clip to [lo, hi]
      if (dw) {
        const uint32_t T = resizedOutTileH * resizedOutTileW;
        const uint32_t shiftSram = outputSramBaseAddr + T * nFilterinLoop + nFilterinLoop;
        VTALoadBuffer2D(vtaCmdH, dw->shift, filterIdx * nVirtualThread * nFilterinLoop + threadIdx * nFilterinLoop,
                        nFilterinLoop, 1, 16 * nFilterinLoop, 0, 0, 0, 0, shiftSram, 3);
        if (evtaRequant()) {
          VTARequant(vtaCmdH, 1, 0, 0, dw->lo, dw->hi);
          pushDwShr(vtaCmdH, idEVTA, outputSramBaseAddr, shiftSram, T, nFilterinLoop);
          VTARequant(vtaCmdH, 0, 0, 0, 0, 0);
        } else {
          pushDwShr(vtaCmdH, idEVTA, outputSramBaseAddr, shiftSram, T, nFilterinLoop);
          pushAlu(vtaCmdH, idEVTA, outputSramBaseAddr, outputSramBaseAddr, T * nFilterinLoop, RES_ALU_MAX, 1, dw->lo);
          pushAlu(vtaCmdH, idEVTA, outputSramBaseAddr, outputSramBaseAddr, T * nFilterinLoop, RES_ALU_MIN, 1, dw->hi);
        }
      } else
      //#### JO added (2026-11-04, requantize): no SHR / MAX / MIN when the out write requantizes
      if (!rqConv && !rqPool) {
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
      //#### JO added (2026-11-06): the relu's upper clip (gEvtaClipHi < 127: ReLU6)
      if (relu && gEvtaClipHi < 127)
        pushAlu(vtaCmdH, idEVTA, outputSramBaseAddr, outputSramBaseAddr,
                resizedOutTileH * resizedOutTileW * nFilterinLoop, RES_ALU_MIN, 1, gEvtaClipHi);
      }   //#### JO: !rqConv && !rqPool
      //#### JO added (2026-10-31): fused elementadd (+ relu) on the requantized conv output (ConvResidual)
      if (res) {
        uint32_t size = resizedOutTileH * resizedOutTileW * nFilterinLoop;
        uint32_t resSramBaseAddr = threadIdx * (INP_MEM_ENTRY / nVirtualThread);
        for (uint32_t i = 0; i < res->convShl; i++)
          pushAlu(vtaCmdH, idEVTA, outputSramBaseAddr, outputSramBaseAddr, size, RES_ALU_ADD, 0, 0);
        VTADepPop(vtaCmdH, 1, 2);
        //#### JO added (2026-11-04, requantize): rqRes: the residual GEMM writes round / shift / clip of the sum to out
        if (rqRes) {
          VTARequant(vtaCmdH, 1, res->sumShr ? 1 : 0, res->sumShr, res->relu ? 0 : -128, 127);
          pushResGemm(vtaCmdH, idEVTA, outputSramBaseAddr, resSramBaseAddr, size);
          VTARequant(vtaCmdH, 0, 0, 0, 0, 0);
        } else {
        pushResGemm(vtaCmdH, idEVTA, outputSramBaseAddr, resSramBaseAddr, size);
        if (res->sumShr) {
          pushAlu(vtaCmdH, idEVTA, outputSramBaseAddr, outputSramBaseAddr, size, RES_ALU_ADD, 1,
                  1 << (res->sumShr - 1));
          pushAlu(vtaCmdH, idEVTA, outputSramBaseAddr, outputSramBaseAddr, size, RES_ALU_SHR, 1, res->sumShr);
        }
        pushAlu(vtaCmdH, idEVTA, outputSramBaseAddr, outputSramBaseAddr, size, RES_ALU_MAX, 1,
                res->relu ? 0 : -128);
        //#### JO added (2026-11-05): relu to a scale 2^reluShl smaller: shift left (SHR by a negative amount) before the clip
        if (res->reluShl)
          pushAlu(vtaCmdH, idEVTA, outputSramBaseAddr, outputSramBaseAddr, size, RES_ALU_SHR, 1, -(int)res->reluShl);
        pushAlu(vtaCmdH, idEVTA, outputSramBaseAddr, outputSramBaseAddr, size, RES_ALU_MIN, 1, 127);
        }
      }
      //#### JO added (2026-11-02): fused max pool (ConvPool): pooled area after the bias entries
      if (pool) {
        const uint32_t T = resizedOutTileH * resizedOutTileW, P = pool->pth * pool->ptw;
        const uint32_t pooled = outputSramBaseAddr + T * nFilterinLoop + nFilterinLoop;
        const uint32_t TW = resizedOutTileW, nF = nFilterinLoop;
        pushReset(vtaCmdH, idEVTA, pooled, P * nF);
        // rows: centre column 2j + haloW takes the column left (j >= 1 without the halo) and right of it
        const uint32_t j0 = pool->haloW ? 0 : 1, centre = outputSramBaseAddr + pool->haloW;
        pushPoolMax2(vtaCmdH, idEVTA, {centre + 2 * j0, centre + 2 * j0 - 1, resizedOutTileH, pool->ptw - j0, TW, 2, TW,
                                       2, nF, T, T});
        pushPoolMax2(vtaCmdH, idEVTA, {centre, centre + 1, resizedOutTileH, pool->ptw, TW, 2, TW, 2, nF, T, T});
        // cols: pooled (i, j) takes centre column j of rows 2i + haloH + dy - 1 (i >= 1 for dy = 0 without the halo)
        //#### JO added (2026-11-04, requantize): rqPool: these passes write the requantized max to out
        if (rqPool) VTARequant(vtaCmdH, 1, 0, shift, 0, 127);
        for (uint32_t dy = 0; dy < 3; dy++) {
          uint32_t i0 = (!pool->haloH && dy == 0) ? 1 : 0, r = 2 * i0 + pool->haloH + dy - 1;
          pushPoolMax2(vtaCmdH, idEVTA, {pooled + i0 * pool->ptw, centre + r * TW, pool->pth - i0, pool->ptw,
                                         pool->ptw, 1, 2 * TW, 2, nF, P, T});
        }
        if (rqPool) VTARequant(vtaCmdH, 0, 0, 0, 0, 0);
      }
      VTADepPush(vtaCmdH, 2, 3);
    }
    if (rqConv) VTARequant(vtaCmdH, 0, 0, 0, 0, 0);   //#### JO added (2026-11-04)

    // Push operations for storing the results
    for (uint32_t threadIdx = 0; threadIdx < nVirtualThread; threadIdx++) {

      uint32_t outputSramBaseAddr = threadIdx * ACC_MEM_ENTRY / nVirtualThread;
      VTADepPop(vtaCmdH, 2, 3);

      //#### JO added (2026-11-02): fused max pool: the pooled area, not the conv tile
      if (pool) {
        const uint32_t T = resizedOutTileH * resizedOutTileW, P = pool->pth * pool->ptw;
        const uint32_t pooled = outputSramBaseAddr + T * nFilterinLoop + nFilterinLoop;
        for (uint32_t fIdx = 0; fIdx < nFilterinLoop; fIdx++)
          VTAStoreBuffer2D(vtaCmdH, pooled + fIdx * P, 4, pool->out,
                           ((filterIdx * nVirtualThread + threadIdx) * nFilterinLoop + fIdx) * pool->PH * pool->PW +
                               pool->py0 * pool->PW + pool->px0,
                           pool->ptw, pool->pth, pool->PW);
      } else if (outDummyW == 0) {
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
            VTAStoreBuffer2D(vtaCmdH, outputSramBaseAddr + (hIdx+fIdx*sizeOutTileH) * resizedOutTileW + fIdx * outDummyH * resizedOutTileW,
                             4, vta_output_buf,
                             ((filterIdx * nVirtualThread + threadIdx) * nFilterinLoop + fIdx) *
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
            VTAStoreBuffer2D(vtaCmdH, outputSramBaseAddr + (hIdx+fIdx*sizeOutTileH) * resizedOutTileW + fIdx * outDummyH * resizedOutTileW,
                             4, vta_output_buf,
                             ((filterIdx * nVirtualThread + threadIdx) * nFilterinLoop + fIdx) *
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
  vBiasResetUopHandle[idEVTA].clear();   //#### JO added (2026-11-04)
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

//#### JO added (2026-11-05): the input of an EVTA conv written while the conv runs (preprocess_conv0pack_stream, the
//#### packed conv0 input: 1.3 ms on one A53 core for Resnet18 / 50, mostly NEON quantize and byte shuffles, not DRAM).
//#### The conv that reads buf (convolution_wo_tr_impl) makes one EVTA run per tile row, tagged with the input rows the
//#### row reads (VTASetSegmentTag); its replay calls inputStreamHook with the next run's rows while a run is running
//#### (cmd_queue.h SetSegmentHook), so the CPU packs and flushes those rows meanwhile. The recording call (the first)
//#### writes all rows first. One stream pending per thread (multi-EVTA copies). EVTA_INPUT_STREAM=0 writes the rows at once.
struct InputStream {
  void *buf;
  int rows, done, W2, CB;
  // the preprocess_conv0pack_rows arguments
  const float *input;
  int N, H, W, C, nchw;
  float scale;
  int32_t offset;
  const int8_t *lut;
  int KH, KW, stride, pad, CO, padEnd;
};
static thread_local InputStream gInputStream = {nullptr};

// rows 0 .. rows - 1 (-1 = all) written and flushed
static void inputStreamTo(InputStream *s, int rows) {
  rows = rows < 0 ? s->rows : std::min(rows, s->rows);
  if (rows <= s->done)
    return;
  preprocess_conv0pack_rows(s->input, (int8_t *)VTABufferGetVirtAddr(s->buf), s->N, s->H, s->W, s->C, s->nchw,
                            s->scale, s->offset, s->lut, s->KH, s->KW, s->stride, s->pad, s->CO, s->done, rows,
                            s->padEnd);
  const size_t rowBytes = (size_t)s->W2 * 16;
  for (int b = 0; b < s->N * s->CB; b++)
    VTABufferFlush(s->buf, ((size_t)b * s->rows + s->done) * rowBytes, (size_t)(rows - s->done) * rowBytes);
  s->done = rows;
}
static void inputStreamHook(void *ctx, int tag) { inputStreamTo(static_cast<InputStream *>(ctx), tag); }
static void inputStreamEnd(InputStream *s) {
  inputStreamTo(s, -1);
  s->buf = nullptr;
}

//#### JO changed (2026-11-07): + padEnd (VTABundle.h)
void preprocess_conv0pack_stream(const float *input, void *buf, int N, int H, int W, int C, int nchw, float scale,
                                 int32_t offset, const int8_t *lut, int KH, int KW, int stride, int pad, int CO,
                                 int padEnd) {
  if (gInputStream.buf)
    inputStreamEnd(&gInputStream);  // one that no conv read
  static int on = -1;
  if (on < 0) {
    const char *e = getenv("EVTA_INPUT_STREAM");
    on = !e || atoi(e) != 0;
  }
  const int KH2 = (KH + stride - 1) / stride, pe = padEnd < 0 ? pad : padEnd;
  gInputStream = {buf, (H + pad + pe - KH) / stride + KH2, 0, (W + pad + pe - KW) / stride + 1, CO / 16,
                  input, N, H, W, C, nchw, scale, offset, lut, KH, KW, stride, pad, CO, padEnd};
  if (!on || N != 1)
    inputStreamEnd(&gInputStream);
}

//#### JO changed (2026-10-31): body of convolution_wo_tr and convolution_wo_tr_res (res = nullptr: plain conv)
//#### JO added (2026-11-02): EVTA_DUMP_CONV=<dir> writes the output of every EVTA conv call (the EVTA layout as the
//#### buffer holds it) to <dir>/conv_<n>.bin, n counting the calls of the process from 0: compares two bundles' layers
static void dumpConvOutput(void *output, size_t bytes) {
  static const char *dir = getenv("EVTA_DUMP_CONV");
  static std::atomic<int> n{0};
  if (!dir)
    return;
  VTABufferInvalidate(output, 0, bytes);
  char name[512];
  snprintf(name, sizeof(name), "%s/conv_%d.bin", dir, n++);
  FILE *fp = fopen(name, "wb");
  if (fp) {
    fwrite(VTABufferGetVirtAddr(output), 1, bytes, fp);
    fclose(fp);
  }
}

//#### JO added (2026-11-05): schedule override at run time, to try schedules on the board without a new bundle.
//#### EVTA_SCHED=<H>,<W>,<C>,<KN>,<K>,<stride>:<nVirtualThread>,<tileH>,<tileW>[;...] replaces the bundle's schedule of the convs
//#### of that shape (input H x W x C, KN filters K x K); EVTA_SCHED_LOG=1 prints each conv's shape and schedule once.
//#### A schedule whose tiles do not divide the output can change the result: compare EVTA_DUMP_CONV dumps.
static void schedOverride(int H, int W, int C, int KN, int KH, int stride, int &vt, int &th, int &tw) {
  struct Sched { int key[6], vt, th, tw; };
  static std::vector<Sched> table;
  static int log = -1;
  static std::mutex mu;
  std::lock_guard<std::mutex> lock(mu);
  if (log < 0) {
    const char *l = getenv("EVTA_SCHED_LOG");
    log = l && atoi(l) > 0;
    if (const char *e = getenv("EVTA_SCHED")) {
      std::string all(e);
      size_t b = 0;
      while (b < all.size()) {
        size_t n = all.find(';', b);
        std::string one = all.substr(b, n == std::string::npos ? std::string::npos : n - b);
        Sched t;
        if (sscanf(one.c_str(), "%d,%d,%d,%d,%d,%d:%d,%d,%d", &t.key[0], &t.key[1], &t.key[2], &t.key[3], &t.key[4],
                   &t.key[5], &t.vt, &t.th, &t.tw) == 9)
          table.push_back(t);
        else if (!one.empty())
          printf("EVTA_SCHED: cannot parse '%s'\n", one.c_str());
        if (n == std::string::npos) break;
        b = n + 1;
      }
    }
  }
  const int key[6] = {H, W, C, KN, KH, stride};
  int vt0 = vt, th0 = th, tw0 = tw;
  for (auto &t : table)
    if (!memcmp(t.key, key, sizeof(key))) { vt = t.vt; th = t.th; tw = t.tw; break; }
  if (log) {
    static std::vector<std::vector<int>> seen;
    std::vector<int> k = {H, W, C, KN, KH, stride, vt, th, tw};
    bool first = true;
    for (auto &x : seen)
      if (x == k) { first = false; break; }
    if (first) {
      seen.push_back(k);
      printf("EVTA_SCHED %d,%d,%d,%d,%d,%d:%d,%d,%d%s\n", H, W, C, KN, KH, stride, vt, th, tw,
             (vt != vt0 || th != th0 || tw != tw0) ? " (override)" : "");
    }
  }
}

static int convolution_wo_tr_impl(int8_t* input, int8_t *kernel, int32_t *bias, int8_t *output, int N, int H, int W, int C, int KN, int KH, int KW, int pad_size,
                      int stride_size, bool doRelu, bool doBias, uint8_t shift, int out_h, int out_w, void* vtaCmdH, int nVirtualThread, int tileHSize, int tileWSize, uint32_t idEVTA,
                      const ConvResidual *res, const ConvPool *pool = nullptr, const ConvDw *dw = nullptr) {
  //#### JO changed (2026-11-08): a depthwise conv's schedule under KN 0 (dw_convolution_evta checks it after the override)
  if (!dw)
  schedOverride(H, W, C, KN, KH, stride_size, nVirtualThread, tileHSize, tileWSize);  //#### JO added (2026-11-05)
  //#### JO added (2026-11-05): input written while this call runs (InputStream): one run per tile row
  InputStream *ins = gInputStream.buf && gInputStream.buf == (void *)input && N == 1 ? &gInputStream : nullptr;
  const bool stream = ins != nullptr;
  auto streamRows = [&](uint32_t tileHIdx, uint32_t tileH) {
    int o1 = std::min<int>(out_h, (tileHIdx + 1) * tileH);
    return std::min(H, (o1 - 1) * stride_size + KH - pad_size);
  };

  int8_t *in_NC_HW16 = input;
  int8_t *filter_N_C_HW1616 = kernel;
#ifdef VTA_DEBUG_MODE
  clock_t conv_stage0_start = clock();
#endif

  void *vta_input_buf = input;
  void *vta_filter_buf = kernel;
  void *vta_output_buf = output;
  void *vta_bias_buf = bias;

  //#### JO added (2026-11-02): a call with the same arguments as an earlier one runs that one's recorded instructions
  //#### (cmd_queue.h ReplayBegin); the buffers are relocated
  int32_t replayKey[] = {1, N, H, W, C, KN, KH, KW, pad_size, stride_size, doRelu, doBias, shift, out_h, out_w,
                         nVirtualThread, tileHSize, tileWSize, (int32_t)idEVTA, res != nullptr,
                         res ? (int32_t)res->resShl : 0, res ? (int32_t)res->convShl : 0,
                         res ? (int32_t)res->sumShr : 0, res ? (int32_t)res->relu + 2 * (int32_t)res->reluShl : 0, res ? res->buf == output : 0,
                         pool != nullptr, stream, gEvtaClipHi,   //#### JO: + gEvtaClipHi (2026-11-06)
                         dw != nullptr, dw ? dw->lo : 0, dw ? dw->hi : 0};   //#### JO: + depthwise (2026-11-08)
  //#### JO changed (2026-11-02): a fused max pool writes its output, not the conv's
  void *replayBufs[] = {input, kernel, doBias ? bias : nullptr, pool ? pool->out : (void *)output, res ? res->buf : nullptr,
                        dw ? dw->shift : nullptr};
  void *dumpBuf = pool ? pool->out : (void *)output;
  const size_t dumpBytes = pool ? (size_t)KN * pool->PH * pool->PW : (size_t)N * KN * out_h * out_w;
  if (stream)
    VTASetSegmentHook(vtaCmdH, inputStreamHook, ins);
  const int replayed = VTAReplayBegin(vtaCmdH, replayKey, sizeof(replayKey), replayBufs, 6);
  if (stream) {
    VTASetSegmentHook(vtaCmdH, nullptr, nullptr);
    inputStreamEnd(ins);  // the rows a replay wrote; all of them before a recording
  }
  if (replayed) {
    dumpConvOutput(dumpBuf, dumpBytes);
    return 0;
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
  //#### JO added (2026-11-02): fused max pool (ConvPool): tiles with the extra row / column of the pool window
  //#### (convolution_wo_tr_pool checked the shapes: tiles even, dividing the output, no remainder tiles)
  for (uint32_t tileHIdx = 0; pool && tileHIdx < nTileH; tileHIdx++) {
    if (stream)
      VTASetSegmentTag(vtaCmdH, streamRows(tileHIdx, sizeOutTileH));
    for (uint32_t tileWIdx = 0; tileWIdx < nTileW; tileWIdx++) {
      ConvPool tp = *pool;
      tp.haloH = tileHIdx > 0;
      tp.haloW = tileWIdx > 0;
      tp.pth = sizeOutTileH / 2;
      tp.ptw = sizeOutTileW / 2;
      tp.py0 = tileHIdx * tp.pth;
      tp.px0 = tileWIdx * tp.ptw;
      uint32_t outH0 = tileHIdx * sizeOutTileH - tp.haloH, outW0 = tileWIdx * sizeOutTileW - tp.haloW;
      uint32_t tileH = sizeOutTileH + tp.haloH, tileW = sizeOutTileW + tp.haloW;
      conv_kernel(tileH, tileW, pad_size, stride_size,
                  tileWIdx == 0 ? pad_size : 0, tileWIdx == nTileW - 1 ? pad_size : 0,
                  tileHIdx == 0 ? pad_size : 0, tileHIdx == nTileH - 1 ? pad_size : 0,
                  outW0, outH0, (tileH - 1) * stride_size + KH, (tileW - 1) * stride_size + KW,
                  C, H, W, vta_input_buf, KN, KH, KW, vta_filter_buf, out_h, out_w, vta_output_buf, doRelu, shift,
                  doBias, vta_bias_buf, vtaCmdH, idEVTA, nVirtualThread, nullptr, &tp);
    }
    if (stream)
      VTASynchronize(vtaCmdH, 1 << 31);
  }
  for (int tileHIdx = 0; !pool && tileHIdx < nTileH; tileHIdx++) {
    if (stream)
      VTASetSegmentTag(vtaCmdH, streamRows(tileHIdx, sizeOutTileH));
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
                    vtaCmdH, idEVTA, nVirtualThread, res, nullptr, dw);
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
                    doBias, vta_bias_buf, vtaCmdH, idEVTA, nVirtualThread, res, nullptr, dw);
      }
    }
    if (stream)
      VTASynchronize(vtaCmdH, 1 << 31);
  }

#ifdef VTA_DEBUG_MODE
  clock_t conv_stage2_end = clock();
#endif
  if (stream)
    VTASetSegmentTag(vtaCmdH, -1);
  else
    VTASynchronize(vtaCmdH, 1 << 31);
  VTAReplayEnd(vtaCmdH, 1);






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
  dumpConvOutput(dumpBuf, dumpBytes);
  return 0;
}



int convolution_wo_tr(int8_t* input, int8_t *kernel, int32_t *bias, int8_t *output, int N, int H, int W, int C, int KN, int KH, int KW, int pad_size,
                      int stride_size, bool doRelu, bool doBias, uint8_t shift, int out_h, int out_w, void* vtaCmdH, int nVirtualThread, int tileHSize, int tileWSize, uint32_t idEVTA) {
  return convolution_wo_tr_impl(input, kernel, bias, output, N, H, W, C, KN, KH, KW, pad_size, stride_size, doRelu,
                                doBias, shift, out_h, out_w, vtaCmdH, nVirtualThread, tileHSize, tileWSize, idEVTA,
                                nullptr);
}

//#### JO added (2026-11-09): int8 C[M][N] = clip(floor((A[M][K] B[K][N] + 2^(shift-1)) / 2^shift)), both operands
//#### written at run time (the DeiT attention products Q K^T and A V; VTASave.cpp -vta-evta-matmul): a 1x1 conv over the
//#### M rows as an H x W image (W the largest divisor of M <= sqrt(M), as VTA.cpp matMulToConv), A as the input and B^T as
//#### the filter, K and N zero-padded to multiples of 16, bias 2^(shift-1) (round half up, as -vta-round-shift).
//#### The packing and unpacking are on the CPU. EVTA_MATMUL_CHECK=1 compares every output with the CPU integer result.
int matmul_evta(const int8_t *A, const int8_t *B, int8_t *C, int M, int K, int N, int shift, void *vtaCmdH,
                int nVirtualThread, int tileHSize, int tileWSize, uint32_t idEVTA) {
  const int Kp = (K + 15) / 16 * 16, Np = (N + 15) / 16 * 16, KB = Kp / 16, NB = Np / 16;
  int w = 1;
  for (int d = 1; d * d <= M; d++)
    if (M % d == 0)
      w = d;
  const int h = M / w;
  void *inBuf = VTABufferAlloc((size_t)M * Kp), *wgtBuf = VTABufferAlloc((size_t)Np * Kp);
  void *biasBuf = VTABufferAlloc((size_t)Np * 4), *outBuf = VTABufferAlloc((size_t)M * Np);
  int8_t *in = (int8_t *)VTABufferGetVirtAddr(inBuf), *wgt = (int8_t *)VTABufferGetVirtAddr(wgtBuf);
  int8_t *out = (int8_t *)VTABufferGetVirtAddr(outBuf);
  int32_t *bias = (int32_t *)VTABufferGetVirtAddr(biasBuf);
  // input N{C/16}HW16 with HW = the M rows: in[kb][m][16]
#pragma omp parallel for schedule(static)
  for (int kb = 0; kb < KB; kb++) {
    const int nk = std::min(16, K - kb * 16);
    int8_t *o = in + (size_t)kb * M * 16;
    if (nk == 16)
      for (int m = 0; m < M; m++)
        memcpy(o + m * 16, A + (size_t)m * K + kb * 16, 16);
    else
      for (int m = 0; m < M; m++) {
        memset(o + m * 16, 0, 16);
        memcpy(o + m * 16, A + (size_t)m * K + kb * 16, nk);
      }
  }
  // filter {N/16}{C/16}HW1616 with filter[n][k] = B[k][n]: wgt[nb][kb][n % 16][k % 16]
#pragma omp parallel for schedule(static)
  for (int nb = 0; nb < NB; nb++) {
    int8_t *o = wgt + (size_t)nb * KB * 256;
    memset(o, 0, (size_t)KB * 256);
    const int nn = std::min(16, N - nb * 16);
    for (int k = 0; k < K; k++) {
      const int8_t *b = B + (size_t)k * N + nb * 16;
      int8_t *ok = o + (k / 16) * 256 + (k % 16);
      for (int n = 0; n < nn; n++)
        ok[n * 16] = b[n];
    }
  }
  const bool doBias = shift > 0;
  for (int n = 0; n < Np; n++)
    bias[n] = doBias ? 1 << (shift - 1) : 0;
  VTABufferFlush(inBuf, 0, (size_t)M * Kp);
  VTABufferFlush(wgtBuf, 0, (size_t)Np * Kp);
  VTABufferFlush(biasBuf, 0, (size_t)Np * 4);
  int rc = convolution_wo_tr((int8_t *)inBuf, (int8_t *)wgtBuf, (int32_t *)biasBuf, (int8_t *)outBuf, 1, h, w, Kp, Np, 1,
                             1, 0, 1, false, doBias, (uint8_t)shift, h, w, vtaCmdH, nVirtualThread, tileHSize, tileWSize,
                             idEVTA);
  VTABufferInvalidate(outBuf, 0, (size_t)M * Np);
  // output N{C/16}HW16 -> C[M][N]
#pragma omp parallel for schedule(static)
  for (int m = 0; m < M; m++)
    for (int nb = 0; nb < NB; nb++)
      memcpy(C + (size_t)m * N + nb * 16, out + ((size_t)nb * M + m) * 16, std::min(16, N - nb * 16));
  static const bool check = [] {
    const char *e = getenv("EVTA_MATMUL_CHECK");
    return e && atoi(e) != 0;
  }();
  if (check) {
    size_t bad = 0;
    for (int m = 0; m < M; m++)
      for (int n = 0; n < N; n++) {
        int64_t acc = doBias ? 1 << (shift - 1) : 0;
        for (int k = 0; k < K; k++)
          acc += (int32_t)A[(size_t)m * K + k] * B[(size_t)k * N + n];
        acc >>= shift;
        const int want = (int)std::max<int64_t>(-128, std::min<int64_t>(127, acc));
        if (C[(size_t)m * N + n] != want && bad++ < 5)
          fprintf(stderr, "matmul_evta %dx%dx%d [%d][%d]: %d, want %d\n", M, K, N, m, n, C[(size_t)m * N + n], want);
      }
    fprintf(stderr, "matmul_evta %dx%dx%d shift %d: %zu differ\n", M, K, N, shift, bad);
  }
  VTABufferFree(inBuf);
  VTABufferFree(wgtBuf);
  VTABufferFree(biasBuf);
  VTABufferFree(outBuf);
  return rc;
}

//#### JO added (2026-10-31): conv + elementadd (+ relu) in one EVTA call, see ConvResidual. output may be residual
//#### (in place: each tile reads its residual before it stores), not input.
//#### JO added (2026-11-02): conv (with relu) + max pool 3x3 / stride 2 / pad 1 in one EVTA call (ConvPool). output: the
//#### conv's output buffer, written only when the shapes do not fit the fusion (then the conv, then maxpool_evta on it).
//#### EVTA_RES_CHECK=1: also runs the conv into output and the CPU maxpool on it, and compares with the fused output.
int convolution_wo_tr_pool(int8_t* input, int8_t *kernel, int32_t *bias, int8_t *output, int N, int H, int W, int C, int KN, int KH, int KW, int pad_size,
                           int stride_size, bool doRelu, bool doBias, uint8_t shift, int out_h, int out_w, void* vtaCmdH,
                           int nVirtualThread, int tileHSize, int tileWSize, uint32_t idEVTA,
                           int8_t *pool_out, int poolK, int poolStride, int poolPad) {
  const int PH = (out_h + 2 * poolPad - poolK) / poolStride + 1, PW = (out_w + 2 * poolPad - poolK) / poolStride + 1;
  // acc per thread: the tile with its halo, the bias entries and the pooled area, nFilterinLoop blocks as conv_kernel
  uint32_t nF = 0;
  if (nVirtualThread > 0 && tileHSize > 0 && tileWSize > 0) {
    nF = std::min<uint32_t>(KN / nVirtualThread / 16, 2048 / nVirtualThread / (tileHSize + 1) / (tileWSize + 1));
    for (; nF > 0; nF--)
      if ((KN / 16) % (nF * nVirtualThread) == 0)
        break;
  }
  const bool fits = N == 1 && doRelu && poolK == 3 && poolStride == 2 && poolPad == 1 && KN % 16 == 0 && nF > 0 &&
                    tileHSize % 2 == 0 && tileWSize % 2 == 0 && out_h % tileHSize == 0 && out_w % tileWSize == 0 &&
                    tileHSize + 1 >= MIN_H_SIZE && tileWSize + 1 >= MIN_W_SIZE &&
                    (tileHSize + 1) * (tileWSize + 1) * nF + nF + tileHSize * tileWSize / 4 * nF <= 2048 / (uint32_t)nVirtualThread;
  static int check = -1;
  if (check < 0) {
    const char *e = getenv("EVTA_RES_CHECK");
    check = e && atoi(e) > 0;
  }
  int rc = 0;
  if (fits) {
    ConvPool pool = {pool_out, (uint32_t)PH, (uint32_t)PW, 0, 0, 0, 0, false, false};
    rc = convolution_wo_tr_impl(input, kernel, bias, output, N, H, W, C, KN, KH, KW, pad_size, stride_size, doRelu,
                                doBias, shift, out_h, out_w, vtaCmdH, nVirtualThread, tileHSize, tileWSize, idEVTA,
                                nullptr, &pool);
  }
  if (!fits || check) {
    rc |= convolution_wo_tr(input, kernel, bias, output, N, H, W, C, KN, KH, KW, pad_size, stride_size, doRelu, doBias,
                            shift, out_h, out_w, vtaCmdH, nVirtualThread, tileHSize, tileWSize, idEVTA);
    if (!fits)
      return rc | maxpool_evta(output, pool_out, N, out_h, out_w, KN, poolK, poolK, poolPad, poolStride, vtaCmdH, idEVTA);
    size_t no = (size_t)KN * PH * PW, ni = (size_t)KN * out_h * out_w;
    VTABufferInvalidate(output, 0, ni);
    VTABufferInvalidate(pool_out, 0, no);
    std::vector<int8_t> ref(no);
    maxpool((int8_t *)VTABufferGetVirtAddr(output), ref.data(), KN / 16, out_h, out_w, 16, poolK, poolK, poolPad,
            poolStride);
    const int8_t *o = (const int8_t *)VTABufferGetVirtAddr(pool_out);
    size_t bad = 0, first = no;
    for (size_t i = 0; i < no; i++)
      if (o[i] != ref[i]) {
        if (!bad)
          first = i;
        bad++;
      }
    printf("EVTA_RES_CHECK conv %dx%dx%d -> %d + maxpool k%d s%d p%d: %zu of %zu differ", out_h, out_w, C, KN, poolK,
           poolStride, poolPad, bad, no);
    if (bad)
      printf(" (first at %zu: fused %d, conv + cpu maxpool %d)", first, o[first], ref[first]);
    printf("\n");
  }
  return rc;
}

int convolution_wo_tr_res(int8_t* input, int8_t *kernel, int32_t *bias, int8_t *output, int N, int H, int W, int C, int KN, int KH, int KW, int pad_size,
                          int stride_size, bool doRelu, bool doBias, uint8_t shift, int out_h, int out_w,
                          int8_t *residual, int convShl, int resShl, int sumShr, int sumRelu,
                          void* vtaCmdH, int nVirtualThread, int tileHSize, int tileWSize, uint32_t idEVTA) {
  //#### JO changed (2026-11-05): sumRelu 0 = no relu, 1 = relu, 1 + q = relu, then << q and clip to 127 (the relu's output
  //#### scale is the sum's / 2^q; was bool, the bundles generated before pass 0 / 1)
  if (convShl < 0 || convShl > 8 || resShl < 0 || resShl > RES_MAX_SHL || sumShr < 0 || sumShr > 15 || N != 1 ||
      sumRelu < 0 || sumRelu > 8) {
    printf("convolution_wo_tr_res: unsupported shifts conv %d residual %d sum %d (N %d)\n", convShl, resShl, sumShr, N);
    return -1;
  }
  ConvResidual res = {residual, getIdentityWeights(idEVTA), (uint32_t)resShl, (uint32_t)convShl, (uint32_t)sumShr,
                      sumRelu > 0, sumRelu > 1 ? (uint32_t)sumRelu - 1 : 0};
  //#### EVTA_RES_CHECK=1: also run the plain conv into a scratch buffer and the CPU elemadd + relu on it
  //#### (std::round as CPUBundle elemadd), compare with the fused output and print the mismatches
  static int check = -1;
  if (check < 0) {
    const char *e = getenv("EVTA_RES_CHECK");
    check = e && atoi(e) > 0;
  }
  if (!check)
    return convolution_wo_tr_impl(input, kernel, bias, output, N, H, W, C, KN, KH, KW, pad_size, stride_size, doRelu,
                                  doBias, shift, out_h, out_w, vtaCmdH, nVirtualThread, tileHSize, tileWSize, idEVTA,
                                  &res);
  size_t n = (size_t)out_h * out_w * KN;
  VTABufferInvalidate(residual, 0, n);
  std::vector<int8_t> r((int8_t *)VTABufferGetVirtAddr(residual), (int8_t *)VTABufferGetVirtAddr(residual) + n);
  int rc = convolution_wo_tr_impl(input, kernel, bias, output, N, H, W, C, KN, KH, KW, pad_size, stride_size, doRelu,
                                  doBias, shift, out_h, out_w, vtaCmdH, nVirtualThread, tileHSize, tileWSize, idEVTA,
                                  &res);
  void *tmp = VTABufferAlloc(n);
  convolution_wo_tr_impl(input, kernel, bias, (int8_t *)tmp, N, H, W, C, KN, KH, KW, pad_size, stride_size, doRelu,
                         doBias, shift, out_h, out_w, vtaCmdH, nVirtualThread, tileHSize, tileWSize, idEVTA, nullptr);
  VTABufferInvalidate(output, 0, n);
  VTABufferInvalidate(tmp, 0, n);
  const int8_t *o = (const int8_t *)VTABufferGetVirtAddr(output), *c = (const int8_t *)VTABufferGetVirtAddr(tmp);
  size_t bad = 0, first = n;
  for (size_t i = 0; i < n; i++) {
    float v = std::round(std::ldexp((float)c[i], convShl - sumShr) + std::ldexp((float)r[i], resShl - sumShr));
    v = v > 127 ? 127 : (v < -128 ? -128 : v);
    if (sumRelu && v < 0)
      v = 0;
    if (sumRelu > 1)
      v = std::min(v * (float)(1 << (sumRelu - 1)), 127.f);
    if ((int8_t)v != o[i]) {
      if (!bad)
        first = i;
      bad++;
    }
  }
  printf("EVTA_RES_CHECK %dx%dx%d -> %d, shl %d/%d shr %d: %zu of %zu differ", H, W, C, KN, convShl, resShl, sumShr,
         bad, n);
  if (bad)
    printf(" (first at %zu: fused %d, conv %d residual %d)", first, o[first], c[first], r[first]);
  printf("\n");
  VTABufferFree(tmp);
  return rc;
}


//#### JO added (2026-11-08, docs/evta_runtime_compare.md 7.21): depthwise conv (group = C = KN) on the EVTA, see ConvDw.
//#### input / output VTA buffers in the EVTA layout (C % 16 == 0), N = 1, square pads: pad = top = left, the bottom / right
//#### pad follows from out_h / out_w (TF SAME stride 2: pad 0, one row / column at the end), dilation 1. kernel: per
//#### 16-channel block KH x KW wgt entries diag(w[c][ky][kx]); bias int32 per channel with the rounding 2^(shift - 1);
//#### shift int32 per channel (0..31); out = clip((bias + sum) >> shift, lo, hi). The schedule (nVirtualThread, tiles)
//#### is shrunk until a block fits (an odd number of blocks: nVirtualThread 1); EVTA_SCHED takes it with KN 0.
//#### EVTA_RES_CHECK=1 compares with a CPU loop of the same arithmetic.
int dw_convolution_evta(int8_t *input, void *kernel, void *bias, void *shift, int8_t *output, int N, int H, int W, int C,
                        int KH, int KW, int pad, int stride, int out_h, int out_w, int lo, int hi, void *vtaCmdH,
                        int nVirtualThread, int tileHSize, int tileWSize, uint32_t idEVTA) {
  if (N != 1 || C % 16 || pad < 0 || pad >= KH || pad >= KW || (out_h - 1) * stride + KH > H + pad + KH - 1 ||
      (out_w - 1) * stride + KW > W + pad + KW - 1) {
    printf("dw_convolution_evta: unsupported shape N %d C %d pad %d out %dx%d\n", N, C, pad, out_h, out_w);
    return -1;
  }
  schedOverride(H, W, C, 0, KH, stride, nVirtualThread, tileHSize, tileWSize);
  if (nVirtualThread < 1 || (C / 16) % nVirtualThread) nVirtualThread = 1;
  tileHSize = std::max(1, std::min(tileHSize, out_h));
  tileWSize = std::max(1, std::min(tileWSize, out_w));
  // one block per thread: acc tile + 2 entries, inp input tile (minimum tile as conv_kernel pads to it)
  for (;;) {
    uint32_t th = std::max<uint32_t>(tileHSize, evtaMinTile(MIN_H_SIZE)), tw = std::max<uint32_t>(tileWSize, evtaMinTile(MIN_W_SIZE));
    uint32_t inT = ((th - 1) * stride + KH) * ((tw - 1) * stride + KW);
    if (th * tw + 2 <= 2048u / nVirtualThread && inT <= 2048u / nVirtualThread) break;
    if (tileWSize >= tileHSize && tileWSize > 1) tileWSize = (tileWSize + 1) / 2;
    else if (tileHSize > 1) tileHSize = (tileHSize + 1) / 2;
    else if (nVirtualThread > 1) nVirtualThread = 1;
    else { printf("dw_convolution_evta: no schedule\n"); return -1; }
  }
  ConvDw d = {shift, lo, hi};
  int rc = convolution_wo_tr_impl(input, (int8_t *)kernel, (int32_t *)bias, output, N, H, W, C, C, KH, KW, pad, stride,
                                  false, true, 0, out_h, out_w, vtaCmdH, nVirtualThread, tileHSize, tileWSize, idEVTA,
                                  nullptr, nullptr, &d);
  static int check = -1;
  if (check < 0) {
    const char *e = getenv("EVTA_RES_CHECK");
    check = e && atoi(e) > 0;
  }
  if (!check)
    return rc;
  const size_t ni = (size_t)C * H * W, no = (size_t)C * out_h * out_w, KK = KH * KW;
  VTABufferInvalidate(input, 0, ni);
  VTABufferInvalidate(output, 0, no);
  const int8_t *in = (const int8_t *)VTABufferGetVirtAddr(input), *o = (const int8_t *)VTABufferGetVirtAddr(output);
  const int8_t *k = (const int8_t *)VTABufferGetVirtAddr(kernel);
  const int32_t *b = (const int32_t *)VTABufferGetVirtAddr(bias), *sh = (const int32_t *)VTABufferGetVirtAddr(shift);
  size_t bad = 0, first = no;
  int got = 0, want = 0;
  for (int c = 0; c < C; c++)
    for (int y = 0; y < out_h; y++)
      for (int x = 0; x < out_w; x++) {
        int32_t acc = b[c];
        for (int ky = 0; ky < KH; ky++)
          for (int kx = 0; kx < KW; kx++) {
            int iy = y * stride - pad + ky, ix = x * stride - pad + kx;
            if (iy < 0 || iy >= H || ix < 0 || ix >= W) continue;
            int8_t w = k[((size_t)(c / 16) * KK + ky * KW + kx) * 256 + (c % 16) * 17];
            acc += (int32_t)w * in[(((size_t)(c / 16) * H + iy) * W + ix) * 16 + c % 16];
          }
        int32_t v = acc >> sh[c];
        v = v < lo ? lo : v > hi ? hi : v;
        size_t i = (((size_t)(c / 16) * out_h + y) * out_w + x) * 16 + c % 16;
        if (o[i] != (int8_t)v) {
          if (!bad) { first = i; got = o[i]; want = v; }
          bad++;
        }
      }
  printf("EVTA_RES_CHECK depthwise %dx%dx%d k%d s%d p%d: %zu of %zu differ", H, W, C, KH, stride, pad, bad, no);
  if (bad) printf(" (first at %zu: evta %d, cpu %d)", first, got, want);
  printf("\n");
  return rc;
}

//#### JO added (2026-10-31): max pool on the EVTA (maxpool_evta), for an input >= 0 (the output of a conv with relu).
//#### Input and output in the EVTA layout N{C/16}HW16 (VTA buffers). The output is cut into tiles of oh x ow pixels of
//#### one 16-channel block; a tile LOADs its input window ((oh-1)*s+K) x ((ow-1)*s+K) with the halo (neighbouring
//#### tiles read the overlapping rows/columns again, the outputs do not overlap) and the zero padding of the image
//#### border (LOAD pad = 0: the same max as the CPU's in-bounds max because the input is >= 0), moves it into the acc
//#### buffer with the identity-weight GEMM, sets the output to 0 and takes ALU MAX (acc, acc) over the K x K window
//#### positions, then STOREs the output. One ALU instruction per window position (a 2-level loop over the output
//#### pixels, one uop): consecutive uops then write different acc entries, as in the conv's ALU instructions
//#### (K x K uops in one loop iteration would write the same entry back to back through the ALU pipeline).
//#### Two tiles at a time in the two halves of inp / acc (load, compute and store of the two overlap).
struct PoolMaxContext {
  uint32_t out_, in_, oh_, ow_, iw_, stride_;  // in_ = input entry of window position (dy, dx) of output pixel 0
  bool operator==(PoolMaxContext &op) {
    return out_ == op.out_ && in_ == op.in_ && oh_ == op.oh_ && ow_ == op.ow_ && iw_ == op.iw_ &&
           stride_ == op.stride_;
  }
};
int alu_poolmax(void *param, VTACommandHandle cmdh) {
  PoolMaxContext *p = (PoolMaxContext *)(param);
  VTAUopLoopBegin(cmdh, p->oh_, p->ow_, p->stride_ * p->iw_, 0);
  VTAUopLoopBegin(cmdh, p->ow_, 1, p->stride_, 0);
  VTAUopPush(cmdh, 1, 0, p->out_, p->in_, 0, RES_ALU_MAX, 0, 0);
  VTAUopLoopEnd(cmdh);
  VTAUopLoopEnd(cmdh);
  return 0;
}
struct PoolMaxUopHandle {
  PoolMaxContext ctx;
  void *uopHandle[2];
  PoolMaxUopHandle() { uopHandle[0] = nullptr; }
};
std::vector<PoolMaxUopHandle *> vPoolMaxUopHandle[4];

static void pushReset(VTACommandHandle &cmdh, uint32_t idEVTA, uint32_t base, uint32_t size) {
  ResetAcc reset;
  reset.base_ = base;
  reset.size_ = size;
  void **h = nullptr;
  for (auto u : vResetUopHandle[idEVTA])
    if (u->strReset == reset) { h = u->uopHandle_reset; break; }
  if (!h) {
    auto u = new ResetUopHandle();
    u->strReset = reset;
    h = u->uopHandle_reset;
    vResetUopHandle[idEVTA].push_back(u);
  }
  VTAPushGEMMOp(cmdh, h, &reset_acc, &reset, 0);
}

static void pushPoolMax(VTACommandHandle &cmdh, uint32_t idEVTA, const PoolMaxContext &c) {
  PoolMaxContext ctx = c;
  void **h = nullptr;
  for (auto u : vPoolMaxUopHandle[idEVTA])
    if (u->ctx == ctx) { h = u->uopHandle; break; }
  if (!h) {
    auto u = new PoolMaxUopHandle();
    u->ctx = ctx;
    h = u->uopHandle;
    vPoolMaxUopHandle[idEVTA].push_back(u);
  }
  VTAPushALUOp(cmdh, h, &alu_poolmax, &ctx, 0);
}

static int maxpool_evta_run(int8_t *input, int8_t *output, int H, int W, int C, int K, int pad, int stride,
                            VTACommandHandle vtaCmdH, uint32_t idEVTA) {
  const uint32_t ACC_HALF = 2048 / 2, INP_HALF = 2048 / 2;  // as conv_kernel (ACC_MEM_ENTRY, INP_MEM_ENTRY)
  const int OH = (H + 2 * pad - K) / stride + 1, OW = (W + 2 * pad - K) / stride + 1;
  // the largest tile (full rows first) whose input window and output fit one half of acc (and of inp)
  int ow = OW, oh = 0;
  for (; ow > 0; ow--) {
    uint32_t iw = (ow - 1) * stride + K;
    for (oh = OH; oh > 0; oh--) {
      uint32_t ih = (oh - 1) * stride + K;
      if (ih * iw <= INP_HALF && ih * iw + oh * ow <= ACC_HALF)
        break;
    }
    if (oh > 0)
      break;
  }
  if (ow == 0)
    return -1;
  struct Tile { int cb, oy, ox, oh, ow; };
  std::vector<Tile> tiles;
  for (int cb = 0; cb < C / 16; cb++)
    for (int oy = 0; oy < OH; oy += oh)
      for (int ox = 0; ox < OW; ox += ow)
        tiles.push_back({cb, oy, ox, std::min(oh, OH - oy), std::min(ow, OW - ox)});

  VTASetDebugMode(vtaCmdH, 0);
  VTALoadBuffer2D(vtaCmdH, getIdentityWeights(idEVTA), 0, 1, 1, 1, 0, 0, 0, 0, RES_WGT_ENTRY, 1);
  for (size_t g = 0; g < tiles.size(); g += 2) {
    size_t n = std::min<size_t>(2, tiles.size() - g);
    for (size_t t = 0; t < n; t++)
      VTADepPush(vtaCmdH, 2, 1);
    for (size_t t = 0; t < n; t++) {
      const Tile &T = tiles[g + t];
      int iw = (T.ow - 1) * stride + K, ih = (T.oh - 1) * stride + K;
      int y0 = T.oy * stride - pad, x0 = T.ox * stride - pad;
      int yb = std::max(0, -y0), xb = std::max(0, -x0);
      int ys = std::min(H, y0 + ih) - std::max(0, y0), xs = std::min(W, x0 + iw) - std::max(0, x0);
      VTADepPop(vtaCmdH, 2, 1);
      VTALoadBuffer2D(vtaCmdH, input, (uint32_t)(T.cb * H * W + std::max(0, y0) * W + std::max(0, x0)), xs, ys, W,
                      xb, yb, iw - xb - xs, ih - yb - ys, t * INP_HALF, 2);
      VTADepPush(vtaCmdH, 1, 2);
    }
    for (size_t t = 0; t < n; t++) {
      const Tile &T = tiles[g + t];
      uint32_t iw = (T.ow - 1) * stride + K, ih = (T.oh - 1) * stride + K;
      uint32_t aIn = t * ACC_HALF, aOut = aIn + ih * iw;
      VTADepPop(vtaCmdH, 1, 2);
      pushReset(vtaCmdH, idEVTA, aIn, ih * iw + T.oh * T.ow);
      pushResGemm(vtaCmdH, idEVTA, aIn, t * INP_HALF, ih * iw);
      for (int dy = 0; dy < K; dy++)
        for (int dx = 0; dx < K; dx++)
          pushPoolMax(vtaCmdH, idEVTA, {aOut, aIn + dy * iw + dx, (uint32_t)T.oh, (uint32_t)T.ow, iw, (uint32_t)stride});
      VTADepPush(vtaCmdH, 2, 3);
    }
    for (size_t t = 0; t < n; t++) {
      const Tile &T = tiles[g + t];
      uint32_t iw = (T.ow - 1) * stride + K, ih = (T.oh - 1) * stride + K;
      VTADepPop(vtaCmdH, 2, 3);
      VTAStoreBuffer2D(vtaCmdH, t * ACC_HALF + ih * iw, 4, output, (uint32_t)(T.cb * OH * OW + T.oy * OW + T.ox),
                       T.ow, T.oh, OW);
      VTADepPush(vtaCmdH, 3, 2);
    }
    for (size_t t = 0; t < n; t++)
      VTADepPop(vtaCmdH, 3, 2);
  }
  VTASynchronize(vtaCmdH, 1 << 31);
  return 0;
}

int maxpool_evta(int8_t *input, int8_t *output, int N, int H, int W, int C, int KH, int KW, int pad_size,
                 int stride_size, void *vtaCmdH, uint32_t idEVTA) {
  if (N != 1 || C % 16 || KH != KW) {
    printf("maxpool_evta: unsupported N %d C %d kernel %dx%d\n", N, C, KH, KW);
    return -1;
  }
  //#### JO added (2026-11-02): replay (cmd_queue.h ReplayBegin)
  int32_t replayKey[] = {2, H, W, C, KH, pad_size, stride_size, (int32_t)idEVTA};
  void *replayBufs[] = {input, output};
  int rc = 0;
  if (!VTAReplayBegin(vtaCmdH, replayKey, sizeof(replayKey), replayBufs, 2)) {
    rc = maxpool_evta_run(input, output, H, W, C, KH, pad_size, stride_size, (VTACommandHandle)vtaCmdH, idEVTA);
    VTAReplayEnd(vtaCmdH, rc == 0);
  }
  //#### JO added (2026-11-02): EVTA_DUMP_CONV also writes the max pool outputs (compare with a fused conv + pool)
  dumpConvOutput(output, (size_t)C * ((H + 2 * pad_size - KH) / stride_size + 1) * ((W + 2 * pad_size - KW) / stride_size + 1));
  if (rc)
    printf("maxpool_evta: window does not fit the buffers (%dx%d, kernel %d, stride %d)\n", H, W, KH, stride_size);
  //#### EVTA_RES_CHECK=1: compare with the CPU maxpool (16-channel blocks as the batch)
  const char *e = getenv("EVTA_RES_CHECK");
  if (!rc && e && atoi(e) > 0) {
    int OH = (H + 2 * pad_size - KH) / stride_size + 1, OW = (W + 2 * pad_size - KW) / stride_size + 1;
    size_t ni = (size_t)H * W * C, no = (size_t)OH * OW * C;
    VTABufferInvalidate(input, 0, ni);
    VTABufferInvalidate(output, 0, no);
    std::vector<int8_t> ref(no);
    maxpool((int8_t *)VTABufferGetVirtAddr(input), ref.data(), C / 16, H, W, 16, KH, KW, pad_size, stride_size);
    const int8_t *o = (const int8_t *)VTABufferGetVirtAddr(output);
    size_t bad = 0, first = no;
    for (size_t i = 0; i < no; i++)
      if (o[i] != ref[i]) {
        if (!bad)
          first = i;
        bad++;
      }
    printf("EVTA_RES_CHECK maxpool %dx%dx%d k%d s%d p%d: %zu of %zu differ", H, W, C, KH, stride_size, pad_size, bad,
           no);
    if (bad)
      printf(" (first at %zu: evta %d, cpu %d)", first, o[first], ref[first]);
    printf("\n");
  }
  return rc;
}


//#### JO added (2026-11-02): fully connected layer on the EVTA (fullyconnected_evta), batch 1, the arguments of the CPU
//#### fullyconnected plus the command handle. out[j] = clip(round((sum_k in[k] * W[k][j] + B[j]) * 2^-shift)), W[k][j] =
//#### kernel[k * OUT + j] (Glow's FC weights), bit-exact with cpuFullyConnected (std::round, half away from zero).
//#### On the first call of a kernel the weights go to a VTA buffer in the EVTA layout, one 16x16 wgt entry per
//#### (output block o, input block c), o-major (the IN/16 entries of an output block are contiguous), the output padded
//#### to a multiple of 16; the bias (rounded as the CPU does) to an int32 VTA buffer. Each call copies the input to a VTA
//#### buffer, then: bias -> acc[o]; per chunk of g output blocks (one half of the wgt buffer) LOAD the weights in one
//#### contiguous transfer (the other half is loaded while this one is in the GEMM) and GEMM acc[o] += inp[c] x wgt[o][c]
//#### (loop over c, one uop per o: the same acc entry comes back every g uops); then round half away from zero with the
//#### ALU only (MIN / MAX / ADD / SHR): s = max(min(acc, 0), -1) (-1 if negative), acc = (acc + s + 2^(shift-1)) >> shift,
//#### clip;
//#### STORE, copy the OUT bytes out. Not on the EVTA (falls back to the CPU fullyconnected): offsets, a scale ratio that
//#### is not 2^-shift (0..15), batch > 1, IN not a multiple of 16, chunks under 8 output blocks.
struct FcGemmContext {
  uint32_t acc_, wgt_, g_, cin_;
  bool operator==(FcGemmContext &op) { return acc_ == op.acc_ && wgt_ == op.wgt_ && g_ == op.g_ && cin_ == op.cin_; }
};
int gemm_fc(void *param, VTACommandHandle cmdh) {
  FcGemmContext *p = (FcGemmContext *)(param);
  VTAUopLoopBegin(cmdh, p->cin_, 0, 1, 1);
  for (uint32_t o = 0; o < p->g_; o++)
    VTAUopPush(cmdh, 0, 0, p->acc_ + o, 0, p->wgt_ + o * p->cin_, 0, 0, 0);
  VTAUopLoopEnd(cmdh);
  return 0;
}
struct FcGemmUopHandle {
  FcGemmContext ctx;
  void *uopHandle[2];
  FcGemmUopHandle() { uopHandle[0] = nullptr; }
};
std::vector<FcGemmUopHandle *> vFcGemmUopHandle[4];

static void pushFcGemm(VTACommandHandle &cmdh, uint32_t idEVTA, const FcGemmContext &c) {
  FcGemmContext ctx = c;
  void **h = nullptr;
  for (auto u : vFcGemmUopHandle[idEVTA])
    if (u->ctx == ctx) { h = u->uopHandle; break; }
  if (!h) {
    auto u = new FcGemmUopHandle();
    u->ctx = ctx;
    h = u->uopHandle;
    vFcGemmUopHandle[idEVTA].push_back(u);
  }
  VTAPushGEMMOp(cmdh, h, &gemm_fc, &ctx, 0);
}

// chunks of the output blocks (one half of the wgt buffer each, sizes as even as possible); false: not on the EVTA
// (the same acc entry must come back no closer than minG uops, both acc halves must hold the output)
static bool fc_evta_plan(uint32_t IN, uint32_t OUT, uint32_t *gEven, uint32_t *nChunks) {
  const uint32_t WGT_HALF = 1024 / 2, ACC_ENTRY = 2048;  // as conv_kernel (WGT_MEM_ENTRY, ACC_MEM_ENTRY)
  //#### JO changed (2026-11-05): the 8 uops only for the HLS GEMM (no acc hazard check); the systolic GEMM forwards
  //#### (sa_rmw.v), there any chunk size (evtaSystolic)
  const uint32_t minG = evtaSystolic() ? 1 : 8;
  const uint32_t CIN = IN / 16, NB = (OUT + 15) / 16;
  const uint32_t g = CIN ? std::min<uint32_t>(NB, WGT_HALF / CIN) : 0;
  *nChunks = g ? (NB + g - 1) / g : 0;
  *gEven = *nChunks ? (NB + *nChunks - 1) / *nChunks : 0;
  return IN % 16 == 0 && g > 0 && *gEven >= minG && 2 * NB <= ACC_ENTRY;
}

//#### JO added (2026-11-05): fullyconnected_evta_packed on the CPU over the packed weights / bias, the same result as the
//#### EVTA (round half away from zero, clip), when the plan does not fit this vtalib / bitstream (the bundle checks only
//#### what fits some EVTA)
static void fc_packed_cpu(const int8_t *input, const int8_t *w, const int32_t *bb, int8_t *output, uint32_t IN,
                          uint32_t OUT, int shift) {
  const uint32_t CIN = IN / 16;
  for (uint32_t ob = 0; ob * 16 < OUT; ob++) {
    int32_t acc[16];
    for (int r = 0; r < 16; r++)
      acc[r] = bb[ob * 16 + r];
    for (uint32_t ib = 0; ib < CIN; ib++) {
      const int8_t *blk = w + ((size_t)ob * CIN + ib) * 256, *x = input + ib * 16;
      for (int r = 0; r < 16; r++) {
        int32_t s = 0;
        for (int c = 0; c < 16; c++)
          s += (int16_t)blk[r * 16 + c] * (int16_t)x[c];
        acc[r] += s;
      }
    }
    for (uint32_t r = 0; r < 16 && ob * 16 + r < OUT; r++) {
      int64_t v = acc[r];
      if (shift > 0) {
        const int64_t h = (int64_t)1 << (shift - 1);
        v = v >= 0 ? (v + h) >> shift : -((-v + h) >> shift);
      }
      output[ob * 16 + r] = (int8_t)std::max<int64_t>(-128, std::min<int64_t>(127, v));
    }
  }
}

struct FcEvtaBuffers {
  void *wgt, *bias, *in, *out;
  uint32_t nb;  // output blocks
};

// the EVTA part: b.in (IN bytes), b.wgt, b.bias ready (flushed); b.out is written (not invalidated)
static void fc_evta_core(const FcEvtaBuffers &b, uint32_t CIN, uint32_t NB, uint32_t gEven, uint32_t nChunks, int shift,
                         VTACommandHandle cmdh, uint32_t idEVTA) {
  const uint32_t WGT_HALF = 1024 / 2;
  VTASetDebugMode(cmdh, 0);
  VTALoadBuffer2D(cmdh, b.bias, 0, NB, 1, NB, 0, 0, 0, 0, 0, 3);  // compute stage, before the GEMMs
  VTADepPush(cmdh, 2, 1);
  VTADepPush(cmdh, 2, 1);
  //#### JO added (2026-11-04, requantize, evtaRequant): the GEMMs write round half away from zero / shift / clip to out
  //#### (the ALU part below goes away); each output block's last write is in its chunk's GEMM
  const bool rq = evtaRequant();
  if (rq) VTARequant(cmdh, 1, shift > 0 ? 2 : 0, shift > 0 ? shift : 0, -128, 127);
  for (uint32_t ch = 0; ch < nChunks; ch++) {
    uint32_t o0 = ch * gEven, gc = std::min(gEven, NB - o0), wb = (ch % 2) * WGT_HALF;
    VTADepPop(cmdh, 2, 1);
    if (ch == 0)
      VTALoadBuffer2D(cmdh, b.in, 0, CIN, 1, CIN, 0, 0, 0, 0, 0, 2);
    VTALoadBuffer2D(cmdh, b.wgt, o0 * CIN, gc * CIN, 1, gc * CIN, 0, 0, 0, 0, wb, 1);
    VTADepPush(cmdh, 1, 2);
    VTADepPop(cmdh, 1, 2);
    pushFcGemm(cmdh, idEVTA, {o0, wb, gc, CIN});
    VTADepPush(cmdh, 2, 1);
  }
  VTADepPop(cmdh, 2, 1);
  VTADepPop(cmdh, 2, 1);
  if (rq) VTARequant(cmdh, 0, 0, 0, 0, 0);
  if (!rq) {   //#### JO added (2026-11-04)
  if (shift > 0) {
    pushReset(cmdh, idEVTA, NB, NB);
    pushAlu(cmdh, idEVTA, NB, 0, NB, RES_ALU_ADD, 0, 0);
    //#### not SHR 31: the ALU shift is a 5-bit signed field (-16..15), 31 is a shift left by 1
    pushAlu(cmdh, idEVTA, NB, NB, NB, RES_ALU_MIN, 1, 0);
    pushAlu(cmdh, idEVTA, NB, NB, NB, RES_ALU_MAX, 1, -1);
    pushAlu(cmdh, idEVTA, 0, NB, NB, RES_ALU_ADD, 0, 0);
    pushAlu(cmdh, idEVTA, 0, 0, NB, RES_ALU_ADD, 1, 1 << (shift - 1));
    pushAlu(cmdh, idEVTA, 0, 0, NB, RES_ALU_SHR, 1, shift);
  }
  pushAlu(cmdh, idEVTA, 0, 0, NB, RES_ALU_MAX, 1, -128);
  pushAlu(cmdh, idEVTA, 0, 0, NB, RES_ALU_MIN, 1, 127);
  }   //#### JO: !rq
  VTADepPush(cmdh, 2, 3);
  VTADepPop(cmdh, 2, 3);
  VTAStoreBuffer2D(cmdh, 0, 4, b.out, 0, NB, 1, NB);
  VTADepPush(cmdh, 3, 2);
  VTADepPop(cmdh, 3, 2);
  VTASynchronize(cmdh, 1 << 31);
}

//#### EVTA_FC_PROBE=1 (bring-up): a permutation weight (output j = input j % IN) and distinct inputs, shift 0, no bias:
//#### the output tells which input the EVTA added for each output
static void fc_evta_probe(uint32_t IN, uint32_t OUT, VTACommandHandle cmdh, uint32_t idEVTA) {
  const uint32_t CIN = IN / 16, NB = (OUT + 15) / 16, g = std::min<uint32_t>(NB, 512 / CIN);
  const uint32_t nChunks = (NB + g - 1) / g, gEven = (NB + nChunks - 1) / nChunks;
  FcEvtaBuffers b;
  b.nb = NB;
  b.wgt = VTABufferAlloc((size_t)NB * CIN * 256);
  b.bias = VTABufferAlloc((size_t)NB * 64);
  b.in = VTABufferAlloc(IN);
  b.out = VTABufferAlloc((size_t)NB * 16);
  int8_t *w = (int8_t *)VTABufferGetVirtAddr(b.wgt), *x = (int8_t *)VTABufferGetVirtAddr(b.in);
  memset(w, 0, (size_t)NB * CIN * 256);
  memset(VTABufferGetVirtAddr(b.bias), 0, (size_t)NB * 64);
  for (uint32_t k = 0; k < IN; k++)
    x[k] = (int8_t)((k * 7) % 255 - 127);
  for (uint32_t j = 0; j < OUT; j++) {
    uint32_t k = j % IN;
    w[((size_t)(j / 16) * CIN + k / 16) * 256 + (j % 16) * 16 + k % 16] = 1;
  }
  VTABufferFlush(b.wgt, 0, (size_t)NB * CIN * 256);
  VTABufferFlush(b.bias, 0, (size_t)NB * 64);
  VTABufferFlush(b.in, 0, IN);
  memset(VTABufferGetVirtAddr(b.out), 0x5a, (size_t)NB * 16);
  VTABufferFlush(b.out, 0, (size_t)NB * 16);
  fc_evta_core(b, CIN, NB, gEven, nChunks, 0, cmdh, idEVTA);
  VTABufferInvalidate(b.out, 0, (size_t)NB * 16);
  const int8_t *o = (const int8_t *)VTABufferGetVirtAddr(b.out);
  uint32_t ok = 0;
  for (uint32_t j = 0; j < OUT; j++)
    ok += o[j] == x[j % IN];
  printf("EVTA_FC_PROBE %u -> %u (%u chunks of %u blocks): %u of %u right\n", IN, OUT, nChunks, gEven, ok, OUT);
  for (uint32_t j = 0; j < std::min<uint32_t>(OUT, 40); j++) {
    int found = -1, n = 0;
    for (uint32_t k = 0; k < IN; k++)
      if (x[k] == o[j]) { if (found < 0) found = k; n++; }
    printf("  out %u = %d: input %d%s (want %u)\n", j, o[j], found, n > 1 ? "+" : "", j % IN);
  }
  VTABufferFree(b.wgt);
  VTABufferFree(b.bias);
  VTABufferFree(b.in);
  VTABufferFree(b.out);
}

// random weights (nz nonzeros per output in -4..4, or dense in -128..127 with nz = 0), inputs in -128..127, bias in
// -bmax..bmax, shift: EVTA vs the exact model (round half away from zero, clip)
static void fc_evta_probe2(uint32_t IN, uint32_t OUT, int nz, int bmax, int shift, VTACommandHandle cmdh,
                           uint32_t idEVTA) {
  const uint32_t CIN = IN / 16, NB = (OUT + 15) / 16, g = std::min<uint32_t>(NB, 512 / CIN);
  const uint32_t nChunks = (NB + g - 1) / g, gEven = (NB + nChunks - 1) / nChunks;
  FcEvtaBuffers b;
  b.wgt = VTABufferAlloc((size_t)NB * CIN * 256);
  b.bias = VTABufferAlloc((size_t)NB * 64);
  b.in = VTABufferAlloc(IN);
  b.out = VTABufferAlloc((size_t)NB * 16);
  int8_t *w = (int8_t *)VTABufferGetVirtAddr(b.wgt), *x = (int8_t *)VTABufferGetVirtAddr(b.in);
  int32_t *bb = (int32_t *)VTABufferGetVirtAddr(b.bias);
  memset(w, 0, (size_t)NB * CIN * 256);
  memset(bb, 0, (size_t)NB * 64);
  uint32_t r = 12345;
  auto rnd = [&]() { r = r * 1103515245u + 12345u; return (r >> 8) & 0xffff; };
  std::vector<int8_t> W((size_t)IN * OUT, 0);
  for (uint32_t k = 0; k < IN; k++)
    x[k] = (int8_t)(rnd() % 256 - 128);
  for (uint32_t j = 0; j < OUT; j++) {
    if (nz)
      for (int t = 0; t < nz; t++)
        W[(size_t)(rnd() % IN) * OUT + j] = (int8_t)(rnd() % 9 - 4);
    else
      for (uint32_t k = 0; k < IN; k++)
        W[(size_t)k * OUT + j] = (int8_t)(rnd() % 256 - 128);
    if (bmax)
      bb[j] = (int32_t)(rnd() % (2 * bmax + 1)) - bmax;
  }
  for (uint32_t k = 0; k < IN; k++)
    for (uint32_t j = 0; j < OUT; j++)
      w[((size_t)(j / 16) * CIN + k / 16) * 256 + (j % 16) * 16 + k % 16] = W[(size_t)k * OUT + j];
  VTABufferFlush(b.wgt, 0, (size_t)NB * CIN * 256);
  VTABufferFlush(b.bias, 0, (size_t)NB * 64);
  VTABufferFlush(b.in, 0, IN);
  fc_evta_core(b, CIN, NB, gEven, nChunks, shift, cmdh, idEVTA);
  VTABufferInvalidate(b.out, 0, (size_t)NB * 16);
  const int8_t *o = (const int8_t *)VTABufferGetVirtAddr(b.out);
  uint32_t ok = 0, okFloor = 0, okNoRnd = 0;
  int shown = 0;
  for (uint32_t j = 0; j < OUT; j++) {
    int64_t acc = bb[j];
    for (uint32_t k = 0; k < IN; k++)
      acc += (int64_t)x[k] * W[(size_t)k * OUT + j];
    auto clip = [](int64_t v) { return (int)std::max<int64_t>(-128, std::min<int64_t>(127, v)); };
    int want = clip((int64_t)std::round(double(acc) / (1 << shift)));
    ok += o[j] == want;
    okFloor += o[j] == clip(acc >> shift);
    okNoRnd += o[j] == clip(acc);
    if (o[j] != want && shown++ < 6)
      printf("  out %u: evta %d, want %d (acc %lld)\n", j, o[j], want, (long long)acc);
  }
  printf("EVTA_FC_PROBE2 %u -> %u nz %d bias %d shift %d: %u of %u right (floor %u, no shift %u)\n", IN, OUT, nz, bmax,
         shift, ok, OUT, okFloor, okNoRnd);
  VTABufferFree(b.wgt);
  VTABufferFree(b.bias);
  VTABufferFree(b.in);
  VTABufferFree(b.out);
}

int fullyconnected_evta(int8_t *input, float inputScale, int32_t inputOffset, int8_t *kernel, float kernelScale,
                        int32_t kernelOffset, int8_t *bias, float biasScale, int32_t biasOffset, int8_t *output,
                        float outputScale, int32_t outputOffset, dim_t inputDim0, dim_t inputDim1, dim_t kernelDim0,
                        dim_t kernelDim1, dim_t outputDim0, dim_t outputDim1, bool doBias, void *vtaCmdH,
                        uint32_t idEVTA) {
  const uint32_t IN = inputDim1, OUT = outputDim1, CIN = IN / 16, NB = (OUT + 15) / 16;
  const float matMulScale = inputScale * kernelScale, ratio = matMulScale / outputScale;
  int shift = -1;
  for (int k = 0; k <= 15; k++)  // ALU SHR: -16..15
    if (ratio == std::ldexp(1.0f, -k)) shift = k;
  uint32_t gEven, nChunks;
  bool fits = fc_evta_plan(IN, OUT, &gEven, &nChunks);
  if (inputDim0 != 1 || outputDim0 != 1 || !fits || kernelDim0 != IN || kernelDim1 != OUT || inputOffset ||
      kernelOffset || outputOffset || (doBias && biasOffset) || shift < 0) {
    static bool said = false;
    if (!said) {
      said = true;
      printf("fullyconnected_evta: %dx%d -> %d not on the EVTA (offsets %d %d %d, scale ratio %g), on the CPU\n",
             (int)inputDim0, (int)IN, (int)OUT, (int)inputOffset, (int)kernelOffset, (int)outputOffset, ratio);
    }
    return fullyconnected(input, inputScale, inputOffset, kernel, kernelScale, kernelOffset, bias, biasScale,
                          biasOffset, output, outputScale, outputOffset, inputDim0, inputDim1, kernelDim0, kernelDim1,
                          outputDim0, outputDim1, doBias);
  }

  {
    static bool probed = false;
    const char *pe = getenv("EVTA_FC_PROBE");
    if (pe && atoi(pe) > 0 && !probed) {
      probed = true;
      fc_evta_probe(128, 256, (VTACommandHandle)vtaCmdH, idEVTA);
      fc_evta_probe(IN, OUT, (VTACommandHandle)vtaCmdH, idEVTA);
      for (int nz : {1, 4})
        for (int bmax : {0, 50})
          fc_evta_probe2(IN, OUT, nz, bmax, 0, (VTACommandHandle)vtaCmdH, idEVTA);
      for (int bmax : {0, 3000})
        for (int sh : {0, 8})
          fc_evta_probe2(IN, OUT, 0, bmax, sh, (VTACommandHandle)vtaCmdH, idEVTA);
      fc_evta_probe2(IN, OUT, 4, 0, 1, (VTACommandHandle)vtaCmdH, idEVTA);
    }
  }
  // the buffers of this kernel (one set per call site and EVTA copy), made on the first call
  static std::mutex mtx;
  static std::map<const int8_t *, FcEvtaBuffers> bufs;
  FcEvtaBuffers b;
  {
    std::lock_guard<std::mutex> lock(mtx);
    auto it = bufs.find(kernel);
    if (it == bufs.end()) {
      b.nb = NB;
      b.wgt = VTABufferAlloc((size_t)NB * CIN * 256);
      int8_t *w = (int8_t *)VTABufferGetVirtAddr(b.wgt);
      memset(w, 0, (size_t)NB * CIN * 256);
      for (uint32_t k = 0; k < IN; k++)
        for (uint32_t j = 0; j < OUT; j++)
          w[((size_t)(j / 16) * CIN + k / 16) * 256 + (j % 16) * 16 + k % 16] = kernel[(size_t)k * OUT + j];
      VTABufferFlush(b.wgt, 0, (size_t)NB * CIN * 256);
      b.bias = VTABufferAlloc((size_t)NB * 16 * 4);
      int32_t *bb = (int32_t *)VTABufferGetVirtAddr(b.bias);
      memset(bb, 0, (size_t)NB * 16 * 4);
      for (uint32_t j = 0; doBias && j < OUT; j++)
        bb[j] = std::round(float(((int32_t *)bias)[j] - biasOffset) * (biasScale / matMulScale));
      VTABufferFlush(b.bias, 0, (size_t)NB * 16 * 4);
      b.in = VTABufferAlloc(IN);
      b.out = VTABufferAlloc((size_t)NB * 16);
      bufs[kernel] = b;
    } else {
      b = it->second;
    }
  }
  memcpy(VTABufferGetVirtAddr(b.in), input, IN);
  VTABufferFlush(b.in, 0, IN);

  //#### JO added (2026-11-02): replay (cmd_queue.h ReplayBegin)
  int32_t replayKey[] = {3, (int32_t)CIN, (int32_t)NB, (int32_t)gEven, (int32_t)nChunks, shift, (int32_t)idEVTA};
  void *replayBufs[] = {b.wgt, b.bias, b.in, b.out};
  if (!VTAReplayBegin(vtaCmdH, replayKey, sizeof(replayKey), replayBufs, 4)) {
    fc_evta_core(b, CIN, NB, gEven, nChunks, shift, (VTACommandHandle)vtaCmdH, idEVTA);
    VTAReplayEnd(vtaCmdH, 1);
  }
  VTABufferInvalidate(b.out, 0, (size_t)NB * 16);
  memcpy(output, VTABufferGetVirtAddr(b.out), OUT);

  //#### EVTA_RES_CHECK=1: compare with the CPU fullyconnected
  const char *e = getenv("EVTA_RES_CHECK");
  if (e && atoi(e) > 0) {
    std::vector<int8_t> ref(OUT);
    fullyconnected(input, inputScale, inputOffset, kernel, kernelScale, kernelOffset, bias, biasScale, biasOffset,
                   ref.data(), outputScale, outputOffset, inputDim0, inputDim1, kernelDim0, kernelDim1, outputDim0,
                   outputDim1, doBias);
    size_t bad = 0, first = OUT;
    for (size_t i = 0; i < OUT; i++)
      if (output[i] != ref[i]) {
        if (!bad)
          first = i;
        bad++;
      }
    printf("EVTA_RES_CHECK fullyconnected %u -> %u (shift %d): %zu of %u differ", IN, OUT, shift, bad, OUT);
    if (bad)
      printf(" (first at %zu: evta %d, cpu %d)", first, output[first], ref[first]);
    printf("\n");
  }
  return 0;
}

//#### JO added (2026-11-02): fullyconnected_evta with the weights and the bias already in the EVTA layout, as VTASave.cpp
//#### writes them into the weights file (-vta-evta-fc): wgt = NB x IN/16 entries of 16x16 ([o][c], [16 out][16 in]),
//#### bias = NB x 16 int32 rounded to the GEMM scale (VTA buffer handles), NB = ceil(OUT / 16). The CPU only copies the
//#### input and the output (and builds the instructions); EVTA_RES_CHECK=1 compares with a model computed from them.
int fullyconnected_evta_packed(int8_t *input, void *wgt, void *bias, int8_t *output, int IN, int OUT, int shift,
                               void *vtaCmdH, uint32_t idEVTA) {
  uint32_t gEven, nChunks;
  if (IN % 16 || shift < 0 || shift > 15) {
    printf("fullyconnected_evta_packed: %d -> %d shift %d does not fit the EVTA\n", IN, OUT, shift);
    return -1;
  }
  if (!fc_evta_plan(IN, OUT, &gEven, &nChunks)) {   //#### JO added (2026-11-05)
    fc_packed_cpu(input, (const int8_t *)VTABufferGetVirtAddr(wgt), (const int32_t *)VTABufferGetVirtAddr(bias), output,
                  IN, OUT, shift);
    return 0;
  }
  const uint32_t CIN = IN / 16, NB = (OUT + 15) / 16;
  // the input / output buffers of this call site (EVTA copy), made on the first call
  static std::mutex mtx;
  static std::map<const void *, FcEvtaBuffers> bufs;
  FcEvtaBuffers b;
  {
    std::lock_guard<std::mutex> lock(mtx);
    auto it = bufs.find(wgt);
    if (it == bufs.end()) {
      b.wgt = wgt;
      b.bias = bias;
      b.nb = NB;
      b.in = VTABufferAlloc(IN);
      b.out = VTABufferAlloc((size_t)NB * 16);
      bufs[wgt] = b;
    } else {
      b = it->second;
    }
  }
  memcpy(VTABufferGetVirtAddr(b.in), input, IN);
  VTABufferFlush(b.in, 0, IN);
  //#### JO added (2026-11-02): replay (cmd_queue.h ReplayBegin)
  int32_t replayKey[] = {4, (int32_t)CIN, (int32_t)NB, (int32_t)gEven, (int32_t)nChunks, shift, (int32_t)idEVTA};
  void *replayBufs[] = {b.wgt, b.bias, b.in, b.out};
  if (!VTAReplayBegin(vtaCmdH, replayKey, sizeof(replayKey), replayBufs, 4)) {
    fc_evta_core(b, CIN, NB, gEven, nChunks, shift, (VTACommandHandle)vtaCmdH, idEVTA);
    VTAReplayEnd(vtaCmdH, 1);
  }
  VTABufferInvalidate(b.out, 0, (size_t)NB * 16);
  memcpy(output, VTABufferGetVirtAddr(b.out), OUT);

  const char *e = getenv("EVTA_RES_CHECK");
  if (e && atoi(e) > 0) {
    const int8_t *w = (const int8_t *)VTABufferGetVirtAddr(wgt);
    const int32_t *bb = (const int32_t *)VTABufferGetVirtAddr(bias);
    size_t bad = 0, first = OUT;
    int want0 = 0;
    for (int j = 0; j < OUT; j++) {
      int64_t acc = bb[j];
      for (int k = 0; k < IN; k++)
        acc += (int64_t)input[k] * w[((size_t)(j / 16) * CIN + k / 16) * 256 + (j % 16) * 16 + k % 16];
      int want = (int)std::max<int64_t>(-128, std::min<int64_t>(127, (int64_t)std::round(double(acc) / (1 << shift))));
      if (output[j] != want) {
        if (!bad) { first = j; want0 = want; }
        bad++;
      }
    }
    printf("EVTA_RES_CHECK fullyconnected_evta_packed %d -> %d (shift %d): %zu of %d differ", IN, OUT, shift, bad, OUT);
    if (bad)
      printf(" (first at %zu: evta %d, want %d)", first, output[first], want0);
    printf("\n");
  }
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