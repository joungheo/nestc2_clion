#!/bin/bash
#### JO added (2026-11-22, docs/evta_accuracy.md §6): a CNN bundle with the calibration profile made from many images instead of the
#### one cat_285.png of its CMakeLists.txt (VGG16 also with the input normalization it was trained with, add_input_norm_onnx.py).
#### Same image-classifier / model-compiler flags as the CMakeLists.txt, plus -image-channel-order as the bundle main feeds the
#### model (the CMakeLists used Glow's default BGR for Resnet18 / VGG16, whose mains feed RGB) and -minibatch=1 (one image per run).
#### Run where Debug2_remote's tools run (PC: inside the nestc-build container; or the board). Output into
#### Debug2_remote/vta/bundles/<Dir> as the CMake rules write it (then collect_hw_bundles.py <Dir>), logs prof.log / gen.log there.
#### usage: gen_calib_bundle.sh <Resnet18Test|Resnet50Test|MobileNetV2Test|Vgg16Cifar10Test> <image list file | profile.yaml> [model-compiler options]
####   a .yaml: that profile, no image-classifier run (the profiles of 2026-11-22 are in vta/bundles/<Dir>/calib/, e.g.
####     gen_calib_bundle.sh Resnet50Test ../vta/bundles/Resnet50Test/calib/resnet50_calib_n200.yaml)
####   an image list: one PNG per line, absolute or relative to IMGDIR=<dir> (imagenet_eval/calib_imagenetv2_200.txt:
####     every 50th ImageNetV2 list.txt image, 2 % of the 10,000 the accuracy is measured on; calib_cifar10_train100.txt:
####     the first 100 CIFAR-10 training images, prep_cifar10.py train 100)
#### -vta-round-shift is always on here (EVTA conv shifts round as Glow's Interpreter; the first conv's bias got the rounding
#### term only since 2026-11-22: docs/evta_accuracy.md §6).
#### KL + power-of-two MSE (2026-11-22, docs/evta_accuracy.md §7; the DPU comparison bundles): BINS=2048 when profiling, then
####   gen_calib_bundle.sh <Dir> <profile> -quantization-calibration=KL -quantization-pow2-mse=2 (R50=tv for the torchvision ResNet-50)
####   e.g. R50=tv gen_calib_bundle.sh Resnet50Test ../vta/bundles/Resnet50Test/calib/resnet50_tv_calib_n200_b2048.yaml.gz \
####          -quantization-calibration=KL -quantization-pow2-mse=2
####   BINS=<n>: -num-histogram-bins (Glow's default 10 is too few for KL: it needs > 255 bins, else no calibration happens).
####   QSCHEMA=<schema> (default symmetric_with_power2_scale), CALIB=<suffix> names the profile <net>_calib_<suffix>.yaml (default "n"); QCAL=KL adds -quantization-calibration=KL.
set -u
D=$1; L=$2; shift 2
H=$(cd "$(dirname "$0")" && pwd)
B=$(cd "$H/../Debug2_remote" && pwd)
O=$B/vta/bundles/$D
case $L in
  *.yaml|*.yaml.gz) IMGS="" ;;
  *) IMGS=$(awk -v d="${IMGDIR:-}" '{ print (d != "" && substr($1, 1, 1) != "/") ? d "/" $1 : $1 }' "$L") ;;
esac
#### QSCHEMA=symmetric: free scales, VTA.cpp -vta-pow2-ratio (default on) keeps every EVTA ratio in * w / out a power of two (as DeiT)
Q=-quantization-schema=${QSCHEMA:-symmetric_with_power2_scale}
QC=${QCAL:+-quantization-calibration=$QCAL}
case $D in
  Resnet18Test)
    NET=mxnet_exported_resnet18; M=$B/models/mxnet_exported_resnet18.onnx; IN=data; SHAPE=[1,224,224,3]
    PF="-image-layout=NHWC -image-mode=0to255 -image-channel-order=RGB"; MC="-vta-round-shift" ;;
  Resnet50Test)
    #### R50=tv: torchvision ResNet-50 (export_tv_resnet50.py -> $O/tv/resnet50.onnx; same input / output as the Caffe2 one)
    NET=resnet50; M=$O/${R50:+$R50/}resnet50.onnx; IN=gpu_0/data_0; SHAPE=[1,3,224,224]
    PF="-image-layout=NCHW -image-mode=0to1 -image-channel-order=BGR"; MC="-vta-round-shift" ;;
  MobileNetV2Test)
    NET=mobilenetv2; M=$O/mobilenetv2_qat_f32.onnx; IN=input; SHAPE=[1,3,224,224]
    PF="-do-not-lower-nodes-for-profiling=Convolution -image-layout=NCHW -image-mode=neg1to1 -image-channel-order=RGB"
    MC="-network-name=mobilenetv2 -enable-channelwise-depthwise -vta-round-shift -vta-evta-depthwise" ;;
  Vgg16Cifar10Test)
    NET=vgg16_cifar10; IN=input; SHAPE=[1,3,32,32]
    # CIFAR-10 mean / std on the 0..1 RGB input the main feeds (float top-1 30.07 -> 86.58 %)
    mkdir -p $O/norm
    [ -s $O/norm/vgg16_cifar10.onnx ] || python3 $H/add_input_norm_onnx.py $O/vgg16_cifar10.onnx $O/norm/vgg16_cifar10.onnx \
      0.4914,0.4822,0.4465 0.2470,0.2435,0.2616 || exit 1
    M=$O/norm/vgg16_cifar10.onnx
    PF="-image-layout=NCHW -image-mode=0to1 -image-channel-order=RGB"; MC="-network-name=vgg16_cifar10 -vta-round-shift" ;;
  *) echo "unknown $D"; exit 2 ;;
esac
P=$O/${NET}${R50:+_$R50}_calib_${CALIB:-n}${QCAL:+_$QCAL}.yaml
case $L in *.yaml) P=$(readlink -f "$L") ;; esac
#### JO added (2026-11-22): a gzipped profile (the 2048-bin ones in vta/bundles/<Dir>/calib/) is unpacked next to the bundle
case $L in *.yaml.gz) P=$O/$(basename "$L" .gz); gzip -dc "$L" > $P || exit 1 ;; esac
cd $O
if [ -n "$IMGS" ]; then
  echo "$D: $(echo $IMGS | wc -w) images -> $P"
  $B/glow/bin/image-classifier $IMGS -m=$M -model-input-name=$IN -backend=VTAInterpreter -minibatch=1 $PF -compute-softmax -topk=1 \
    -dump-profile=$P $Q ${BINS:+-num-histogram-bins=$BINS} > prof.log 2>&1 || { echo "profile failed ($O/prof.log)"; exit 1; }
else
  echo "$D: profile $P"
fi
# model-compiler reads ./VTASkipQuantizeNodes.txt (a skip-quant ON build may have left one)
rm -f VTASkipQuantizeNodes.txt
$B/glow/bin/model-compiler -g -model=$M -backend=VTA -emit-bundle=$O -bundle-api=dynamic -model-input=$IN,float,$SHAPE \
  -load-profile=$P $Q $QC $MC "$@" > gen.log 2>&1
r=$?; echo "$D rc=$r"
grep "^VTA_FUSION_LOG [0-9 ]" gen.log | sed 's/^VTA_FUSION_LOG /  /'
exit $r
