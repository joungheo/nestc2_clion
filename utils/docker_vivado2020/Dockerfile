# Ubuntu 18.04 image for Vivado / Vivado HLS 2020.1 (not supported on the host's Ubuntu 20.04).
# Vivado itself is not in the image: it is installed into the bind-mounted /tools/Xilinx/v2020.1.
FROM ubuntu:18.04
ARG UID=1000
ARG GID=1000
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      locales lsb-release sudo ca-certificates tzdata \
      build-essential gcc-multilib g++-multilib make git unzip xz-utils file \
      python3 python3-pip \
      libtinfo5 libncurses5 libncursesw5 \
      libx11-6 libxext6 libxrender1 libxtst6 libxi6 libxrandr2 libxft2 \
      libfreetype6 libfontconfig1 libglib2.0-0 libgtk2.0-0 libsm6 libice6 \
      net-tools iproute2 procps \
  && rm -rf /var/lib/apt/lists/* \
  && locale-gen en_US.UTF-8 \
  && ln -sf /bin/bash /bin/sh
ENV LANG=en_US.UTF-8 LC_ALL=en_US.UTF-8
# user "ubuntu" with the host user's uid/gid, so files written to the mounted tree belong to the host user
# (the docs use `docker exec -u ubuntu vivado2020-build ...`)
RUN groupadd -g ${GID} ubuntu && useradd -m -u ${UID} -g ${GID} -s /bin/bash ubuntu \
  && echo 'ubuntu ALL=(ALL) NOPASSWD:ALL' > /etc/sudoers.d/ubuntu
USER ubuntu
WORKDIR /home/ubuntu
CMD ["sleep", "infinity"]
