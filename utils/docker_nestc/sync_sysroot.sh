#!/bin/bash
# Copy the parts of the ZCU102 board's rootfs the board executables compile / link against into a sysroot on this PC
# (default ~/zcu102_sysroot, about 2.1 GB) and make its absolute symlinks relative (they would point into this PC).
#   usage: utils/docker_nestc/sync_sysroot.sh [user@board] [sysroot dir]
set -e
B=${1:-xilinx@192.168.20.2}
S=${2:-$HOME/zcu102_sysroot}
mkdir -p $S
for d in /usr/include /usr/lib/aarch64-linux-gnu /usr/lib/llvm-8 /lib/aarch64-linux-gnu /usr/local/lib /usr/local/include \
         /usr/lib/gcc/aarch64-linux-gnu /lib/ld-linux-aarch64.so.1 /usr/lib/libcma.so; do
  mkdir -p $S$(dirname $d)
  rsync -a --delete $B:$d $S$(dirname $d)/
done
python3 - "$S" <<'PY'
import os, sys
S = sys.argv[1]
n = 0
for root, ds, fs in os.walk(S):
    for f in ds + fs:
        p = os.path.join(root, f)
        if os.path.islink(p) and os.readlink(p).startswith('/'):
            t = os.readlink(p)
            os.remove(p)
            os.symlink(os.path.relpath(S + t, root), p)
            n += 1
print('sysroot', S, ':', n, 'absolute symlinks made relative')
PY
