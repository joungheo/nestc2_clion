#### JO added (2026-11-22, docs/evta_todo.md §5 DPU comparison): the TF-slim float MobileNetV2 of vta/bundles/MobileNetV2TfTest (the
#### DPU's float model) on 4 EVTAs, for the multi-EVTA throughput against the DPU (3 x B4096); copy i runs on EVTA i. Model, profile
#### and options are the single bundle's (KL + -quantization-pow2-mse=2): copy 0 is byte-identical to it, its accuracy is
#### MobileNetV2TfTest's. Not added to ../CMakeLists.txt (a new directory re-runs cmake in Debug2_remote).
#### vtabundles_xcmpl/gen_multi4_bundles.sh MultiEVTAMobileNetV2TfTest generates the copies (after gen_calib_bundle.sh
#### MobileNetV2TfTest), vtabundles_xcmpl builds them. This file lists the executables for collect_hw_bundles.py.
add_executable(singlevtaMobileNetV2TfBundle single_thread_main.cpp ${CMAKE_CURRENT_BINARY_DIR}/mobilenetv2.cpp)
target_link_libraries(singlevtaMobileNetV2TfBundle VTABundle ${BUNDLE_LINK_LIB} pthread)
add_nestc_test(ZCU102 MULTIEVTA NAME singlevtaMobileNetV2TfBundle COMMAND ${CMAKE_CURRENT_BINARY_DIR}/singlevtaMobileNetV2TfBundle USE_SH 1 PARAMS ${GLOW_SOURCE_DIR}/tests/images/imagenet/cat_285.png)
add_executable(multivtaMobileNetV2TfBundle multi_threads_main.cpp ${CMAKE_CURRENT_BINARY_DIR}/mobilenetv2.cpp ${CMAKE_CURRENT_BINARY_DIR}/mobilenetv2_1.cpp ${CMAKE_CURRENT_BINARY_DIR}/mobilenetv2_2.cpp ${CMAKE_CURRENT_BINARY_DIR}/mobilenetv2_3.cpp)
target_link_libraries(multivtaMobileNetV2TfBundle VTABundle ${BUNDLE_LINK_LIB} pthread)
add_nestc_test(ZCU102 MULTIEVTA NAME multivtaMobileNetV2TfBundle COMMAND ${CMAKE_CURRENT_BINARY_DIR}/multivtaMobileNetV2TfBundle USE_SH 1 PARAMS ${GLOW_SOURCE_DIR}/tests/images/imagenet/cat_285.png)
