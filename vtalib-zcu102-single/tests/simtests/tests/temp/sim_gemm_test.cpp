#include <vta/runtime.h>
#include <iostream>
#include <ctime>
#include <cstring>
#include <cstdint>
#include <fstream>
#include <vector>

#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>


#include <time.h>

using namespace std;



int gemm(void* param)
{
  VTAUopLoopBegin( 1,0,0,0);
  VTAUopPush(1, 0, 0, 0, 0, 2, 0, 0);
  VTAUopLoopEnd();
  return 0;
}

int finit_0(void*)
{
  VTAUopLoopBegin(1,0,0,0);
  VTAUopPush(0,1,0,0,0,0,0,0);
  VTAUopLoopEnd();
  return 0;
}

int gemm_16_test()
{
  clock_t start, end;
  double result;

  // init seed
  srand((unsigned int)time(0));

  // init input data

  int8_t input[16];
  int8_t weight[16][16];
  int8_t output[16];
  for(int i = 0 ; i < 16; i++)
  {
    input[i] = rand()%20-10;
  }
  for(int i = 0 ; i < 16; i++)
  {
    for(int j = 0 ; j < 16; j++) {
      if(i==j){
        weight[i][j] = 2;
      }
      else {
        weight[i][j] = 0;
      }
    }
  }


  void* input_buf = VTABufferAlloc( 16);
  void* weight_buf = VTABufferAlloc( 16*16);
  void* output_buf = VTABufferAlloc( 16);

  VTABufferCopy(input, 0, input_buf, 0, sizeof(input), 1);
  VTABufferCopy(weight, 0, weight_buf, 0, sizeof(weight), 1);


  // uop kernel handle
  void* uopHandle1[2];
  uopHandle1[0]=nullptr;
  void* uopHandle2[2];
  uopHandle2[0]=nullptr;

  VTACommandHandle vtaCmdH{nullptr};
  vtaCmdH = VTATLSCommandHandle();

  VTASetDebugMode(vtaCmdH,VTA_DEBUG_DUMP_INSN | VTA_DEBUG_DUMP_UOP);

  start = clock();



  VTAPushGEMMOp(uopHandle1,&finit_0,nullptr,0);

  VTADepPush(vtaCmdH,2,1);

/*! Mem ID constant: uop memory */
//#define VTA_MEM_ID_UOP 0
/*! Mem ID constant: weight memory */
//#define VTA_MEM_ID_WGT 1
/*! Mem ID constant: input memory */
//#define VTA_MEM_ID_INP 2
/*! Mem ID constant: accumulator/bias memory */
//#define VTA_MEM_ID_ACC 3
/*! Mem ID constant: output store buffer */
//#define VTA_MEM_ID_OUT 4
  VTADepPop(vtaCmdH,2,1);

  VTALoadBuffer2D(vtaCmdH, input_buf, 0, 1, 1, 1, 0, 0, 0, 0, 0, 2);
  VTALoadBuffer2D(vtaCmdH, weight_buf, 0, 1, 1, 1, 0, 0, 0, 0, 0, 1);
  VTADepPush(vtaCmdH,1,2);
  VTADepPop(vtaCmdH,1,2);

  VTAPushGEMMOp(uopHandle2,&gemm,nullptr,0);


  //acc mem init
  VTADepPush(vtaCmdH,2,3);

  VTADepPop(vtaCmdH,2,3);
  VTAStoreBuffer2D(vtaCmdH , 0, 4, output_buf, 0, 1, 1, 1 );

  VTASynchronize(vtaCmdH,1<<31);

  end = clock();
  result = (double)(end - start);

  VTABufferCopy(output_buf, 0, output, 0, sizeof(output), 2);  // 1 MemCopyToHost
  for(int i = 0; i < 16; i++)
  {
    if(input[i]*2!=(uint32_t)output[i]){
      return -1;
    }
  }

  VTARuntimeShutdown();

  VTABufferFree( input_buf );
  VTABufferFree( output_buf);
  return 0;
}


