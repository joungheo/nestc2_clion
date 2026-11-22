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

//#### JO added (2026-11-22): Resnet18 (MXNet) on one EVTA for multi_threads_main.cpp with MULTI_SINGLE (resnet18EvalMain.cpp:
//#### the dataset accuracy, imagenet_eval/eval_board.sh); input as mxnet_exported_resnet18BundleMain.cpp makes it
#include "mxnet_exported_resnet18.h"

#define MODEL_HEIGHT 224
#define MODEL_WIDTH 224
// the stock main: PNG bytes as 0..255, channels reversed (2 - z), NCHW transposed to NHWC (its NHWC 1)
#define MODEL_RANGE_LO 0.0f
#define MODEL_RANGE_HI 255.0f
#define MODEL_PNG_REVERSE 1
#define MODEL_NHWC 1
#define MODEL_INPUT "data"
#define MODEL_OUTPUT "resnetv10_dense0_fwd__1"
// the output is the logits (the stock main applies a softmax)
#define MODEL_SOFTMAX 1
#define MODEL_EXPECT 281
#define MODEL_COPY0 \
    {&mxnet_exported_resnet18_config, mxnet_exported_resnet18_load_module, mxnet_exported_resnet18_destroy_module, mxnet_exported_resnet18, "mxnet_exported_resnet18.weights.bin", initVTARuntime, destroyVTARuntime}
#define MODEL_COPIES {MODEL_COPY0}
