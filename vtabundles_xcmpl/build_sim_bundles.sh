#!/bin/bash
#### JO (2026-09-30; moved here from Debug2_remote/ 2026-11-21 so that it is in git): build (generate + compile) vta/bundles
#### targets in the simulator build Debug2_remote, one target at a time so a failure does not stop the rest. Relay/TVM and BNN
#### bundles excluded. Outputs land in Debug2_remote/vta/bundles/<Dir>, from where collect_hw_bundles.py copies them to
#### vtabundles_xcmpl/hw/<Dir>. Log per target: Debug2_remote/bundle_logs/<target>.log; summary: bundle_logs/summary.txt.
#### A target's make is quiet until it ends (on the board ~2 min for a model, ~1 h if cmake re-runs: Glow is rebuilt);
#### follow it with tail -f on the log printed at its start. The model-compiler summary of what runs where (VTA_FUSION_LOG
#### lines, docs/evta_fusion_rules.md) is printed after each model target.
#### usage (from anywhere): vtabundles_xcmpl/build_sim_bundles.sh [target ...]   (Debug2_remote/build_sim_bundles.sh calls this)
set -u
H=$(cd "$(dirname "$0")" && pwd)
B=${NESTC_SIM_BUILD:-$(cd "$H/../Debug2_remote" && pwd)}
LOG=$B/bundle_logs; mkdir -p $LOG
export TVM_HOME=$B/../tvm PYTHONPATH=$B/../tvm/python TVM_LIBRARY_PATH=$B/tvm

TARGETS=${@:-
vtaConvTestBundle vtaConv2TestBundle vtaConv3TestBundle vtaConv4TestBundle vtaConv5TestBundle
vtaResConv1TestBundle vtaResConv2TestBundle vtaResConv3TestBundle vtaResConv4TestBundle
vtaResConv5TestBundle vtaResConv6TestBundle vtaResConv7TestBundle vtaResConv8TestBundle
vtaResConv9TestBundle vtaResConv10TestBundle Conv_inception_5b_5x5_1__1Bundle
vtaFloatConvTestBundle vtaResnetv10S1C0TestBundle vtaAvgPoolTestBundle
vtaElemAddTestBundle vtaElemSubTestBundle vtaElemDivTestBundle vtaFCTestBundle
vtaMaxPoolTestBundle vtaReluTestBundle vtaSoftMaxTestBundle vtaSplatMaxTestBundle
vtaTransposeTestBundle vtaQuantizeTestBundle vtaDequantizeTestBundle vtaNonVTAConvTestBundle
vtaMxnetResnet18Bundle vtaMxnetResnet18PartitionBundle vtaVGG16Cifar10Bundle vtaCaffe2Resnet50Bundle}

cd $B
for t in $TARGETS; do
  # The S3 calibration download for the partition bundle is gone: reuse Resnet18Test's profile.
  if [ $t = vtaMxnetResnet18PartitionBundle ]; then
    src=$B/vta/bundles/Resnet18Test/mxnet_exported_resnet18_calib_1.yaml
    dst=$B/vta/bundles/Resnet18PartitionTest/mxnet_exported_resnet18_calib_1.yaml
    [ -f $src ] && [ ! -f $dst ] && cp -p $src $dst
  fi
  echo "$t: make -j2 (log: $LOG/$t.log)"
  s=$(date +%s)
  make -j2 $t > $LOG/$t.log 2>&1; rc=$?
  printf "%-40s %s %5ds\n" $t $([ $rc = 0 ] && echo OK || echo "FAIL(rc=$rc)") $(( $(date +%s) - s )) | tee -a $LOG/summary.txt
  grep "^VTA_FUSION_LOG [0-9 ]" $LOG/$t.log | sed 's/^VTA_FUSION_LOG /  /'
done
echo "== done $(date)" >> $LOG/summary.txt
