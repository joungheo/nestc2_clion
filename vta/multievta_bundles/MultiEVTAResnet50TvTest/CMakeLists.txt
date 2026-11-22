#### JO added (2026-11-22, docs/evta_todo.md §5 DPU comparison): the torchvision ResNet-50 of vta/bundles/Resnet50TvTest (the DPU's
#### float model) on 4 EVTAs, for the multi-EVTA throughput against the DPU (3 x B4096); copy i runs on EVTA i. Model, profile and
#### options are the single bundle's (KL + -quantization-pow2-mse=2): copy 0 is byte-identical to it, its accuracy is Resnet50TvTest's.
#### Not added to ../CMakeLists.txt (a new directory re-runs cmake in Debug2_remote).
#### vtabundles_xcmpl/gen_multi4_bundles.sh MultiEVTAResnet50TvTest generates the copies (after gen_calib_bundle.sh Resnet50TvTest),
#### vtabundles_xcmpl builds them. This file lists the executables for collect_hw_bundles.py.
add_executable(singlevtaTvResnet50Bundle single_thread_main.cpp ${CMAKE_CURRENT_BINARY_DIR}/resnet50.cpp)
target_link_libraries(singlevtaTvResnet50Bundle VTABundle ${BUNDLE_LINK_LIB} pthread)
add_nestc_test(ZCU102 MULTIEVTA NAME singlevtaTvResnet50Bundle COMMAND ${CMAKE_CURRENT_BINARY_DIR}/singlevtaTvResnet50Bundle USE_SH 1 PARAMS ${GLOW_SOURCE_DIR}/tests/images/imagenet/cat_285.png)
add_executable(multivtaTvResnet50Bundle multi_threads_main.cpp ${CMAKE_CURRENT_BINARY_DIR}/resnet50.cpp ${CMAKE_CURRENT_BINARY_DIR}/resnet50_1.cpp ${CMAKE_CURRENT_BINARY_DIR}/resnet50_2.cpp ${CMAKE_CURRENT_BINARY_DIR}/resnet50_3.cpp)
target_link_libraries(multivtaTvResnet50Bundle VTABundle ${BUNDLE_LINK_LIB} pthread)
add_nestc_test(ZCU102 MULTIEVTA NAME multivtaTvResnet50Bundle COMMAND ${CMAKE_CURRENT_BINARY_DIR}/multivtaTvResnet50Bundle USE_SH 1 PARAMS ${GLOW_SOURCE_DIR}/tests/images/imagenet/cat_285.png)
