#include <iostream>
#include <ctime>
#include <cstring>
#include <cstdint>
#include <fstream>
#include <vector>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include <vta/runtime.h>

#include <time.h>

using namespace std;

#define EVTA_ID 0


namespace sim_test_conv1 {


int finit_gemmop_init(void *param,VTACommandHandle cmdh ) {
  // p = oc
//  int *p = (int *) param;
  VTAUopLoopBegin(cmdh, 14, 14, 0, 0);
  VTAUopLoopBegin(cmdh, 14, 1, 0, 0);
  //for(int j=0;j<14;j++) { // w,h
  VTAUopPush(cmdh, 0, 1, 0, 0, 0, 0, 0, 0);
  //VTAUopPush(0,1,j,0,0,0,0,0);
  //}
  VTAUopLoopEnd(cmdh);
  VTAUopLoopEnd(cmdh);
  return 0;
}

int finit_gemmop_3x3(void *param, VTACommandHandle cmdh) {

  VTAUopLoopBegin(cmdh, 14, 14, 16, 0);
  VTAUopLoopBegin(cmdh, 3, 0, 16, 3);
  for (int dx = 0; dx < 3; dx++) {
    for (int j = 0; j < 14; j++) {
      VTAUopPush(cmdh, 0, 0, j, (j + dx), dx, 0, 0, 0);
    }
  }
  VTAUopLoopEnd(cmdh);
  VTAUopLoopEnd(cmdh);
  return 0;
}

// alu func
int finit_shr(void *param, VTACommandHandle cmdh) {
  
  
  int *p = (int*) param;
  VTAUopLoopBegin(cmdh, *p, 1, 1, 0);
  VTAUopPush(cmdh, 1, 0, 0, 0, 0, 3, 1, 8);
  //VTAUopPush(1, 0, 0, 0, 0, 3, 1, 7); // input 5. output 7. 12shift
  VTAUopLoopEnd(cmdh);
  return 0;
}

int finit_max(void *param, VTACommandHandle cmdH) {
  int *p = (int*) param;
  VTAUopLoopBegin(cmdH, *p, 1, 1, 0);
  VTAUopPush(cmdH, 1, 0, 0, 0, 0, 1, 1, 0);
  VTAUopLoopEnd(cmdH);
  return 0;
}

int finit_min(void *param, VTACommandHandle cmdH) {
  int *p = (int*) param;
  VTAUopLoopBegin(cmdH, *p, 1, 1, 0);
  VTAUopPush(cmdH, 1, 0, 0, 0, 0, 0, 1, 127);
  VTAUopLoopEnd(cmdH);
  return 0;
}
}

using namespace sim_test_conv1;
int conv1_test(){
   

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
    int8_t in1[N][H][W][C];
    int8_t filter[KN][KH][KW][C];

    for(int i =0; i < N; i++){
        for(int j =0 ; j < H; j++){
            for(int k =0; k< W; k++){
                for(int l =0; l<C; l++){
                    in1[i][j][k][l]=(int8_t)(rand()%60);
                }
            }
        }
    }

    for(int i =0; i < KN; i++){
        for(int j =0 ; j < KH; j++){
            for(int k =0; k< KW; k++){
                for(int l =0; l<C; l++){
                    filter[i][j][k][l]=(int8_t)(rand()%60);
                    if(rand()%2==0) filter[i][j][k][l]=filter[i][j][k][l]*-1;
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

    //cout << "stride size = " << stride_size << endl;

    int out_h = (height + 2*pad_size - filter_h)/stride_size + 1;
    int out_w = (width + 2*pad_size - filter_w)/stride_size + 1;


    int new_height = height + pad_size*2;
    int new_width = width + pad_size*2;


    int ic = channel_size / 16;
    int oc = filter_size / 16;
    // 일단 filter가 3x3이면 npu사용

    if(filter_h==3 && filter_w == 3 && height == 14)
    {

        //sram할당
        void* DATA_buf = VTABufferAlloc(1 * ic * new_height * new_width * 1 * 16);
        void* KERNEL_buf = VTABufferAlloc(oc*ic*3*3*16*16);
        void* RES_buf = VTABufferAlloc(1*oc*out_h*out_w*1*16);

        // uop kernel handle
        void* uopHandle1[2];
        void* uopHandle2[2];
        void* uopHandle3[2];
        void* uopHandle4[2];
        void* uopHandle5[2];

        uopHandle1[0]=nullptr;
        uopHandle2[0]=nullptr;
        uopHandle3[0]=nullptr;
        uopHandle4[0]=nullptr;
        uopHandle5[0]=nullptr;

        VTACommandHandle vtaCmdH{nullptr};
        vtaCmdH = VTATLSCommandHandle(EVTA_ID);
        //VTASetDebugMode(vtaCmdH,VTA_DEBUG_DUMP_INSN | VTA_DEBUG_DUMP_UOP);
        VTASetDebugMode(vtaCmdH,0);


        // transpose NHWC -> nchwnc
        int8_t *new_in_data8 = (int8_t *)malloc(batch_size*height*width*channel_size);
        memset(new_in_data8, 0x00, sizeof(batch_size*height*width*channel_size));

        for (int b = 0; b < batch_size; b++){
            //for (size_t c = 0; c < channel_size; c++){
            for (int c = 0; c < channel_size; c++){
                for (int h = 0; h < height; h++){
                    for (int w = 0; w < width; w++){
                        int in_ch= c / 16;
                        int cc = c % 16;
                        *(new_in_data8+(in_ch*height*width*16 + h*width*16 + w*16 + cc)) = in1[b][h][w][c];
                    }
                }
            }
        }



        int8_t *new_kernel_data8 = (int8_t *)malloc(filter_size*3*3*channel_size);
        memset(new_kernel_data8, 0x00, sizeof(int8_t[filter_size][3][3][channel_size]));
        for (int f = 0; f < filter_size; f++){
            for (int c = 0; c < channel_size; c++){
                for (int h = 0; h < 3; h++){
                    for (int w = 0; w < 3; w++){
                        int out_ch = f / 16;
                        int in_ch = c / 16;
                        int oo = f % 16;
                        int ii = c % 16;
                        *(new_kernel_data8+(out_ch*ic*3*3*16*16 + in_ch*3*3*16*16 + h*3*16*16 + w*16*16 + oo*16 + ii))  = filter[f][h][w][c];
                    }
                }

            }
        }

        int8_t *output_data8 = (int8_t *)malloc(batch_size*out_h*out_w*filter_size);



        if(new_in_data8 == nullptr || new_kernel_data8 == nullptr || output_data8 == nullptr ) {
            cout << "not enough memory" << endl;
            exit(-1);
        }

        VTABufferCopy(new_in_data8, 0, DATA_buf, 0, width*height*ic*16, 1);  // 1 toBuf
        VTABufferCopy(new_kernel_data8, 0, KERNEL_buf, 0, 3*3*filter_size*channel_size, 1);  // 2 is to is DataBuffer


        //case 1. ic==oc
        if(ic == oc) {
            // i2 outer = loop_h
            // i3 outer = loop_w
            // 몇개로 나뉘나
            int div_height = (height / 14);
            int div_width = (width / 14);



            VTADepPush(vtaCmdH,3,2);
            for ( int i2_out =0; i2_out < div_height; i2_out++) {
                for( int i3_out = 0;i3_out < div_width; i3_out++) {

                    // 3. prepare for uop
                    // batch, height, width, channel
                    //int uop_input[9] = { N, M , K, L, 3, 3, filter_size, pad_size, stride_size}
                    // fixed params.

                    VTADepPop(vtaCmdH,3,2);
                    // 초기화
                    // 초기화는 oc, ic비교안해도됨. 무조건 oc가 큼
                    VTAPushGEMMOp(vtaCmdH, uopHandle1,&sim_test_conv1::finit_gemmop_init, &vtaCmdH,0);

                    VTADepPush(vtaCmdH,2,1);
                    //for(int ic_out=0;ic_out < ic;ic_out++) {
                        VTADepPop(vtaCmdH,2,1);

                        // databuf 로드
                        // padding은 fpga내부에서 추가됨. 14단위로 로드.


                        //general case for 1x1
                        VTALoadBuffer2D(vtaCmdH , DATA_buf,
                                        0,
                                        14,
                                        14,
                                        14,
                                        1,
                                        1,
                                        1,
                                        1,
                                        0, 		//index
                                        2); 	//type

                        // kernel 로드

                        VTALoadBuffer2D(vtaCmdH , KERNEL_buf,
                                        0,
                                        9,
                                        1,
                                        9,
                                        0, 0, 0, 0, 0, 1 );


                        VTADepPush(vtaCmdH,1,2);
                        VTADepPop(vtaCmdH,1,2);
                        VTAPushGEMMOp(vtaCmdH, uopHandle2,&sim_test_conv1::finit_gemmop_3x3,&vtaCmdH,0);

                        VTADepPush(vtaCmdH,2,1);
//                    }
                    VTADepPop(vtaCmdH,2,1);

                    //param_alu = oc * 98;
                    int param_alu = 196;
                    //struct alu_param param_alu;
                    //param_alu.cmdH = &vtaCmdH;
                    //param_alu.param = 196;


                    VTAPushALUOp(vtaCmdH, uopHandle3,&sim_test_conv1::finit_shr,&param_alu,0);
                    VTAPushALUOp(vtaCmdH, uopHandle4,&sim_test_conv1::finit_max,&param_alu,0);
                    VTAPushALUOp(vtaCmdH, uopHandle5,&sim_test_conv1::finit_min,&param_alu,0);

                    VTADepPush(vtaCmdH,2,3);
                    VTADepPop(vtaCmdH,2,3);


                    //for(int i1_in=0;i1_in < oc; i1_in++) {
                        for(int i2_in=0;i2_in < 14; i2_in++) {
                            for(int i3_in=0;i3_in < 14; i3_in++) {
                                //VTAStoreBuffer2D(vtaCmdH ,(((i1_in*98) + (i2_in*14)) + i3_in), 4, RES_buf, (((((i1_in*div_height*div_width*14*7) + (i2_out*div_width*14*7)) + (i2_in*div_width*14)) + (i3_out*14)) + i3_in), 1, 1, 1 ); // memory_type 4 = VTA_MEM_ID_OUT
                                VTAStoreBuffer2D(vtaCmdH, ((i2_in*14) + i3_in), 4, RES_buf, ((i2_in*14) + i3_in), 1, 1, 1);
                            }
                        }
                    //}
                    VTADepPush(vtaCmdH,3,2);




                }
            }	// end loop

            VTADepPop(vtaCmdH,3,2);


        }
            //----------------------------------------
            //case2. ic * 2= oc

        // 실제 수행
        VTASynchronize(vtaCmdH,1<<31);
        VTABufferCopy(RES_buf, 0, output_data8, 0, out_h*out_w*filter_size, 2);  // 1 MemCopyToHost

        VTARuntimeShutdown(EVTA_ID);

        // end case
        VTABufferFree( DATA_buf );
        VTABufferFree( KERNEL_buf );
        VTABufferFree( RES_buf );




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
                        newin[b][pad_size + h][pad_size + w][c] = in1[b][h][w][c];
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
                                            newin[b][new_h_idx + fh][new_w_idx + fw][f] * filter[fc][fh][fw][f];
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

                        int8_t v = *(output_data8+(out_ch*out_h*out_w*16 + i*out_w*16 + j*16 + oo));

                        //printf("%u, %u\n", (uint8_t)v, result[b][i][j][f]);
                        if ((uint8_t)v != result[b][i][j][f]) is_diff = -1;

                    }
                    //printf("\n");

                }
                //printf("-----------------\n");
            }
        }

        free(new_in_data8);
        free(new_kernel_data8);
        free(output_data8);
    }



    return is_diff;
}
