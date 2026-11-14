#!/bin/bash
# Build the image and (re)create the nestc-build container: the NEST-C simulator build (Debug2_remote) on this PC.
#   usage: utils/docker_nestc/setup.sh
# Mounts $HOME at the same path (the tree and its build dir keep the board's relative layout); --init reaps orphans.
# Inside: docker exec -it -u ubuntu nestc-build bash ; then utils/docker_nestc/configure.sh
set -e
cd "$(dirname "$0")"
IMG=nestc:18.04
NAME=nestc-build
# under sudo: the uid / gid of the user who ran sudo (the container user must own $HOME's files)
U=${SUDO_UID:-$(id -u)}; G=${SUDO_GID:-$(id -g "$(id -un)")}; H=$(getent passwd $U | cut -d: -f6)
docker build --build-arg UID=$U --build-arg GID=$G -t $IMG .
docker rm -f $NAME 2>/dev/null || true
docker run -d --init --name $NAME --hostname nestc-build --restart unless-stopped \
  -v "$H":"$H" -e HOME="$H" -e TZ=Asia/Seoul $IMG
docker exec $NAME bash -c 'grep PRETTY /etc/os-release; clang++-8 --version | head -1; cmake --version | head -1; id ubuntu'
