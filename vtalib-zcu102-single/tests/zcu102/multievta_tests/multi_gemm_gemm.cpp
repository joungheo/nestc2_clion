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

int gemm2(void* param, VTACommandHandle cmdh)
{
  VTAUopLoopBegin( cmdh,1,0,0,0);
  VTAUopPush(cmdh,0, 1, 0, 0, 0, 2, 0, 0);
  VTAUopLoopEnd(cmdh);
  return 0;
}

int finit_gemmgemm(void*,VTACommandHandle cmdh)
{
  VTAUopLoopBegin(cmdh,1,0,0,0);
  VTAUopPush(cmdh,0,1,0,0,0,0,0,0);
  VTAUopLoopEnd(cmdh);
  return 0;
}

void *th_vta(void *arg) {
  pthread_t tid;
  tid = pthread_self();

  int evta_id = *(int*)arg;


  for(int iter=0; iter<100; iter++) {

    int8_t input[16];
    int8_t weight[16][16];
    int8_t output[16];
    int8_t output2[16];

    for (int i = 0; i < 16; i++) {
      input[i] = rand() % 20 - 10;
    }
    for (int i = 0; i < 16; i++) {
      for (int j = 0; j < 16; j++) {
        if (i == j) {
          weight[i][j] = 2;
        } else {
          weight[i][j] = 0;
        }
      }
    }

    void *input_buf = VTABufferAlloc(16);
    void *weight_buf = VTABufferAlloc(16 * 16);
    void *output_buf = VTABufferAlloc(16);
    void *output_buf2 = VTABufferAlloc(16);

    {
      VTABufferCopy(input, 0, input_buf, 0, sizeof(input), 1);
      VTABufferCopy(weight, 0, weight_buf, 0, sizeof(weight), 1);


      // uop kernel handle
      void *uopHandle1[2];
      uopHandle1[0] = nullptr;
      void *uopHandle2[2];
      uopHandle2[0] = nullptr;

      VTACommandHandle vtaCmdH{nullptr};
      vtaCmdH = VTATLSCommandHandle(evta_id);

      //VTASetDebugMode(vtaCmdH, VTA_DEBUG_DUMP_INSN | VTA_DEBUG_DUMP_UOP);

      //reset acc mem
      VTAPushGEMMOp(vtaCmdH, uopHandle1, &finit_gemmgemm, nullptr, 0);

      VTADepPush(vtaCmdH, 2, 1);

      VTADepPop(vtaCmdH, 2, 1);

      VTALoadBuffer2D(vtaCmdH, input_buf, 0, 1, 1, 1, 0, 0, 0, 0, 0, 2);
      VTALoadBuffer2D(vtaCmdH, weight_buf, 0, 1, 1, 1, 0, 0, 0, 0, 0, 1);
      VTADepPush(vtaCmdH, 1, 2);
      VTADepPop(vtaCmdH, 1, 2);

      // run gemm
      VTAPushGEMMOp(vtaCmdH, uopHandle2, &gemm2, nullptr, 0);

      VTADepPush(vtaCmdH, 2, 3);

      VTADepPop(vtaCmdH, 2, 3);

      // store stage-1 result
      VTAStoreBuffer2D(vtaCmdH, 0, 4, output_buf, 0, 1, 1, 1);
      VTADepPush(vtaCmdH, 3, 2);
      VTADepPop(vtaCmdH, 3, 2);
      VTASynchronize(vtaCmdH, 1 << 31);

      void *uopHandle3[2];
      uopHandle3[0] = nullptr;

      // for dependency queue form 3 to 1
      VTAPushGEMMOp(vtaCmdH, uopHandle3, &finit_gemmgemm, nullptr, 0);
      VTADepPush(vtaCmdH, 2, 1);
      VTADepPop(vtaCmdH, 2, 1);

      // load stage-1 result into inp mem
      VTALoadBuffer2D(vtaCmdH, output_buf, 0, 1, 1, 1, 0, 0, 0, 0, 0, 2);
      VTADepPush(vtaCmdH, 1, 2);
      VTADepPop(vtaCmdH, 1, 2);
      void *uopHandle4[2];
      uopHandle4[0] = nullptr;

      // run gemm
      VTAPushGEMMOp(vtaCmdH, uopHandle4, &gemm2, nullptr, 0);

      VTADepPush(vtaCmdH, 2, 3);
      VTADepPop(vtaCmdH, 2, 3);

      // store stage-2 result
      VTAStoreBuffer2D(vtaCmdH, 0, 4, output_buf2, 0, 1, 1, 1);

      VTASynchronize(vtaCmdH, 1 << 31);

      VTABufferCopy(output_buf, 0, output, 0, sizeof(output), 2);  // 1 MemCopyToHost
      VTABufferCopy(output_buf2, 0, output2, 0, sizeof(output2), 2);  // 1 MemCopyToHost
    }

    for (int i = 0; i < 16; i++) {
      if (input[i] * 4 != (int32_t) output2[i]) {
        cout << "vta(" << evta_id << "Golden NOT Matched : " << input[i] * 4 << ", " << (int32_t) output[i] << ", "
             << (int32_t) output2[i] << "\n";
        *(int *) arg = -1;
        return (void *) arg;
      }
    }

    cout << "vta(" << evta_id << ") Golden Matched\n";
    VTARuntimeShutdown(evta_id);

    VTABufferFree(input_buf);
    VTABufferFree(output_buf);
    VTABufferFree(output_buf2);
    VTABufferFree(weight_buf);

    *(int *) arg = 0;
  }
  return (void*)arg;

}

int main()
{
  clock_t start, end;
  double result;
  xlnk_reset();
  // init seed
  srand((unsigned int)time(0));

  pthread_t thread[4];
  int a[4];

  int r =0;

  for(int i=0;i<4;i++) {
    a[i]=i;
    pthread_create(&thread[i],NULL,th_vta, (void*) &a[i]);
  }

  int status;
  for(int i=0;i<4;i++) {
    pthread_join(thread[i], (void **)&status);
  }

  printf("\n--------------round %d end-----------------\n\n",r++);
  sleep(1);




  return 0;
}


