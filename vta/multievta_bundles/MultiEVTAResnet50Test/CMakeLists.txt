#### JO added (2026-11-06): Resnet50 on 4 EVTAs, as MultiEVTAResnet18Test.
#### Copy i runs on EVTA i (model-compiler -index-multi-evta=2^i, -network-name / -main-entry-name <name>_i); the
#### model and the profile are the single bundle's (vta/bundles/Resnet50Test). Not added to ../CMakeLists.txt: a new directory re-runs
#### cmake in Debug2_remote, which rebuilds all of Glow on a new day (CLAUDE.md). The board generates the copies with
#### vtabundles_xcmpl/gen_multi4_bundles.sh (the same commands) and builds them in vtabundles_xcmpl.

add_custom_command(
        OUTPUT
        ${CMAKE_CURRENT_BINARY_DIR}/resnet50.cpp
        ${CMAKE_CURRENT_BINARY_DIR}/resnet50.h
        ${CMAKE_CURRENT_BINARY_DIR}/resnet50.weights.bin
        COMMAND
        model-compiler -g
                -model=${NESTC_BINARY_DIR}/vta/bundles/Resnet50Test/resnet50.onnx
                -keep-original-precision-for-nodes=SoftMax
                -backend=VTA
                -emit-bundle=${CMAKE_CURRENT_BINARY_DIR}
                -bundle-api=dynamic
                -model-input=gpu_0/data_0,float,[1,3,224,224]
                -load-profile=${NESTC_BINARY_DIR}/vta/bundles/Resnet50Test/resnet50_calib_2.yaml
                -quantization-schema=symmetric_with_power2_scale
        DEPENDS
        model-compiler VTA vtaCaffe2Resnet50BundleONNX
)

add_custom_command(
        OUTPUT
        ${CMAKE_CURRENT_BINARY_DIR}/resnet50_1.cpp
        ${CMAKE_CURRENT_BINARY_DIR}/resnet50_1.h
        ${CMAKE_CURRENT_BINARY_DIR}/resnet50_1.weights.bin
        COMMAND
        model-compiler -g
                -model=${NESTC_BINARY_DIR}/vta/bundles/Resnet50Test/resnet50.onnx
                -keep-original-precision-for-nodes=SoftMax
                -backend=VTA
                -emit-bundle=${CMAKE_CURRENT_BINARY_DIR}
                -bundle-api=dynamic
                -model-input=gpu_0/data_0,float,[1,3,224,224]
                -load-profile=${NESTC_BINARY_DIR}/vta/bundles/Resnet50Test/resnet50_calib_2.yaml
                -quantization-schema=symmetric_with_power2_scale
                -index-multi-evta=2
                -network-name=resnet50_1
                -main-entry-name=resnet50_1
        DEPENDS
        model-compiler VTA vtaCaffe2Resnet50BundleONNX
)

add_custom_command(
        OUTPUT
        ${CMAKE_CURRENT_BINARY_DIR}/resnet50_2.cpp
        ${CMAKE_CURRENT_BINARY_DIR}/resnet50_2.h
        ${CMAKE_CURRENT_BINARY_DIR}/resnet50_2.weights.bin
        COMMAND
        model-compiler -g
                -model=${NESTC_BINARY_DIR}/vta/bundles/Resnet50Test/resnet50.onnx
                -keep-original-precision-for-nodes=SoftMax
                -backend=VTA
                -emit-bundle=${CMAKE_CURRENT_BINARY_DIR}
                -bundle-api=dynamic
                -model-input=gpu_0/data_0,float,[1,3,224,224]
                -load-profile=${NESTC_BINARY_DIR}/vta/bundles/Resnet50Test/resnet50_calib_2.yaml
                -quantization-schema=symmetric_with_power2_scale
                -index-multi-evta=4
                -network-name=resnet50_2
                -main-entry-name=resnet50_2
        DEPENDS
        model-compiler VTA vtaCaffe2Resnet50BundleONNX
)

add_custom_command(
        OUTPUT
        ${CMAKE_CURRENT_BINARY_DIR}/resnet50_3.cpp
        ${CMAKE_CURRENT_BINARY_DIR}/resnet50_3.h
        ${CMAKE_CURRENT_BINARY_DIR}/resnet50_3.weights.bin
        COMMAND
        model-compiler -g
                -model=${NESTC_BINARY_DIR}/vta/bundles/Resnet50Test/resnet50.onnx
                -keep-original-precision-for-nodes=SoftMax
                -backend=VTA
                -emit-bundle=${CMAKE_CURRENT_BINARY_DIR}
                -bundle-api=dynamic
                -model-input=gpu_0/data_0,float,[1,3,224,224]
                -load-profile=${NESTC_BINARY_DIR}/vta/bundles/Resnet50Test/resnet50_calib_2.yaml
                -quantization-schema=symmetric_with_power2_scale
                -index-multi-evta=8
                -network-name=resnet50_3
                -main-entry-name=resnet50_3
        DEPENDS
        model-compiler VTA vtaCaffe2Resnet50BundleONNX
)

INCLUDE_DIRECTORIES(${CMAKE_CURRENT_BINARY_DIR})
add_executable(singlevtaCaffe2Resnet50Bundle single_thread_main.cpp ${CMAKE_CURRENT_BINARY_DIR}/resnet50.cpp)
target_link_libraries(singlevtaCaffe2Resnet50Bundle VTABundle ${BUNDLE_LINK_LIB})
add_nestc_test(ZCU102 MULTIEVTA NAME singlevtaCaffe2Resnet50Bundle COMMAND ${CMAKE_CURRENT_BINARY_DIR}/singlevtaCaffe2Resnet50Bundle USE_SH 1 PARAMS ${GLOW_SOURCE_DIR}/tests/images/imagenet/cat_285.png)
add_executable(multivtaCaffe2Resnet50Bundle multi_threads_main.cpp ${CMAKE_CURRENT_BINARY_DIR}/resnet50.cpp ${CMAKE_CURRENT_BINARY_DIR}/resnet50_1.cpp ${CMAKE_CURRENT_BINARY_DIR}/resnet50_2.cpp ${CMAKE_CURRENT_BINARY_DIR}/resnet50_3.cpp)
target_link_libraries(multivtaCaffe2Resnet50Bundle VTABundle ${BUNDLE_LINK_LIB} pthread)
add_nestc_test(ZCU102 MULTIEVTA NAME multivtaCaffe2Resnet50Bundle COMMAND ${CMAKE_CURRENT_BINARY_DIR}/multivtaCaffe2Resnet50Bundle USE_SH 1 PARAMS ${GLOW_SOURCE_DIR}/tests/images/imagenet/cat_285.png)
