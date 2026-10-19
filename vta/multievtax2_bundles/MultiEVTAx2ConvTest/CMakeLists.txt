# #### JO added (2026-10-17): 2-EVTA version of ../../multievta_bundles/MultiEVTAConvTest, for the bitstream
# zcu102_2_1x16_i8w8a32_16_15_18_17 (EVTA0 at 0xA000xxxx, EVTA1 at 0xA001xxxx; vtalib built with NESTC_EVTA_MULTI=ON).
# VTASaverTest.multievtaConvTest emits four 14x14x32 conv bundles, bundle i pinned to EVTA i (index mask 1 << i);
# this test runs bundles 0 and 1 in two threads at the same time.
add_custom_command(
        OUTPUT
        ${CMAKE_CURRENT_BINARY_DIR}/multievtaConvTestBundle0.cpp
        ${CMAKE_CURRENT_BINARY_DIR}/multievtaConvTestBundle1.cpp
        ${CMAKE_CURRENT_BINARY_DIR}/multievtaConvTestInput0.bin
        ${CMAKE_CURRENT_BINARY_DIR}/multievtaConvTestInput1.bin
        ${CMAKE_CURRENT_BINARY_DIR}/multievtaConvTestBundle0.weights.bin
        ${CMAKE_CURRENT_BINARY_DIR}/multievtaConvTestBundle1.weights.bin
        ${CMAKE_CURRENT_BINARY_DIR}/multievtaConvTestGolden0.bin
        ${CMAKE_CURRENT_BINARY_DIR}/multievtaConvTestGolden1.bin
        ${CMAKE_CURRENT_BINARY_DIR}/multievtaConvTestMainEntry0.h
        ${CMAKE_CURRENT_BINARY_DIR}/multievtaConvTestMainEntry1.h
        ${CMAKE_CURRENT_BINARY_DIR}/VTARuntime.h
        COMMAND
        VTASaverTest --gtest_filter=VTASaverTest.multievtaConvTest
        DEPENDS
        VTASaverTest
)
add_custom_target(multievtax2ConvTestBundleNet DEPENDS
        ${CMAKE_CURRENT_BINARY_DIR}/multievtaConvTestBundle0.cpp
        ${CMAKE_CURRENT_BINARY_DIR}/multievtaConvTestBundle1.cpp)
INCLUDE_DIRECTORIES(${CMAKE_CURRENT_BINARY_DIR})

add_executable(multievtax2ConvTestBundle multievtax2ConvTestBundleMain.cpp
        ${CMAKE_CURRENT_BINARY_DIR}/multievtaConvTestBundle0.cpp
        ${CMAKE_CURRENT_BINARY_DIR}/multievtaConvTestBundle1.cpp)
add_dependencies(multievtax2ConvTestBundle multievtax2ConvTestBundleNet ${BUNDLE_DEPENDS})
target_link_libraries(multievtax2ConvTestBundle VTABundle ${BUNDLE_LINK_LIB})
set_target_properties(multievtax2ConvTestBundle
        PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
        )

# EVTA0 writes output.bin (checked against Golden0), EVTA1 writes multievtaConvTestOutput1.bin (Golden1)
add_nestc_test(ZCU102 MULTIEVTA NAME multievtax2ConvTestBundle
        COMMAND ${CMAKE_CURRENT_BINARY_DIR}/multievtax2ConvTestBundle USE_SH 1 PARAMS ${CMAKE_CURRENT_BINARY_DIR}/multievtaConvTestInput0.bin
        USE_DIFF 1 DIFF_TARGET ${CMAKE_CURRENT_BINARY_DIR}/multievtaConvTestGolden0.bin)
