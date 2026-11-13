#!/bin/bash
# Install Vivado 2020.1 into /tools/Xilinx/v2020.1 inside the container, then apply the Y2K22 HLS patch.
#   usage: install_vivado.sh <Xilinx_Unified_2020.1_0602_1208.tar.gz> [y2k22_patch-1.2.zip]
set -e
TAR=$(readlink -f "$1"); PATCH=${2:+$(readlink -f "$2")}
CFG=$(readlink -f "$(dirname "$0")/install_config.txt")
X=$HOME/xilinx_installer_2020.1
mkdir -p $X
MD5=b018f7b331ab0446137756156ff944d9   # Xilinx_Unified_2020.1_0602_1208.tar.gz (AMD download page)
if [ ! -x $X/Xilinx_Unified_2020.1_0602_1208/xsetup ]; then
  echo "checking md5 of $TAR ..."
  [ "$(md5sum "$TAR" | cut -d' ' -f1)" = $MD5 ] || { echo "md5 mismatch: re-download $TAR"; exit 1; }
fi
[ -x $X/Xilinx_Unified_2020.1_0602_1208/xsetup ] || tar -xzf "$TAR" -C $X
docker exec -u ubuntu vivado2020-build bash -c "cd $X/Xilinx_Unified_2020.1_0602_1208 && \
  ./xsetup --agree XilinxEULA,3rdPartyEULA,WebTalkTerms --batch Install --config $CFG"
if [ -n "$PATCH" ]; then
  docker exec -u ubuntu -w /tools/Xilinx/v2020.1 vivado2020-build bash -c "unzip -o $PATCH && python3 y2k22_patch/patch.py"
fi
docker exec -u ubuntu vivado2020-build bash -c "source /tools/Xilinx/v2020.1/Vivado/2020.1/settings64.sh && \
  vivado -version | head -1 && vivado_hls -version | head -1"
