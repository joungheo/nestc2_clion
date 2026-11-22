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

//#### JO added (2026-11-22): MultiEVTAMobileNetV2TfTest (TF-slim float MobileNetV2, same input / output): MultiEVTAMobileNetV2Test's header.
//#### JO added (2026-11-22): MobileNetV2 (vta/bundles/MobileNetV2Test) on 4 EVTAs, one copy per EVTA, for multi_threads_main.cpp
//#### (the one main of every model). Copy i = EVTA i (model-compiler -index-multi-evta=2^i -network-name / -main-entry-name
//#### mobilenetv2_i, vtabundles_xcmpl/gen_multi4_bundles.sh MultiEVTAMobileNetV2Test).
#include "mobilenetv2.h"
#include "mobilenetv2_1.h"
#include "mobilenetv2_2.h"
#include "mobilenetv2_3.h"

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
//#### JO added (2026-11-22): model class = ImageNet class + 1 (EVAL_LIST lists ImageNet classes)
#define MODEL_LABEL_OFFSET 1
#define MODEL_COPY0 \
    {&mobilenetv2_config, mobilenetv2_load_module, mobilenetv2_destroy_module, mobilenetv2, "mobilenetv2.weights.bin", initVTARuntime, destroyVTARuntime}
#define MODEL_COPIES \
  { \
    MODEL_COPY0, \
    {&mobilenetv2_1_config, mobilenetv2_1_load_module, mobilenetv2_1_destroy_module, mobilenetv2_1, "mobilenetv2_1.weights.bin", initVTARuntime1, destroyVTARuntime1}, \
    {&mobilenetv2_2_config, mobilenetv2_2_load_module, mobilenetv2_2_destroy_module, mobilenetv2_2, "mobilenetv2_2.weights.bin", initVTARuntime2, destroyVTARuntime2}, \
    {&mobilenetv2_3_config, mobilenetv2_3_load_module, mobilenetv2_3_destroy_module, mobilenetv2_3, "mobilenetv2_3.weights.bin", initVTARuntime3, destroyVTARuntime3} \
  }
