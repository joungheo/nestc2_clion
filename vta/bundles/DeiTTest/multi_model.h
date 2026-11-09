//#### JO added (2026-11-09): DeiT-tiny (facebook/deit-tiny-distilled-patch16-224 as ONNX, shapes fixed and constant-folded:
//#### vtabundles_xcmpl/fold_onnx.py) on one EVTA, for multi_threads_main.cpp with MULTI_SINGLE (deitMain.cpp).
//#### Most of it runs on the CPU (VTASave generic CPU ops); the EVTA runs the patch embedding conv and the classifiers.
#include "deit.h"

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
// ImageNet class (cat_285.png: 281 tabby, as the fp32 model and the int8 Interpreter)
#define MODEL_EXPECT 281
#define MODEL_COPY0 \
    {&deit_config, deit_load_module, deit_destroy_module, deit, "deit.weights.bin", initVTARuntime, destroyVTARuntime}
