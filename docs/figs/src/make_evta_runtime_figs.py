#!/usr/bin/env python3
# EVTA runtime / driver figures for docs/evta_runtime_arch.md (docs/figs/evta_rt_NN_*.svg): the software layers from the
# generated bundle down to the registers, the programming model the runtime drives, one EVTA call in time, the
# instructions of one conv tile, one inference on the CPU and the EVTA, and the multi-EVTA / memory layout.
# usage: python3 docs/figs/src/make_evta_runtime_figs.py [NN ...]   (no argument: every figure)
# Sources: vtalib-zcu102-systolic (lib/zcu102/pynq_driver.cc, lib/zcu102/api.cc, include/zcu102/cmd_queue.h,
# lib/Bundle/VTABundle.cpp), a generated bundle (vtabundles_xcmpl/hw/Resnet18Test), docs/evta_runtime_compare.md §7.
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from svglib import Svg, C  # noqa: E402

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
FIGS = {}


def fig(num, name):
    def deco(f):
        FIGS[num] = (name, f)
        return f
    return deco


@fig(1, "layers")
def f1():
    s = Svg(1240, 1010, "그림 1. EVTA software 계층: 생성된 bundle부터 레지스터까지",
            ["위에서 아래로 호출한다. 파일은 vtalib-zcu102-systolic 기준 (vta/vtalib도 같은 구조).",
             "주황 = CPU에서 도는 코드, 파랑 = EVTA를 움직이는 코드, 회색 = 메모리, 보라 = 하드웨어."])
    X, W = 40, 760
    rows = [
        ("cpu", "application main (예: mxnet_exported_resnet18BundleMain.cpp, multi_threads_main.cpp)",
         ["PNG 읽기 → 입력 float 텐서, initVTARuntime(), <net>_load_module(weights),",
          "<net>(constantWeight, mutableWeight, activations) 를 추론마다 호출, destroy"], 92),
        ("cpu", "생성된 bundle  <net>.cpp  (NEST-C VTASave.cpp가 만든 C++; graph executor 없음)",
         ["load_module: weight / bias를 VTAConstBufferAlloc (CMA, EVTA layout으로 이미 변환됨)",
          "추론 함수: IR 명령 순서대로 호출이 펼쳐진 직선 코드: VTABufferAlloc, preprocess_*, convolution_wo_tr*,",
          "maxpool_evta, fullyconnected_evta_packed, VTABufferFlush / Invalidate, CPU 연산(elemadd, avgpool, transpose …)"], 112),
        ("evta", "Bundle 계층  lib/Bundle/VTABundle.cpp (EVTA 연산), CPUBundle_generic.cpp (CPU 연산)",
         ["conv / maxpool / fc 한 번 = 'EVTA call': schedule(tile, virtual thread) → tile마다 conv_kernel이",
          "LOAD / GEMM / ALU / STORE 명령을 쌓음 → VTASynchronize.  record / replay 키 확인 (VTAReplayBegin)",
          "uop kernel cache (vGemmUOpHandle[evta] …), VTAConstBufferAlloc 공유, InputStream (conv0 입력 겹치기)"], 112),
        ("evta", "runtime API  include/zcu102/vta/runtime.h, lib/zcu102/api.cc  (TVM VTA runtime에서 온 C API)",
         ["VTABufferAlloc/Free (CMA pool), VTABufferFlush/Invalidate, VTATLSCommandHandle(i),",
          "VTALoadBuffer2D, VTAStoreBuffer2D, VTAPushGEMMOp, VTAPushALUOp, VTADepPush/Pop, VTARequant, VTASynchronize"], 92),
        ("evta", "CommandQueue  include/zcu102/cmd_queue.h, dat_queue.h, uop_kernel.h  (EVTA 하나에 하나)",
         ["InsnQueue (256비트 명령), UopQueue (uop + uop LOAD) : CMA 버퍼에 쌓음, 물리 주소를 명령에 직접 기록",
          "Synchronize: dependence 정리 + FINISH → cache flush → 드라이버 Run → 끝까지 대기 → 큐 비움",
          "Replay: 같은 키의 호출은 저장해 둔 명령 사본을 주소만 고쳐 다시 실행 (segment hook)"], 112),
        ("evta", "driver  lib/zcu102/pynq_driver.cc  (사용자 공간; 커널 드라이버 없음)",
         ["CMA: PYNQ libcma (cma_alloc / cma_flush_cache / cma_invalidate_cache + dsb)",
          "레지스터: /dev/mem mmap (sudo).  RunStart: insn 주소·개수 쓰고 START  →  RunWait: done을 폴링 (인터럽트 없음)"], 92),
    ]
    y = 100
    boxes = []
    for kind, title, lines, h in rows:
        s.box(X, y, W, h, kind, lines=lines, title=title, size=12.5, tsize=13.5, align="left")
        boxes.append((y, h))
        y += h + 26
    for (y0, h0), (y1, _) in zip(boxes, boxes[1:]):
        s.arrow(X + W / 2, y0 + h0, X + W / 2, y1 - 2, "a")
    # hardware
    hy = y
    s.box(X, hy, W, 70, "hls", title="EVTA 하드웨어 (PL)",
          lines=["fetch → load / compute (GEMM, ALU) / store, SRAM inp·wgt·acc·out·uop, dependence token 큐",
                 "레지스터: PS HPM0 → AXI-Lite (0xA0000000 + i·0x10000),  DRAM: S_AXI_HP<i>_FPD"], size=12.5, tsize=13.5,
          align="left")
    s.arrow(X + W / 2, boxes[-1][0] + boxes[-1][1], X + W / 2, hy - 2, "a")
    # right column: memory
    MX = 840
    s.box(MX, 100, 360, 300, "dram", title="DRAM (CMA, 128 MB, 물리 연속)", size=12.5, tsize=13.5, align="left",
          lines=["명령 큐 / uop 큐 (EVTA마다 2개)",
                 "  single: 32 MB × 2,  multi: 4 MB × 2",
                 "weight / bias  (load_module, 공유)",
                 "activation (VTABufferAlloc, 크기별 pool)",
                 "replay 사본 (명령 + uop)",
                 "",
                 "CPU는 cache로 접근, EVTA는 물리 주소로",
                 "→ 일관성 없음: CPU가 쓴 뒤 Flush,",
                 "   EVTA가 쓴 뒤 Invalidate (명시적)"])
    s.box(MX, 430, 360, 150, "mem", title="일반 CPU 메모리 (malloc)", size=12.5, tsize=13.5, align="left",
          lines=["입력 float 텐서 (mutableWeight)",
                 "CPU 연산만 쓰는 activation",
                 "uop kernel 정의 (UopKernel), replay 표",
                 "명령 생성 중의 임시 구조"])
    s.box(MX, 610, 360, 150, "note", title="계층마다 공개 범위", size=12.5, tsize=13.5, align="left",
          lines=["bundle → Bundle 계층: VTABundle.h",
                 "Bundle → runtime: vta/runtime.h",
                 "runtime → driver: vta/driver.h",
                 "  (VTADeviceAlloc/Run/RunStart/RunWait,",
                 "   VTAMemAlloc, VTAFlushCache …)"])
    s.arrow(X + W + 4, 330, MX - 4, 250, "k", dash="5,4")
    s.arrow(X + W + 4, 560, MX - 4, 300, "k", dash="5,4")
    s.arrow(X + W + 4, 760, MX - 4, 360, "k", dash="5,4")
    s.legend(X, 990, [("cpu", "CPU 코드"), ("evta", "EVTA 제어 코드"), ("dram", "CMA"), ("hls", "하드웨어")])
    return s


@fig(2, "programming_model")
def f2():
    s = Svg(1240, 820, "그림 2. runtime이 다루는 EVTA 프로그래밍 모델 (원래 VTA의 decoupled access-execute)",
            ["CPU는 DRAM에 명령 열을 쓰고 fetch에게 주소와 개수만 알려준다. 모듈 넷은 따로 돌고 token으로만 순서를 맞춘다."])
    # CPU and fetch
    s.box(40, 100, 300, 150, "cpu", title="CPU (A53, vtalib)", size=12.5, tsize=13.5, align="left",
          lines=["① 명령·uop를 CMA에 씀", "② cache flush (+ dsb)", "③ 레지스터: insn 주소·개수, START",
                 "④ compute의 done을 폴링"])
    s.rect(380, 90, 820, 520, "#f7f9fc", "#2f5f98", 1.4, 8)
    s.text(394, 112, "EVTA (하나, 레지스터 0xA0000000 + i·0x10000)", 14, weight="bold", color="#2f5f98")
    s.box(410, 130, 170, 80, "hls", title="fetch", lines=["명령을 읽어", "종류별 큐로"], size=12.5)
    s.arrow(342, 170, 406, 170, "o")
    s.text(350, 160, "③", 12, color="#c06a10")
    # modules
    M = [("load", 620, ["DRAM → SRAM", "(pad 0 채움)"]), ("compute", 830, ["GEMM 16×16", "ALU MIN MAX ADD SHR"]),
         ("store", 1030, ["SRAM → DRAM"])]
    for n, x, l in M:
        s.box(x, 250, 150, 80, "hls", title=n, lines=l, size=12.5)
        s.box(x + 20, 160, 110, 34, "mem", lines=[n + " 큐"], size=12)
        s.arrow(x + 75, 196, x + 75, 246, "b")
    s.path("M582,170 L602,170 L602,140 L1105,140 L1105,156", "b", arrow="b")
    s.path("M695,140 L695,156", "b", arrow="b")
    s.path("M905,140 L905,156", "b", arrow="b")
    # SRAMs
    s.box(620, 370, 150, 70, "mem", title="inp SRAM", lines=["4096 × 16 B (multi 2048)"], size=11.5)
    s.box(620, 455, 150, 70, "mem", title="wgt SRAM", lines=["2048 × 256 B (multi 1024)"], size=11.5)
    s.box(830, 370, 150, 70, "mem", title="acc SRAM (int32)", lines=["4096 × 64 B (multi 2048)"], size=11.5)
    s.box(830, 455, 150, 70, "mem", title="uop SRAM", lines=["64 KB"], size=11.5)
    s.box(1030, 370, 150, 70, "mem", title="out SRAM (int8)", lines=["acc를 줄인 값"], size=11.5)
    s.arrow(695, 332, 695, 366, "a")
    s.path("M612,332 L612,490 L616,490", "a", arrow="a")
    s.arrow(772, 405, 826, 405, "a")
    s.arrow(772, 490, 826, 415, "a")
    s.arrow(982, 405, 1026, 405, "a")
    s.arrow(1105, 366, 1105, 334, "a")
    s.box(410, 545, 770, 50, "note", size=12, lines=[("dependence token 큐 (VTADepPush / VTADepPop)", "bold"),
          "load→compute: 데이터 도착 (RAW)   compute→load: SRAM 반쪽이 비었음 (WAR)   compute→store, store→compute"])
    # DRAM
    s.box(40, 640, 1160, 120, "dram", title="DRAM (CMA, 물리 연속)", size=12.5, tsize=13.5, align="left", lines=[
        "명령 열 (256비트 × N, 끝은 FINISH)  ·  uop 열 (GEMM/ALU 루프 몸통)  ·  inp (int8, N·C/16·H·W·16)  ·  wgt (int8 16×16 블록)",
        "bias (int32, acc로 LOAD)  ·  out (int8, inp와 같은 layout)",
        "주소는 명령 안에 물리 주소로 들어간다 (base 레지스터는 모두 0). CPU 쪽 cache와 일관성 없음 → flush / invalidate."])
    s.path("M495,636 L495,214", "k", arrow="k", dash="5,4")
    s.text(502, 628, "명령 읽기", 11.5, color="#666666")
    s.path("M700,636 L700,620 L600,620 L600,300 L616,300", "k", arrow="k", dash="5,4")
    s.text(606, 632, "inp·wgt·bias·uop", 11.5, color="#666666")
    s.path("M1185,290 L1215,290 L1215,700 L1204,700", "k", arrow="k", dash="5,4")
    s.text(1140, 632, "out 쓰기", 11.5, color="#666666")
    s.path("M190,252 L190,636", "o", arrow="o")
    s.text(198, 450, "① ②", 12, color="#c06a10")
    s.text(40, 790, "명령 종류: LOAD / STORE / GEMM / ALU / FINISH (256비트, docs/evta_isa_ext.md). GEMM·ALU 명령은 uop 범위와 2중 루프를 가진다.",
           12.5, color="#444444")
    return s


@fig(3, "one_call")
def f3():
    s = Svg(1240, 800, "그림 3. EVTA call 하나 (convolution_wo_tr) 의 순서",
            ["처음 보는 인자: 명령을 만들며 기록(record). 같은 인자로 다시 오면: 기록을 주소만 고쳐 실행(replay).",
             "위에서 아래로 시간 순서. 실행 중 CPU는 done 레지스터를 폴링하며 기다린다 (yield)."])
    cols = [("bundle (.cpp)", "cpu"), ("VTABundle.cpp", "evta"), ("CommandQueue", "evta"), ("driver", "evta"),
            ("EVTA", "hls")]
    xs = [110, 340, 590, 830, 1080]
    for (n, k), x in zip(cols, xs):
        s.box(x - 90, 100, 180, 40, k, lines=[(n, "bold")], size=13)
        s.line(x, 140, x, 780, "#bbbbbb", 1.2, "4,4")
    D = 34

    def msg(y, a, b, text, color="a", dash=None):
        x1, x2 = xs[a], xs[b]
        if a == b:
            if a == len(xs) - 1:
                s.path(f"M{x1},{y - 6} l-40,0 l0,14 l38,0", color, arrow=color)
                s.text(x1 - 48, y + 5, text, 12, anchor="end")
            else:
                s.path(f"M{x1},{y - 6} l40,0 l0,14 l-38,0", color, arrow=color)
                s.text(x1 + 48, y + 5, text, 12)
        else:
            s.arrow(x1, y, x2 + (-4 if x2 > x1 else 4), y, color, dash=dash)
            s.text((x1 + x2) / 2, y - 6, text, 12, anchor="middle")

    msg(176, 0, 1, "convolution_wo_tr(in, w, b, out, shape, tile, vtaCmdH, nVT, idEVTA)", "b")
    msg(210, 1, 2, "VTAReplayBegin(key = 인자 28개, bufs)  → 기록이 있으면 replay", "b")
    # record
    r0 = 232
    rec = [(1, 1, "tile × 출력채널묶음 × 입력채널묶음 × virtual thread 루프 (conv_kernel)", "a"),
           (1, 2, "VTALoadBuffer2D / VTAPushGEMMOp / VTAPushALUOp / VTADepPush·Pop / VTAStoreBuffer2D", "b"),
           (2, 2, "256비트 명령을 InsnQueue에, uop를 UopQueue에 (물리 주소 기록, 재배치 표)", "a"),
           (1, 2, "VTASynchronize", "b"),
           (2, 2, "FINISH 추가, cache flush, 명령·uop 사본 저장 (RecordSegment)", "a"),
           (2, 3, "VTADeviceRun → RunStart(insn 주소, 개수)", "b")]
    s.rect(20, r0, 1200, 30 + D * len(rec), "#fbfbf3", "#9a7b00", 1, 4, dash="6,4")
    s.text(28, r0 + 18, "기록 (처음 한 번)", 12.5, weight="bold", color="#9a7b00")
    y = r0 + 44
    for a_, b_, t, c in rec:
        msg(y, a_, b_, t, c)
        y += D
    # replay
    p0 = y + 6
    rep = [(2, 2, "버퍼 주소가 바뀌었으면 사본 명령의 dram_base만 고치고 flush", "a"),
           (2, 3, "segment(run)마다 RunStart;  hook: 실행 중 CPU 일 (예: 다음 입력 행 pack)", "g")]
    s.rect(20, p0, 1200, 30 + D * len(rep), "#f3f8f2", "#3c8a35", 1, 4, dash="6,4")
    s.text(28, p0 + 18, "replay (두 번째부터; Resnet18 추론의 모든 call)", 12.5, weight="bold", color="#3c8a35")
    y = p0 + 44
    for a_, b_, t, c in rep:
        msg(y, a_, b_, t, c)
        y += D
    y += 14
    s.text(28, y - 4, "공통", 12.5, weight="bold", color="#555555")
    y += 14
    msg(y, 3, 4, "insn count / addr, base 0, START (fetch), AUTORESTART (load·compute·store)", "o")
    y += D
    msg(y, 4, 4, "fetch → 큐 → load / compute / store, token으로 맞물림 … FINISH")
    y += D
    msg(y, 3, 4, "RunWait: done_o_ap_vld → done_o == 1 까지 폴링 (5 s 넘으면 상태 덤프, 종료)", "o", dash="5,4")
    y += D
    msg(y, 3, 1, "return 0", "a")
    y += D
    msg(y, 1, 0, "return: 출력은 out 버퍼 (EVTA layout).  CPU가 읽을 때만 VTABufferInvalidate", "a")
    return s


@fig(4, "conv_tile")
def f4():
    s = Svg(1240, 700, "그림 4. conv tile 하나의 명령 (conv_kernel, virtual thread 2개)",
            ["SRAM을 반으로 나눈 두 '가상 스레드'(VT0, VT1)가 번갈아 load와 GEMM을 해서 load가 계산 뒤에 숨는다.",
             "화살표는 dependence token. 같은 모듈의 명령은 순서대로 실행된다."])
    lanes = [("load", 150), ("compute", 300), ("store", 450)]
    for n, y in lanes:
        s.text(40, y + 30, n, 15, weight="bold", color="#6d4596")
        s.line(130, y + 60, 1200, y + 60, "#cccccc", 1)
    x = 140

    def blk(lane, w, text, kind, vt=None):
        nonlocal x
        y = dict(lanes)[lane]
        s.box(x, y, w, 50, kind, lines=[text] + ([vt] if vt else []), size=11.5)
        r = (x, y, w)
        return r

    # load inp/wgt for VT0, VT1 ; compute GEMM VT0, VT1 ; repeated per input channel block; then ALU; store
    pos = {}
    x = 140
    pos["l0"] = blk("load", 120, "LOAD inp+wgt", "evta", "VT0, c0")
    x = 270
    pos["l1"] = blk("load", 120, "LOAD inp+wgt", "evta", "VT1, c0")
    x = 280
    pos["g0"] = blk("compute", 120, "GEMM", "rtl", "VT0, c0")
    x = 400
    pos["l2"] = blk("load", 120, "LOAD inp+wgt", "evta", "VT0, c1")
    x = 410
    pos["g1"] = blk("compute", 120, "GEMM", "rtl", "VT1, c0")
    x = 530
    pos["l3"] = blk("load", 120, "LOAD inp+wgt", "evta", "VT1, c1")
    x = 540
    pos["g2"] = blk("compute", 120, "GEMM", "rtl", "VT0, c1")
    x = 670
    pos["g3"] = blk("compute", 120, "GEMM", "rtl", "VT1, c1")
    x = 800
    pos["a"] = blk("compute", 150, "ALU / requantize", "rtl", "bias·shift·relu")
    x = 960
    pos["s0"] = blk("store", 110, "STORE", "evta", "VT0")
    x = 1080
    pos["s1"] = blk("store", 110, "STORE", "evta", "VT1")

    def tok(a, b, col="g"):
        xa, ya, wa = pos[a]
        xb, yb, wb = pos[b]
        s.arrow(xa + wa - 10, ya + 50, xb + 10, yb - 2, col)

    tok("l0", "g0")
    tok("l1", "g1")
    tok("l2", "g2")
    tok("l3", "g3")
    s.arrow(pos["g0"][0] + 100, 300, pos["l2"][0] + 20, 202, "r", dash="4,3")
    s.arrow(pos["g1"][0] + 100, 300, pos["l3"][0] + 20, 202, "r", dash="4,3")
    tok("a", "s0", "b")
    s.box(140, 540, 1060, 140, "note", size=12.5, align="left", title="루프 구조 (lib/Bundle/VTABundle.cpp conv_kernel)", lines=[
        "for 출력채널묶음 (nFilterinLoop × 16 × nVT):  acc reset (bias reset bitstream: bias LOAD 후 GEMM reset이 bias로 채움)",
        "  for 입력채널묶음 c (16채널씩):  VT마다 [pop c→l] LOAD inp tile(+halo, pad) · LOAD wgt (KH·KW 블록) [push l→c]",
        "                                   VT마다 [pop l→c] GEMM (uop: KH·KW·출력픽셀 루프) [push c→l]",
        "  VT마다 ALU: bias ADD, SHR, MAX/MIN (또는 _rq bitstream: GEMM이 out에 쓸 때 requantize)  [push c→s]",
        "  VT마다 [pop c→s] STORE (출력 행마다 또는 한 번) [push s→c]       … tile이 끝나도 Synchronize는 call당 한 번",
        "초록 = load→compute (RAW), 빨강 점선 = compute→load (WAR: SRAM 반쪽이 비었음), 파랑 = compute→store"])
    return s


@fig(5, "inference_timeline")
def f5():
    s = Svg(1240, 560, "그림 5. 추론 한 번: CPU와 EVTA의 시간 (Resnet18, single systolic _rq, OMP 1)",
            ["2026-11-06 측정: 24.07 ms (EVTA call 약 23.4 ms). 대부분 EVTA가 일하고 CPU는 기다린다 (폴링).",
             "bundle은 call을 하나씩 동기적으로 부른다: call 사이에만 CPU 일(입력 pack, invalidate, elemadd 없음 – fused)."])
    X0, X1 = 140, 1200
    T = 24.07

    def tx(t):
        return X0 + (X1 - X0) * t / T

    s.text(40, 170, "CPU", 15, weight="bold", color="#c06a10")
    s.text(40, 270, "EVTA", 15, weight="bold", color="#2f5f98")
    # conv0 with stream: CPU packing overlapped
    segs = [(0.0, 1.9, "conv0+pool", "evta"), (1.9, 6.6, "stage1 (4 conv)", "evta"), (6.6, 11.6, "stage2", "evta"),
            (11.6, 16.8, "stage3", "evta"), (16.8, 22.9, "stage4", "evta"), (22.9, 23.4, "fc", "evta")]
    for a, b, t, k in segs:
        s.box(tx(a), 240, tx(b) - tx(a) - 2, 50, k, lines=[t], size=11.5)
    s.box(tx(0), 140, tx(1.9) - tx(0) - 2, 50, "cpu", lines=["입력 행 pack", "(conv0과 겹침)"], size=10.5)
    s.box(tx(23.4), 140, tx(24.07) - tx(23.4), 50, "cpu", lines=["출력"], size=10.5)
    for a in [1.9, 6.6, 11.6, 16.8, 22.9]:
        s.line(tx(a), 195, tx(a), 236, "#c06a10", 1, "3,3")
    s.rect(tx(1.9), 150, tx(23.4) - tx(1.9), 30, "#ffffff", "#c06a10", 1, 3, dash="3,3")
    s.text((tx(1.9) + tx(23.4)) / 2, 170, "done 폴링 (yield) — call 사이에 짧은 CPU 일: 다음 call의 인자, 필요한 invalidate", 12,
           anchor="middle", color="#c06a10")
    for t in [0, 5, 10, 15, 20, 24.07]:
        s.line(tx(t), 300, tx(t), 310, "#555555", 1)
        s.text(tx(t), 326, f"{t:g} ms", 11.5, anchor="middle", color="#555555")
    s.box(140, 360, 1060, 170, "note", size=12.5, align="left", title="숫자 (docs/evta_runtime_compare.md §7.14, §7.18, §7.19)", lines=[
        "EVTA 안: GEMM ~18 ms, ALU(pool MAX) 소량, LOAD/STORE 대기·run 시작 비용 나머지. run 하나 시작+끝 비용 수십 µs.",
        "replay 전 (11-02 이전): call마다 명령 생성(JIT)이 CPU에서 2.6 ms → replay로 사라짐.",
        "conv0 입력 pack 1.3 ms는 tile 행 단위 run으로 나눠 EVTA 계산 뒤에 숨김 (InputStream, 11-06): 25.4 → 24.1 ms.",
        "층 사이 activation은 EVTA layout 그대로 CMA에 남고, CPU를 거치지 않는다 (-vta-keep-layout).",
        "구간 경계는 그림을 위한 근사다 (stage별 정확한 값은 _sysprof 로그)."])
    return s


@fig(6, "multi_evta_memory")
def f6():
    s = Svg(1240, 760, "그림 6. EVTA 4개 (multi4): thread, 명령 큐, 장치, 메모리",
            ["bundle copy i 는 EVTA i 만 쓴다 (model-compiler -index-multi-evta=2^i). copy마다 pthread 하나, 서로 공유하는 것은 weight뿐."])
    for i in range(4):
        x = 40 + i * 300
        s.box(x, 110, 260, 60, "cpu", lines=[(f"pthread {i}: copy {i}", "bold"), f"<net>_{i}(…) , vtaCmdH{i if i else ''}"], size=12)
        s.box(x, 200, 260, 70, "evta", lines=[(f"CommandQueue {i}", "bold"), "VTATLSCommandHandle(" + str(i) + ")",
                                               "insn 4 MB + uop 4 MB (CMA)"], size=11.5)
        s.box(x, 300, 260, 60, "evta", lines=[(f"VTADevice {i}", "bold"), f"regs 0xA00{i}0000 (fetch/load/compute/store)"],
              size=11.5)
        s.box(x, 400, 260, 60, "hls", lines=[(f"EVTA {i}", "bold"), f"S_AXI_HP{i}_FPD → DDR"], size=12)
        s.arrow(x + 130, 172, x + 130, 196, "b")
        s.arrow(x + 130, 272, x + 130, 296, "b")
        s.arrow(x + 130, 362, x + 130, 396, "o")
    s.box(40, 500, 1160, 110, "dram", title="CMA 128 MB", size=12.5, tsize=13.5, align="left", lines=[
        "명령·uop 큐 4 × 8 MB = 32 MB   |   weight / bias: 같은 바이트면 copy들이 버퍼 하나를 공유 (VTAConstBufferAlloc, refcount)",
        "activation: copy마다 VTABufferAlloc (크기별 pool, 기본 32 MB까지 보관)   |   replay 사본: copy마다",
        "Resnet50 × 4: 공유 없이는 weight만 102 MB → 공유로 실행 중 여유 85 MB"])
    for i in range(4):
        s.arrow(40 + i * 300 + 130, 462, 40 + i * 300 + 130, 496, "k", dash="4,3")
    s.box(40, 630, 1160, 100, "note", size=12.5, align="left", title="동시 실행이 느려지는 이유 (§7.19 ⑧)", lines=[
        "copy 0 혼자: multi4 bitstream의 HP register slice 때문에 single bitstream보다 +1.1 %.",
        "EVTA 둘이 같이: DDR 컨트롤러 공유로 copy마다 +0.9 ms (HP1+HP2는 같은 DDR 포트라 +1.25 ms). 대역폭이 아니라 지연.",
        "CPU 쪽: 폴링 thread 4개가 A53 4코어를 나눔 (OMP_NUM_THREADS=1 권장)."])
    return s


def main():
    nums = [int(a) for a in sys.argv[1:]] or sorted(FIGS)
    for n in nums:
        name, f = FIGS[n]
        path = os.path.join(OUT, f"evta_rt_{n:02d}_{name}.svg")
        f().save(path)
        print(path)


if __name__ == "__main__":
    main()
