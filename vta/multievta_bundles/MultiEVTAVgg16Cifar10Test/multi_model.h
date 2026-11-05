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

//#### JO added (2026-11-06): VGG16 CIFAR-10 (vta/bundles/Vgg16Cifar10Test) on 4 EVTAs, one copy per EVTA
//#### for multi_threads_main.cpp: the 4 copies (copy i = EVTA i, model-compiler -index-multi-evta=2^i
//#### -network-name / -main-entry-name <name>_i, vtabundles_xcmpl/gen_multi4_bundles.sh), the input and the output
#include "vgg16_cifar10.h"
#include "vgg16_cifar10_1.h"
#include "vgg16_cifar10_2.h"
#include "vgg16_cifar10_3.h"

#define MODEL_HEIGHT 32
#define MODEL_WIDTH 32
// pixel range and layout of the input as the single bundle's main makes it (BGR)
#define MODEL_RANGE_LO 0.0f
#define MODEL_RANGE_HI 1.0f
// 1: the main's PNG reader stores R G B at z = 2 1 0 (then the BGR swap of the input), 0: at z = 0 1 2
#define MODEL_PNG_REVERSE 1
#define MODEL_NHWC 0
#define MODEL_INPUT "input"
#define MODEL_OUTPUT "output"
// 1: the output is the logits (softmax in the main)
#define MODEL_SOFTMAX 1
#define MODEL_EXPECT 0
// copy 0 alone: the single-thread executable links only its .cpp
#define MODEL_COPY0 \
    {&vgg16_cifar10_config, vgg16_cifar10_load_module, vgg16_cifar10_destroy_module, vgg16_cifar10, "vgg16_cifar10.weights.bin", initVTARuntime, destroyVTARuntime}
#define MODEL_COPIES \
  { \
    MODEL_COPY0, \
    {&vgg16_cifar10_1_config, vgg16_cifar10_1_load_module, vgg16_cifar10_1_destroy_module, vgg16_cifar10_1, "vgg16_cifar10_1.weights.bin", initVTARuntime1, destroyVTARuntime1}, \
    {&vgg16_cifar10_2_config, vgg16_cifar10_2_load_module, vgg16_cifar10_2_destroy_module, vgg16_cifar10_2, "vgg16_cifar10_2.weights.bin", initVTARuntime2, destroyVTARuntime2}, \
    {&vgg16_cifar10_3_config, vgg16_cifar10_3_load_module, vgg16_cifar10_3_destroy_module, vgg16_cifar10_3, "vgg16_cifar10_3.weights.bin", initVTARuntime3, destroyVTARuntime3} \
  }
