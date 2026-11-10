/*****************************************************************************
*
* Copyright Next-Generation System Software Research Group, All rights reserved.
* Future Computing Research Division, Artificial Intelligence Reserch Laboratory
* Electronics and Telecommunications Research Institute (ETRI)
*
* THESE DOCUMENTS CONTAIN CONFIDENTIAL INFORMATION AND KNOWLEDGE
* WHICH IS THE PROPERTY OF ETRI. NO PART OF THIS PUBLICATION IS
* TO BE USED FOR ANY OTHER PURPOSE, AND THESE ARE NOT TO BE
* REPRODUCED, COPIED, DISCLOSED, TRANSMITTED, STORED IN A RETRIEVAL
* SYSTEM OR TRANSLATED INTO ANY OTHER HUMAN OR COMPUTER LANGUAGE,
* IN ANY FORM, BY ANY MEANS, IN WHOLE OR IN PART, WITHOUT THE
* COMPLETE PRIOR WRITTEN PERMISSION OF ETRI.
*
* LICENSE file : LICENSE_ETRI located in the top directory
*
*****************************************************************************/

//#### JO added (2026-11-10): DeiT-tiny (vta/bundles/DeiTTest) on 4 EVTAs, one copy per EVTA, for multi_threads_main.cpp
//#### (the DeiTTest copy: per channel mean / std, top-5). Copy i = EVTA i (model-compiler -index-multi-evta=2^i
//#### -network-name / -main-entry-name deit_i, vtabundles_xcmpl/gen_multi4_bundles.sh MultiEVTADeiTTest).
#include "deit.h"
#include "deit_1.h"
#include "deit_2.h"
#include "deit_3.h"

#define MODEL_HEIGHT 224
#define MODEL_WIDTH 224
// [0, 1], then (x - mean) / std per channel (image-classifier -image-mode=0to1 -mean=... -stddev=...)
#define MODEL_RANGE_LO 0.0f
#define MODEL_RANGE_HI 1.0f
#define MODEL_MEAN {0.485f, 0.456f, 0.406f}
#define MODEL_STD {0.229f, 0.224f, 0.225f}
// 1: the input's channels are R G B (-image-channel-order=RGB)
#define MODEL_PNG_REVERSE 1
#define MODEL_NHWC 0
#define MODEL_INPUT "pixel_values"
#define MODEL_OUTPUT "logits"
// the output is the logits (the mean of the class and distillation heads)
#define MODEL_SOFTMAX 1
#define MODEL_PRINT_TOP5 1
#define MODEL_EXPECT 281
// copy 0 alone: the single-thread executable links only its .cpp
#define MODEL_COPY0 \
    {&deit_config, deit_load_module, deit_destroy_module, deit, "deit.weights.bin", initVTARuntime, destroyVTARuntime}
#define MODEL_COPIES \
  { \
    MODEL_COPY0, \
    {&deit_1_config, deit_1_load_module, deit_1_destroy_module, deit_1, "deit_1.weights.bin", initVTARuntime1, destroyVTARuntime1}, \
    {&deit_2_config, deit_2_load_module, deit_2_destroy_module, deit_2, "deit_2.weights.bin", initVTARuntime2, destroyVTARuntime2}, \
    {&deit_3_config, deit_3_load_module, deit_3_destroy_module, deit_3, "deit_3.weights.bin", initVTARuntime3, destroyVTARuntime3} \
  }
