#### JO added (2026-11-22): MobileNetV2 on 4 EVTAs, as MultiEVTAResnet50Test. Copy i runs on EVTA i (model-compiler
#### -index-multi-evta=2^i, -network-name / -main-entry-name mobilenetv2_i); the model and the options are the single bundle's
#### (vta/bundles/MobileNetV2Test). Not added to ../CMakeLists.txt (a new directory re-runs cmake in Debug2_remote).
#### vtabundles_xcmpl/gen_multi4_bundles.sh MultiEVTAMobileNetV2Test generates the copies, vtabundles_xcmpl builds them.
#### This file lists the executables for collect_hw_bundles.py.
add_executable(singlevtaMobileNetV2Bundle single_thread_main.cpp ${CMAKE_CURRENT_BINARY_DIR}/mobilenetv2.cpp)
target_link_libraries(singlevtaMobileNetV2Bundle VTABundle ${BUNDLE_LINK_LIB} pthread)
add_nestc_test(ZCU102 MULTIEVTA NAME singlevtaMobileNetV2Bundle COMMAND ${CMAKE_CURRENT_BINARY_DIR}/singlevtaMobileNetV2Bundle USE_SH 1 PARAMS ${GLOW_SOURCE_DIR}/tests/images/imagenet/cat_285.png)
add_executable(multivtaMobileNetV2Bundle multi_threads_main.cpp ${CMAKE_CURRENT_BINARY_DIR}/mobilenetv2.cpp ${CMAKE_CURRENT_BINARY_DIR}/mobilenetv2_1.cpp ${CMAKE_CURRENT_BINARY_DIR}/mobilenetv2_2.cpp ${CMAKE_CURRENT_BINARY_DIR}/mobilenetv2_3.cpp)
target_link_libraries(multivtaMobileNetV2Bundle VTABundle ${BUNDLE_LINK_LIB} pthread)
add_nestc_test(ZCU102 MULTIEVTA NAME multivtaMobileNetV2Bundle COMMAND ${CMAKE_CURRENT_BINARY_DIR}/multivtaMobileNetV2Bundle USE_SH 1 PARAMS ${GLOW_SOURCE_DIR}/tests/images/imagenet/cat_285.png)
