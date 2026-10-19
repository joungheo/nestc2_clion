INCLUDE_DIRECTORIES(../)
if(NOT NESTC_USE_PRECOMPILED_EVTA_LIBRARY)
    add_subdirectory(vtalib)
endif()

if(NOT NESTC_BNN)
    add_subdirectory(tests)
    if(NOT NESTC_USE_PRECOMPILED_EVTA_LIBRARY)
        add_nestc_test(name VTABackendSimulatorTest ${NESTC_BINARY_DIR}/tests/VTABackendSimulatorTest --gtest_output=xml:VTABackendSimulatorTest.xml)
    endif()
endif()

add_subdirectory(bundles)
if(NESTC_EVTA_MULTI)
    if(NOT NESTC_BNN)
        add_subdirectory(multievta_bundles)
    endif()
endif()
#### JO added: bundle sources for the 2-EVTA bitstream (zcu102_2_1x16_*). Only generated here (simulator build);
#### they are compiled for the board by vtabundles_xcmpl with NESTC_EVTA_MULTI=ON. A separate option, so that the
#### simulator build keeps the single-EVTA vtalib settings (NESTC_EVTA_MULTI changes the EVTA buffer sizes).
option(NESTC_EVTA_MULTI_BUNDLES "Generate the multi-EVTA (x2) bundle sources" OFF)
if(NESTC_EVTA_MULTI_BUNDLES AND NOT NESTC_BNN)
    add_subdirectory(multievtax2_bundles)
endif()

