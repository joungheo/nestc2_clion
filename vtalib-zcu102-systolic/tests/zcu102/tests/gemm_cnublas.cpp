#include <iostream>
#include <ctime>
#include <cstring>
#include <cstdint>
#include <fstream>
#include <vector>

#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include "vta/runtime.h"
#include "vta/data_buf.h"

#include <time.h>

// #define VTA_COHERENT_ACCESSES true

#define EVTA_ID 0
#define GEMM_VAL 2

#define INP_IDX 16
#define WEIGHT_IDX 16
#define OUT_IDX 16
#define RESET_IOCTL _IOWR('X', 101, unsigned long)

using namespace std;
using namespace vta;

struct timespec u_time;
double get_time(){
  clock_gettime(CLOCK_REALTIME, &u_time);
  return (u_time.tv_sec) + (u_time.tv_nsec) * 1e-9;
}

struct meta{
  int M;
  int K;
  int N;
  int MIDX;
  int NIDX;
  int K2048;
  int SHR;
};

void xlnk_reset() {
  int xlnkfd = open("/dev/xlnk", O_RDWR | O_CLOEXEC);
  if (xlnkfd < 0) {
    printf("Reset failed - could not open device: %d\n", xlnkfd);
    return;
  }
  if (ioctl(xlnkfd, RESET_IOCTL, 0) < 0) {
    printf("Reset failed - IOCTL failed: %d\n", errno);
  }
  close(xlnkfd);
}

int gemm_core(void* par, VTACommandHandle cmdh)
{
  // loop ->               extent,                                 dst,                       src,                   wgt
  // VTAUopPush(handle, mode, reset,  
  //                                                            dstidx,                    srcidx,                wgtidx, 
  //           opcode, useimm, immval)
  struct meta* p = (struct meta*)(par);
  VTAUopLoopBegin(cmdh, (p->K2048/16),                                   0,                        16,                     1);
  VTAUopLoopBegin(cmdh,        16,                                   1,                         1,                     0);
  VTAUopPush(cmdh,0,0,             16+(p->MIDX*(p->N/16) + p->NIDX)*16, 16, 16,  0,0,0);
  VTAUopLoopEnd(cmdh);
  VTAUopLoopEnd(cmdh);
  return 0;
}

int finit_gemmgemm(void* par, VTACommandHandle cmdh )
{
  struct meta* p = (struct meta*)(par);
  int init_num = p->N/16 * p->M / 16;
  VTAUopLoopBegin(cmdh, 16,1,0,0);
  for(int i=0; i<init_num; i++){
    VTAUopPush(cmdh,0,1,INP_IDX+16*i,0,0,0,0,0);
  }
  // VTAUopPush(cmdh,0,1,INP_IDX,INP_IDX,WEIGHT_IDX,0,0,0);
  VTAUopLoopEnd(cmdh);
  return 0;
}

int alu_max(void *par, VTACommandHandle cmdh) {
  struct meta* p = (struct meta*)(par);
  int init_num = p->N/16 * p->M/16;
  VTAUopLoopBegin(cmdh, 16, 1, 1, 0);
  for(int i=0; i<init_num; i++){
    VTAUopPush(cmdh, 1, 0, INP_IDX+16*i, 0, 0, 1, 1, -128);
  }
  VTAUopLoopEnd(cmdh);
  return 0;
}
int alu_min(void *par, VTACommandHandle cmdh) {
  struct meta* p = (struct meta*)(par);
  int init_num = p->N/16 * p->M/16;
  VTAUopLoopBegin(cmdh, 16, 1, 1, 0);
  for(int i=0; i<init_num; i++){
    VTAUopPush(cmdh, 1, 0, INP_IDX+16*i, 0, 0, 0, 1, 127);
  }
  VTAUopLoopEnd(cmdh);
  return 0;
}
int alu_shr(void *par,VTACommandHandle cmdh) {
  struct meta* p = (struct meta*)(par);
  int init_num = p->N/16 * p->M/16;
  VTAUopLoopBegin(cmdh, 16, 1, 1, 0);
  for(int i=0; i<init_num; i++){
    VTAUopPush(cmdh, 1, 0, INP_IDX+16*i, 0, 0, 3, 1, p->SHR);
  }
  VTAUopLoopEnd(cmdh);
  return 0;
}

int main()
{
  xlnk_reset();
  // init seed
  srand((unsigned int)time(0));

  int m = 512;
  int k = 2048;
  int n = 2048;
  
  int input_size = m*k;
  int weight_size = k*n;
  int output_size = m*n;

  // init input data
  int8_t *input  = (int8_t*)malloc(input_size);
  int8_t *weight = (int8_t*)malloc(weight_size);
  int8_t *output = (int8_t*)malloc(output_size);


  //INPUT Matrix

  /* K
   ┌───┐
  M│   │
   └───┘
  */
  int8_t input_temp[input_size];
  for(int i = 0 ; i < m; i++){
    for(int j = 0 ; j < k; j++) {
      input_temp[i*k + j] = rand()%10-5;
    }
  }
  
  //WEIGHT MATRIX
  /* K
   ┌───┐
  N│   │
   └───┘
  */
  int8_t weight_temp[weight_size];
  for(int i = 0 ; i < n; i++){
    for(int j = 0 ; j < k; j++) {
      weight_temp[i*k + j] = rand()%10-5;
    }
  }

  /*     K
   ┌  ─  ┬  ─  ┐
   │  1  │  2  │
  M├  ─  ┼  ─  ┫
   │  3  │  4  │ M/16
   └  ─  ┻  ─  ┘
           K/16
  */
  //M/16,K/16,16,16
  for(int i = 0 ; i < m/16; i++){
    for(int j = 0 ; j < k/16; j++) {
      for(int r = 0; r < 16; r++){
        for(int s = 0; s < 16; s++){
          int tmp_idx = (i*16 + r)*k + (j*16 + s);
          int inp_idx = i*k/16*16*16 + j*16*16 + r*16+ s;
          input[inp_idx] = input_temp[tmp_idx];
        }
      }
    }
  }

  /*     K
   ┌  ─  ┬  ─  ┐
   │  1  │  2  │
  N├  ─  ┼  ─  ┫
   │  3  │  4  │ N/16
   └  ─  ┻  ─  ┘
           K/16
  */
  //N/16,K/16,16,16
  for(int i = 0 ; i < n/16; i++){
      for(int j = 0 ; j < k/16; j++){
        for(int r = 0; r < 16; r++){
          for(int s = 0; s < 16; s++){
            int tmp_idx = (i*16 + r)*k + (j*16 + s);
            int wgt_idx = i*k/16*16*16 + j*16*16 + r*16+ s;
            weight[wgt_idx] = weight_temp[tmp_idx];
          }
        }
      }
  }

  double latency;

latency = get_time();  

  void* input_buf  = VTABufferAlloc( input_size );
  void* weight_buf = VTABufferAlloc( weight_size);
  void* output_buf = VTABufferAlloc( output_size);
  
  VTABufferCopy(input, 0, input_buf, 0, m*k, 1);
  VTABufferCopy(weight, 0, weight_buf, 0, n*k, 1);

  {

    VTACommandHandle vtaCmdH{nullptr};
    vtaCmdH = VTATLSCommandHandle(EVTA_ID);

    int k2048_loop = (k-1)/2048 + 1;
    int m_div_loop = (m/16*n/16)/128;
    int m_size;
    if(m_div_loop == 0){
      m_size = m;
    }else{
      m_size = m / m_div_loop;
    }
    m_div_loop += 1;    
    // uop kernel handle
    void *uop_init[2];
    uop_init[0] = nullptr;

    void *uop_shr[2];
    uop_shr[0] = nullptr;

    void *uop_max[2];
    uop_max[0] = nullptr;

    void *uop_min[2];
    uop_min[0] = nullptr;

    struct meta par;
    par.M = m_size;
    par.K = k;
    par.N = n;

    void*** uop = (void***)malloc((m_size/16*n/16*k2048_loop) * sizeof(size_t));
    for(int i =0; i<(m_size/16*n/16*k2048_loop); i++){
      uop[i] = (void**)malloc(2  * sizeof(size_t));
      uop[i][0] = nullptr;
    }

    for(int m_div = 0; m_div < m_div_loop; m_div++){

      //reset acc mem
      VTAPushGEMMOp(vtaCmdH,uop_init, &finit_gemmgemm, &par, 0);

      VTADepPush(vtaCmdH, 2, 1);    VTADepPop(vtaCmdH, 2, 1);
      // run gemm
      for(int i = 0; i < m_size/16; i++){
        
        for(int k_div = 0; k_div < k2048_loop; k_div++){  
          int k2048 = 2048;
          if(k%2048 != 0 && k2048_loop == k_div+1){
            k2048 = k % 2048;
          }

          int in_16_K     = 16 * k/16;
          int in_16_K2048 = 16 * k2048/16;
          VTALoadBuffer2D(vtaCmdH, input_buf,  m_div*m_size*in_16_K/16 + in_16_K*i + k_div*2048,  in_16_K2048,  1, 1, 0, 0, 0, 0, INP_IDX, 2);

          for(int j = 0; j < n/16; j++){
            par.MIDX = i;
            par.NIDX = j;

            int wg_16_K     = k/16;
            int wg_16_K2048 = k2048/16;
            par.K2048 = k2048;

            VTALoadBuffer2D(vtaCmdH, weight_buf, wg_16_K*j + 2048/16*k_div,  wg_16_K2048, 1, 1, 0, 0, 0, 0, WEIGHT_IDX, 1);

            VTADepPush(vtaCmdH, 1, 2);    VTADepPop(vtaCmdH, 1, 2);
            
            VTAPushGEMMOp(vtaCmdH, uop[k_div*m_size/16*n/16 + i*n/16 +j], &gemm_core, &par, 0);

            VTADepPush(vtaCmdH, 2, 1);    VTADepPop(vtaCmdH, 2, 1);  
          }
        }
      }
      VTADepPush(vtaCmdH, 1, 2);    VTADepPop(vtaCmdH, 1, 2);
      par.SHR = 6;
      VTAPushALUOp(vtaCmdH, uop_shr, &alu_shr, &par, 0);
      VTAPushALUOp(vtaCmdH, uop_max, &alu_max, &par, 0);
      VTAPushALUOp(vtaCmdH, uop_min, &alu_min, &par, 0);

      VTADepPush(vtaCmdH, 2, 3);    VTADepPop(vtaCmdH, 2, 3);

      // store stage-1 result
      int ou_M_N_d16 = m_size * n/16;
      VTAStoreBuffer2D(vtaCmdH, OUT_IDX, 4, output_buf, m_div*m_size*n/16, ou_M_N_d16, 1, 1);
      VTADepPush(vtaCmdH, 3, 2);    VTADepPop(vtaCmdH, 3, 2);  
    }
    double latency_sub = get_time();  
    VTASynchronize(vtaCmdH, 1 << 31);
    latency_sub =get_time() - latency_sub;
    double OPS = (double)(2.f*k-1.f);
    OPS = (double)(m*n)*OPS * 1e-9;
    printf("m k n : %d %d %d => %lf GOPS (time : %lf)\n",m,k,n, OPS/latency_sub, latency_sub);
  }


  VTABufferCopy(output_buf, 0, output, 0, output_size, 2);  // 1 MemCopyToHost


  //검증..
  int8_t golden_temp[output_size];
  for(int i = 0; i< m; i++){
    for(int j = 0; j < n; j++){
      int summ = 0;
      for(int z = 0; z < k; z++){
        summ += input_temp[i*k +z] * weight_temp[j*k+z];
      }
      summ = summ >> 6;
      if(summ > 127) summ = 127;
      if(summ < -128) summ = -128;
      golden_temp[i*n+j] = (int8_t)summ;
    }
  }

  int8_t golden[output_size];
  for(int i = 0 ; i < m/16; i++){
    for(int j = 0 ; j < n/16; j++) {
      for(int r = 0; r < 16; r++){
        for(int s = 0; s < 16; s++){
          int tmp_idx = (i*16 + r)*n + (j*16 + s);
          int out_idx = i*n/16*16*16 + j*16*16 + r*16+ s;
          golden[out_idx] = golden_temp[tmp_idx];
        }
      }
    }
  }

  for(int i = 0; i < m*n/16; i++){
    for(int j = 0; j < 16; j++){
      if(golden[i*16+j]!=output[i*16+j]){
        cout<<"not matched!!\n";
        VTARuntimeShutdown(EVTA_ID);

        VTABufferFree( input_buf );
        VTABufferFree( output_buf);
        VTABufferFree( weight_buf);

        return -1;
      }
    }
  }

  cout<<"Golden Matched\n";
  VTARuntimeShutdown(EVTA_ID);

  VTABufferFree( input_buf );
  VTABufferFree( output_buf);
  VTABufferFree( weight_buf);
  return 0;
}

