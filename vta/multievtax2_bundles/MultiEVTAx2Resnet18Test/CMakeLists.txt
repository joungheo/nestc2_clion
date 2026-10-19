# #### JO added (2026-10-17): 2-EVTA version of ../../multievta_bundles/MultiEVTAResnet18Test, for the bitstream
# zcu102_2_1x16_i8w8a32_16_15_18_17 (vtalib built with NESTC_EVTA_MULTI=ON).
# Two copies of the Resnet18 bundle: mxnet_exported_resnet18 on EVTA0 (-index-multi-evta=1, the default) and
# mxnet_exported_resnet18_1 on EVTA1 (-index-multi-evta=2). multi_threads_main.cpp runs them in two threads that
# share 100 inferences; single_thread_main.cpp runs the EVTA0 copy alone (baseline on the same bitstream).
# Quantization profile and skip list are the same as vta/bundles/Resnet18Test.
add_custom_command(
        OUTPUT
        ${CMAKE_CURRENT_BINARY_DIR}/mxnet_exported_resnet18_calib_1.yaml
        COMMAND
        image-classifier ${NESTC_ROOT_DIR}/glow/tests/images/imagenet/cat_285.png
        -m=${NESTC_BINARY_DIR}/models/mxnet_exported_resnet18.onnx
        -model-input-name=data
        -backend=VTAInterpreter
        -image-layout=NHWC
        -compute-softmax
        -topk=5
        -image-mode=0to255
        -dump-profile=${CMAKE_CURRENT_BINARY_DIR}/mxnet_exported_resnet18_calib_1.yaml
        -quantization-schema="symmetric_with_power2_scale"
        DEPENDS image-classifier MxnetResnet18ONNXDownload
)

foreach(copy 0 1)
    if(copy EQUAL 0)
        set(name mxnet_exported_resnet18)
        set(mask 1)
        set(after "")
    else()
        set(name mxnet_exported_resnet18_${copy})
        math(EXPR mask "1 << ${copy}")
        # every run also writes VTARuntime.h / VTASkipQuantizeNodes.txt here: run one after the other
        set(after ${CMAKE_CURRENT_BINARY_DIR}/mxnet_exported_resnet18.cpp)
    endif()
    add_custom_command(
            OUTPUT
            ${CMAKE_CURRENT_BINARY_DIR}/${name}.cpp
            ${CMAKE_CURRENT_BINARY_DIR}/${name}.h
            ${CMAKE_CURRENT_BINARY_DIR}/${name}.weights.bin
            COMMAND ${CMAKE_COMMAND} -E copy
            ${CMAKE_CURRENT_SOURCE_DIR}/VTASkipQuantizeNodes.txt
            ${CMAKE_CURRENT_BINARY_DIR}/VTASkipQuantizeNodes.txt
            COMMAND
            model-compiler -g
            -model=${NESTC_BINARY_DIR}/models/mxnet_exported_resnet18.onnx
            -backend=VTA
            -emit-bundle=${CMAKE_CURRENT_BINARY_DIR}
            -bundle-api=dynamic
            -model-input=data,float,[1,224,224,3]
            -load-profile=${CMAKE_CURRENT_BINARY_DIR}/mxnet_exported_resnet18_calib_1.yaml
            -quantization-schema=symmetric_with_power2_scale
            -index-multi-evta=${mask}
            -network-name=${name}
            -main-entry-name=${name}
            DEPENDS
            model-compiler VTA MxnetResnet18ONNXDownload
            ${CMAKE_CURRENT_BINARY_DIR}/mxnet_exported_resnet18_calib_1.yaml
            ${CMAKE_CURRENT_SOURCE_DIR}/VTASkipQuantizeNodes.txt ${after}
    )
endforeach()

INCLUDE_DIRECTORIES(${CMAKE_CURRENT_BINARY_DIR})

add_custom_target(multievtax2SingleResnet18BundleNet DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/mxnet_exported_resnet18.cpp)
add_executable(multievtax2SingleResnet18Bundle single_thread_main.cpp ${CMAKE_CURRENT_BINARY_DIR}/mxnet_exported_resnet18.cpp)
add_dependencies(multievtax2SingleResnet18Bundle multievtax2SingleResnet18BundleNet ${BUNDLE_DEPENDS})
target_link_libraries(multievtax2SingleResnet18Bundle VTABundle ${BUNDLE_LINK_LIB})
set_target_properties(multievtax2SingleResnet18Bundle PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
add_nestc_test(ZCU102 MULTIEVTA NAME multievtax2SingleResnet18Bundle COMMAND ${CMAKE_CURRENT_BINARY_DIR}/multievtax2SingleResnet18Bundle USE_SH 1 PARAMS ${GLOW_SOURCE_DIR}/tests/images/imagenet/cat_285.png)

add_custom_target(multievtax2Resnet18BundleNet DEPENDS
        ${CMAKE_CURRENT_BINARY_DIR}/mxnet_exported_resnet18.cpp ${CMAKE_CURRENT_BINARY_DIR}/mxnet_exported_resnet18_1.cpp)
add_executable(multievtax2Resnet18Bundle multi_threads_main.cpp
        ${CMAKE_CURRENT_BINARY_DIR}/mxnet_exported_resnet18.cpp ${CMAKE_CURRENT_BINARY_DIR}/mxnet_exported_resnet18_1.cpp)
add_dependencies(multievtax2Resnet18Bundle multievtax2Resnet18BundleNet ${BUNDLE_DEPENDS})
target_link_libraries(multievtax2Resnet18Bundle VTABundle ${BUNDLE_LINK_LIB})
set_target_properties(multievtax2Resnet18Bundle PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
add_nestc_test(ZCU102 MULTIEVTA NAME multievtax2Resnet18Bundle COMMAND ${CMAKE_CURRENT_BINARY_DIR}/multievtax2Resnet18Bundle USE_SH 1 PARAMS ${GLOW_SOURCE_DIR}/tests/images/imagenet/cat_285.png)
