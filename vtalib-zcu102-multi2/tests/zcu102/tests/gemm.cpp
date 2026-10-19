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

#define EVTA_ID 0
#define GEMM_VAL 2

#define INP_IDX 8
#define WEIGHT_IDX 8
#define RESET_IOCTL _IOWR('X', 101, unsigned long)

using namespace std;
using namespace vta;

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

int gemm2(void* idx, VTACommandHandle cmdh)
{
  VTAUopLoopBegin(cmdh, 1,0,0,0);
  VTAUopPush(cmdh,0,0,INP_IDX,INP_IDX,WEIGHT_IDX,0,0,0);
  VTAUopLoopEnd(cmdh);
  return 0;
}

int finit_gemmgemm(void* idx, VTACommandHandle cmdh )
{
  VTAUopLoopBegin(cmdh, 1,0,0,0);
  VTAUopPush(cmdh,0,1,INP_IDX,INP_IDX,WEIGHT_IDX,0,0,0);
  VTAUopLoopEnd(cmdh);
  return 0;
}

int main()
{
  clock_t start, end;
  double result;
  //xlnk_reset();
  // init seed
  srand((unsigned int)time(0));

  // init input data

  int8_t input[16];
  int8_t weight[16][16];
  int8_t output[16];
  int8_t output_mid[16];

  for(int i = 0 ; i < 16; i++)
  {
    input[i] = rand()%20-10;
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


  DataBuffer* input_buf = (DataBuffer*)VTABufferAlloc( 16);
  DataBuffer* weight_buf = (DataBuffer*)VTABufferAlloc( 16*16);
  DataBuffer* output_buf = (DataBuffer*)VTABufferAlloc( 16);


  {
    //VTABufferCopy(input, 0, input_buf, 0, sizeof(input), 1);
    VTABufferCopy(weight, 0, weight_buf, 0, sizeof(weight), 1);
    memcpy((void*)input_buf->virt_addr(), input, sizeof(input));
    memcpy((void*)weight_buf->virt_addr(), weight, sizeof(weight));


    // uop kernel handle
    void *uopHandle1[2];
    uopHandle1[0] = nullptr;
    void *uopHandle2[2];
    uopHandle2[0] = nullptr;

    VTACommandHandle vtaCmdH{nullptr};
    vtaCmdH = VTATLSCommandHandle(EVTA_ID);

    VTASetDebugMode(vtaCmdH, VTA_DEBUG_DUMP_INSN | VTA_DEBUG_DUMP_UOP);

    //reset acc mem
    VTAPushGEMMOp(vtaCmdH,uopHandle1, &finit_gemmgemm, nullptr, 0);

    VTADepPush(vtaCmdH, 2, 1);

    VTADepPop(vtaCmdH, 2, 1);

    VTALoadBuffer2D(vtaCmdH, input_buf, 0, 1, 1, 1, 0, 0, 0, 0, INP_IDX, 2);
    VTALoadBuffer2D(vtaCmdH, weight_buf, 0, 1, 1, 1, 0, 0, 0, 0, WEIGHT_IDX, 1);
    VTADepPush(vtaCmdH, 1, 2);
    VTADepPop(vtaCmdH, 1, 2);

    // run gemm
    VTAPushGEMMOp(vtaCmdH, uopHandle2, &gemm2, nullptr, 0);

    VTADepPush(vtaCmdH, 2, 3);

    VTADepPop(vtaCmdH, 2, 3);

    // store stage-1 result
    VTAStoreBuffer2D(vtaCmdH, INP_IDX, 4, output_buf, 0, 1, 1, 1);

    VTASynchronize(vtaCmdH, 1 << 31);


    VTABufferCopy(output_buf, 0, output, 0, sizeof(output), 2);  // 1 MemCopyToHost
  }



  end = clock();
  result = (double)(end - start);

for(int i = 0; i < 16; i++)
  {
    printf("%d:%d\n", i,  (int8_t)input[i]);
  }

for(int i = 0; i < 16; i++)
  {
    printf("%d:%d\n", i,  (int32_t)output[i]);
  }


  for(int i = 0; i < 16; i++)
  {
    if(input[i]*GEMM_VAL!=(uint32_t)output[i]){
      cout<<"not matched!!\n";
      VTARuntimeShutdown(EVTA_ID);

      VTABufferFree( input_buf );
      VTABufferFree( output_buf);
      VTABufferFree( weight_buf);

      return -1;
    }
  }

  cout<<"Golden Matched\n";
  VTARuntimeShutdown(EVTA_ID);

  VTABufferFree( input_buf );
  VTABufferFree( output_buf);
  VTABufferFree( weight_buf);
  return 0;
}


