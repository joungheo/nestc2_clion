cmake_minimum_required(VERSION 3.10)
project(vtalib)

option(NESTC_EVTA_RUN_WITH_GENERIC_BUNDLE "Use Generic library for EVTA" ON)
option(NESTC_EVTA_RUN_ON_ZCU102 "Run on ZCU102 board" OFF)
option(NESTC_EVTA_LIB_VALIDATION "Validate EVTA library while running" OFF)
option(NESTC_EVTA_DEBUG "Debug mode for EVTA library" OFF)
option(NESTC_EVTA_PROFILE "Profile mode for EVTA HW" OFF)
#### JO added: the ZCU102 EVTA bitstream zcu102_1x16_i8w8a32_16_16_19_18 on our board is not
#### cache-coherent (coherent=true hangs), so the runtime must flush/invalidate CPU caches.
#### Turn ON only for a design whose EVTA DMA is coherent with the CPU (HPC ports + CCI snoop).
option(NESTC_EVTA_COHERENT_ACCESSES "EVTA DMA is cache-coherent with the CPU (ZCU102 only)" OFF)
INCLUDE_DIRECTORIES(include 3rdparty/dlpack/include)
INCLUDE_DIRECTORIES(include 3rdparty/dmlc-core/include)
option(VTALIB_WITHOUT_NESTC "use VTALIB_ONLY running without nest-c" OFF)
option(NESTC_EVTA_MULTI "use Multi-EVTA" OFF)

#use VTALIB_ONLY running without nest-c
if(VTALIB_WITHOUT_NESTC)
    add_definitions(-DVTALIB_ONLY=1)
endif()

if(NOT NESTC_EVTA_RUN_ON_ZCU102)
    message(STATUS "VTALIB with Simulator")
    add_definitions(-DNESTC_USE_VTASIM=1)
    INCLUDE_DIRECTORIES(include include/simulator include/simulator/vta)
    #add_library(vta_runtime lib/simulator/runtime.cc lib/simulator/sim_driver.cc lib/simulator/virtual_memory.cc lib/simulator/sim_tlpp.cc)
    if(NOT NESTC_BNN)
        add_library(vta_runtime lib/simulator/runtime.cc lib/simulator/sim_driver.cc lib/simulator/virtual_memory.cc lib/simulator/sim_tlpp.cc)
    else()
        add_library(vta_runtime lib/simulator/runtime.cc lib/simulator/sim_driver_bnn.cc lib/simulator/virtual_memory.cc lib/simulator/sim_tlpp.cc)
    endif()
    add_subdirectory(tests/simtests)
else()
    message(STATUS "VTALIB with ZCU102")
    add_definitions(-DNESTC_USE_VTASIM=0)
    add_definitions(-DDMLC_USE_FOPEN64=0
            -DVTA_TARGET=zcu102
            -DVTA_HW_VER=0.0.1
            -DVTA_LOG_INP_WIDTH=3
            -DVTA_LOG_WGT_WIDTH=3
            -DVTA_LOG_ACC_WIDTH=5
            -DVTA_LOG_BATCH=0
            -DVTA_LOG_BLOCK=4
            -DVTA_LOG_UOP_BUFF_SIZE=16
            -DVTA_LOG_BLOCK_IN=4
            -DVTA_LOG_BLOCK_OUT=4
            -DVTA_LOG_OUT_WIDTH=3
            -DVTA_LOG_OUT_BUFF_SIZE=16
            -DVTA_LOG_BUS_WIDTH=7
            -DVTA_DEVICE_NUM=4
            -DVTA_IP_REG_MAP_RANGE=0x1000
            -DVTA_FETCH_ADDR=0xA0000000
            -DVTA_LOAD_ADDR=0xA0001000
            -DVTA_COMPUTE_ADDR=0xA0002000
            -DVTA_STORE_ADDR=0xA0003000
            -DVTA_FETCH_ADDR_1=0xA0010000
            -DVTA_LOAD_ADDR_1=0xA0011000
            -DVTA_COMPUTE_ADDR_1=0xA0012000
            -DVTA_STORE_ADDR_1=0xA0013000
            -DVTA_FETCH_ADDR_2=0xA0020000
            -DVTA_LOAD_ADDR_2=0xA0021000
            -DVTA_COMPUTE_ADDR_2=0xA0022000
            -DVTA_STORE_ADDR_2=0xA0023000
            -DVTA_FETCH_ADDR_3=0xA0030000
            -DVTA_LOAD_ADDR_3=0xA0031000
            -DVTA_COMPUTE_ADDR_3=0xA0032000
            -DVTA_STORE_ADDR_3=0xA0033000
            -DVTA_FETCH_INSN_COUNT_OFFSET=16
            -DVTA_FETCH_INSN_ADDR_OFFSET=24
            -DVTA_LOAD_INP_ADDR_OFFSET=16
            -DVTA_LOAD_WGT_ADDR_OFFSET=24
            -DVTA_COMPUTE_DONE_WR_OFFSET=16
            -DVTA_COMPUTE_DONE_RD_OFFSET=24
            -DVTA_COMPUTE_UOP_ADDR_OFFSET=32
            -DVTA_COMPUTE_BIAS_ADDR_OFFSET=40
            -DVTA_STORE_OUT_ADDR_OFFSET=16
            #### JO changed: coherence now selected by NESTC_EVTA_COHERENT_ACCESSES (default OFF)
            #-DVTA_COHERENT_ACCESSES=true
            -Dvta_EXPORTS)
    #### JO added
    if(NESTC_EVTA_COHERENT_ACCESSES)
        add_definitions(-DVTA_COHERENT_ACCESSES=true)
    else()
        add_definitions(-DVTA_COHERENT_ACCESSES=false)
    endif()
    if(NESTC_EVTA_MULTI)
    add_definitions(-DVTA_LOG_INP_BUFF_SIZE=15
            -DVTA_LOG_WGT_BUFF_SIZE=18
            -DVTA_LOG_ACC_BUFF_SIZE=17)
    else()
        if(NOT NESTC_BNN)
            add_definitions(-DVTA_LOG_INP_BUFF_SIZE=16
                    -DVTA_LOG_WGT_BUFF_SIZE=19
                    -DVTA_LOG_ACC_BUFF_SIZE=18)
        else()
            add_definitions(-DVTA_LOG_INP_BUFF_SIZE=15
                -DVTA_LOG_WGT_BUFF_SIZE=18
                -DVTA_LOG_ACC_BUFF_SIZE=17)
        endif()
    endif()

    if ("${CMAKE_BUILD_TYPE}" STREQUAL "Debug")
        add_compile_options(-std=c++14 -faligned-new -O0 -g -Wall -fPIC -static -fopenmp  -Wno-psabi  -mtune=cortex-a53 -march=armv8-a+crc -ftree-vectorize)
    else()
        add_compile_options(-std=c++14 -faligned-new -O3 -Wall -fPIC  -static -fopenmp  -Wno-psabi -mtune=cortex-a53 -march=armv8-a+crc -ftree-vectorize)
    endif()
    INCLUDE_DIRECTORIES(include include/zcu102)
    #link_libraries(prebuilt_lib)
    add_library(vta_runtime lib/zcu102/api.cc lib/zcu102/pynq_driver.cc) 
    #link_libraries(arm_compute_graph arm_compute arm_compute_core)
    add_subdirectory(tests/zcu102)
    #target_link_libraries(gomp)
    #target_link_libraries(vta_runtime lib/acl/libarm_compute_graph-static.a arm_compute lib/acl/libarm_compute-static.a arm_compute lib/acl/libarm_compute_core-static.a)
    
    find_library(EXTERN_ACL_COMPUTE_LIB
            NAMES arm_compute-static.a libarm_compute-static.a
            HINTS "lib/acl"
            )
    find_library(EXTERN_ACL_COMPUTE_CORE_LIB
            NAMES arm_compute_core-static.a libarm_compute_core-static.a
            HINTS "lib/acl"
            )
    find_library(EXTERN_ACL_COMPUTE_GRAPH_LIB
            NAMES arm_compute_graph-static.a libarm_compute_graph-static.a
            HINTS "lib/acl"
            )
    message(STATUS "Build with Arm Compute Library graph runtime support: "
    ${EXTERN_ACL_COMPUTE_LIB} ", \n"
    ${EXTERN_ACL_COMPUTE_CORE_LIB} ", \n"
    ${EXTERN_ACL_COMPUTE_GRAPH_LIB})
    target_link_libraries(vta_runtime ${EXTERN_ACL_COMPUTE_LIB}  ${EXTERN_ACL_COMPUTE_CORE_LIB} ${EXTERN_ACL_COMPUTE_GRAPH_LIB})
endif()
if(NOT NESTC_EVTA_RUN_WITH_GENERIC_BUNDLE)
    add_definitions(-DVTA_RUN_ON_AARCH64)
    add_library(VTABundle lib/Bundle/VTABundle.cpp lib/Bundle/CPUBundle_aarch64.cpp)
    INCLUDE_DIRECTORIES(/usr/lib/llvm-8.0/include)
else()
    add_library(VTABundle lib/Bundle/VTABundle.cpp lib/Bundle/CPUBundle_generic.cpp)
endif()

if(NESTC_EVTA_DEBUG)
    add_definitions(-DVTA_DEBUG_MODE)
endif()
if(NESTC_EVTA_LIB_VALIDATION)
    add_definitions(-DVTA_VALIDATION)
endif()
if(NESTC_EVTA_PROFILE)
    message("profile enabled")
    add_definitions(-DPROFILE_ENABLE=1)
else()
    message("profile disabled")
    add_definitions(-DPROFILE_ENABLE=0)
endif()

#### delete LLVMSupport when ZCU102=ON
target_link_libraries(VTABundle PUBLIC
        vta_runtime LLVMSupport pthread glog gflags gomp)
#if(NESTC_EVTA_RUN_ON_ZCU102)
#    target_link_libraries(VTABundle PUBLIC vta_runtime pthread glog gflags gomp)
#else()
#    target_link_libraries(VTABundle PUBLIC vta_runtime LLVMSupport pthread glog gflags gomp)
#endif()

if(NOT NESTC_USE_VTASIM)
    message("not vtasim. linking cma library")
    target_link_libraries(VTABundle PUBLIC cma)
endif()
