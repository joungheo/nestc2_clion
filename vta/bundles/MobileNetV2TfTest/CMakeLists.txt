#### JO added (2026-11-22, docs/evta_todo.md §5 DPU comparison): MobileNetV2 for the comparison with AMD's DPU (Vitis AI
#### tf_mobilenetv2_1.0_imagenet_224_224_602M, ImageNet val 70.13 % float / 67.67 % int8). A dir of its own because both the model
#### and the test images differ from MobileNetV2Test (TFLite QAT model, ImageNetV2):
####   model:  TF-slim mobilenet_v2_1.0_224 float (the DPU's float model), its .tflite as float ONNX without the last softmax
####           (logits, as MobileNetV2Test; 1001 classes, 0 = background):
####             curl -LO https://storage.googleapis.com/download.tensorflow.org/models/tflite_11_05_08/mobilenet_v2_1.0_224.tgz
####             tar xzf mobilenet_v2_1.0_224.tgz && python3 ../MobileNetV2Test/tfl2onnx.py mobilenet_v2_1.0_224.tflite mobilenetv2_tf_f32.onnx
####           (needs the tflite / onnx Python packages); the result is in git here since 2026-11-25 (mobilenetv2_tf_f32.onnx,
####           14 MB; gen_calib_bundle.sh reads it when Debug2_remote/vta/bundles/MobileNetV2TfTest has no copy)
####   test:   ImageNet val (ILSVRC2012, 50,000 images) as the DPU numbers, TF-slim preprocessing (centre crop 87.5 %, resize to
####           224 bilinear): imagenet_eval/prep_imagenet_val.py tf; eval_board.sh picks the set by Dir (imagenet_eval/eval_sets.txt)
####   calibration: calib/mobilenetv2_tf_calib_n200_b2048.yaml.gz, 200 ImageNetV2 images (no image of the test set), 2048 bins,
####           KL + -quantization-pow2-mse=2 (docs/evta_accuracy.md §7)
#### The bundle is generated with vtabundles_xcmpl/gen_calib_bundle.sh MobileNetV2TfTest; without the generated mobilenetv2.cpp
#### the targets are left out. The add_executable / add_nestc_test lines below are what collect_hw_bundles.py reads.
if(NOT EXISTS ${CMAKE_CURRENT_BINARY_DIR}/mobilenetv2.cpp)
    message(STATUS "MobileNetV2TfTest: no generated bundle (vtabundles_xcmpl/gen_calib_bundle.sh MobileNetV2TfTest), skipped")
    return()
endif()

INCLUDE_DIRECTORIES(${CMAKE_CURRENT_BINARY_DIR})
add_executable(vtaMobileNetV2TfBundle mobilenetv2Main.cpp ${CMAKE_CURRENT_BINARY_DIR}/mobilenetv2.cpp)
add_dependencies(vtaMobileNetV2TfBundle ${BUNDLE_DEPENDS})
target_link_libraries(vtaMobileNetV2TfBundle VTABundle png pthread ${BUNDLE_LINK_LIB})
set_target_properties(vtaMobileNetV2TfBundle PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")

if(NESTC_EVTA_RUN_WITH_GENERIC_BUNDLE)
    add_nestc_test(ZCU102 NAME vtaMobileNetV2TfBundle COMMAND ${CMAKE_CURRENT_BINARY_DIR}/vtaMobileNetV2TfBundle USE_SH 1 PARAMS ${GLOW_SOURCE_DIR}/tests/images/imagenet/cat_285.png)
else()
    add_nestc_test(NAME vtaMobileNetV2TfBundle COMMAND ${CMAKE_CURRENT_BINARY_DIR}/vtaMobileNetV2TfBundle USE_SH 1 PARAMS ${GLOW_SOURCE_DIR}/tests/images/imagenet/cat_285.png)
endif()
