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
#include <time.h>


#define RESET_IOCTL _IOWR('X', 101, unsigned long)

using namespace std;

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




#define IMM_VAL 2
#define GEMM_VAL 3
int gemm2(void* param, VTACommandHandle cmdh)
{
  VTAUopLoopBegin( cmdh, 1,1,1,0);
  VTAUopPush(cmdh, 0, 0, 0, 0, 0, 2, 0, 0);
  VTAUopLoopEnd(cmdh);
  return 0;
}

int mulimm(void* param, VTACommandHandle cmdh)
{
  VTAUopLoopBegin( cmdh, 1,0,0,0);
  VTAUopPush(cmdh, 1, 0, 0, 0, 0, 4, 1, IMM_VAL);
  VTAUopLoopEnd(cmdh);
  return 0;
}

int initacc(void* param, VTACommandHandle cmdh)
{
  VTAUopLoopBegin(cmdh,  1024,1,0,0);
  VTAUopPush(cmdh, 0, 1, 0, 0, 0, 2, 0,0);
  VTAUopLoopEnd(cmdh);
  return 0;

}
int main()
{
  clock_t start, end;
  double result;

  srand((unsigned int)time(0));
  xlnk_reset();

  int32_t input1[16];
  int8_t output[16];
  int8_t weight[16][16];

  for(int i = 0 ; i < 16; i++)
  {
    input1[i] = rand()%30-15;
    output[i] = 0;
  }
  for(int i = 0 ; i < 16; i++)
  {
    for(int j = 0 ; j < 16; j++) {
      if(i==j){
        weight[i][j] = GEMM_VAL;
      }
      else {
        weight[i][j] = 0;
      }
    }
  }

  void* input_buf = VTABufferAlloc( 4*16);
  void* output_buf = VTABufferAlloc( 16);
  void* output_buf_int = VTABufferAlloc( 16);
  void* weight_buf = VTABufferAlloc( 16*16);

  VTABufferCopy(input1, 0, input_buf, 0, sizeof(input1), 1);
  VTABufferCopy(weight, 0, weight_buf, 0, sizeof(weight), 1);

  void* uopHandle2[2];
  uopHandle2[0]=nullptr;

  VTACommandHandle vtaCmdH{nullptr};
  vtaCmdH = VTATLSCommandHandle();

  VTASetDebugMode(vtaCmdH,VTA_DEBUG_DUMP_INSN | VTA_DEBUG_DUMP_UOP);

  start = clock();

  VTALoadBuffer2D(vtaCmdH, input_buf, 0, 1, 1, 1, 0, 0, 0, 0, 0, 3);

  int sign = 1;
  VTAPushALUOp(vtaCmdH, uopHandle2, &mulimm, &sign, 0);

  // Move instruction
  int src_addr=0;
  //VTALoadBuffer2D(vtaCmdH, &src_addr, 0, 1, 1, 1, 0, 0, 0, 0, 0, VTA_MEM_ID_MOVE);
  VTALoadBuffer2D(vtaCmdH, &src_addr, 0, 1, 1, 1, 0, 0, 0, 0, 0, 5);

  // init Acc mem
  void* uopHandle_init[2];
  uopHandle_init[0]=nullptr;
  VTAPushGEMMOp(vtaCmdH, uopHandle_init, &initacc, nullptr, 0);




  VTADepPush(vtaCmdH,2,1);
  VTADepPop(vtaCmdH,2,1);


  // Load Weight
  VTALoadBuffer2D(vtaCmdH, weight_buf, 0, 1, 1, 1, 0, 0, 0, 0, 0, 1);
  VTADepPush(vtaCmdH,1,2);
  VTADepPop(vtaCmdH,1,2);


  // GEMM using moved inp mem
  void* uopHandle3[2];
  uopHandle3[0]=nullptr;
  VTAPushGEMMOp(vtaCmdH, uopHandle3, &gemm2, nullptr, 0);



  VTADepPush(vtaCmdH,2,3);
  VTADepPop(vtaCmdH,2,3);


  VTAStoreBuffer2D(vtaCmdH , 0, 4, output_buf, 0, 1, 1, 1 );
  VTADepPush(vtaCmdH,3,2);
  VTADepPop(vtaCmdH,3,2);

  VTASynchronize(vtaCmdH,1<<31);

  end = clock();
  result = (double)(end - start);
  printf("VTA: MUL->MOV->GEMM execution time: %.5f ms \n", result);

  VTABufferCopy(output_buf, 0, output, 0, sizeof(output), 2);  // 1 MemCopyToHost

  int8_t output_int[16];
  VTABufferCopy(output_buf_int, 0, output_int, 0, sizeof(output_int), 2);  // 1 MemCopyToHost
  for(int i = 0; i < 16; i++)
  {
    if(input1[i]*IMM_VAL*GEMM_VAL != (int8_t)output[i]){
      printf("Golden mismatched\n");
      return -1;
    }
    //printf("%d -> %d : %d\n",input1[i], input1[i]*IMM_VAL*GEMM_VAL,(int8_t)output[i]);
  }
  cout<<"Golden Matched\n";
  VTARuntimeShutdown();

  VTABufferFree( input_buf );
  VTABufferFree( output_buf);
  return 0;
}
