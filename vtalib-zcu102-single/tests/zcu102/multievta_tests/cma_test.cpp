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
#include <assert.h>
#include "vta/data_buf.h"
#include <stdlib.h>
#include <cstdlib>
#define RESET_IOCTL _IOWR('X', 101, unsigned long)

using namespace std;


int main(int argc, char** argv) {
  std::cout << "TEST START" << std::endl;
  int M=200;
  char *pos = NULL;
  if (argc > 1) {
    M = atoi(argv[1]);
  }

  int key = 0;

  if (argc > 2) {
    key = atoi(argv[2]);
  }
  std::cout<<M<<std::endl;
  vta::DataBuffer *d_buff = (vta::DataBuffer *)VTABufferAlloc(M * 1024*1024);
  char *v_buff = (char *)(d_buff->virt_addr());
  for(int i = 0; i < M * 1024*1024; i++){
    v_buff[i] = (i + M + key)%256;
  }
  std::cout<<"TEST ALLOCATED"<<std::endl;
  sleep(20);
  for(int i = 0; i < M * 1024*1024; i++){
    if(v_buff[i] != (i + M + key)%256)
      return -1;
  }
  std::cout<<"TEST FINISHED"<<std::endl;
  VTABufferFree(d_buff);
  return 0;
}
