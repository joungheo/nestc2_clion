#!/bin/sh
#### JO added (2026-11-09): generate vta/bundles/DeiTTest (profile + VTA bundle) with the simulator build's image-classifier /
#### model-compiler, as gen_mobilenetv2_bundle.sh (no cmake re-run of Debug2_remote). The model is the ONNX export of
#### facebook/deit-tiny-distilled-patch16-224 (Xenova/deit-tiny-distilled-patch16-224, onnx/model.onnx, opset 11) with the
#### input fixed to 1x3x224x224 and the shape computations constant-folded (fold_onnx.py; needs python3 onnx >= 1.13).
#### Most of the graph stays on the CPU (VTASave generic CPU ops: matmul, layer norm / GELU pieces, copies); the EVTA runs the
#### patch embedding conv and the two classifiers.
#### The Glow graph hash of the loaded model is not stable between runs (the same graph gives different hashes), so the
#### profile check fails at random: model-compiler runs on a copy of the profile whose GraphPreLowerHash is set to the hash
#### the failed run printed, up to 4 times.
#### Output and gen.log / prof.log in Debug2_remote/vta/bundles/DeiTTest; then collect_hw_bundles.py DeiTTest.
#### usage (on the board): vtabundles_xcmpl/gen_deit_bundle.sh [model-compiler options]
set -u
H=$(cd "$(dirname "$0")" && pwd)
B=$(cd "$H/../Debug2_remote" && pwd)
export TVM_HOME=$B/../tvm PYTHONPATH=$B/../tvm/python TVM_LIBRARY_PATH=$B/tvm
O=$B/vta/bundles/DeiTTest
I=$B/../glow/tests/images/imagenet
Q=-quantization-schema=symmetric_with_power2_scale
PRE="-image-layout=NCHW -image-mode=0to1 -image-channel-order=RGB -mean=0.485,0.456,0.406 -stddev=0.229,0.224,0.225"
mkdir -p $O && cd $O || exit 1
if [ ! -s deit_fold.onnx ]; then
  [ -s model.onnx ] || { echo "$O/model.onnx missing: https://huggingface.co/Xenova/deit-tiny-distilled-patch16-224/resolve/main/onnx/model.onnx"; exit 1; }
  python3 $H/fold_onnx.py model.onnx deit_fold.onnx 1,3,224,224 || exit 1
fi
if [ ! -s deit_calib.yaml ]; then
  # one image per run (-minibatch=1): the profile of a 3-image batch has another graph hash
  $B/glow/bin/image-classifier $I/cat_285.png $I/dog_207.png $I/zebra_340.png -m=deit_fold.onnx -model-input-name=pixel_values \
    -backend=VTAInterpreter -minibatch=1 $PRE -topk=5 -dump-profile=deit_calib.yaml $Q > prof.log 2>&1 || { echo "profile failed (prof.log)"; exit 1; }
fi
cp deit_calib.yaml deit_calib_mc.yaml
for i in 1 2 3 4; do
  $B/glow/bin/model-compiler -g -model=deit_fold.onnx -backend=VTA -emit-bundle=$O -bundle-api=dynamic -network-name=deit \
    -model-input=pixel_values,float,[1,3,224,224] -load-profile=deit_calib_mc.yaml $Q "$@" > gen.log 2>&1
  r=$?
  h=$(grep -o "Current graph hash: 0x[0-9A-Fa-f]*" gen.log | awk '{print $4}')
  [ -z "$h" ] && break
  h=$(printf "0x%016X" $h)
  echo "graph hash mismatch (run $i): profile hash set to $h"
  sed -i "s/^GraphPreLowerHash: .*/GraphPreLowerHash: $h/" deit_calib_mc.yaml
done
echo "DeiTTest rc=$r"; exit $r
