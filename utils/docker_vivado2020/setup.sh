#!/bin/bash
# Build the image and (re)create the vivado2020-build container.
#   usage: utils/docker_vivado2020/setup.sh
# Mounts /tools/Xilinx and $HOME at the same paths; --network host so a node-locked
# license sees the host's NIC MAC address.
# --init: tini as PID 1 reaps orphans; with `sleep` as PID 1 Vivado synthesis hangs after synth_design
# (its parallel-synthesis workers stay zombies and HARTSJobManager waits for them forever).
set -e
cd "$(dirname "$0")"
IMG=vivado2020:18.04
NAME=vivado2020-build
docker build --build-arg UID=$(id -u) --build-arg GID=$(id -g "$(id -un)") -t $IMG .
docker rm -f $NAME 2>/dev/null || true
docker run -d --init --name $NAME --hostname $(hostname) --network host \
  --restart unless-stopped \
  -v /tools/Xilinx:/tools/Xilinx -v "$HOME":"$HOME" \
  -e XILINXD_LICENSE_FILE="$HOME/.Xilinx" -e TZ=Asia/Seoul \
  $IMG
docker exec $NAME bash -c 'cat /etc/os-release | head -2; id'
