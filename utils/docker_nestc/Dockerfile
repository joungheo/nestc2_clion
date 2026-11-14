# NEST-C simulator build (NESTC_USE_VTASIM=ON) on the x86 host PC, matching the ZCU102 board's PYNQ 2.4 rootfs
# (Ubuntu 18.04 bionic: clang-8 / llvm-8 8.0.0-3~ubuntu18.04.2, cmake 3.10.2, protobuf 3.0.0, glog 0.3.5, gflags 2.2.1,
# libpng 1.6.34, boost 1.65.1, fmt 7.0.3 from source, python 3.6 with numpy 1.19.5 / onnx 1.11.0 / protobuf 3.19.6;
# checked on the board 2026-11-14).
FROM ubuntu:18.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential binutils-aarch64-linux-gnu rsync clang-8 llvm-8 llvm-8-dev cmake ninja-build make git wget ca-certificates pkg-config \
      graphviz libpng-dev libprotobuf-dev protobuf-compiler libgoogle-glog-dev libgflags-dev \
      libboost-all-dev libdouble-conversion-dev libevent-dev libssl-dev libjemalloc-dev libpthread-stubs0-dev \
      opencl-headers libtinfo-dev zlib1g-dev libedit-dev libxml2-dev \
      python3 python3-pip python3-dev python3-setuptools sudo locales tzdata \
 && rm -rf /var/lib/apt/lists/* && locale-gen en_US.UTF-8
# fmt 7.0.3 (the board's /usr/local/lib/cmake/fmt), position independent as Glow links it into shared objects
RUN git clone -q --depth 1 -b 7.0.3 https://github.com/fmtlib/fmt /tmp/fmt && cd /tmp/fmt && mkdir build && cd build \
 && cmake -DCMAKE_C_COMPILER=clang-8 -DCMAKE_CXX_COMPILER=clang++-8 -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DFMT_TEST=OFF -DFMT_DOC=OFF .. && make -j8 install && rm -rf /tmp/fmt
RUN python3 -m pip install -q --upgrade "pip<22" && python3 -m pip install -q numpy==1.19.5 protobuf==3.19.6 onnx==1.11.0 \
      decorator==5.1.1 attrs==22.2.0 "scipy<1.6" Pillow pytest
ARG UID=1000
ARG GID=1000
RUN groupadd -g $GID ubuntu && useradd -m -u $UID -g $GID -s /bin/bash ubuntu \
 && echo "ubuntu ALL=(ALL) NOPASSWD:ALL" > /etc/sudoers.d/ubuntu
ENV LANG=en_US.UTF-8 TZ=Asia/Seoul
CMD ["sleep", "infinity"]
