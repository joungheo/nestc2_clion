/// input NHWC : 1 112 112 16
/// kernel NHWC : 16 6 4 16
/// pad : 1
/// stride : 1
/// output Tile Size(W/H) : 13 / 13


#define MIN_W_SIZE 7
#define MIN_H_SIZE 7
#define OUT_TILE_SIZE_W 13
#define OUT_TILE_SIZE_H 13

#include <iostream>
#include <cstring>
#include <cstdint>
#include <stdlib.h>
#include <unistd.h>
#include <assert.h>
#include <vta/runtime.h>

#include <time.h>

using namespace std;



namespace sim_test_conv6 {


struct ResetAcc{
  uint32_t base_;
  uint32_t size_;
};

int reset_acc(void *param) {
  ResetAcc *p = (ResetAcc *) (param);
  VTAUopLoopBegin(p->size_, 1, 0, 0);
  VTAUopPush(0, 1, p->base_, 0, 0, 0, 0, 0);
  VTAUopLoopEnd();
  return 0;
}




struct gemmop_mxn{
  uint32_t tile_size_h_;
  uint32_t tile_size_w_;
  uint32_t src_factor_;
  uint32_t kernel_h_;
  uint32_t kernel_w_;
};
int gemmop_mxn(void *param) {

  struct gemmop_mxn* ctx = (struct gemmop_mxn*) (param);
  VTAUopLoopBegin(ctx->tile_size_h_, ctx->tile_size_w_, ctx->src_factor_, 0);
  VTAUopLoopBegin(ctx->kernel_h_, 0, ctx->src_factor_, ctx->kernel_w_);

  for (int dx = 0; dx < ctx->kernel_w_; dx++) {
    for (int j = 0; j < ctx->tile_size_w_; j++) {
      VTAUopPush(0, 0, j, j + dx, dx, 0, 0, 0);
    }
  }
  VTAUopLoopEnd();
  VTAUopLoopEnd();

  return 0;
}

// alu func
int finit_shr(void *param) {
  int *p = (int *) (param);
  VTAUopLoopBegin(1, 0, 0, 0);
  VTAUopPush(1, 0, 0, 0, 0, 3, 1, 8);
  VTAUopLoopEnd();
  return 0;
}

int finit_max(void *param) {
  int *p = (int *) (param);
  VTAUopLoopBegin(1, 0, 0, 0);
  VTAUopPush(1, 0, 0, 0, 0, 1, 1, -128);
  VTAUopLoopEnd();
  return 0;
}

int finit_min(void *param) {
  int *p = (int *) (param);
  VTAUopLoopBegin(1, 0, 0, 0);
  VTAUopPush(1, 0, 0, 0, 0, 0, 1, 127);
  VTAUopLoopEnd();
  return 0;
}

void conv_kernel(uint32_t sizeOutTileH, uint32_t sizeOutTileW,
                 uint32_t pad_size, uint32_t x_pad_before, uint32_t x_pad_after, uint32_t y_pad_before, uint32_t y_pad_after,
                 uint32_t outputWOffset, uint32_t outputHOffset,
                 uint32_t sizeInTileH, uint32_t sizeInTileW,
                 uint32_t inputStride,
                 void* vta_input_buf,
                 uint32_t KH, uint32_t KW,
                 void* vta_filter_buf,
                 uint32_t out_w,
                 void * vta_output_buf

){

  VTACommandHandle vtaCmdH{nullptr};
  vtaCmdH = VTATLSCommandHandle();
  VTASetDebugMode(vtaCmdH, VTA_DEBUG_DUMP_INSN | VTA_DEBUG_DUMP_UOP);

// for very short width input
  uint32_t w_dummy = 0;
  uint32_t h_dummy = 0;
  if(sizeInTileW < MIN_W_SIZE){
    w_dummy = MIN_W_SIZE - sizeInTileW;
  }

  if(sizeInTileH < MIN_H_SIZE){
    h_dummy = MIN_H_SIZE - sizeInTileH;
  }
// acc buffer reset
  void *uopHandle_reset[2];
  uopHandle_reset[0] = nullptr;

  ResetAcc reset;
  reset.base_=0;
  reset.size_=(sizeOutTileH + h_dummy) * (sizeOutTileW + w_dummy);
  VTAPushGEMMOp(uopHandle_reset, &reset_acc, &reset, 0);



  VTADepPush(vtaCmdH, 2, 1);
  VTADepPop(vtaCmdH, 2, 1);






  uint32_t inputWOffset = (int)(outputWOffset - pad_size)<0?0:outputWOffset - pad_size;
  uint32_t inputHOffset = (int)(outputHOffset - pad_size)<0?0:outputHOffset - pad_size;

  uint32_t sram_offset = inputHOffset * inputStride + inputWOffset;

  uint32_t y_size = sizeInTileH - y_pad_before - y_pad_after;
  uint32_t x_size = sizeInTileW - x_pad_before - x_pad_after;

  VTALoadBuffer2D(vtaCmdH, vta_input_buf,
                  sram_offset,
                  x_size,
                  y_size,
                  inputStride,
                  x_pad_before,
                  y_pad_before,
                  x_pad_after + w_dummy,
                  y_pad_after + h_dummy,
                  0,        //index
                  2);    //type

  VTALoadBuffer2D(vtaCmdH, vta_filter_buf,
                  0,
                  KH * KW,
                  1,
                  KH * KW,
                  0, 0, 0, 0, 0, 1);

  VTADepPush(vtaCmdH, 1, 2);
  VTADepPop(vtaCmdH, 1, 2);

  void *uopHandle_gemm[2];
  uopHandle_gemm[0] = nullptr;

  struct gemmop_mxn gemm_ctx;
  gemm_ctx.tile_size_h_ = sizeOutTileH + h_dummy;
  gemm_ctx.tile_size_w_ = sizeOutTileW + w_dummy;
  gemm_ctx.src_factor_ = sizeInTileW + w_dummy;
  gemm_ctx.kernel_h_ = KH;
  gemm_ctx.kernel_w_ = KW;

  VTAPushGEMMOp(uopHandle_gemm, &gemmop_mxn, &gemm_ctx, 0);

  VTADepPush(vtaCmdH, 2, 3);
  VTADepPop(vtaCmdH, 2, 3);

  if(w_dummy==0) {
    VTAStoreBuffer2D(vtaCmdH,
                     0,
                     4,
                     vta_output_buf,
                     outputHOffset * out_w + outputWOffset,
                     sizeOutTileW + w_dummy,
                     sizeOutTileH,
                     out_w);
  }
  else{
    for(int hIdx = 0 ; hIdx < sizeOutTileH ; hIdx++){
      VTAStoreBuffer2D(vtaCmdH,
                       0 + hIdx*(sizeOutTileW+w_dummy),
                       4,
                       vta_output_buf,
                       (outputHOffset + hIdx) * out_w+ outputWOffset,
                       sizeOutTileW,
                       1,
                       out_w);
    }
  }
  VTASynchronize(vtaCmdH, 1 << 31);
}


}

using namespace sim_test_conv6;
int conv6_test(){
  int is_diff = 0;
  srand(time(NULL));

  //N, H, W, C
  //N == 1
  //group == 1
  struct timeval tv_s, tv_e;



  int N = 1;
  int H = 112;
  int W = 112;
  int C = 16;
  int KN = 16;
  int KH = 6;
  int KW = 4;

  // input NCHW
  int8_t in_NCHW[N][C][H][W];


  for(int i =0; i < N; i++){
    for(int j =0 ; j < H; j++){
      for(int k =0; k< W; k++){
        for(int l =0; l<C; l++){
          in_NCHW[i][l][j][k]=(int8_t)(rand()%2);
        }
      }
    }
  }

  //transpose input NCHW -> NHWC
  int8_t in_NHWC[N][H][W][C];
  for(int i =0; i < N; i++){
    for(int j =0 ; j < H; j++){
      for(int k =0; k< W; k++){
        for(int l =0; l<C; l++){
          in_NHWC[i][j][k][l] = in_NCHW[i][l][j][k];
        }
      }
    }
  }


  // reshape input NCHW -> N{C//16}16HW
  assert(C%16==0);
  int8_t in_NC_16HW[N][C/16][16][H][W];
  for(int i = 0 ; i< N; i++){
    for(int j = 0 ; j < C/16; j++){
      for(int k=0; k<16; k++){
        for(int l=0; l<H; l++){
          for(int o=0; o<W; o++){
            in_NC_16HW[i][j][k][l][o] =
                in_NCHW[i][j*16+k][l][o];
          }
        }
      }
    }
  }

  //transpose input N{C//16}16HW -> N{C//16}HW16
  int8_t in_NC_HW16[N][C/16][H][W][16];
  for(int i = 0 ; i< N; i++){
    for(int j = 0 ; j < C/16; j++){
      for(int k=0; k<H; k++){
        for(int l=0; l<W; l++){
          for(int o=0; o<16; o++){
            in_NC_HW16[i][j][k][l][o] =
                in_NC_16HW[i][j][o][k][l];
          }
        }
      }
    }
  }




  // filter NHWC
  int8_t filter_NHWC[KN][KH][KW][C];

  for(int i =0; i < KN; i++){
    for(int j =0 ; j < KH; j++){
      for(int k =0; k< KW; k++){
        for(int l =0; l<C; l++){
          filter_NHWC[i][j][k][l]=(int8_t)(rand()%2);
          if(rand()%2==0) filter_NHWC[i][j][k][l]=filter_NHWC[i][j][k][l]*-1;
        }
      }
    }
  }


  // reshape filter NHWC -> {N//16}16HW{C//16}16
  assert(KN%16==0);

  int8_t filter_N_16HWC_16[KN/16][16][KH][KW][C/16][16];
  for(int i = 0 ; i< KN/16; i++){
    for(int j = 0 ; j < 16; j++){
      for(int k=0; k<KH; k++){
        for(int l=0; l<KW; l++){
          for(int o=0; o<C/16; o++) {
            for (int p = 0; p < 16; p++) {
              filter_N_16HWC_16[i][j][k][l][o][p] =
                  filter_NHWC[i*16+j][k][l][o*16+p];
            }
          }
        }
      }
    }
  }

  // transpose filter {N//16}16HW{C//16}16 =>  {N//16}{C//16}HW1616
  int8_t filter_N_C_HW1616[KN/16][C/16][KH][KW][16][16];
  for(int i = 0 ; i< KN/16; i++){
    for(int j = 0 ; j < C/16; j++){
      for(int k=0; k<KH; k++){
        for(int l=0; l<KW; l++){
          for(int o=0; o<16; o++) {
            for (int p = 0; p < 16; p++) {
              filter_N_C_HW1616[i][j][k][l][o][p] =
                  filter_N_16HWC_16[i][o][k][l][j][p];
            }
          }
        }
      }
    }
  }








  int pad_size = 1;
  int stride_size = 1;





  int out_h = (H + 2*pad_size - KH)/stride_size + 1;
  int out_w = (W + 2*pad_size - KW)/stride_size + 1;


  int new_height = H + pad_size*2;
  int new_width = W + pad_size*2;

  int8_t *out_VTA = (int8_t *)malloc(out_h*out_w*KN);


  // VTA DRAM alloc
  // vta's input buffer, as the same size of in_NC_HW16[N][C/16][H][W][16];
  void* vta_input_buf = VTABufferAlloc(N*C*H*W);
  // vta's filter buffer, as the same size of filter_N_C_HW1616[KN/16][C/16][KH][KW][16][16];
  void* vta_filter_buf = VTABufferAlloc(KN*KH*KW*C);
  void* vta_output_buf = VTABufferAlloc(out_h*out_w*KN);

  // VTA Buffer copy
  VTABufferCopy(in_NC_HW16, 0, vta_input_buf, 0, N*C*H*W, 1);
  VTABufferCopy(filter_N_C_HW1616, 0, vta_filter_buf, 0, KN*KH*KW*C, 1);


  uint32_t nTileW, nTileH;
  uint32_t sizeOutTileW = OUT_TILE_SIZE_W;
  uint32_t sizeOutTileH = OUT_TILE_SIZE_H;

  uint32_t sizeInTileW = sizeOutTileW+KW-1;
  uint32_t sizeInTileH = sizeOutTileH+KH-1;

  int isRemainTileW = (out_w%sizeOutTileW)?1:0;
  int isRemainTileH = (out_h%sizeOutTileH)?1:0;
  nTileW = out_w/sizeOutTileW + isRemainTileW;
  nTileH = out_h/sizeOutTileH + isRemainTileH;



/*
  for(int tileH = 0 ; tileH < nTileH; tileH ++) {
    for (int tileW = 0; tileW < nTileW; tileW++) {
*/
  for(int tileH = 0 ; tileH < nTileH; tileH ++) {
    for (int tileW = 0; tileW < nTileW; tileW++) {

      uint32_t x_pad_before = tileW == 0 ? pad_size : 0;
      uint32_t x_pad_after = tileW == (nTileW - 1) ? pad_size : 0;
      uint32_t y_pad_before = tileH == 0 ? pad_size : 0;
      uint32_t y_pad_after = tileH == (nTileH - 1) ? pad_size : 0;
      uint32_t outputWOffset = tileW * sizeOutTileW;
      uint32_t outputHOffset = tileH * sizeOutTileH;




      uint32_t resizedOutTileH = sizeOutTileH;
      uint32_t resizedInTileH = sizeInTileH;
      if(tileH== nTileH -1) {
        resizedOutTileH = out_h - tileH * sizeOutTileH;
        resizedInTileH = resizedOutTileH + KH -1;
      }

      if((tileW != nTileW-1)) {
        conv_kernel(resizedOutTileH,
                    sizeOutTileW,
                    pad_size,
                    x_pad_before,
                    x_pad_after,
                    y_pad_before,
                    y_pad_after,
                    outputWOffset,
                    outputHOffset,
                    resizedInTileH,
                    sizeInTileW,
                    W,
                    vta_input_buf,
                    KH,
                    KW,
                    vta_filter_buf,
                    out_w,
                    vta_output_buf
        );
      }
      else{
        uint32_t sizeOutTileBoundaryW = out_w - tileW * sizeOutTileW;
        uint32_t sizeInTileBoundaryW = sizeOutTileBoundaryW + KW -1;
        conv_kernel(resizedOutTileH,
                    sizeOutTileBoundaryW,
                    pad_size,
                    x_pad_before,
                    x_pad_after,
                    y_pad_before,
                    y_pad_after,
                    outputWOffset,
                    outputHOffset,
                    resizedInTileH,
                    sizeInTileBoundaryW,
                    W,
                    vta_input_buf,
                    KH,
                    KW,
                    vta_filter_buf,
                    out_w,
                    vta_output_buf
        );
      }
    }
  }
  VTABufferCopy(vta_output_buf, 0, out_VTA, 0, out_h*out_w*KN, 2);


  VTARuntimeShutdown();

  // end case
  VTABufferFree( vta_input_buf );
  VTABufferFree( vta_output_buf );
  VTABufferFree( vta_filter_buf );




  // reference output
  int8_t newin[N][new_height][new_width][C];
  for(int i = 0; i<N; i++){
    for(int j = 0; j<new_height; j++){
      for(int k=0; k<new_width; k++){
        for(int l=0; l<C; l++){
          newin[i][j][k][l]=0;
        }

      }
    }
  }

  for (int b = 0; b < N; b++) {
    for (size_t c = 0; c < C; c++) {
      for (int h = 0; h < H; h++) {
        for (int w = 0; w < W; w++) {
          newin[b][pad_size + h][pad_size + w][c] =  in_NHWC[b][h][w][c];
        }
      }
    }
  }

  int8_t result[N][out_h][out_w][KN];
  for(int i = 0; i<N; i++){
    for(int j = 0; j<out_h; j++){
      for(int k=0; k<out_w; k++){
        for(int l=0; l<KN; l++){
          result[i][j][k][l]=0;
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
          /*
          total = total >> 8;
          if(total > 127) total = 127;
          if(total <0) total = 0;
*/
          result[b][i][j][fc] = total;
          new_w_idx += stride_size;
        }
        new_h_idx += stride_size;
        new_w_idx = 0;
      }
      new_h_idx = 0;
    }
  }


  for (int b = 0; b < N; b++){
    for (int f = 0; f < KN; f++){
      for (int i = 0; i<out_h; i++){	//h
        for (int j = 0; j < out_w; j++){	//w

          int out_ch = f / 16;
          int oo = f % 16;


          int8_t v = *(out_VTA+(out_ch*out_h*out_w*16 + i*out_w*16 + j*16 + oo));

/*
          if(i==108&&(j==13))

            printf("CHW : %u|%u|%u  (%d, %d)\n", f,i,j,(int8_t) v, result[b][i][j][f]);
*/
          if ((int8_t) v != result[b][i][j][f]) {
            printf("CHW : %u|%u|%u  (%d, %d)\n", f,i,j,(int8_t) v, result[b][i][j][f]);
            is_diff = -1;
            //return -1;
          }

        }
      }
    }
  }

  free(out_VTA);



  printf("TEST COMPLETE\n");


  return is_diff;
}
