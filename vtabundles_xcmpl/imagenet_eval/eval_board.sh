#!/bin/sh
#### JO added (2026-11-10): top-1 of a bundle executable over the ImageNetV2 PNGs (prep_imagenetv2.py, copied to the board)
#### with its EVAL_LIST mode (vta/bundles/DeiTTest/multi_threads_main.cpp). Loads the bitstream first as the run scripts do
#### (SYS_BIN of hw/script/run_all.sh unless BIN=<file>), holds /tmp/evta_run.lock while it runs.
#### usage (board, vtabundles_xcmpl/): sudo /bin/sh -c "imagenet_eval/eval_board.sh <exe> <name> [n images]"
####   e.g. imagenet_eval/eval_board.sh build_sys/DeiTTest/vtaDeiTBundle_sys base 10000
####   env: PNG=<png dir> (default ~xilinx/JO/imagenetv2_png224), OMP_NUM_THREADS (default 4), any EVTA_* / bundle switch
#### Result: imagenet_eval/result/<name>.log (one EV line every 500 images, the EVAL line last).
set -u
H=$(cd "$(dirname "$0")" && pwd)
EXE=$(readlink -f "$1"); NAME=$2; N=${3:-10000}
PNG=${PNG:-/home/xilinx/JO/imagenetv2_png224}
[ -x "$EXE" ] || { echo "$EXE not built"; exit 1; }
BIN=${BIN:-$(sed -n 's/^SYS_BIN=\${SYS_BIN:-\(.*\)}$/\1/p' $H/../hw/script/run_all.sh)}
$H/../hw/script/load_bit.sh $BIN | head -1 || exit 1
mkdir -p $H/result
L=$H/result/list_$N.txt
head -n $N $PNG/list.txt | awk -v d=$PNG '{print d "/" $1, $2}' > $L
exec 9>/tmp/evta_run.lock; flock -n 9 || { echo "another EVTA run is active (/tmp/evta_run.lock)"; exit 1; }
cd "$(dirname "$EXE")"
export OMP_NUM_THREADS=${OMP_NUM_THREADS:-4}
EVAL_LIST=$L EVAL_QUIET=1 "$EXE" > $H/result/$NAME.log 2>&1
tail -1 $H/result/$NAME.log
