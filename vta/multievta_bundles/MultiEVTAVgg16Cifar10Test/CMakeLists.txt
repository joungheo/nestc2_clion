#### JO added (2026-11-06): VGG16 CIFAR-10 on 4 EVTAs, as MultiEVTAResnet18Test.
#### Copy i runs on EVTA i (model-compiler -index-multi-evta=2^i, -network-name / -main-entry-name <name>_i); the
#### model and the profile are the single bundle's (vta/bundles/Vgg16Cifar10Test). Not added to ../CMakeLists.txt: a new directory re-runs
#### cmake in Debug2_remote, which rebuilds all of Glow on a new day (CLAUDE.md). The board generates the copies with
#### vtabundles_xcmpl/gen_multi4_bundles.sh (the same commands) and builds them in vtabundles_xcmpl.

add_custom_command(
        OUTPUT
        ${CMAKE_CURRENT_BINARY_DIR}/vgg16_cifar10.cpp
        ${CMAKE_CURRENT_BINARY_DIR}/vgg16_cifar10.h
        ${CMAKE_CURRENT_BINARY_DIR}/vgg16_cifar10.weights.bin
        COMMAND
        model-compiler -g
                -model=${NESTC_BINARY_DIR}/vta/bundles/Vgg16Cifar10Test/vgg16_cifar10.onnx
                -backend=VTA
                -emit-bundle=${CMAKE_CURRENT_BINARY_DIR}
                -bundle-api=dynamic
                -model-input=input,float,[1,3,32,32]
                -load-profile=${NESTC_BINARY_DIR}/vta/bundles/Vgg16Cifar10Test/vgg16_calib_1.yaml
                -quantization-schema=symmetric_with_power2_scale
        DEPENDS
        model-compiler VTA vtaVGG16Cifar10Bundle
)

add_custom_command(
        OUTPUT
        ${CMAKE_CURRENT_BINARY_DIR}/vgg16_cifar10_1.cpp
        ${CMAKE_CURRENT_BINARY_DIR}/vgg16_cifar10_1.h
        ${CMAKE_CURRENT_BINARY_DIR}/vgg16_cifar10_1.weights.bin
        COMMAND
        model-compiler -g
                -model=${NESTC_BINARY_DIR}/vta/bundles/Vgg16Cifar10Test/vgg16_cifar10.onnx
                -backend=VTA
                -emit-bundle=${CMAKE_CURRENT_BINARY_DIR}
                -bundle-api=dynamic
                -model-input=input,float,[1,3,32,32]
                -load-profile=${NESTC_BINARY_DIR}/vta/bundles/Vgg16Cifar10Test/vgg16_calib_1.yaml
                -quantization-schema=symmetric_with_power2_scale
                -index-multi-evta=2
                -network-name=vgg16_cifar10_1
                -main-entry-name=vgg16_cifar10_1
        DEPENDS
        model-compiler VTA vtaVGG16Cifar10Bundle
)

add_custom_command(
        OUTPUT
        ${CMAKE_CURRENT_BINARY_DIR}/vgg16_cifar10_2.cpp
        ${CMAKE_CURRENT_BINARY_DIR}/vgg16_cifar10_2.h
        ${CMAKE_CURRENT_BINARY_DIR}/vgg16_cifar10_2.weights.bin
        COMMAND
        model-compiler -g
                -model=${NESTC_BINARY_DIR}/vta/bundles/Vgg16Cifar10Test/vgg16_cifar10.onnx
                -backend=VTA
                -emit-bundle=${CMAKE_CURRENT_BINARY_DIR}
                -bundle-api=dynamic
                -model-input=input,float,[1,3,32,32]
                -load-profile=${NESTC_BINARY_DIR}/vta/bundles/Vgg16Cifar10Test/vgg16_calib_1.yaml
                -quantization-schema=symmetric_with_power2_scale
                -index-multi-evta=4
                -network-name=vgg16_cifar10_2
                -main-entry-name=vgg16_cifar10_2
        DEPENDS
        model-compiler VTA vtaVGG16Cifar10Bundle
)

add_custom_command(
        OUTPUT
        ${CMAKE_CURRENT_BINARY_DIR}/vgg16_cifar10_3.cpp
        ${CMAKE_CURRENT_BINARY_DIR}/vgg16_cifar10_3.h
        ${CMAKE_CURRENT_BINARY_DIR}/vgg16_cifar10_3.weights.bin
        COMMAND
        model-compiler -g
                -model=${NESTC_BINARY_DIR}/vta/bundles/Vgg16Cifar10Test/vgg16_cifar10.onnx
                -backend=VTA
                -emit-bundle=${CMAKE_CURRENT_BINARY_DIR}
                -bundle-api=dynamic
                -model-input=input,float,[1,3,32,32]
                -load-profile=${NESTC_BINARY_DIR}/vta/bundles/Vgg16Cifar10Test/vgg16_calib_1.yaml
                -quantization-schema=symmetric_with_power2_scale
                -index-multi-evta=8
                -network-name=vgg16_cifar10_3
                -main-entry-name=vgg16_cifar10_3
        DEPENDS
        model-compiler VTA vtaVGG16Cifar10Bundle
)

INCLUDE_DIRECTORIES(${CMAKE_CURRENT_BINARY_DIR})
add_executable(singlevtaVGG16Cifar10Bundle single_thread_main.cpp ${CMAKE_CURRENT_BINARY_DIR}/vgg16_cifar10.cpp)
target_link_libraries(singlevtaVGG16Cifar10Bundle VTABundle ${BUNDLE_LINK_LIB})
add_nestc_test(ZCU102 MULTIEVTA NAME singlevtaVGG16Cifar10Bundle COMMAND ${CMAKE_CURRENT_BINARY_DIR}/singlevtaVGG16Cifar10Bundle USE_SH 1 PARAMS ${NESTC_ROOT_DIR}/examples/bundles/vgg16_cifar10/cifar10/test/airplane/0001.png)
add_executable(multivtaVGG16Cifar10Bundle multi_threads_main.cpp ${CMAKE_CURRENT_BINARY_DIR}/vgg16_cifar10.cpp ${CMAKE_CURRENT_BINARY_DIR}/vgg16_cifar10_1.cpp ${CMAKE_CURRENT_BINARY_DIR}/vgg16_cifar10_2.cpp ${CMAKE_CURRENT_BINARY_DIR}/vgg16_cifar10_3.cpp)
target_link_libraries(multivtaVGG16Cifar10Bundle VTABundle ${BUNDLE_LINK_LIB} pthread)
add_nestc_test(ZCU102 MULTIEVTA NAME multivtaVGG16Cifar10Bundle COMMAND ${CMAKE_CURRENT_BINARY_DIR}/multivtaVGG16Cifar10Bundle USE_SH 1 PARAMS ${NESTC_ROOT_DIR}/examples/bundles/vgg16_cifar10/cifar10/test/airplane/0001.png)
