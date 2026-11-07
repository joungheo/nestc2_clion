#!/bin/sh
#### JO added (2026-11-06): generate vta/bundles/MobileNetV2Test (profile + VTA bundle) with the simulator build's
#### image-classifier / model-compiler, as its CMakeLists.txt says, without a cmake re-run of Debug2_remote (a new day
#### rebuilds all of Glow). The model Debug2_remote/vta/bundles/MobileNetV2Test/mobilenetv2_qat_f32.onnx comes from
#### tfl2onnx.py (see that CMakeLists.txt). Output and gen.log / prof.log there; then collect_hw_bundles.py MobileNetV2Test.
#### Profiling lowers every node; a depthwise conv (group = C) lowered is C convs + slices + concat, thousands of nodes, and
#### the profile took hours (O(n^2) graph verify, LLVM register allocation): -do-not-lower-nodes-for-profiling=Convolution (27 s).
#### 2026-11-07: -enable-channelwise-depthwise (per-channel power-of-two scales for the depthwise filters; with one scale per
#### tensor cat_285.png is 293 instead of 282), the depthwise convs as vtalib cw_convolution (NEON) on the CPU.
#### usage (on the board): vtabundles_xcmpl/gen_mobilenetv2_bundle.sh [model-compiler options, e.g. -vta-conv0-pack=0]
set -u
B=$(cd "$(dirname "$0")/../Debug2_remote" && pwd)
export TVM_HOME=$B/../tvm PYTHONPATH=$B/../tvm/python TVM_LIBRARY_PATH=$B/tvm
O=$B/vta/bundles/MobileNetV2Test
M=$O/mobilenetv2_qat_f32.onnx
Q=-quantization-schema=symmetric_with_power2_scale
cd $O || exit 1
[ -f $M ] || { echo "$M missing"; exit 1; }
if [ ! -s mobilenetv2_calib.yaml ]; then
  $B/glow/bin/image-classifier $B/../glow/tests/images/imagenet/cat_285.png -m=$M -model-input-name=input \
    -backend=VTAInterpreter -do-not-lower-nodes-for-profiling=Convolution -image-layout=NCHW -image-mode=neg1to1 -image-channel-order=RGB -compute-softmax -topk=5 \
    -dump-profile=mobilenetv2_calib.yaml $Q > prof.log 2>&1 || { echo "profile failed (prof.log)"; exit 1; }
fi
$B/glow/bin/model-compiler -g -model=$M -backend=VTA -emit-bundle=$O -bundle-api=dynamic -network-name=mobilenetv2 \
  -model-input=input,float,[1,3,224,224] -load-profile=mobilenetv2_calib.yaml $Q -enable-channelwise-depthwise "$@" > gen.log 2>&1
r=$?; echo "MobileNetV2Test rc=$r"; exit $r
