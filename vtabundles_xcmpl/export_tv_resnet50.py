#!/usr/bin/env python3
#### JO added (2026-11-22, docs/evta_todo.md §5 DPU comparison): torchvision ResNet-50 (IMAGENET1K_V1, ImageNet top-1 76.13 %,
#### the float model of Vitis AI's pt_resnet50_imagenet_224_224_8.2G) as an ONNX for vta/bundles/Resnet50TvTest (Debug2_remote/vta/bundles/Resnet50TvTest/resnet50.onnx):
#### same input / output names and input as the Caffe2 resnet50.onnx (gpu_0/data_0: 1 x 3 x 224 x 224 NCHW, BGR, 0..1;
#### gpu_0/softmax_1: softmax of 1000 classes), so the mains / multi_model.h are Resnet50Test's.
#### BGR: conv1's weight input channels reversed (exact); torchvision's mean / std (RGB order, of 0..1) as Sub / Div on the input
#### in BGR order (VTASave -vta-fuse-preprocess folds them into the first conv's input copy).
#### usage (a venv with torch, torchvision, onnx): export_tv_resnet50.py <out.onnx> [opset, default 10]
import sys, numpy as np, torch, torchvision, onnx
from onnx import helper, numpy_helper, TensorProto

out = sys.argv[1]
opset = int(sys.argv[2]) if len(sys.argv) > 2 else 10
m = torchvision.models.resnet50(weights=torchvision.models.ResNet50_Weights.IMAGENET1K_V1).eval()
with torch.no_grad():
    m.conv1.weight.copy_(m.conv1.weight.flip(1))   # the network now reads BGR


class Net(torch.nn.Module):
    def __init__(self, net):
        super().__init__()
        self.net = net
        # BGR order
        self.register_buffer('mean', torch.tensor([0.406, 0.456, 0.485]).view(1, 3, 1, 1))
        self.register_buffer('std', torch.tensor([0.225, 0.224, 0.229]).view(1, 3, 1, 1))

    def forward(self, x):
        return torch.softmax(self.net((x - self.mean) / self.std), dim=1)


x = torch.rand(1, 3, 224, 224)
tmp = out + '.tmp'
torch.onnx.export(Net(m), x, tmp, input_names=['gpu_0/data_0'], output_names=['gpu_0/softmax_1'], opset_version=opset,
                  do_constant_folding=True)
g = onnx.load(tmp)
onnx.checker.check_model(g)
onnx.save(g, out)
import os
os.remove(tmp)
print(out, 'opset', opset, 'nodes', len(g.graph.node), sorted({n.op_type for n in g.graph.node}))
