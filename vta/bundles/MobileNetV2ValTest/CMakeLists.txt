#### JO added (2026-11-22, docs/evta_todo.md §5 DPU comparison): MobileNetV2Test's model (TFLite QAT mobilenet_v2_1.0_224_quant,
#### weights as float: ../MobileNetV2Test/tfl2onnx.py) on another test set, so a dir of its own (MobileNetV2Test: ImageNetV2):
####   test:   ImageNet val (ILSVRC2012, 50,000 images) as the DPU numbers, TF-slim preprocessing (centre crop 87.5 %, resize to
####           224 bilinear): imagenet_eval/prep_imagenet_val.py tf; eval_board.sh picks the set by Dir (imagenet_eval/eval_sets.txt)
####   calibration: ../MobileNetV2Test/calib/mobilenetv2_calib_n200_b2048.yaml.gz (200 ImageNetV2 images, no image of the test set),
####           KL + -quantization-pow2-mse=2 (docs/evta_accuracy.md §7)
####   model file: Debug2_remote/vta/bundles/MobileNetV2Test/mobilenetv2_qat_f32.onnx (MobileNetV2Test's)
#### The DPU's own float model (TF-slim, not QAT) is MobileNetV2TfTest. The bundle is generated with
#### vtabundles_xcmpl/gen_calib_bundle.sh MobileNetV2ValTest; without the generated mobilenetv2.cpp the targets are left out.
if(NOT EXISTS ${CMAKE_CURRENT_BINARY_DIR}/mobilenetv2.cpp)
    message(STATUS "MobileNetV2ValTest: no generated bundle (vtabundles_xcmpl/gen_calib_bundle.sh MobileNetV2ValTest), skipped")
    return()
endif()

INCLUDE_DIRECTORIES(${CMAKE_CURRENT_BINARY_DIR})
add_executable(vtaMobileNetV2ValBundle mobilenetv2Main.cpp ${CMAKE_CURRENT_BINARY_DIR}/mobilenetv2.cpp)
add_dependencies(vtaMobileNetV2ValBundle ${BUNDLE_DEPENDS})
target_link_libraries(vtaMobileNetV2ValBundle VTABundle png pthread ${BUNDLE_LINK_LIB})
set_target_properties(vtaMobileNetV2ValBundle PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")

if(NESTC_EVTA_RUN_WITH_GENERIC_BUNDLE)
    add_nestc_test(ZCU102 NAME vtaMobileNetV2ValBundle COMMAND ${CMAKE_CURRENT_BINARY_DIR}/vtaMobileNetV2ValBundle USE_SH 1 PARAMS ${GLOW_SOURCE_DIR}/tests/images/imagenet/cat_285.png)
else()
    add_nestc_test(NAME vtaMobileNetV2ValBundle COMMAND ${CMAKE_CURRENT_BINARY_DIR}/vtaMobileNetV2ValBundle USE_SH 1 PARAMS ${GLOW_SOURCE_DIR}/tests/images/imagenet/cat_285.png)
endif()
