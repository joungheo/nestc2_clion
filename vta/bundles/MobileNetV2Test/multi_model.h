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


//#### JO added (2026-11-06): MobileNetV2 (TFLite mobilenet_v2_1.0_224_quant, QAT weights as float: tfl2onnx.py) on one EVTA,
//#### for multi_threads_main.cpp with MULTI_SINGLE (mobilenetv2Main.cpp)
#include "mobilenetv2.h"

#define MODEL_HEIGHT 224
#define MODEL_WIDTH 224
// the TFLite input: (q - 128) / 128 for RGB bytes q (image-classifier -image-mode=neg1to1)
#define MODEL_RANGE_LO -1.0f
#define MODEL_RANGE_HI 1.0f
// 1: R G B at z = 2 1 0, so that the input's channels are R G B
#define MODEL_PNG_REVERSE 1
#define MODEL_NHWC 0
#define MODEL_INPUT "input"
#define MODEL_OUTPUT "output"
// the output is the logits
#define MODEL_SOFTMAX 1
// 1001 classes, 0 = background: ImageNet class + 1 (cat_285.png: 281 tabby, as the fp32 / TFLite models)
#define MODEL_EXPECT 282
#define MODEL_COPY0 \
    {&mobilenetv2_config, mobilenetv2_load_module, mobilenetv2_destroy_module, mobilenetv2, "mobilenetv2.weights.bin", initVTARuntime, destroyVTARuntime}
