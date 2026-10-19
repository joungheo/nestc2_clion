#include <iostream>
#include <cstring>
#include <cstdint>
#include <stdlib.h>
#include <unistd.h>
#include <assert.h>
#include <vta/runtime.h>

#include <time.h>

using namespace std;



namespace sim_test_conv2 {
int finit_gemmop_init(void *param) {
  VTAUopLoopBegin(1, 0, 0, 0);
  VTAUopPush(0, 1, 0, 0, 0, 0, 0, 0);
  VTAUopLoopEnd();
  return 0;
}

int finit_gemmop_3x3(void *param) {
  VTAUopLoopBegin(3, 0, 3, 3);
  VTAUopLoopBegin(3, 0, 1, 1);
  VTAUopPush(0, 0, 0, 0, 0, 0, 0, 0);
  VTAUopLoopEnd();
  VTAUopLoopEnd();
  return 0;
}

// alu func
int finit_shr(void *param) {
  int *p = (int *) (param);
  VTAUopLoopBegin(1, 0, 0, 0);
  VTAUopPush(1, 0, 0, 0, 0, 3, 1, 8);
  //VTAUopPush(1, 0, 0, 0, 0, 3, 1, 7); // input 5. output 7. 12shift
  VTAUopLoopEnd();
  return 0;
}

int finit_max(void *param) {
  int *p = (int *) (param);
  VTAUopLoopBegin(1, 0, 0, 0);
  VTAUopPush(1, 0, 0, 0, 0, 1, 1, 0);
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
}

using namespace sim_test_conv2;
int conv2_test(){
  int is_diff = 0;
  srand(time(NULL));

  //N, H, W, C
  //N == 1
  //group == 1
  struct timeval tv_s, tv_e;



  int N = 1;
  int H = 14;
  int W = 14;
  int C = 16;
  int KN = 16;
  int KH = 3;
  int KW = 3;

  // input NCHW
  int8_t in_NCHW[N][C][H][W];


  for(int i =0; i < N; i++){
    for(int j =0 ; j < H; j++){
      for(int k =0; k< W; k++){
        for(int l =0; l<C; l++){
          in_NCHW[i][l][j][k]=(int8_t)(rand()%60);
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
          filter_NHWC[i][j][k][l]=(int8_t)(rand()%60);
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








  int batch_size = N;
  int pad_size = 1;
  int stride_size = 1;

  int filter_h = KH;
  int filter_w = KW;
  int filter_size = KN;



  int height = H;
  int width = W;
  size_t channel_size = C;

  int out_h = (height + 2*pad_size - filter_h)/stride_size + 1;
  int out_w = (width + 2*pad_size - filter_w)/stride_size + 1;


  int new_height = height + pad_size*2;
  int new_width = width + pad_size*2;


  int ic = channel_size / 16;
  int oc = filter_size / 16;



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


  for(int outHIdx = 0 ; outHIdx < out_h; outHIdx ++) {
    for (int outWIdx = 0; outWIdx < out_w; outWIdx++) {
      // acc buffer reset
      void *uopHandle_reset[2];
      uopHandle_reset[0] = nullptr;
      VTACommandHandle vtaCmdH{nullptr};
      vtaCmdH = VTATLSCommandHandle();
      VTASetDebugMode(vtaCmdH, VTA_DEBUG_DUMP_INSN | VTA_DEBUG_DUMP_UOP);
      VTAPushGEMMOp(uopHandle_reset, &finit_gemmop_init, nullptr, 0);

      VTADepPush(vtaCmdH, 2, 1);
      VTADepPop(vtaCmdH, 2, 1);

      uint32_t x_pad_before = outWIdx < pad_size ? pad_size - outWIdx : 0;
      uint32_t x_pad_after = outWIdx>=out_w-pad_size?out_w-pad_size-outWIdx+1:0;

      uint32_t y_pad_before = outHIdx < pad_size ? pad_size - outHIdx : 0;
      uint32_t y_pad_after = outHIdx >= out_h - pad_size ? out_h - pad_size - outHIdx + 1 : 0;


      uint32_t sram_offset = (outHIdx < pad_size ? 0 : outHIdx - pad_size) * W;
      sram_offset += outWIdx < pad_size ? 0 : outWIdx - pad_size;
      uint32_t y_size = KH - y_pad_before - y_pad_after;
      uint32_t x_size = KW - x_pad_before - x_pad_after;

      VTALoadBuffer2D(vtaCmdH, vta_input_buf,
                      sram_offset,
                      x_size,
                      y_size,
                      W,
                      x_pad_before,
                      y_pad_before,
                      x_pad_after,
                      y_pad_after,
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
      VTAPushGEMMOp(uopHandle_gemm, &finit_gemmop_3x3, nullptr, 0);

      int param_alu = 196;

      void *uopHandle_shr[2];
      uopHandle_shr[0] = nullptr;
      VTAPushALUOp(uopHandle_shr, &finit_shr, &param_alu, 0);

      void *uopHandle_max[2];
      uopHandle_max[0] = nullptr;
      VTAPushALUOp(uopHandle_max, &finit_max, &param_alu, 0);

      void *uopHandle_min[2];
      uopHandle_min[0] = nullptr;
      VTAPushALUOp(uopHandle_min, &finit_min, &param_alu, 0);

      VTADepPush(vtaCmdH, 2, 3);
      VTADepPop(vtaCmdH, 2, 3);

      VTAStoreBuffer2D(vtaCmdH, 0, 4, vta_output_buf, outHIdx*out_w+outWIdx, 1, 1, 1);
      VTASynchronize(vtaCmdH, 1 << 31);
    }
  }
  VTABufferCopy(vta_output_buf, 0, out_VTA, 0, out_h*out_w*filter_size, 2);


  VTARuntimeShutdown();

  // end case
  VTABufferFree( vta_input_buf );
  VTABufferFree( vta_output_buf );
  VTABufferFree( vta_filter_buf );





  // reference output
  int8_t newin[batch_size][new_height][new_width][channel_size];
  for(int i = 0; i<batch_size; i++){
    for(int j = 0; j<new_height; j++){
      for(int k=0; k<new_width; k++){
        for(int l=0; l<channel_size; l++){
          newin[i][j][k][l]=0;
        }

      }
    }
  }

  for (int b = 0; b < batch_size; b++) {
    for (size_t c = 0; c < channel_size; c++) {
      for (int h = 0; h < height; h++) {
        for (int w = 0; w < width; w++) {
          newin[b][pad_size + h][pad_size + w][c] =  in_NHWC[b][h][w][c];
        }
      }
    }
  }

  int8_t result[batch_size][out_h][out_w][filter_size];
  for(int i = 0; i<batch_size; i++){
    for(int j = 0; j<out_h; j++){
      for(int k=0; k<out_w; k++){
        for(int l=0; l<filter_size; l++){
          result[i][j][k][l]=0;
        }

      }
    }
  }

  int new_h_idx = 0, new_w_idx = 0;
  for (int b = 0; b < batch_size; b++) {
    for (int fc = 0; fc < filter_size; fc++) {
      for (int i = 0; i < out_h; i++) {
        for (int j = 0; j < out_w; j++) {

          int32_t total = 0;
          for (int fh = 0; fh < filter_h; fh++) {
            for (int fw = 0; fw < filter_w; fw++) {
              for (size_t f = 0; f < channel_size; f++) {

                total = total +
                    newin[b][new_h_idx + fh][new_w_idx + fw][f] * filter_NHWC[fc][fh][fw][f];
              }
            }
          }
          total = total >> 8;
          if(total > 127) total = 127;
          if(total <0) total = 0;

          result[b][i][j][fc] = total;
          new_w_idx += stride_size;
        }
        new_h_idx += stride_size;
        new_w_idx = 0;
      }
      new_h_idx = 0;
    }
  }



  for (int b = 0; b < batch_size; b++){
    for (int f = 0; f < filter_size; f++){
      for (int i = 0; i<out_h; i++){	//h
        for (int j = 0; j < out_w; j++){	//w

          int out_ch = f / 16;
          int oo = f % 16;

          int8_t v = *(out_VTA+(out_ch*out_h*out_w*16 + i*out_w*16 + j*16 + oo));

          //printf("%u, %u\n", (uint8_t) v, result[b][i][j][f]);
          if ((uint8_t) v != result[b][i][j][f]) {
            is_diff = -1;
            return -1;
          }

        }
      }
    }
  }

  free(out_VTA);




  return is_diff;
}
