#### JO added (2026-11-22, docs/evta_todo.md §5 DPU comparison): ResNet-50 for the comparison with AMD's DPU (Vitis AI
#### pt_resnet50_imagenet_224_224_8.2G, ImageNet val 76.1 % float / 76.0 % int8). A dir of its own because both the model and the
#### test images differ from Resnet50Test (Caffe2 model, ImageNetV2):
####   model:  torchvision ResNet-50 IMAGENET1K_V1 (the DPU's float model; vtabundles_xcmpl/export_tv_resnet50.py, same input /
####           output as the Caffe2 resnet50.onnx, so the mains are Resnet50Test's)
####   test:   ImageNet val (ILSVRC2012, 50,000 images) as the DPU numbers, torchvision preprocessing (short side 256 bilinear,
####           centre crop 224): imagenet_eval/prep_imagenet_val.py tv; eval_board.sh picks the set by Dir (imagenet_eval/eval_sets.txt)
####   calibration: calib/resnet50_tv_calib_n200_b2048.yaml.gz, 200 ImageNetV2 images (no image of the test set), 2048 bins,
####           KL + -quantization-pow2-mse=2 (docs/evta_accuracy.md §7)
#### The bundle is generated with vtabundles_xcmpl/gen_calib_bundle.sh Resnet50TvTest (no cmake rules here: the ONNX is not in git,
#### and a cmake re-run of Debug2_remote rebuilds Glow); without the generated resnet50.cpp the targets are left out.
#### The add_executable / add_nestc_test lines below are what vtabundles_xcmpl/collect_hw_bundles.py reads.
if(NOT EXISTS ${CMAKE_CURRENT_BINARY_DIR}/resnet50.cpp)
    message(STATUS "Resnet50TvTest: no generated bundle (vtabundles_xcmpl/gen_calib_bundle.sh Resnet50TvTest), skipped")
    return()
endif()

INCLUDE_DIRECTORIES(${CMAKE_CURRENT_BINARY_DIR})
add_executable(vtaTvResnet50Bundle caffe2_resnet50Main.cpp ${CMAKE_CURRENT_BINARY_DIR}/resnet50.cpp)
add_dependencies(vtaTvResnet50Bundle ${BUNDLE_DEPENDS})
target_link_libraries(vtaTvResnet50Bundle VTABundle png ${BUNDLE_LINK_LIB})
set_target_properties(vtaTvResnet50Bundle PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")

if(NESTC_EVTA_RUN_WITH_GENERIC_BUNDLE)
    add_nestc_test(ZCU102 NAME vtaTvResnet50Bundle COMMAND ${CMAKE_CURRENT_BINARY_DIR}/vtaTvResnet50Bundle USE_SH 1 PARAMS ${GLOW_SOURCE_DIR}/tests/images/imagenet/cat_285.png)
else()
    add_nestc_test(NAME vtaTvResnet50Bundle COMMAND ${CMAKE_CURRENT_BINARY_DIR}/vtaTvResnet50Bundle USE_SH 1 PARAMS ${GLOW_SOURCE_DIR}/tests/images/imagenet/cat_285.png)
endif()
