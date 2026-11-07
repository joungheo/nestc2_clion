#### JO added (2026-11-06): MobileNetV2 on one EVTA. The model is TFLite's QAT mobilenet_v2_1.0_224_quant (uint8,
#### per tensor, ImageNet + background = 1001 classes) with its weights dequantized to float and RELU6 as Clip
#### (tfl2onnx.py; the MXNet MobileNetV2 of the ONNX zoo quantizes badly with per-tensor power-of-two scales:
#### cat_285.png 617 instead of 281). NEST-C quantizes it as the other models (profile, symmetric_with_power2_scale).
#### The ONNX file is not in git; make it on a PC with the tflite / onnx Python packages and put it in this build dir:
####   curl -LO https://storage.googleapis.com/download.tensorflow.org/models/tflite_11_05_08/mobilenet_v2_1.0_224_quant.tgz
####   tar xzf mobilenet_v2_1.0_224_quant.tgz && python3 tfl2onnx.py mobilenet_v2_1.0_224_quant.tflite mobilenetv2_qat_f32.onnx
#### Without it the targets are left out (vta/bundles/CMakeLists.txt globs this dir). On the board the bundle is generated with
#### vtabundles_xcmpl/gen_mobilenetv2_bundle.sh (the same commands, no cmake re-run of Debug2_remote) and built in vtabundles_xcmpl.
set(MNV2_ONNX ${CMAKE_CURRENT_BINARY_DIR}/mobilenetv2_qat_f32.onnx)
if(NOT EXISTS ${MNV2_ONNX})
    message(STATUS "MobileNetV2Test: ${MNV2_ONNX} missing, skipped (see vta/bundles/MobileNetV2Test/CMakeLists.txt)")
    return()
endif()

add_custom_command(
        OUTPUT
        ${CMAKE_CURRENT_BINARY_DIR}/mobilenetv2_calib.yaml
        COMMAND
        image-classifier ${NESTC_ROOT_DIR}/glow/tests/images/imagenet/cat_285.png
        -m=${MNV2_ONNX}
        -model-input-name=input
        -backend=VTAInterpreter
        -do-not-lower-nodes-for-profiling=Convolution   # depthwise lowered = C convs: hours (gen_mobilenetv2_bundle.sh)
        -image-layout=NCHW
        -image-mode=neg1to1
        -image-channel-order=RGB
        -compute-softmax
        -topk=5
        -dump-profile=${CMAKE_CURRENT_BINARY_DIR}/mobilenetv2_calib.yaml
        -quantization-schema=symmetric_with_power2_scale
        DEPENDS
        image-classifier
)

add_custom_command(
        OUTPUT
        ${CMAKE_CURRENT_BINARY_DIR}/mobilenetv2.cpp
        ${CMAKE_CURRENT_BINARY_DIR}/mobilenetv2.h
        ${CMAKE_CURRENT_BINARY_DIR}/mobilenetv2.weights.bin
        ${CMAKE_CURRENT_BINARY_DIR}/VTARuntime.h
        COMMAND
        model-compiler -g
        -model=${MNV2_ONNX}
        -backend=VTA
        -emit-bundle=${CMAKE_CURRENT_BINARY_DIR}
        -bundle-api=dynamic
        -network-name=mobilenetv2
        -model-input=input,float,[1,3,224,224]
        -load-profile=${CMAKE_CURRENT_BINARY_DIR}/mobilenetv2_calib.yaml
        -quantization-schema=symmetric_with_power2_scale
        #### JO added (2026-11-07): per-channel scales for the depthwise convs (one per tensor: 293 instead of 282)
        -enable-channelwise-depthwise
        DEPENDS
        model-compiler VTA ${CMAKE_CURRENT_BINARY_DIR}/mobilenetv2_calib.yaml
)

add_custom_target(vtaMobileNetV2BundleNet DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/mobilenetv2.cpp)
INCLUDE_DIRECTORIES(${CMAKE_CURRENT_BINARY_DIR})
add_executable(vtaMobileNetV2Bundle mobilenetv2Main.cpp ${CMAKE_CURRENT_BINARY_DIR}/mobilenetv2.cpp)
add_dependencies(vtaMobileNetV2Bundle vtaMobileNetV2BundleNet ${BUNDLE_DEPENDS})
target_link_libraries(vtaMobileNetV2Bundle VTABundle png pthread ${BUNDLE_LINK_LIB})
set_target_properties(vtaMobileNetV2Bundle PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")

if(NESTC_EVTA_RUN_WITH_GENERIC_BUNDLE)
    add_nestc_test(ZCU102 NAME vtaMobileNetV2Bundle COMMAND ${CMAKE_CURRENT_BINARY_DIR}/vtaMobileNetV2Bundle USE_SH 1 PARAMS ${GLOW_SOURCE_DIR}/tests/images/imagenet/cat_285.png)
else()
    add_nestc_test(NAME vtaMobileNetV2Bundle COMMAND ${CMAKE_CURRENT_BINARY_DIR}/vtaMobileNetV2Bundle USE_SH 1 PARAMS ${GLOW_SOURCE_DIR}/tests/images/imagenet/cat_285.png)
endif()
