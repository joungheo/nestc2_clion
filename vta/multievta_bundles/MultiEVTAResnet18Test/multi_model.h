//#### JO added (2026-11-26): Resnet18 (MXNet, vta/bundles/Resnet18Test) on 4 EVTAs for the generic multi_threads_main.cpp (as
//#### MultiEVTAResnet50Test; the upstream ETRI main of this Dir had no int8 input): the 4 copies (copy i = EVTA i,
//#### vtabundles_xcmpl/gen_multi4_bundles.sh), input as vta/bundles/Resnet18Test/multi_model.h (the stock main's)
#include "mxnet_exported_resnet18.h"
#include "mxnet_exported_resnet18_1.h"
#include "mxnet_exported_resnet18_2.h"
#include "mxnet_exported_resnet18_3.h"

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
// copy 0 alone: the single-thread executable links only its .cpp
#define MODEL_COPY0 \
    {&mxnet_exported_resnet18_config, mxnet_exported_resnet18_load_module, mxnet_exported_resnet18_destroy_module, mxnet_exported_resnet18, "mxnet_exported_resnet18.weights.bin", initVTARuntime, destroyVTARuntime}
#define MODEL_COPIES \
  { \
    MODEL_COPY0, \
    {&mxnet_exported_resnet18_1_config, mxnet_exported_resnet18_1_load_module, mxnet_exported_resnet18_1_destroy_module, mxnet_exported_resnet18_1, "mxnet_exported_resnet18_1.weights.bin", initVTARuntime1, destroyVTARuntime1}, \
    {&mxnet_exported_resnet18_2_config, mxnet_exported_resnet18_2_load_module, mxnet_exported_resnet18_2_destroy_module, mxnet_exported_resnet18_2, "mxnet_exported_resnet18_2.weights.bin", initVTARuntime2, destroyVTARuntime2}, \
    {&mxnet_exported_resnet18_3_config, mxnet_exported_resnet18_3_load_module, mxnet_exported_resnet18_3_destroy_module, mxnet_exported_resnet18_3, "mxnet_exported_resnet18_3.weights.bin", initVTARuntime3, destroyVTARuntime3} \
  }
