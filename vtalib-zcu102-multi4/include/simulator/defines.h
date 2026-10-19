//
// Created by crojjang on 4/10/20.
//

#ifndef VTA_SIM_DEFINES_H
#define VTA_SIM_DEFINES_H

#define VTA_DEVICE_NUM  4

//#### JO change to ZCU102 spec 260908
//#define VTA_LOG_INP_BUFF_SIZE 15
//#define VTA_LOG_UOP_BUFF_SIZE 15    // 16 // 15->16 update for evta
//#define VTA_LOG_ACC_BUFF_SIZE 17
//#define VTA_LOG_WGT_BUFF_SIZE 18
#define VTA_LOG_INP_BUFF_SIZE 16
#define VTA_LOG_UOP_BUFF_SIZE 16    // 16 // 15->16 update for evta
#define VTA_LOG_ACC_BUFF_SIZE 18
#define VTA_LOG_WGT_BUFF_SIZE 19

#define VTA_LOG_BATCH 0
#define VTA_LOG_BLOCK_OUT 4
#define VTA_LOG_BLOCK_IN 4
#define VTA_LOG_ACC_WIDTH 5
#define VTA_LOG_INP_WIDTH 3
#define VTA_LOG_WGT_WIDTH 3
#define VTA_OUT_WIDTH 8
#define VTA_LOG_OUT_WIDTH 3

#define CHECK_LT //CHECK_
#define CHECK_LE //CHECK_
#define CHECK_EQ //CHECK_


#endif //VTA_SIM_DEFINES_H
