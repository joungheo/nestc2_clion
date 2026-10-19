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

int addimm(void* param, VTACommandHandle cmdh)
{
  int *p = (int*)(param);
  VTAUopLoopBegin( cmdh, *p,0,0,0);
  VTAUopPush(cmdh, 1, 0, 0, 0, 0, 2, 1, IMM_VAL);
  VTAUopLoopEnd(cmdh);
  return 0;
}

int main()
{
  clock_t start, end;
  double result;

  srand((unsigned int)time(0));
  xlnk_reset();

  uint32_t input1[16];
  uint8_t output[16];
  for(int i = 0 ; i < 16; i++)
  {
    input1[i] = rand()%100;
  }

  void* input_buf = VTABufferAlloc( 4*16);
  void* output_buf = VTABufferAlloc( 16);

  VTABufferCopy(input1, 0, input_buf, 0, sizeof(input1), 1);


  void* uopHandle2[2];
  uopHandle2[0]=nullptr;

  VTACommandHandle vtaCmdH{nullptr};
  vtaCmdH = VTATLSCommandHandle();

  VTASetDebugMode(vtaCmdH,VTA_DEBUG_DUMP_INSN | VTA_DEBUG_DUMP_UOP);

  start = clock();

  VTALoadBuffer2D(vtaCmdH, input_buf, 0, 1, 1, 1, 0, 0, 0, 0, 0, 3);

  int sign = 1;
  VTAPushALUOp(vtaCmdH, uopHandle2, &addimm, &sign, 0);

  VTADepPush(vtaCmdH,2,3);
  VTADepPop(vtaCmdH,2,3);
  VTAStoreBuffer2D(vtaCmdH , 0, 4, output_buf, 0, 1, 1, 1 );

  VTASynchronize(vtaCmdH,1<<31);

  end = clock();
  result = (double)(end - start);
  printf("VTA: ADD execution time: %.5f ms \n", result);

  VTABufferCopy(output_buf, 0, output, 0, sizeof(output), 2);  // 1 MemCopyToHost
  for(int i = 0; i < 16; i++)
  {
    printf("%d %d\n",input1[i]+IMM_VAL,(uint32_t)output[i]);
  }

  VTARuntimeShutdown();

  VTABufferFree( input_buf );
  VTABufferFree( output_buf);
  return 0;
}
