#### JO added (2026-11-10): DeiT-tiny on 4 EVTAs, as MultiEVTAResnet50Test. Copy i runs on EVTA i (model-compiler
#### -index-multi-evta=2^i, -network-name / -main-entry-name deit_i, plus -vta-round-shift as gen_deit_bundle.sh); the model
#### (deit_fold.onnx) and the profile are the single bundle's (vta/bundles/DeiTTest). Not added to ../CMakeLists.txt (a new
#### directory re-runs cmake in Debug2_remote). The board generates the copies with vtabundles_xcmpl/gen_multi4_bundles.sh
#### MultiEVTADeiTTest (it retries the unstable Glow graph hash of this model as gen_deit_bundle.sh) and builds them in
#### vtabundles_xcmpl. This file lists the executables for collect_hw_bundles.py.
add_executable(singlevtaDeiTBundle single_thread_main.cpp ${CMAKE_CURRENT_BINARY_DIR}/deit.cpp)
target_link_libraries(singlevtaDeiTBundle VTABundle ${BUNDLE_LINK_LIB} pthread)
add_nestc_test(ZCU102 MULTIEVTA NAME singlevtaDeiTBundle COMMAND ${CMAKE_CURRENT_BINARY_DIR}/singlevtaDeiTBundle USE_SH 1 PARAMS ${GLOW_SOURCE_DIR}/tests/images/imagenet/cat_285.png)
add_executable(multivtaDeiTBundle multi_threads_main.cpp ${CMAKE_CURRENT_BINARY_DIR}/deit.cpp ${CMAKE_CURRENT_BINARY_DIR}/deit_1.cpp ${CMAKE_CURRENT_BINARY_DIR}/deit_2.cpp ${CMAKE_CURRENT_BINARY_DIR}/deit_3.cpp)
target_link_libraries(multivtaDeiTBundle VTABundle ${BUNDLE_LINK_LIB} pthread)
add_nestc_test(ZCU102 MULTIEVTA NAME multivtaDeiTBundle COMMAND ${CMAKE_CURRENT_BINARY_DIR}/multivtaDeiTBundle USE_SH 1 PARAMS ${GLOW_SOURCE_DIR}/tests/images/imagenet/cat_285.png)
