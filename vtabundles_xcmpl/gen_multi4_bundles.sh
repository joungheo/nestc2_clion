#!/bin/sh
#### JO added (2026-11-06): generate the 4 copies of a multi-EVTA bundle set (vta/multievta_bundles/<Dir>: copy i runs on
#### EVTA i) with model-compiler of the simulator build, as their CMakeLists.txt say, without adding them to the NEST-C
#### CMake (a cmake re-run in Debug2_remote rebuilds all of Glow on a new day). Output in
#### Debug2_remote/vta/multievta_bundles/<Dir>, log gen_<i>.log there; then collect_hw_bundles.py --set multievtax4 <Dir>.
#### The single bundle (build_sim_bundles.sh) must exist: the model and its profile come from its build dir.
#### usage (board, or PC in the nestc-build container): vtabundles_xcmpl/gen_multi4_bundles.sh MultiEVTAResnet18Test MultiEVTAResnet50Test MultiEVTAVgg16Cifar10Test MultiEVTADeiTTest MultiEVTAMobileNetV2Test
set -u
B=$(cd "$(dirname "$0")/../Debug2_remote" && pwd)
export TVM_HOME=$B/../tvm PYTHONPATH=$B/../tvm/python TVM_LIBRARY_PATH=$B/tvm
MCB=$B/glow/bin/model-compiler
rc=0
for D in "$@"; do
  case $D in
    MultiEVTAResnet18Test) base=mxnet_exported_resnet18; S=$B/vta/multievta_bundles/$D
      set -- -model=$B/models/mxnet_exported_resnet18.onnx -model-input=data,float,[1,224,224,3] \
        -load-profile=$S/mxnet_exported_resnet18_calib_1.yaml ;;
    MultiEVTAResnet50Test) base=resnet50; S=$B/vta/bundles/Resnet50Test
      set -- -model=$S/resnet50.onnx -keep-original-precision-for-nodes=SoftMax \
        -model-input=gpu_0/data_0,float,[1,3,224,224] -load-profile=$S/resnet50_calib_2.yaml ;;
    MultiEVTAVgg16Cifar10Test) base=vgg16_cifar10; S=$B/vta/bundles/Vgg16Cifar10Test
      set -- -model=$S/vgg16_cifar10.onnx -model-input=input,float,[1,3,32,32] -load-profile=$S/vgg16_calib_1.yaml ;;
    #### JO added (2026-11-10): DeiT-tiny (the single bundle's folded model and profile, -vta-round-shift as gen_deit_bundle.sh)
    MultiEVTADeiTTest) base=deit; S=$B/vta/bundles/DeiTTest
      set -- -model=$S/deit_fold.onnx -model-input=pixel_values,float,[1,3,224,224] -load-profile=$S/deit_calib.yaml \
        -vta-round-shift ;;
    #### JO added (2026-11-22): MobileNetV2 (the single bundle's model; copy 0 named mobilenetv2 below)
    MultiEVTAMobileNetV2Test) base=mobilenetv2; S=$B/vta/bundles/MobileNetV2Test
      set -- -model=$S/mobilenetv2_qat_f32.onnx -model-input=input,float,[1,3,224,224] \
        -load-profile=$B/../vta/bundles/MobileNetV2Test/calib/mobilenetv2_calib_n200.yaml -enable-channelwise-depthwise -vta-round-shift -vta-evta-depthwise ;;
    *) echo "$D: unknown"; rc=1; continue ;;
  esac
  #### JO added (2026-11-22): MODEL=<onnx> / PROF=<profile> replace the model / profile, MCX=<options> adds model-compiler options
  #### (e.g. the torchvision ResNet-50 with its KL + power-of-two MSE profile: MODEL=$B/vta/bundles/Resnet50Test/tv/resnet50.onnx
  #### PROF=<yaml> MCX="-vta-round-shift -quantization-calibration=KL -quantization-pow2-mse=2")
  [ -n "${MODEL:-}" ] && set -- $(echo "$@" | sed "s|-model=[^ ]*|-model=$MODEL|")
  [ -n "${PROF:-}" ] && set -- $(echo "$@" | sed "s|-load-profile=[^ ]*|-load-profile=$PROF|")
  [ -n "${MCX:-}" ] && set -- "$@" $MCX
  O=$B/vta/multievta_bundles/$D
  mkdir -p $O && cd $O || exit 1
  # the single bundle's skip list (model-compiler reads ./VTASkipQuantizeNodes.txt)
  rm -f VTASkipQuantizeNodes.txt
  [ $D != MultiEVTAResnet18Test ] && [ -f $S/VTASkipQuantizeNodes.txt ] && cp $S/VTASkipQuantizeNodes.txt .
  for i in 0 1 2 3; do
    # if [ $i = 0 ]; then extra=""; else n=${base}_$i; extra="-index-multi-evta=$((1 << i)) -network-name=$n -main-entry-name=$n"; fi
    #### JO changed (2026-11-10): DeiT copy 0 is named deit (-network-name) as the single bundle
    if [ $i = 0 ]; then extra=""; [ $D = MultiEVTADeiTTest ] && extra="-network-name=deit"; [ $D = MultiEVTAMobileNetV2Test ] && extra="-network-name=mobilenetv2"
    else n=${base}_$i; extra="-index-multi-evta=$((1 << i)) -network-name=$n -main-entry-name=$n"; fi
    #### JO added (2026-11-10): DeiT's Glow graph hash is not stable: retry with the hash the failed run printed set in a copy
    #### of the profile (as gen_deit_bundle.sh), up to 6 times
    if [ $D = MultiEVTADeiTTest ]; then
      prof=$(echo "$@" | sed -n 's/.*-load-profile=\([^ ]*\).*/\1/p'); cp $prof calib_mc_$i.yaml
      args=$(echo "$@" | sed "s|-load-profile=[^ ]*|-load-profile=$O/calib_mc_$i.yaml|")
      for k in 1 2 3 4 5 6; do
        $MCB -g $args -backend=VTA -emit-bundle=$O -bundle-api=dynamic -quantization-schema=symmetric_with_power2_scale \
          $extra > gen_$i.log 2>&1
        r=$?
        h=$(grep -o "Current graph hash: 0x[0-9A-Fa-f]*" gen_$i.log | awk '{print $4}')
        [ -z "$h" ] && break
        h=$(printf "0x%016X" $h); sed -i "s/^GraphPreLowerHash: .*/GraphPreLowerHash: $h/" calib_mc_$i.yaml
      done
    else
    $MCB -g "$@" -backend=VTA -emit-bundle=$O -bundle-api=dynamic -quantization-schema=symmetric_with_power2_scale \
      $extra > gen_$i.log 2>&1
    r=$?
    fi
    echo "$D copy $i rc=$r"; [ $r = 0 ] || rc=1
  done
done
exit $rc
