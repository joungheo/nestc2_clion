# Cross build of the board executables (vtabundles_xcmpl, vtalib) on the host PC in the nestc-build container:
# the board's compiler (clang-8 8.0.0, --target aarch64-linux-gnu) against a copy of the board's rootfs
# (utils/docker_nestc/sync_sysroot.sh, default ~/zcu102_sysroot), linked by binutils-aarch64-linux-gnu.
#   cmake -DCMAKE_TOOLCHAIN_FILE=<tree>/utils/docker_nestc/zcu102_toolchain.cmake [the usual -D options] ..
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
if(DEFINED ENV{ZCU102_SYSROOT})
  set(CMAKE_SYSROOT $ENV{ZCU102_SYSROOT})
else()
  set(CMAKE_SYSROOT $ENV{HOME}/zcu102_sysroot)
endif()
set(CMAKE_C_COMPILER clang-8)
set(CMAKE_CXX_COMPILER clang++-8)
set(CMAKE_C_COMPILER_TARGET aarch64-linux-gnu)
set(CMAKE_CXX_COMPILER_TARGET aarch64-linux-gnu)
# the board's clang finds LLVMSupport / libomp in its own /usr/lib/llvm-8/lib, searched after the system dirs (so -lgomp
# is GNU libgomp.so.1 as on the board, not llvm-8/lib/libgomp.so = libomp): -Wl,-L comes after the driver's -L list;
# ld from binutils-aarch64-linux-gnu
set(CMAKE_EXE_LINKER_FLAGS_INIT "-Wl,-L${CMAKE_SYSROOT}/usr/lib/llvm-8/lib -Wl,--hash-style=both -B/usr/bin/aarch64-linux-gnu-")   # both: the board ld default (.hash + .gnu.hash)
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-Wl,-L${CMAKE_SYSROOT}/usr/lib/llvm-8/lib -B/usr/bin/aarch64-linux-gnu-")
set(CMAKE_C_FLAGS_INIT "-B/usr/bin/aarch64-linux-gnu-")
set(CMAKE_CXX_FLAGS_INIT "-B/usr/bin/aarch64-linux-gnu-")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)   # python3 (timed_bundle.py) runs on the host
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY BOTH)   # vtalib find_library()s the ACL static libraries in its own lib/acl
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
