#include <vta/runtime.h>

#include <ctime>
#include <cstring>
#include <cstdint>
#include <fstream>
#include <vector>



#include <sys/ioctl.h>


#include <time.h>




using namespace std;

//#include "xtensor/xarray.hpp"
//#include "xtensor/xio.hpp"

#define BITSTREAM_PATH "1x16_i8w8a32_15_15_18_17.bin"

#define BS_FPGA_MAN "/sys/class/fpga_manager/fpga0/firmware"
#define BS_FPGA_MAN_FLAGS "/sys/class/fpga_manager/fpga0/flags"


void writeBitStream()
{
  fstream fs_flag;
  fs_flag.open(BS_FPGA_MAN_FLAGS,std::fstream::out);
  fs_flag << "1";  //  // flag 1 = download bitstream fully

  fstream fs_bitstream;
  fs_bitstream.open(BS_FPGA_MAN,std::fstream::out);
  fs_bitstream << BITSTREAM_PATH;
}



#define IMM_VAL 2

int addimm(void* param)
{
  int *p = (int*)(param);
  VTAUopLoopBegin( *p,0,0,0);
  VTAUopPush(1, 0, 0, 0, 0, 2, 1, IMM_VAL);
  VTAUopLoopEnd();
  return 0;
}
int add_16_test()
{
  clock_t start, end;
  double result;

  // init seed
  srand((unsigned int)time(0));

  // 안해도 뒤에서 open해서 메모리는 mapping시키는데 free안된 경우를 위해 강제 reset
   writeBitStream();

  // init input data

  uint32_t input1[16];
  uint8_t output[16];
  for(int i = 0 ; i < 16; i++)
  {
    input1[i] = rand()%100;
  }

  void* input_buf = VTABufferAlloc( 4*16);

  void* output_buf = VTABufferAlloc( 16);

  VTABufferCopy(input1, 0, input_buf, 0, sizeof(input1), 1);


  // uop kernel handle
  void* uopHandle1[2];
  void* uopHandle2[2];
  uopHandle1[0]=nullptr;
  uopHandle2[0]=nullptr;

  VTACommandHandle vtaCmdH{nullptr};
  vtaCmdH = VTATLSCommandHandle();

  VTASetDebugMode(vtaCmdH,VTA_DEBUG_DUMP_INSN | VTA_DEBUG_DUMP_UOP);

  start = clock();

  VTALoadBuffer2D(vtaCmdH, input_buf, 0, 1, 1, 1, 0, 0, 0, 0, 0, 3);


  int sign = 1;
  VTAPushALUOp(uopHandle2, &addimm, &sign, 0);



  VTADepPush(vtaCmdH,2,3);
  VTADepPop(vtaCmdH,2,3);
  VTAStoreBuffer2D(vtaCmdH , 0, 4, output_buf, 0, 1, 1, 1 );

  VTASynchronize(vtaCmdH,1<<31);

  end = clock();
  result = (double)(end - start);

  VTABufferCopy(output_buf, 0, output, 0, sizeof(output), 2);  // 1 MemCopyToHost
  for(int i = 0; i < 16; i++)
  {
    if(input1[i]+IMM_VAL!=(uint32_t)output[i]){
      return -1;
    }
  }

  VTARuntimeShutdown();

  VTABufferFree( input_buf );
  VTABufferFree( output_buf);
  return 0;
}


