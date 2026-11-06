#!/usr/bin/env python3
# NEST-C compiler figures for docs/evta_compiler_frontend.md, docs/evta_compiler_backend.md and docs/evta_stack_arch.md
# (docs/figs/evta_cc_NN_*.svg): the whole pipeline (profile pass, compile pass), the NEST-C overlay on Glow, one Resnet18
# block from ONNX to generated code, the power-of-two quantization, and the VTA backend's bundle writer.
# usage: python3 docs/figs/src/make_evta_compiler_figs.py [NN ...]   (no argument: every figure)
# Sources: tools/loader/Loader.cpp, lib/Optimizer/GraphOptimizer/GraphOptimizer.cpp (optimizeFunction),
# lib/Quantization/Base/Base.cpp, lib/Backends/VTA/VTA.cpp, VTASave.cpp, cmake/glow/**, an IR dump of Resnet18
# (model-compiler -dump-ir) and its generated bundle (vtabundles_xcmpl/hw/Resnet18Test).
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from svglib import Svg  # noqa: E402

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
FIGS = {}


def fig(num, name):
    def deco(f):
        FIGS[num] = (name, f)
        return f
    return deco


@fig(1, "pipeline")
def f1():
    s = Svg(1240, 1000, "그림 C1. 모델에서 보드 실행까지: 두 번의 컴파일러 실행과 bundle 빌드",
            ["① profile: float 모델을 VTAInterpreter로 실행해 텐서마다 min/max를 YAML로.  ② compile: 그 YAML로 int8 양자화 후 bundle.",
             "파랑 = Glow 공통 단계, 초록 = VTA backend의 hook, 주황 = 도구·파일, 보라 = 보드."])
    # left: profile pass
    s.box(40, 100, 330, 70, "cpu", title="모델 파일", lines=["ONNX (또는 Caffe2, TFLite)", "float32 weight"], size=12.5)
    s.box(40, 200, 330, 100, "cpu", title="① image-classifier -dump-profile", size=12, align="left", lines=[
        "-backend=VTAInterpreter (C++ 참조 실행)", "-quantization-schema=symmetric_with_power2_scale",
        "모든 노드를 lower하고 출력마다 profile 노드", "보정 이미지 1장 (cat_285.png)으로 실행"])
    s.box(40, 330, 330, 60, "cpu", title="<model>_calib.yaml", lines=["노드 출력 이름 → min, max, histogram"], size=12)
    s.arrow(205, 172, 205, 196, "o")
    s.arrow(205, 302, 205, 326, "o")
    # right: compile pass pipeline
    X, W = 420, 780
    steps = [
        ("cpu", "② model-compiler -backend=VTA -load-profile=… -emit-bundle=<dir>", "tools/loader/Loader.cpp"),
        ("evta", "importer: ONNXModelLoader → Glow Function (노드 그래프, NHWC, float)", "lib/Importer (NEST-C 사본)"),
        ("evta", "optimizeFunctionBeforeLowering: 상수 접기, BatchNorm을 conv에, transpose 정리 …", "GraphOptimizer.cpp"),
        ("evta", "lower(F, VTA): VTA::shouldLower = !isOpSupported  (conv·pool·fc·add는 그대로)", "Glow Lower.cpp"),
        ("evta", "transformForPrecisionMode: quantizeFunction (YAML의 범위 → int8 type, power-of-two scale)", "lib/Quantization"),
        ("evta", "optimize, foldActivations (conv + relu / clip → FusedActivation), lower, optimize", "GraphOptimizer.cpp"),
        ("new", "VTA::transformPostLowering (channelwise bias, NESTC_EVTA_GRAPH_OPT면 VTAConv)", "lib/Backends/VTA/VTA.cpp"),
        ("evta", "convertQuantizedConstants, optimize, VTA::verify", "GraphOptimizer.cpp"),
        ("new", "VTA::save → generateAndOptimizeIR (IRGen + IR optimizer: alloc/dealloc, 버퍼 공유)", "VTASave.cpp, Glow IR"),
        ("new", "VTASave: 분석 pass 5개 → 명령마다 C++ 문장 (그림 C5)", "VTASave.cpp"),
    ]
    y = 100
    prev = None
    for k, t, src in steps:
        s.box(X, y, W, 52, k, lines=[(t, "bold"), src], size=12, align="left")
        if prev is not None:
            s.arrow(X + 40, prev + 52, X + 40, y - 2, "b")
        prev = y
        y += 70
    s.path(f"M372,360 L395,360 L395,{100 + 70 * 4 + 26} L{X - 4},{100 + 70 * 4 + 26}", "o", arrow="o")
    s.text(376, 352, "범위", 12, color="#c06a10")
    # outputs
    oy = y + 4
    s.box(X, oy, W, 80, "cpu", title="bundle 디렉터리 (생성물)", size=12, align="left", lines=[
        "<net>.cpp: load_module / 추론 함수 / destroy_module       <net>.h: 심볼 표, 메모리 크기",
        "<net>.weights.bin: EVTA layout으로 바꾼 weight·bias       VTARuntime.h: vtaCmdH, initVTARuntime()"])
    s.arrow(X + 40, prev + 52, X + 40, oy - 2, "b")
    s.box(40, oy, 330, 80, "cpu", title="손으로 쓴 main + CMake", size=12, align="left", lines=[
        "vta/bundles/<Test>/*Main.cpp", "vtabundles_xcmpl/ (보드용 독립 CMake)"])
    by = oy + 110
    s.box(40, by, 1160, 70, "hls", title="보드: collect_hw_bundles.py → make (vtalib + bundle + main) → run_all.sh / run_prof.sh", size=12.5,
          lines=["실행 파일 = main + <net>.cpp + libVTABundle (vtalib, 그림 1~6) ; 실행 시 <net>.weights.bin 을 읽음"])
    s.arrow(X + 390, oy + 82, X + 390, by - 2, "a")
    s.arrow(205, oy + 82, 205, by - 2, "a")
    return s


@fig(2, "overlay")
def f2():
    s = Svg(1240, 520, "그림 C2. NEST-C = Glow submodule + 덮어쓰기 (overlay)",
            ["Glow 저장소는 그대로 두고, cmake 실행 때 NEST-C 파일로 바꿔 끼운다. 파일을 고칠 때는 어느 쪽이 실제로 빌드되는지 먼저 본다."])
    s.box(40, 100, 540, 380, "mem", title="glow/ (git submodule, pytorch/glow)", size=12.5, tsize=14, align="left", lines=[
        "include/glow/…   lib/Graph, lib/IR, lib/Optimizer/Lower,",
        "lib/Optimizer/IROptimizer, lib/Backends/{CPU, Interpreter},",
        "lib/Importer/{Caffe2, TFLite}ModelLoader.cpp, lib/Runtime …",
        "", "그대로 쓰는 것:", "  Lower.cpp (노드 → 저수준 노드), IRGen, IR optimizer,",
        "  Caffe2 / TFLite importer, HostManager 기본 틀", "",
        "cmake 때 사라지는 것 (NEST-C 헤더로 대체):",
        "  tools/loader/Loader.h, Quantization/Base/Base.h, Base/Image.h", "",
        "CMakeLists.txt 전부: cmake/glow/** 로 덮어씀",
        "  → glow/**/CMakeLists.txt를 고치면 다음 cmake 때 사라진다"])
    s.box(660, 100, 540, 380, "new", title="NEST-C 저장소 (덮어쓰는 쪽)", size=12.5, tsize=14, align="left", lines=[
        "cmake/glow/**/CMakeLists.txt → glow/ 위로 복사 (copy_directory)",
        "include/glow/** : 같은 이름 헤더를 먼저 찾게 (BEFORE)",
        "  Graph.h, Nodes.h, Tensor.h, Type.h, Flags.h, Partitioner …", "",
        "Glow target에 끼워 넣는 NEST-C 소스:",
        "  lib/Importer/ONNXModelLoader.cpp  (-npu-conv-fitting 등)",
        "  lib/Optimizer/GraphOptimizer/GraphOptimizer.cpp (BNN pass 등)",
        "  lib/Quantization/Quantization.cpp, Base/Base.cpp (power-of-two)",
        "  lib/Converter/FunctionConverter.cpp (VTASkipQuantizeNodes.txt)",
        "  lib/Graph/{Graph,Nodes}.cpp, lib/Base/Tensor.cpp, Image.cpp",
        "  lib/Partitioner/NestPartitioner*.cpp, Runtime/{HostManager,Provisioner}",
        "  lib/LLVMIRCodeGen/*, lib/Interpreter/* (CPU, Interpreter backend)", "",
        "NEST-C 고유: lib/Backends/{VTA, VTAInterpreter, Relay, …}",
        "  tools/loader/{Loader, ModelPartitionTuner, …}.cpp",
        "  vta/vtalib (runtime, submodule), vta/bundles (예제 모델)"])
    s.arrow(658, 200, 584, 200, "g")
    s.text(600, 190, "덮어씀", 12, color="#3c8a35")
    return s


@fig(3, "resnet_block")
def f3():
    s = Svg(1240, 900, "그림 C3. Resnet18 stage1 block 하나가 단계마다 어떻게 바뀌나",
            ["실제 IR(model-compiler -dump-ir)과 생성 코드에서 옮겼다. scale은 2의 거듭제곱: S:0.03125 = 1/32.",
             "conv → relu → conv → add → relu 다섯 노드가 EVTA call 두 개가 된다."])
    cols = [(40, "ONNX / Glow graph (float)"), (330, "양자화 + fold 뒤 graph (int8)"), (640, "IR (명령 열, IRGen)"),
            (940, "bundle 코드 (VTASave)")]
    for x, t in cols:
        s.text(x, 112, t, 14, weight="bold", color="#2f5f98")
    # column 1
    c1 = [("Conv 3×3 (64→64)", 140), ("BatchNorm", 200), ("Relu", 260), ("Conv 3×3", 320), ("BatchNorm", 380),
          ("Add (+ block 입력)", 440), ("Relu", 500)]
    for t, y in c1:
        s.box(40, y, 250, 44, "cpu" if "Batch" not in t else "mem", lines=[t], size=12.5)
    for a, b in zip(c1, c1[1:]):
        s.arrow(165, a[1] + 46, 165, b[1] - 2, "a")
    s.text(40, 568, "ONNX는 NCHW: importer가 conv 앞뒤에", 11.5, color="#555555")
    s.text(40, 584, "Transpose를 넣어 NHWC로 (Glow conv 기본)", 11.5, color="#555555")
    # column 2
    c2 = [("Conv i8 + FusedActivation RELU", 140, "BN은 weight·bias로 접힘"), ("Conv i8 (활성화 없음)", 250, ""),
          ("Add i8", 340, ""), ("Relu i8", 410, "")]
    for t, y, n in c2:
        s.box(330, y, 270, 50 if not n else 70, "evta", lines=[t] + ([n] if n else []), size=12)
    s.arrow(465, 212, 465, 246, "a")
    s.arrow(465, 302, 465, 336, "a")
    s.arrow(465, 392, 465, 406, "a")
    s.box(330, 480, 270, 120, "note", size=11.5, align="left", lines=[
        "type: i8[S:1/32]<1×56×56×64> 등", "input  S = 1/32 (pool0)", "weight S = 1/128, bias i32 S = 1/4096",
        "conv0 out S = 1/32, conv1 out S = 1/16", "foldActivations: conv 바로 뒤 relu만", "(add 뒤 relu는 그대로)"])
    # column 3 IR
    ir = ["14 alloc  conv0_res  i8<1×56×56×64>", "15 convolution  conv0_res ← pool0_res, w, b",
          "     FusedActivation: RELU", "16 alloc  plus1_res", "17 convolution  plus1_res ← conv0_res, w, b",
          "18 dealloc conv0_res", "19 elementadd  plus1_res ← pool0_res + plus1_res", "20 dealloc pool0_res",
          "21 relu  plus1_res ← plus1_res"]
    s.box(640, 140, 280, 300, "mem", size=11, align="left", lines=ir, lh=28)
    s.box(640, 470, 280, 130, "note", size=11.5, align="left", lines=[
        "IR = 버퍼(WeightVar, alloc) + 명령", "in-place: add와 relu가 같은 버퍼", "dealloc 위치 = 수명 끝",
        "이 IR을 VTASave가 위에서", "아래로 한 번 읽으며 코드를 쓴다"])
    # column 4 code
    code = ["// 15: EVTA conv + bias + relu, shift 7", "convolution_wo_tr(in, w0, b0, conv0,",
            "  1,56,56,64, 64,3,3, 1,1, relu 1,bias 1,", "  shift 7, 56,56, vtaCmdH, VT 2, 8,56, EVTA 0);", "",
            "// 17 + 19 + 21: conv + add + relu", "convolution_wo_tr_res(conv0, w1, b1, out,",
            "  …, shift 7, …, residual=pool0_vbuf,", "  convShl 1, resShl 0, sumShr 1, relu 1, …);",
            "// 19, 21: 'done by the EVTA conv before'"]
    s.box(940, 140, 270, 300, "cpu", size=10.5, align="left", lines=code, lh=27, )
    s.box(940, 470, 270, 130, "note", size=11.5, align="left", lines=[
        "activation은 EVTA layout 그대로", "(VTABufferAlloc, transpose 없음)", "shift = log2(out / (in·w))",
        "  = log2((1/32)/(1/32·1/128)) = 7", "add: conv 쪽과 residual 쪽 scale을", "  shift로 맞춰 EVTA 안에서 더함"])
    for x in (300, 610, 925):
        s.arrow(x, 300, x + 24, 300, "b")
    # bottom: legend of what happened
    s.box(40, 630, 1170, 240, "note", size=12.5, align="left", title="단계별로 일어난 일", lines=[
        "1. importer (ONNXModelLoader): ONNX Conv / BatchNormalization / Relu / Add → Glow ConvolutionNode, BatchNormalizationNode, ReluNode,",
        "   AddNode. layout은 NHWC, ONNX의 NCHW 입력·weight 앞뒤에 TransposeNode (weight 쪽은 상수라 접힘).",
        "2. optimizeFunctionBeforeLowering: BatchNorm을 앞 conv의 weight·bias로 접고(OptimizeBatchNorm), 연속된 transpose를 없앤다.",
        "3. lower: VTA가 지원하는 노드(Convolution, Add, Relu, MaxPool, AvgPool, FullyConnected …)는 남기고 나머지만 기본 노드로 쪼갠다.",
        "4. quantizeFunction: profile의 min/max로 텐서마다 int8 type(scale = 2^ceil(log2(max/127)), offset 0). bias는 i32, scale = in·w.",
        "   float → int8 경계에 Quantize / Dequantize 노드. 모델 입력의 Quantize는 나중에 conv0 입력 복사로 합쳐진다.",
        "5. foldActivations: conv 바로 뒤의 relu(와 Clip(0, x))를 conv의 FusedActivation으로. add 뒤의 relu는 남는다.",
        "6. IRGen: 노드 → 명령, 텐서 → alloc/dealloc된 버퍼. IR optimizer가 in-place와 버퍼 공유를 정한다.",
        "7. VTASave: 분석 pass가 '이 activation은 EVTA layout으로 둔다'(findPackedActivations), '17·19·21은 call 하나로'",
        "   (findResidualFusions)를 정한 뒤, 명령마다 C++ 문장을 쓴다. 지운 명령 자리에는 주석만 남긴다.",
        "8. weight는 이 단계에서 EVTA layout(16×16 블록)으로 바꿔 <net>.weights.bin에 쓰고, load_module이 CMA로 올린다."])
    return s


@fig(4, "quantization")
def f4():
    s = Svg(1240, 640, "그림 C4. power-of-two 양자화와 EVTA의 정수 연산",
            ["EVTA는 곱셈 없이 shift만으로 scale을 맞춘다. 그래서 모든 scale이 2의 거듭제곱이고 offset은 0이어야 한다 (symmetric_with_power2_scale)."])
    s.box(40, 100, 560, 190, "evta", title="텐서 하나의 type (lib/Quantization/Base/Base.cpp)", size=12.5, tsize=13.5, align="left", lines=[
        "profile: 노드 출력의 min, max (보정 이미지로 실행)",
        "symmetric: r = max(|min|, |max|),  s = r / 127",
        "power-of-two: s = 2^ceil(log2 s)  (NEST-C: int8이면 지수 ≥ −7로 제한)",
        "offset = 0,  q = clamp(round(x / s), −128, 127)",
        "",
        "예) r = 3.9 → s = 3.9/127 = 0.0307 → 2^-5 = 1/32 → 표현 범위 [−4, 3.97]",
        "    r = 0.05 → s ≈ 2^-11 이 맞지만 2^-7로 제한 → 6단계 정도만 사용"])
    s.box(640, 100, 560, 190, "new", title="conv 하나의 정수 연산 (EVTA)", size=12.5, tsize=13.5, align="left", lines=[
        "acc(int32) = Σ q_in · q_w  + q_bias       (bias scale = s_in · s_w)",
        "q_out = clip( acc >> shift , relu ? 0 : −128, 127 or clip hi )",
        "shift = log2( s_out / (s_in · s_w) )   ← 정수여야 함",
        "",
        "GEMM: int8 × int8 → int32 누적 (16×16)",
        "ALU 또는 _rq bitstream의 requantize: >> shift, round, clip",
        "residual add: 두 입력의 scale 차이도 2^k라 shift로 맞춤"])
    s.box(40, 310, 1160, 120, "cpu", title="흐름", size=12.5, tsize=13.5, align="left", lines=[
        "① image-classifier -dump-profile=x.yaml -quantization-schema=symmetric_with_power2_scale  (float 실행, VTAInterpreter)",
        "② model-compiler -load-profile=x.yaml -quantization-schema=symmetric_with_power2_scale  (int8 graph → bundle)",
        "   ② 의 graph hash가 ① 과 같아야 한다 (같은 모델·같은 옵션).  VTASkipQuantizeNodes.txt 에 적은 노드는 float로 남긴다.",
        "   -keep-original-precision-for-nodes=SoftMax 처럼 종류 단위로도 남길 수 있다.  depthwise가 있는 모델은 ①에 -do-not-lower-nodes-for-profiling=Convolution"])
    s.box(40, 450, 1160, 130, "note", title="한계 (측정, docs/evta_runtime_compare.md §7.19, MobileNetV2 2026-11-06)", size=12.5, tsize=13.5, align="left", lines=[
        "per-tensor라 채널마다 범위가 크게 다른 weight(BatchNorm을 접은 depthwise)에서 정확도가 무너진다: MobileNetV2 cat 282 → 293.",
        "같은 모델을 depthwise weight만 per-channel power-of-two로 하면 맞다 (Glow -enable-channelwise, Interpreter에서 282 / 208 / 341).",
        "그러나 EVTA conv 경로는 per-channel scale을 받지 않는다 (shift가 명령 하나에 하나). depthwise는 지금 어차피 CPU 경로.",
        "보정은 이미지 1장. 정확도 평가 셋은 아직 없다 (cat / dog / zebra 3장으로 확인)."])
    return s


@fig(5, "vtasave")
def f5():
    s = Svg(1240, 1110, "그림 C5. VTA backend의 bundle 작성기 (VTA::save, lib/Backends/VTA/VTASave.cpp)",
            ["IR을 받아 ① 전체를 한 번씩 훑는 분석 pass 5개로 결정을 모으고, ② 명령을 위에서 아래로 한 번 읽으며 C++ 문장을 쓴다."])
    s.box(40, 100, 300, 70, "mem", title="IR (generateAndOptimizeIR)", lines=["명령 열 + 버퍼 + 상수"], size=12)
    passes = [("findPackedActivations", "-vta-keep-layout", "모든 사용자가 EVTA conv / elemwise / pool인 activation은 EVTA layout(N·C/16·H·W·16)으로, VTA buffer에"),
              ("findResidualFusions", "-vta-fuse-residual", "EVTA conv → elementadd → relu 를 convolution_wo_tr_res 하나로 (scale 비 2^k)"),
              ("findEvtaMaxPools", "-vta-evta-maxpool", "입력 ≥ 0(relu 뒤)이고 EVTA layout인 maxpool 은 maxpool_evta"),
              ("findInputPreprocess", "-vta-fuse-preprocess", "모델 입력의 quantize / 채널별 sub·div / NCHW→NHWC 를 conv0 입력 복사 하나로"),
              ("findPoolFusions", "-vta-fuse-pool", "packed conv0 바로 뒤 3×3/2 maxpool 을 convolution_wo_tr_pool 하나로")]
    y = 200
    for n, opt, d in passes:
        s.box(40, y, 1160, 50, "new", lines=[(f"{n}   ({opt}, 기본 켜짐)", "bold"), d], size=12, align="left")
        y += 60
    s.arrow(190, 172, 190, 196, "a")
    y += 10
    s.text(40, y + 14, "② 명령마다 (switch on kind)", 14, weight="bold", color="#2f5f98")
    y += 30
    rows = [
        ("convolution", "getConvPath → 아래 결정 나무.  VTASetActivationClip (ReLU6)"),
        ("allocactivation / dealloc", "packed면 VTABufferAlloc / VTABufferFree,  아니면 malloc / free"),
        ("elementadd, relu", "fusion으로 지워졌으면 주석만.  아니면 CPU 함수 (packed 그대로 elementwise)"),
        ("maxpool, avgpool", "maxpool_evta 또는 CPU (16채널 블록을 batch로)"),
        ("fullyconnected", "fullyconnected_evta_packed (weight를 컴파일 때 EVTA layout으로) 또는 CPU fc"),
        ("quantize / transpose / elemsub / elemdiv", "모델 입력 쪽이면 preprocess_* 하나로 합쳐짐, 아니면 CPU 함수"),
        ("그 밖 (softmax, splat, tensorview …)", "CPU 함수 (CPUBundle_generic.cpp)"),
    ]
    for k, d in rows:
        s.box(40, y, 330, 36, "evta", lines=[k], size=12)
        s.box(380, y, 820, 36, "note", lines=[d], size=12, align="left")
        y += 42
    y += 10
    # decision tree for conv
    s.text(40, y + 14, "getConvPath (conv 하나가 어디로 가나)", 14, weight="bold", color="#2f5f98")
    y += 28
    s.box(40, y, 240, 60, "mem", lines=[("양자화된 conv?", "bold"), "아니면 Float (CPU)"], size=12)
    s.box(310, y, 280, 60, "new", lines=[("VTA", "bold"), "group 1, C·KN % 16 = 0, pad 대칭,", "offset 0, shift ≥ 0"], size=11.5)
    s.box(610, y, 280, 60, "new", lines=[("FirstBNN (conv0)", "bold"), "C = 3, KN % 16 = 0: conv0pack 또는", "16채널 0 채움 → EVTA"], size=11.5)
    s.box(910, y, 290, 60, "cpu", lines=[("DepthwiseCPU / Other", "bold"), "group = C (depthwise), 채널 % 16 ≠ 0,", "비대칭 pad → nonvtaconvolution (CPU)"], size=11.5)
    s.arrow(282, y + 30, 306, y + 30, "a")
    s.arrow(592, y + 30, 606, y + 30, "a")
    s.arrow(892, y + 30, 906, y + 30, "a")
    y += 80
    s.box(40, y, 1160, 110, "note", size=12, align="left", title="EVTA conv 문장 (generateVTAConvolutionCall)", lines=[
        "weight·bias를 EVTA layout으로 바꿔 weights.bin에 (prepareVTAConvolutionInst), schedule = VTASchedules.h 표 (shape 일치) 또는 VT 1 / 8×8",
        "입력이 packed가 아니면 transpose_nhwc2vtaio + Flush, 출력이 packed가 아니면 Invalidate + transpose_vtaio2nhwc",
        "Resnet18: conv 20개 모두 VTA/FirstBNN, MobileNetV2: 53개 중 23개가 CPU (depthwise 17, 24채널 4, 비대칭 pad conv0, 마지막 1001채널)"])
    return s


def main():
    nums = [int(a) for a in sys.argv[1:]] or sorted(FIGS)
    for n in nums:
        name, f = FIGS[n]
        path = os.path.join(OUT, f"evta_cc_{n:02d}_{name}.svg")
        f().save(path)
        print(path)


if __name__ == "__main__":
    main()
