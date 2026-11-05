#!/usr/bin/env python3
# EVTA before / after figures for docs/evta_overview.md (docs/figs/evta_figNN_*.svg).
# usage: python3 docs/figs/src/make_evta_figs.py [NN ...]   (no argument: every figure)
# Numbers come from docs/evta_runtime_compare.md (sections named in each figure) and docs/evta_overview.md.
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from svglib import Svg, C, tw  # noqa: E402

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
FIGS = {}


def fig(num, name):
    def deco(f):
        FIGS[num] = (name, f)
        return f
    return deco


def flow(s, x0, y, items, bw, bh, gap, size=12):
    """a row of boxes joined by arrows; items = (kind, lines) or None for a gap; returns the box x positions"""
    xs = []
    x = x0
    for i, it in enumerate(items):
        kind, lines = it[0], it[1]
        w = it[2] if len(it) > 2 else bw
        s.box(x, y, w, bh, kind, lines, size)
        xs.append((x, w))
        if i + 1 < len(items):
            s.arrow(x + w + 2, y + bh / 2, x + w + gap - 2, y + bh / 2, "a", 1.4)
        x += w + gap
    return xs


# ---------------------------------------------------------------------------------------------------------------
@fig(1, "inference_flow")
def fig01():
    W = 1460
    s = Svg(W, 1010, "그림 1. Resnet18 추론 한 번: CPU와 EVTA가 하는 일 (2026-10-29 → 11-02)",
            ["single EVTA, systolic 400 MHz, OpenMP 1 thread. 10-29: evta_runtime_compare.md §3 (_systimed),"
             " 11-02: §7.14 (_sysprof, EVTA_REPLAY_LOG=2)와 §7.12 (입력 복사 단독 측정)."])
    # ---- A. time bars --------------------------------------------------------------------------------------------
    s.section(88, "A. 시간: 종류별 합 (같은 축, 1 ms = 7.6 px)")
    x0, scale = 150, 7.6
    before = [("EVTA conv 계산", 29.7, "#7aa6d6"), ("명령 생성 (JIT)", 2.5, "#b294d4"),
              ("transpose (NHWC ↔ EVTA layout, conv0 0 채움)", 66.1, "#f3a65a"), ("elemadd", 16.1, "#e98b8b"),
              ("maxpool", 12.8, "#e9a0c0"), ("relu", 8.6, "#f2c38b"), ("buffer 할당·해제·flush", 4.5, "#bdbdbd"),
              ("fc", 4.4, "#c9a27e"), ("전처리", 3.7, "#9bd18f")]
    after = [("conv0+maxpool", 4.48, "#5b8fc9"), ("conv 19개 (residual 포함)", 21.86, "#7aa6d6"), ("fc", 0.13, "#4a7bb7"),
             ("전처리+pack", 1.7, "#9bd18f"), ("기타 CPU", 0.66, "#d9d9d9")]

    def bar(y, label, total, segs, labels_below):
        s.text(x0 - 12, y + 22, label, 14, "end", "bold")
        x = x0
        for i, (name, ms, col) in enumerate(segs):
            w = ms * scale
            s.rect(x, y, w, 34, col, "#555555", 0.8)
            if w > 70:
                s.text(x + w / 2, y + 15, name if tw(name, 11.5) < w - 6 else name.split(" (")[0], 11.5, "middle")
                s.text(x + w / 2, y + 29, f"{ms:g} ms", 11.5, "middle", "bold")
            else:
                labels_below.append((x + w / 2, name, ms))
            x += w
        s.text(x + 10, y + 23, f"합 {total} ms", 15, weight="bold", color="#c0392b" if total > 100 else "#2f7a2a")
        return x

    def labels(lb, ybar, col_x):
        # small segments: elbow leaders from the segment centre to a label column right of them
        for i, (cx, name, ms) in enumerate(lb):
            yy = ybar + 52 + 17 * i
            s.path(f"M{cx:.1f},{ybar + 35} L{cx:.1f},{yy - 4} L{col_x - 4:.1f},{yy - 4}", "#888888", 0.8)
            s.text(col_x, yy, f"{name} {ms:g} ms", 11.5, color="#333333")

    lb = []
    xe = bar(132, "10-29", 148.9, before, lb)
    labels(lb, 132, xe + 12)
    lb2 = []
    xe2 = bar(258, "11-02", 28.83, after, lb2)
    labels(lb2, 258, xe2 + 30)
    s.lines(720, 300, ["• CPU 일: 약 116 ms → 약 2.4 ms.",
                       "• EVTA 계산 시간: 29.7 → 26.5 ms (conv0 pack과 conv0+pool fusion).",
                       "• 추론 한 장: 148.9 → 28.83 ms (5.2배). OMP 4로는 73.6 → 27.71 ms."], 13)
    # ---- B. before flow ------------------------------------------------------------------------------------------
    yb = 380
    s.section(yb, "B. 10-29: 층마다 CPU와 EVTA를 오간다 (activation은 NHWC로 DRAM에)")
    bw, bh, gap = 112, 58, 18
    y1 = yb + 52
    items = [("mem", ["입력", "float NCHW"]), ("cpu", ["quantize"]), ("cpu", ["elemsub", "(평균)"]), ("cpu", ["elemdiv", "(표준편차)"]),
             ("cpu", ["transpose", "+ 3→16ch", "0 채움"]), ("evta", ["conv0", "7×7/2, 16ch"]), ("cpu", ["transpose", "→ NHWC"]),
             ("cpu", ["maxpool", "3×3/2"])]
    flow(s, 40, y1, items, bw, bh, gap)
    s.arrow(40 + 8 * (bw + gap) - gap + 2, y1 + bh / 2, 40 + 8 * (bw + gap) + 30, y1 + bh / 2)
    s.text(40 + 8 * (bw + gap) + 36, y1 + bh / 2 + 5, "residual block으로", 12.5)
    y2 = y1 + 110
    s.rect(30, y2 - 26, 8 * (bw + gap) + 4, bh + 44, "none", "#c06a10", 1.4, 8, "6 4")
    s.text(42, y2 - 8, "residual block × 8 (그중 3개는 1×1 downsample conv가 하나 더 있음)", 12.5, weight="bold", color="#c06a10")
    items = [("cpu", ["transpose", "→ EVTA"]), ("evta", ["conv 3×3", "(+relu)"]), ("cpu", ["transpose", "→ NHWC"]),
             ("cpu", ["transpose", "→ EVTA"]), ("evta", ["conv 3×3"]), ("cpu", ["transpose", "→ NHWC"]), ("cpu", ["elemadd", "(residual)"]),
             ("cpu", ["relu"])]
    flow(s, 40, y2, items, bw, bh, gap)
    x3 = 40 + 8 * (bw + gap) + 20
    s.arrow(x3 - 36, y2 + bh / 2, x3 - 2, y2 + bh / 2)
    flow(s, x3, y2, [("cpu", ["avgpool"], 90), ("cpu", ["fc", "512→1000"], 90), ("cpu", ["softmax"], 90)], 90, bh, gap)
    s.lines(40, y2 + bh + 40,
            ["EVTA 호출마다: VTABufferAlloc(입력, 출력) → 명령·uop 생성(JIT) → flush → 시작 → 완료 폴링 → invalidate → VTABufferFree.",
             "transpose와 elementwise는 CPU OpenMP. EVTA는 conv의 GEMM과 bias·shift·clip만 했다."], 12.5, color="#5a3a10")
    # ---- C. after flow -------------------------------------------------------------------------------------------
    yc = yb + 300
    s.section(yc, "C. 11-02: CPU는 입력과 끝에만, 그 사이는 EVTA 호출이 이어진다 (activation은 EVTA layout N{C/16}HW16로 DRAM에)",
              color="#2f7a2a")
    y1 = yc + 52
    bw2 = 150
    items = [("mem", ["입력", "float NCHW"], 112),
             ("new", ["전처리 + conv0 pack", "quantize, elemsub/div,", "NCHW→NHWC, pad, 2행 pack", "(CPU NEON, 1회)"], 190),
             ("evta", ["conv0 + maxpool", "EVTA 호출 1번", "4×1 over 48ch,", "pool은 acc 안에서"], 170)]
    flow(s, 40, y1, items, bw2, 84, gap)
    xb = 40 + 112 + 190 + 170 + 3 * gap
    s.arrow(xb - gap + 2, y1 + 42, xb + 6, y1 + 42)
    s.rect(xb + 10, y1 - 26, 470, 84 + 44, "none", "#2f5f98", 1.4, 8, "6 4")
    s.text(xb + 22, y1 - 8, "residual block × 8", 12.5, weight="bold", color="#2f5f98")
    flow(s, xb + 22, y1, [("evta", ["conv 3×3", "(+relu)", "EVTA"], 130),
                          ("evta", ["conv 3×3", "+ residual + relu", "EVTA 호출 1번"], 160),
                          ("evta", ["1×1 downsample", "(3개 block)", "EVTA"], 120)], 130, 84, 14)
    xe = xb + 10 + 470 + 14
    s.arrow(xe - 12, y1 + 42, xe + 4, y1 + 42)
    flow(s, xe + 6, y1, [("cpu", ["avgpool"], 82), ("evta", ["fc", "EVTA"], 70), ("cpu", ["softmax"], 80)], 80, 84, 14)
    s.lines(40, y1 + 84 + 44,
            [("바뀐 것", "bold"),
             "• 층 사이 transpose 없음: EVTA가 STORE한 layout을 다음 EVTA 호출이 그대로 LOAD한다 (§7.1).",
             "• elemadd + relu, maxpool, fc를 EVTA에서 (§7.2, §7.3, §7.8). conv0 + maxpool은 한 호출 (§7.13).",
             "• 전처리 4단계와 conv0 입력 준비를 NEON 한 번으로 (§7.9, §7.12).",
             "• EVTA 호출의 CPU 몫: buffer는 pool에서 재사용 (§7.5), 명령·uop은 첫 추론에 기록하고 그 뒤는 주소만 고쳐 replay (§7.10)."],
            13)
    s.legend(40, 990, [("cpu", "CPU"), ("evta", "EVTA 호출"), ("new", "새로 합친 CPU 단계"), ("mem", "데이터")])
    return s


# ---------------------------------------------------------------------------------------------------------------
@fig(12, "gemm_hls_vs_systolic")
def fig12():
    W = 1460
    s = Svg(W, 1300, "그림 12. GEMM 한 단계(uop 하나): HLS broadcast → systolic 어레이 gemm_sa",
            ["한 단계 = acc[dst] += inp[src] (1×16 int8) · wgt[wgt]ᵀ (16×16 int8). 두 구조 모두 1 cycle에 한 단계(II = 1)."
             " 자세히: evta_systolic_design.md, evta_systolic.md §3, §8."])
    # ---------------- A. HLS broadcast ----------------
    ya = 92
    s.section(ya, "A. 이전: HLS compute 안의 gemm() — 한 반복에서 읽고, 곱하고, 더하고, acc에 쓴다 (systolic이 아닌 bitstream은 지금도 이 구조)")
    y0 = ya + 60
    s.box(40, y0, 150, 60, "mem", ["inp_mem", "x = inp[src]", "(16 × int8)"], 12.5)
    s.box(40, y0 + 100, 150, 60, "mem", ["wgt_mem", "W = wgt[wgt]", "(16 × 16 int8)"], 12.5)
    s.box(40, y0 + 200, 150, 60, "mem", ["acc_mem", "a = acc[dst]", "(16 × int32)"], 12.5)
    # multiplier grid 8x8 drawn
    gx, gy, cs = 300, y0 - 10, 26
    s.rect(gx - 14, gy - 14, 8 * cs + 28, 8 * cs + 28, C["hls"][0], C["hls"][1], 1.4, 8)
    for i in range(8):
        for j in range(8):
            s.rect(gx + j * cs + 3, gy + i * cs + 3, cs - 6, cs - 6, "#ffffff", "#6d4596", 0.8, 2)
            s.text(gx + j * cs + cs / 2, gy + i * cs + cs / 2 + 4, "×", 11, "middle", color="#6d4596")
    s.text(gx + 4 * cs, gy + 8 * cs + 34, "곱셈기 16 × 16 = 256개 (그림은 8 × 8)", 12, "middle", color="#6d4596")
    # broadcast x
    for j in range(8):
        s.line(gx + j * cs + cs / 2, gy - 14, gx + j * cs + cs / 2, gy + 8 * cs, "#c0392b", 0.7, "3 2")
    s.arrow(190, y0 + 30, gx - 16, gy + 6, "r", 1.6)
    s.text(gx + 8 * cs + 24, gy - 2, "← x[i]를 같은 열의 곱셈기 16개에 동시에 (broadcast)", 11.5, color="#c0392b")
    s.arrow(190, y0 + 130, gx - 16, y0 + 130, "a", 1.4)
    # adder trees
    tx = gx + 8 * cs + 60
    s.box(tx, y0 + 20, 150, 150, "hls", ["덧셈 트리 16개", "출력 o마다", "Σ_i x[i]·W[o][i]", "(16개 → 1개,", "4단)"], 12.5)
    s.arrow(gx + 8 * cs + 16, y0 + 95, tx - 2, y0 + 95, "p")
    ax = tx + 200
    s.box(ax, y0 + 60, 120, 70, "hls", ["+ a", "(16 × int32)"], 13)
    s.arrow(tx + 152, y0 + 95, ax - 2, y0 + 95, "p")
    s.path(f"M190,{y0 + 230} L{gx - 30},{y0 + 230} L{gx - 30},{y0 + 268} L{ax + 60},{y0 + 268} L{ax + 60},{y0 + 132}", "a", 1.4, arrow="a")
    wx = ax + 170
    s.box(wx, y0 + 30, 180, 56, "mem", ["acc[dst] ← 합", "(reset이면 0)"], 12.5)
    s.box(wx, y0 + 110, 180, 56, "mem", ["out[dst] ← 합의 하위 8비트"], 12.5)
    s.arrow(ax + 122, y0 + 90, wx - 2, y0 + 60)
    s.arrow(ax + 122, y0 + 100, wx - 2, y0 + 136)
    s.lines(wx + 200, y0 + 20, [("이 경로가 400 MHz를 막았다", "bold"),
                                "(실패 경로 상위 200 중 169개):",
                                "• x의 broadcast: fanout 수백,",
                                "  배선이 경로의 60 % 이상",
                                "• wgt_mem → DSP",
                                "• 덧셈 트리",
                                "• acc / out 쓰기",
                                "",
                                "HLS GEMM은 380 MHz가 한계"], 12.5, color="#7a1f1f")
    # ---------------- B. systolic ----------------
    yb = ya + 370
    s.section(yb, "B. 지금 (SYSTOLIC=1): HLS는 uop만 보내고, RTL gemm_sa가 weight-stationary 어레이로 계산한다 (400 MHz 통과)", color="#2f7a2a")
    y1 = yb + 56
    s.box(40, y1, 230, 120, "hls", ["HLS compute: GEMM 명령", "iter_out × iter_in × uop 루프", "(II = 1)를 돌며 uop마다",
                                    "{dst, src, wgt, reset, last,", "first, g2l, g2s}를 보낸다"], 12)
    s.box(310, y1 + 20, 120, 80, "mem", ["AXIS FIFO", "깊이 32"], 12.5)
    s.arrow(272, y1 + 60, 308, y1 + 60, "p")
    s.box(470, y1, 250, 120, "rtl", ["sa_ctrl", "uop을 꺼내 inp 주소를 내고,", "wgt 블록이 바뀌는 uop을 보면", "다음 weight 채우기를 시작한다",
                                     "(채우기 16 clk, 계산 뒤에 숨는다)"], 12)
    s.arrow(432, y1 + 60, 468, y1 + 60, "y")
    # weight fill
    s.box(770, y1, 180, 56, "mem", ["wgt_mem bank j", "= W[j][0..15]"], 12)
    s.box(770, y1 + 76, 180, 44, "rtl", ["PISO j: 16 clk에 걸쳐", "8비트씩 아래로"], 11.5)
    s.arrow(860, y1 + 58, 860, y1 + 74, "y")
    s.arrow(720, y1 + 30, 768, y1 + 30, "y")
    s.text(744, y1 + 22, "fill", 11, "middle", color="#9a7b00")
    # array 4x4
    ax0, ay0, pc, ph = 640, y1 + 200, 150, 82
    n = 4
    s.rect(ax0 - 20, ay0 - 30, n * pc + 20, n * ph + 40, "#fffbea", "#9a7b00", 1.4, 8)
    s.text(ax0 - 10, ay0 - 12, "어레이 16 × 16 DSP48E2 (그림은 4 × 4): 행 i = 입력 채널, 열 j = 출력 채널", 12.5, weight="bold", color="#7a6000")
    for i in range(n):
        for j in range(n):
            x, y = ax0 + j * pc, ay0 + i * ph
            s.box(x, y, pc - 30, ph - 26, "rtl", [f"PE({i},{j}) DSP48E2", "P = A·B2 + PCIN" if i else "P = A·B2 (+0)"], 11)
            if j + 1 < n:
                s.arrow(x + pc - 30, y + (ph - 26) / 2, x + pc - 2, y + (ph - 26) / 2, "r", 1.2)
            s.arrow(x + (pc - 30) / 2, y + ph - 26, x + (pc - 30) / 2, y + ph - 2, "b", 1.2)
    s.text(ax0 + n * pc - 10, ay0 + 26, "x →", 12, color="#c0392b")
    s.text(ax0 + n * pc - 10, ay0 + 42, "(한 clk에 한 열)", 11, color="#c0392b")
    # weights down
    s.path(f"M860,{y1 + 122} L860,{ay0 - 40} L{ax0 + 60},{ay0 - 40} L{ax0 + 60},{ay0 - 2}", "y", 1.4, arrow="y")
    s.text(870, ay0 - 50, "B1 ← 채우기, x의 첫 값과 함께 B2 ← B1 (교체가 계산을 멈추지 않는다)", 11.5, color="#9a7b00")
    # skew input
    sx = 300
    s.box(sx, ay0 + 40, 150, 70, "mem", ["inp_mem", "x = inp[src]", "16 × int8"], 12)
    s.box(sx + 180, ay0 + 10, 110, n * ph - 40, "rtl", ["skew", "행 i를", "i clk 늦춘다"], 12)
    s.arrow(sx + 152, ay0 + 75, sx + 178, ay0 + 75, "r")
    for i in range(n):
        s.arrow(sx + 292, ay0 + i * ph + 28, ax0 - 2, ay0 + i * ph + 28, "r", 1.2)
    s.arrow(600, y1 + 122, sx + 75, ay0 + 38, "y", 1.2, dash="4 3")
    s.text(470, y1 + 160, "inp 주소", 11, color="#9a7b00")
    # deskew and rmw
    dy = ay0 + n * ph + 24
    s.box(ax0, dy, n * pc - 30, 40, "rtl", ["deskew: 열 j를 15−j clk 늦춰 한 행으로 (x를 넣고 33 clk 뒤, 열 합 16 × 20비트)"], 12)
    for j in range(n):
        s.arrow(ax0 + j * pc + (pc - 30) / 2, dy - 22, ax0 + j * pc + (pc - 30) / 2, dy - 2, "b", 1.2)
    ry = dy + 70
    s.box(ax0 - 300, ry, n * pc + 270, 120, "rtl",
          ["sa_rmw (fabric, 3단): S0 acc[dst] 주소 → S2 읽은 값 (진행 중인 같은 dst 쓰기가 있으면 그 값) + 열 합",
           "→ S3 acc[dst] ← reset ? 0 : 합,   out[dst] ← 합의 하위 8비트",
           "명령의 마지막 uop이 쓰이면 done과 g2l / g2s token을 HLS로 돌려준다",
           "acc_mem / out_mem 포트는 gemm_busy로 RTL과 HLS(ALU, acc LOAD)가 나눠 쓴다"], 12.5, title="acc에 더하기는 맨 아래에서")
    s.arrow(ax0 + (n * pc - 30) / 2, dy + 42, ax0 + (n * pc - 30) / 2, ry - 2, "y")
    s.lines(40, ay0 + 180, [("바뀐 점", "bold"),
                            "• x는 왼쪽에서 한 PE씩 옮겨 간다:",
                            "  fanout 1 (broadcast 없음)",
                            "• 열 합은 DSP 안 PCIN 사슬:",
                            "  덧셈 트리 없음",
                            "• weight는 PE 안 B 레지스터에 머문다",
                            "• 곱셈기 수(256), 1 clk 1 단계는 같다",
                            "• 결과: 400 MHz. Resnet18 EVTA 시간",
                            "  333 MHz HLS 대비 −15 % (10-28)"], 12.5, color="#1f4f1f")
    s.legend(40, 1282, [("hls", "HLS (vta.cc)"), ("rtl", "RTL (rtl/*.v)"), ("mem", "BRAM")])
    return s


# ---------------------------------------------------------------------------------------------------------------
@fig(4, "residual_block")
def fig04():
    W = 1460
    s = Svg(W, 960, "그림 4. residual block의 두 번째 conv: conv → CPU elemadd → CPU relu  ⇒  EVTA 호출 하나",
            ["Resnet18의 residual block 8개. out = relu(requant(conv(x)) ⊕ r). r은 block의 입력(또는 downsample conv의 출력)."
             " 자세히: evta_runtime_compare.md §7.2, vtalib convolution_wo_tr_res, VTASave.cpp findResidualFusions."])
    # ---------------- A ----------------
    ya = 92
    s.section(ya, "A. 이전 (10-31 오전): EVTA는 conv까지, 더하기와 relu는 CPU (데이터가 DRAM을 두 번 더 왕복)")
    y0 = ya + 56
    # DRAM column
    s.box(40, y0, 170, 300, "dram", [], title=None)
    s.text(125, y0 + 22, "DRAM", 14, "middle", "bold", "#8a6d3b")
    dr = [("x (conv 입력)", y0 + 50), ("W, bias", y0 + 95), ("r (residual)", y0 + 140), ("y (conv 출력)", y0 + 185),
          ("z (더한 값)", y0 + 230), ("out", y0 + 275)]
    for name, yy in dr:
        s.box(55, yy - 16, 140, 30, "mem", [name], 12)
    # EVTA call
    ex = 300
    s.rect(ex, y0, 520, 175, C["evta"][0], C["evta"][1], 1.6, 8)
    s.text(ex + 12, y0 + 22, "EVTA 호출 convolution_wo_tr (tile마다)", 13.5, weight="bold", color="#2f5f98")
    flow(s, ex + 14, y0 + 44, [("mem", ["LOAD", "x, W, bias"], 100), ("evta", ["GEMM", "3×3 tap", "→ acc"], 100),
                               ("evta", ["ALU", "+bias, >>shift", "clip int8"], 120), ("mem", ["STORE", "y"], 80)], 100, 70, 24, 12)
    s.text(ex + 14, y0 + 140, "acc(int32)는 EVTA 안에서 끝난다. y는 int8로 DRAM에.", 12, color="#2f5f98")
    s.arrow(197, y0 + 50, ex + 12, y0 + 80, "k", 1.2)
    s.arrow(197, y0 + 95, ex + 12, y0 + 86, "k", 1.2)
    sx = ex + 14 + 100 + 24 + 100 + 24 + 120 + 24 + 40
    s.path(f"M{sx},{y0 + 116} L{sx},{y0 + 192} L230,{y0 + 192} L199,{y0 + 186}", "b", 1.4, arrow="b")
    # CPU elemadd / relu
    cx = 880
    s.box(cx, y0 + 120, 250, 90, "cpu", ["CPU elemadd (OpenMP)", "z = clip(round(y·s_y + r·s_r) / s_z)", "y, r 읽기 → z 쓰기"], 12)
    s.box(cx, y0 + 230, 250, 70, "cpu", ["CPU relu", "out = max(z, 0)", "z 읽기 → out 쓰기"], 12)
    s.arrow(197, y0 + 185, cx - 2, y0 + 160, "o", 1.3)
    s.arrow(197, y0 + 140, cx - 2, y0 + 150, "o", 1.3)
    s.arrow(cx - 2, y0 + 190, 197, y0 + 230, "o", 1.3)
    s.arrow(197, y0 + 232, cx - 2, y0 + 260, "o", 1.3)
    s.arrow(cx - 2, y0 + 280, 197, y0 + 275, "o", 1.3)
    s.lines(cx + 280, y0 + 120, [("비용 (8 block, OMP 1)", "bold"), "elemadd 16.1 ms", "relu 8.6 ms",
                                 "y, z의 DRAM 왕복", "+ CPU cache flush/invalidate"], 12.5, color="#7a3a00")
    # ---------------- B ----------------
    yb = ya + 400
    s.section(yb, "B. 지금 (10-31 ~, -vta-fuse-residual): 같은 EVTA 호출 안에서 r을 inp buffer로 LOAD하고 identity GEMM으로 acc에 더한다",
              color="#2f7a2a")
    y1 = yb + 56
    s.box(40, y1, 170, 230, "dram", [])
    s.text(125, y1 + 22, "DRAM", 14, "middle", "bold", "#8a6d3b")
    for name, yy in [("x", y1 + 55), ("W, bias", y1 + 100), ("r (residual)", y1 + 145), ("out", y1 + 200)]:
        s.box(55, yy - 16, 140, 30, "mem", [name], 12)
    ex = 270
    s.rect(ex, y1, 1170, 300, C["evta"][0], C["evta"][1], 1.6, 8)
    s.text(ex + 12, y1 + 22, "EVTA 호출 convolution_wo_tr_res (tile마다, 모두 acc buffer 안에서)", 13.5, weight="bold", color="#2f5f98")
    st = [("mem", ["① LOAD", "x, W, bias"], 96), ("evta", ["② GEMM", "3×3 tap", "acc = conv"], 96),
          ("evta", ["③ ALU", "conv의 requant:", "+bias, >>shift,", "clip int8"], 116),
          ("new", ["④ ALU ADD", "acc += acc", "(convShl번)", "= conv·2^convShl"], 116),
          ("new", ["⑤ LOAD r", "inp buffer의", "acc 자리에"], 100),
          ("new", ["⑥ GEMM", "weight = 2^resShl·I", "acc += r·2^resShl"], 136),
          ("new", ["⑦ ALU", "+2^(sumShr−1),", ">> sumShr", "(반올림)"], 116),
          ("new", ["⑧ ALU", "MAX(relu ? 0 : −128)", "MIN 127"], 140), ("mem", ["STORE", "out"], 70)]
    flow(s, ex + 14, y1 + 50, st, 100, 92, 16, 11.5)
    s.arrow(197, y1 + 55, ex + 14, y1 + 90, "k", 1.2)
    s.path(f"M197,{y1 + 145} L{ex - 20},{y1 + 145} L{ex - 20},{y1 + 175} L{ex + 14 + 96 + 96 + 116 + 116 + 4 * 16 + 50},{y1 + 175} "
           f"L{ex + 14 + 96 + 96 + 116 + 116 + 4 * 16 + 50},{y1 + 144}", "g", 1.4, arrow="g")
    xs = ex + 14 + 96 + 96 + 116 + 116 + 100 + 136 + 116 + 140 + 8 * 16 + 35
    s.path(f"M{xs},{y1 + 144} L{xs},{y1 + 200} L197,{y1 + 200}", "b", 1.4, arrow="b")
    s.lines(ex + 14, y1 + 225,
            ["• 결과 = round((conv·2^convShl + r·2^resShl) / 2^sumShr)를 clip: scale이 2의 거듭제곱일 때 CPU elemadd와 bit 단위로 같다",
             "  (.5에서 올림. 합이 0 이상이거나 relu가 있으면 CPU의 반올림과 같다). 조건이 맞지 않으면 A처럼 따로 돈다.",
             "• 쓰는 것은 ISA에 있는 ALU MIN/MAX/ADD/SHR와 GEMM뿐이다(MUL 없음). identity weight는 wgt buffer 마지막 entry에 둔다.",
             "• EVTA_RES_CHECK=1이면 실행 중에 A 경로 결과와 비교한다."], 12.5)
    s.lines(40, y1 + 330, [("효과", "bold"),
                           "Resnet18 single OMP 1 90.9 → 67.1 ms, OMP 4 56.6 → 50.2 ms, multi4 100회 2.47 → 1.85 s (8 block 모두 합침).",
                           "y와 z가 DRAM을 오가지 않는다. CPU elemadd, relu와 그 앞뒤의 flush / invalidate가 없어진다."], 13)
    s.legend(40, 945, [("evta", "EVTA (원래 있던 단계)"), ("new", "새로 넣은 EVTA 단계"), ("cpu", "CPU"), ("mem", "LOAD / STORE"),
                       ("dram", "DRAM")])
    return s


# ---------------------------------------------------------------------------------------------------------------
@fig(5, "conv0_pack")
def fig05():
    W = 1460
    s = Svg(W, 1010, "그림 5. Resnet18 conv0 (7×7, stride 2, 3채널 → 64): 16채널 0 채움 → packed 입력의 4×1 conv",
            ["EVTA GEMM 한 단계는 입력 16채널 블록 하나를 쓴다. 자세히: evta_runtime_compare.md §7.12, VTASave.cpp getConv0Pack /"
             " writeConv0PackFilter, vtalib preprocess_conv0pack."])
    # ---------------- A ----------------
    ya = 92
    s.section(ya, "A. 이전 (10-28 ~ 11-02 오전): 3채널을 16채널로 0 채워 7×7 conv — GEMM의 13/16이 0을 곱한다")
    y0 = ya + 60
    s.box(40, y0, 170, 90, "mem", ["입력 224×224×3", "(quantize 뒤 int8)"], 12.5)
    s.box(280, y0, 220, 90, "cpu", ["CPU: 0 채움 + pad 3", "230×230×16", "(16채널 중 13채널이 0)"], 12.5)
    s.arrow(212, y0 + 45, 278, y0 + 45)
    s.box(570, y0, 250, 90, "evta", ["EVTA conv 7×7, stride 2", "출력 112×112×64", "출력 픽셀·블록마다 49 단계"], 12.5)
    s.arrow(502, y0 + 45, 568, y0 + 45)
    # channel bar
    bx, by = 880, y0 + 6
    s.text(bx, by - 2, "GEMM 한 단계의 입력 16채널:", 12.5)
    for c in range(16):
        s.rect(bx + c * 26, by + 8, 24, 28, "#7aa6d6" if c < 3 else "#e6e6e6", "#666666", 0.8)
        s.text(bx + c * 26 + 12, by + 27, "RGB"[c] if c < 3 else "0", 11, "middle")
    s.text(bx, by + 60, "7×7 = 49 tap × 입력 블록 1개 = 49 단계, 쓸모 있는 곱은 3/16", 12.5, color="#c0392b")
    s.text(bx, by + 78, "conv0 EVTA 9.13 ms (GEMM 하한 6.27 ms, 그중 약 5 ms가 0)", 12.5, color="#c0392b")
    # ---------------- B ----------------
    yb = ya + 200
    s.section(yb, "B. 지금 (-vta-conv0-pack): stride 행 2개와 7 tap을 채널로 접어 48채널(3 블록) 이미지로, 그 위에서 4×1 / stride 1 conv",
              color="#2f7a2a")
    y1 = yb + 50
    # padded grid
    gx, gy, cs = 60, y1 + 52, 32
    rows, cols = 9, 9
    s.text(gx, y1 + 22, "pad 3을 한 입력 (행 r, 열 c), 칸마다 RGB 3채널", 12.5, weight="bold")
    for r in range(rows):
        for c in range(cols):
            fill = "#ffffff"
            if r in (0, 1) and c < 7:
                fill = "#cfe8c8"
            elif r in (2, 3) and c < 7:
                fill = "#fff0b3"
            elif r in (4, 5) and c < 7:
                fill = "#f9d3e3"
            elif r in (6, 7) and c < 7:
                fill = "#d6e4f5"
            s.rect(gx + c * cs, gy + r * cs, cs, cs, fill, "#999999", 0.7)
        s.text(gx - 8, gy + r * cs + 22, str(r), 11, "end", color="#555555")
    for c in range(cols):
        s.text(gx + c * cs + cs / 2, gy - 6, str(c), 11, "middle", color="#555555")
    s.rect(gx, gy, 7 * cs, 7 * cs, "none", "#c0392b", 2.4)
    s.rect(gx, gy + 7 * cs, 7 * cs, cs, "none", "#c0392b", 1.4, dash="5 3")
    s.text(gx, gy + rows * cs + 22, "빨간 틀: 출력 (0, 0)의 7×7 창. 점선 행 7은 weight 0", 12, color="#c0392b")
    for k, (lbl, col) in enumerate([("q=0", "#cfe8c8"), ("q=1", "#fff0b3"), ("q=2", "#f9d3e3"), ("q=3", "#d6e4f5")]):
        s.rect(gx + 9 * cs + 14, gy + (2 * k) * cs + 6, 18, 2 * cs - 12, col, "#666666", 0.8)
        s.text(gx + 9 * cs + 38, gy + (2 * k) * cs + cs + 4, f"packed 행 {lbl}: 행 {2 * k}, {2 * k + 1}", 12)
    # packed pixel channels
    px, py = 560, y1 + 40
    s.text(px, y1 + 24, "packed 픽셀 (q, x) = 행 2q, 2q+1 × 열 2x .. 2x+6 × RGB", 12.5, weight="bold")
    groups = [(dh, j) for dh in (0, 1) for j in range(7)]
    cw = 18
    for gi, (dh, j) in enumerate(groups):
        for c in range(3):
            xx = px + (gi * 3 + c) * cw
            s.rect(xx, py, cw, 30, "#cfe8c8" if dh == 0 else "#a9d69e", "#666666", 0.6)
        s.text(px + gi * 3 * cw + 1.5 * cw, py + 46, f"{dh},{j}", 9.5, "middle", color="#333333")
    for c in range(6):
        s.rect(px + (42 + c) * cw, py, cw, 30, "#e6e6e6", "#666666", 0.6)
    s.text(px + 45 * cw, py + 46, "0 ×6", 10, "middle", color="#555555")
    s.text(px - 6, py + 46, "(dh, j)", 9.5, "end", color="#333333")
    for b in range(3):
        s.rect(px + b * 16 * cw, py - 4, 16 * cw, 38, "none", "#2f5f98", 1.8)
        s.text(px + b * 16 * cw + 8 * cw, py + 64, f"입력 블록 {b} (16채널)", 11.5, "middle", color="#2f5f98")
    s.text(px, py + 92, "채널 (dh·7 + j)·3 + c = 입력 (2q + dh, 2x + j, c).  42채널 + 0 6개 = 48 = 블록 3개.", 12.5)
    s.text(px, py + 112, "→ packed 이미지 115 × 112 × 48 (padded 230행 / 2, 열은 stride 2로 112)", 12.5)
    # conv on packed
    cy = py + 150
    s.box(px, cy, 270, 104, "new", ["CPU (NEON, 1회): 전처리와 같이", "thread마다 padded 행 2개만 만들어", "vqtbl4q_u8 표로 pack (§7.12)",
                                    "0 채움판 1.55 ms → 1.69 ms (OMP 1)"], 12)
    s.box(px + 310, cy, 290, 104, "evta", ["EVTA conv 4×1, stride 1, pad 0", "out(q, x) = Σ_t=0..3 packed(q+t, x) · W'(t)",
                                            "packed 행 q..q+3 = 입력 행 2q..2q+7", "단계: 4 tap × 3 블록 = 12 (이전 49)"], 12)
    s.arrow(px + 272, cy + 52, px + 308, cy + 52)
    s.box(px + 640, cy, 220, 104, "mem", ["weight W'[64][48][4]", "compile time에 7×7×3 weight를", "packed 순서로 다시 씀", "(행 7 자리는 0)"], 12)
    s.arrow(px + 638, cy + 52, px + 602, cy + 52, "k")
    # results table
    ty = gy + rows * cs + 70
    s.text(60, ty, "결과 (systolic 400 MHz)", 14, weight="bold")
    rows_t = [("", "이전 (0 채움 7×7)", "지금 (packed 4×1)"), ("출력 픽셀·블록당 GEMM 단계", "49", "12"),
              ("GEMM 하한", "6.27 ms", "1.63 ms"), ("conv0 EVTA 시간", "9.13 ms", "3.64 ms (pool과 합친 뒤 4.48 ms, 그림 6)"),
              ("Resnet18 single OMP 1", "34.5 ms", "29.2 ms"), ("출력", "", "모든 EVTA conv 출력이 byte 단위로 같다 (EVTA_DUMP_CONV)")]
    colx = [60, 330, 520]
    for i, r in enumerate(rows_t):
        yy = ty + 28 + i * 24
        if i == 0:
            s.rect(56, yy - 17, 1000, 24, "#f3f6fa", "none", 0)
        for k, v in enumerate(r):
            s.text(colx[k], yy, v, 12.5, weight="bold" if i == 0 else "normal")
    s.text(60, ty + 28 + 6 * 24 + 10, "조건이 맞지 않는 첫 conv(schedule tile이 출력을 나누지 못하거나 입력 tile이 inp buffer 1024 entry를 넘을 때)는"
           " A의 0 채움 경로로 간다. 그 함수들은 다른 모델·크기를 위해 남겼다.", 12, color="#555555")
    return s


# ---------------------------------------------------------------------------------------------------------------
@fig(14, "time_waterfall")
def fig14():
    W = 1460
    steps = [("10-29", "시작: conv0 EVTA, 층마다 transpose, elementwise는 CPU", None, 148.9, "§3"),
             ("10-31", "층 사이 EVTA layout 유지 (-vta-keep-layout)", 148.9, 91.1, "§7.1"),
             ("10-31", "conv + elemadd + relu를 EVTA 호출 하나로", 91.1, 67.1, "§7.2"),
             ("10-31", "maxpool을 EVTA에서", 67.1, 52.5, "§7.3"),
             ("11-01", "transpose를 16바이트 복사로", 52.5, 46.9, "§7.4"),
             ("11-01", "VTA buffer pool", 46.9, 44.3, "§7.5"),
             ("11-01", "CPU fc NEON", 44.3, 40.6, "§7.6"),
             ("11-02", "fc를 EVTA에서", 40.6, 39.9, "§7.8"),
             ("11-02", "입력 전처리를 복사 하나로", 39.9, 37.1, "§7.9"),
             ("11-02", "명령 record / replay", 37.1, 34.5, "§7.10"),
             ("11-02", "conv0 packed 입력 4×1", 34.5, 29.19, "§7.12"),
             ("11-02", "conv0 + maxpool 한 호출", 29.19, 28.83, "§7.13"),
             ("11-03", "ALU imm 연산 II = 1 (bitstream, 같은 날 28.98 기준)", 28.98, 27.32, "§7.15"),
             ("11-04", "RMW reset에 bias (bitstream, 같은 날 27.30 기준)", 27.30, 26.59, "§7.16"),
             ("11-04", "out 쓰기에 requantize (bitstream, 같은 날 26.61 기준)", 26.61, 25.49, "§7.18")]
    rh = 46
    H = 150 + len(steps) * rh + 150
    s = Svg(W, H, "그림 14. Resnet18 추론 한 장의 시간: 단계별로 줄어든 폭",
            ["single EVTA, systolic 400 MHz, OpenMP 1 thread, 보드 실측 중앙값(run 2~25). 단계마다 그 시점의 측정이라 앞 단계의 끝 값과"
             " 다음 단계의 시작 값이 조금 다를 수 있다(예: 91.1 / 90.9). 절은 evta_runtime_compare.md."])
    x0, scale = 520, 5.2
    y = 120
    for v in (0, 25, 50, 75, 100, 125, 150):
        s.line(x0 + v * scale, y - 12, x0 + v * scale, y + len(steps) * rh, "#e3e3e3", 1)
        s.text(x0 + v * scale, y - 18, f"{v} ms", 11, "middle", color="#777777")
    for i, (d, name, prev, now, sec) in enumerate(steps):
        yy = y + i * rh
        est = d == "예상"
        s.text(30, yy + 26, d, 12.5, weight="bold", color="#888888" if est else "#333333")
        s.text(84, yy + 26, name, 13, color="#777777" if est else "#222222")
        s.text(x0 - 12, yy + 26, sec, 11.5, "end", color="#2f5f98")
        s.rect(x0, yy + 8, now * scale, 28, "#9ec3e6" if not est else "#e2ecf6", "#4a6f99", 0.8, 2,
               dash="4 3" if est else None)
        if prev is not None:
            s.rect(x0 + now * scale, yy + 8, (prev - now) * scale, 28, "#cfe8c8" if not est else "#eef7ea", "#3c8a35", 1,
                   2, dash="4 3" if est else None)
            pct = (prev - now) / prev * 100
            lbl = f"{prev:g} → {now:g} ms  (−{prev - now:.2f} ms, −{pct:.0f} %)"
            if (prev - now) * scale > tw(lbl, 12) + 20:
                s.text(x0 + (prev + now) / 2 * scale, yy + 27, lbl, 12, "middle", "bold", "#2a6a24")
            else:
                s.text(x0 + prev * scale + 8, yy + 27, lbl, 12, color="#3c8a35" if not est else "#7a9a74")
        else:
            s.text(x0 + now * scale + 8, yy + 27, f"{now} ms", 12.5, weight="bold")
    yb = y + len(steps) * rh + 30
    s.lines(30, yb, [("합", "bold"),
                     "• 148.9 → 25.49 ms (5.8배). 가장 큰 것은 transpose 제거(−57.8 ms)와 CPU elementwise를 EVTA로 옮긴 것(−38.6 ms).",
                     "• 11-03 ~ 11-04 bitstream 세 단계(ALU II = 1, bias reset, requantize): EVTA 26.42 → 23.05 ms (−3.37 ms), 소프트웨어는 vtalib만.",
                     "• OMP 4로는 73.6 → 27.71 ms(11-02). multi4 4 copy 동시: 100회 약 3.9 s → 0.820 s(11-02, _sys),"
                     " 11-04 _rq 400 MHz에서 EVTA마다 23.44~23.56 ms, 610.4 GOPS."], 13)
    s.legend(30, H - 18, [("evta", "그 단계 뒤의 시간"), ("new", "그 단계로 줄어든 몫")])
    return s


# ---------------------------------------------------------------------------------------------------------------
@fig(6, "conv0_maxpool")
def fig06():
    W = 1460
    s = Svg(W, 1040, "그림 6. conv0 + maxpool 3×3/2/1: EVTA 호출 두 번 → 한 번 (pool을 acc 안에서)",
            ["conv0 출력 112×112×64 → maxpool → 56×56×64. 자세히: evta_runtime_compare.md §7.3 (maxpool_evta), §7.13 (convolution_wo_tr_pool, ConvPool)."])
    ya = 92
    s.section(ya, "A. 이전: conv0이 출력 전체를 DRAM에 STORE하고, maxpool 호출이 그것을 다시 LOAD한다")
    y0 = ya + 56
    s.rect(40, y0, 560, 200, C["evta"][0], C["evta"][1], 1.5, 8)
    s.text(52, y0 + 22, "EVTA 호출 1: conv0 (tile 16×16, thread 2)", 13.5, weight="bold", color="#2f5f98")
    flow(s, 54, y0 + 44, [("mem", ["LOAD", "packed 입력", "+ weight"], 100), ("evta", ["GEMM", "4 tap × 3 블록"], 110),
                          ("evta", ["ALU", "bias, shift,", "clip, relu"], 100), ("mem", ["STORE", "16×16×64"], 100)], 100, 70, 26, 12)
    s.text(54, y0 + 150, "3.64 ms", 13, weight="bold", color="#2f5f98")
    s.box(660, y0 + 40, 170, 110, "dram", ["DRAM", "conv0 출력", "112×112×64", "784 KB 쓰고 다시 읽음"], 12.5)
    s.arrow(600, y0 + 95, 658, y0 + 95, "b")
    s.rect(880, y0, 540, 200, C["evta"][0], C["evta"][1], 1.5, 8)
    s.text(892, y0 + 22, "EVTA 호출 2: maxpool_evta (tile 3행 × 56열)", 13.5, weight="bold", color="#2f5f98")
    flow(s, 894, y0 + 44, [("mem", ["LOAD", "창 + halo"], 90), ("evta", ["GEMM", "단위행렬로", "acc에 복사"], 100),
                           ("evta", ["ALU MAX", "창 위치마다 1개", "= 9개"], 120), ("mem", ["STORE", "pool 출력"], 90)], 90, 70, 22, 12)
    s.text(894, y0 + 150, "1.19 ms", 13, weight="bold", color="#2f5f98")
    s.arrow(832, y0 + 95, 892, y0 + 80, "b")
    s.text(40, y0 + 228, "합 4.83 ms. conv0 출력의 DRAM 왕복(쓰기 0.36 ms 분량), maxpool 호출의 고정 비용과 identity GEMM 0.33 ms.", 13)
    # ---------------- B ----------------
    yb = ya + 320
    s.section(yb, "B. 지금 (-vta-fuse-pool): conv tile에 pool halo를 더 계산하고, relu 뒤 acc 안에서 MAX 두 단계, pool 결과만 STORE", color="#2f7a2a")
    y1 = yb + 50
    # tile picture
    gx, gy, cs = 70, y1 + 40, 22
    n = 17
    s.text(gx, y1 + 22, "conv 출력 tile (thread 하나, 출력 블록 하나)", 12.5, weight="bold")
    for r in range(n):
        for c in range(n):
            halo = r == 0 or c == 0
            fill = "#f6d7a8" if halo else "#dce8f5"
            s.rect(gx + c * cs, gy + r * cs, cs, cs, fill, "#9aa9bd", 0.5)
    # pool windows outline for pooled (0,0) and (0,1)
    s.rect(gx, gy, 3 * cs, 3 * cs, "none", "#c0392b", 2.4)
    s.rect(gx + 2 * cs, gy, 3 * cs, 3 * cs, "none", "#3c8a35", 2.0, dash="5 3")
    s.text(gx, gy + n * cs + 22, "주황: halo (위 1행, 왼쪽 1열, 다시 계산, 이미지 경계 tile은 pad라서 없음)", 12, color="#a0601a")
    s.text(gx, gy + n * cs + 40, "빨강 / 초록: pool 창 (0,0), (0,1) — 3×3, stride 2, 서로 1열 겹침", 12, color="#c0392b")
    s.text(gx, gy + n * cs + 58, "16×16 + halo = 17×17 → pool 8×8. GEMM +13 % (1.63 → 1.84 ms)", 12)
    # steps
    sx = 520
    steps = [("evta", ["① LOAD packed 입력 (halo 행 포함), weight"]),
             ("evta", ["② GEMM: 17×17(경계 tile은 16×16) 출력의 conv"]),
             ("evta", ["③ ALU: bias, shift, clip, relu → 값 ≥ 0"]),
             ("new", ["④ 행 방향 MAX (명령 2개, 제자리):", "중심 열 2j+halo ← max(왼쪽 열, 중심, 오른쪽 열)"]),
             ("new", ["⑤ pool 영역 reset (bias entry 뒤 8×8×블록)", "⑥ 열 방향 MAX (명령 3개, dy = 0, 1, 2):",
                      "pool(i, j) ← max(pool, 행 2i+halo+dy−1의 중심 열)"]),
             ("mem", ["⑦ STORE pool 8×8만 → pool 출력 56×56×64의 해당 자리"])]
    yy = y1 + 30
    for kind, lines in steps:
        h = 34 + 18 * (len(lines) - 1)
        s.box(sx, yy, 560, h, kind, lines, 12.5, align="start")
        if kind != "mem":
            s.arrow(sx + 280, yy + h, sx + 280, yy + h + 12, "a", 1.2)
        yy += h + 14
    s.lines(sx + 590, y1 + 40, [("ALU 명령 수 (thread·tile당)", "bold"),
                                "1판: 창 위치 9 × 출력 블록 2 + reset 1 = 19개",
                                "→ ALU 명령 고정 비용 때문에 이득 0",
                                "2판(지금): 행 2 + reset 1 + 열 3 = 6개,",
                                "명령 하나가 uop 2개로 출력 블록 2개를",
                                "",
                                ("값이 같은 이유", "bold"),
                                "relu 뒤라 값 ≥ 0: 0에서 시작한 max가",
                                "CPU의 '경계 안 값만의 max'와 같다",
                                "EVTA_RES_CHECK 0 / 200,704 불일치"], 12.5)
    yr = max(yy, gy + n * cs + 70) + 20
    s.lines(40, yr, [("결과", "bold"),
                     "conv0 + maxpool EVTA 4.83 → 4.48 ms, Resnet18 single OMP 1 29.19 → 28.83 ms, multi4 copy당 29.76 → 29.37 ms.",
                     "conv0 출력 784 KB의 DRAM 쓰기와 읽기, maxpool 호출이 없어진다. 모양이 맞지 않으면 A로 돈다 (컴파일러 findPoolFusions, vtalib 둘 다)."], 13)
    return s


# ---------------------------------------------------------------------------------------------------------------
@fig(3, "activation_layout")
def fig03():
    W = 1460
    s = Svg(W, 790, "그림 3. 층 사이 activation의 layout: 층마다 NHWC ↔ EVTA layout → EVTA layout 그대로",
            ["EVTA는 16채널 블록 단위로 LOAD한다(element 하나 = 한 픽셀의 16채널). 자세히: evta_runtime_compare.md §7.1, §7.4, "
             "VTASave.cpp findPackedActivations."])
    # layouts illustration
    y0 = 96
    s.section(y0, "두 layout (H = W = 2, C = 32의 예: 블록 b0 = 채널 0~15, b1 = 16~31)")
    ly = y0 + 50
    s.text(40, ly + 14, "NHWC: 픽셀마다 32채널이 이어진다", 13, weight="bold", color="#c06a10")
    order_nhwc = [("p0", "b0"), ("p0", "b1"), ("p1", "b0"), ("p1", "b1"), ("p2", "b0"), ("p2", "b1"), ("p3", "b0"), ("p3", "b1")]
    order_evta = [("p0", "b0"), ("p1", "b0"), ("p2", "b0"), ("p3", "b0"), ("p0", "b1"), ("p1", "b1"), ("p2", "b1"), ("p3", "b1")]
    colb = {"b0": "#cfe0f3", "b1": "#f7d9b5"}
    for i, (p_, b) in enumerate(order_nhwc):
        s.rect(40 + i * 82, ly + 26, 80, 34, colb[b], "#777777", 0.8)
        s.text(40 + i * 82 + 40, ly + 48, f"{p_} ch {'0-15' if b == 'b0' else '16-31'}", 11, "middle")
    s.text(760, ly + 14, "EVTA layout N{C/16}HW16: 블록마다 모든 픽셀이 이어진다", 13, weight="bold", color="#2f5f98")
    for i, (p_, b) in enumerate(order_evta):
        s.rect(760 + i * 82, ly + 26, 80, 34, colb[b], "#777777", 0.8)
        s.text(760 + i * 82 + 40, ly + 48, f"{b} {p_}", 11, "middle")
    s.text(40, ly + 84, "주소 순서(왼쪽 → 오른쪽). 16채널 = 16바이트 = EVTA element 하나. CPU 연산(NHWC)과 EVTA LOAD/STORE(N{C/16}HW16)가 원하는 순서가 다르다.", 12.5)
    # ---------------- A ----------------
    ya = y0 + 160
    s.section(ya, "A. 이전: activation은 NHWC로 DRAM에. EVTA conv마다 CPU가 앞뒤로 transpose한다")
    y1 = ya + 54
    items = [("mem", ["NHWC", "(DRAM)"], 90), ("cpu", ["transpose", "nhwc2vtaio", "+ flush"], 120), ("mem", ["EVTA layout", "임시 buffer"], 110),
             ("evta", ["EVTA conv"], 100), ("mem", ["EVTA layout", "임시 buffer"], 110), ("cpu", ["invalidate +", "transpose", "vtaio2nhwc"], 120),
             ("mem", ["NHWC", "(DRAM)"], 90), ("cpu", ["elemadd / relu /", "maxpool (NHWC)"], 140), ("mem", ["NHWC", "→ 다음 conv"], 100)]
    flow(s, 40, y1, items, 100, 70, 22, 12)
    s.lines(40, y1 + 110, ["• conv 21개 × 앞뒤 transpose: OMP 1에서 66.1 ms (vtaio2nhwc 41.8, nhwc2vtaio 17.8, conv0 0 채움 6.5), 추론의 44 %.",
                           "• 매번 임시 buffer 할당·해제, CPU cache flush / invalidate가 따라온다."], 13)
    # ---------------- B ----------------
    yb = ya + 230
    s.section(yb, "B. 지금 (-vta-keep-layout): EVTA가 STORE한 layout을 다음 EVTA 호출이 그대로 LOAD한다", color="#2f7a2a")
    y2 = yb + 54
    items = [("new", ["입력 복사 1회", "NCHW float →", "packed EVTA layout"], 150), ("evta", ["conv0+pool"], 100),
             ("mem", ["N{C/16}HW16", "(DRAM)"], 110), ("evta", ["conv"], 70), ("mem", ["N{C/16}HW16"], 110),
             ("evta", ["conv + residual", "+ relu"], 120), ("mem", ["N{C/16}HW16"], 110), ("cpu", ["avgpool", "(블록을 batch로)"], 120),
             ("evta", ["fc"], 60)]
    flow(s, 40, y2, items, 100, 70, 22, 12)
    s.lines(40, y2 + 110, ["• layout을 바꾸는 곳은 네트워크 입력 한 곳뿐이다. CPU에 남은 avgpool도 16채널 블록을 batch로 보고 EVTA layout에서 바로 한다.",
                           "• 그 뒤 elemadd + relu, maxpool, fc까지 EVTA로 옮겨(그림 4, 6, 8) layout을 바꿀 일이 없어졌다.",
                           "• 효과: Resnet18 single OMP 4 73.6 → 56.4 ms (OMP 1 148.9 → 91.1 ms), multi4 100회 3.9 → 2.8 s.",
                           "  남은 transpose는 다음 날 16바이트 복사로 바꿨다(§7.4, conv0 0 채움 6.8 → 0.2 ms; 지금은 packed 입력 복사로 대체)."], 13)
    s.legend(40, 770, [("cpu", "CPU"), ("evta", "EVTA 호출"), ("new", "새로 합친 CPU 단계"), ("mem", "DRAM / buffer")])
    return s


# ---------------------------------------------------------------------------------------------------------------
@fig(7, "input_preprocess")
def fig07():
    W = 1460
    s = Svg(W, 740, "그림 7. 입력 전처리: CPU 단계 네 번 → NEON 복사 한 번",
            ["Resnet18 입력 224×224×3 float NCHW. 자세히: evta_runtime_compare.md §7.9 (preprocess_nhwc2vtaio_zerofill), §7.12 (preprocess_conv0pack), "
             "VTASave.cpp findInputPreprocess."])
    ya = 92
    s.section(ya, "A. 이전: 연산마다 이미지 전체를 읽고 쓴다 (단계마다 DRAM 왕복, 일부는 OpenMP 없음)")
    y0 = ya + 54
    items = [("mem", ["float NCHW", "224×224×3"], 110), ("cpu", ["quantize", "float → int8"], 110), ("cpu", ["elemsub", "채널 평균"], 100),
             ("cpu", ["elemdiv", "채널 표준편차"], 110), ("cpu", ["transpose +", "0 채움 3→16ch", "(OpenMP 없음)"], 140),
             ("mem", ["EVTA layout", "230×230×16"], 120), ("evta", ["conv0", "7×7/2"], 90)]
    flow(s, 40, y0, items, 100, 76, 26, 12)
    s.lines(40, y0 + 112, ["• OMP 1에서 전처리 3.7 ms + 0 채움 6.5 ms (10-29). OMP 4에서 2.2 ms + 6.4 ms.",
                           "• elemsub / elemdiv는 채널마다 같은 상수(평균, 표준편차)로 int8 값을 바꾼다: 결과는 (채널, quantize 값 256가지)에만 달렸다."], 13)
    yb = ya + 250
    s.section(yb, "B. 지금: 픽셀 16개씩 NEON으로 한 번에 (thread마다 padded 행 2개만 만들고 바로 pack)", color="#2f7a2a")
    y1 = yb + 54
    s.box(40, y1, 130, 90, "mem", ["float NCHW", "224×224×3"], 12.5)
    s.rect(210, y1 - 10, 860, 220, C["new"][0], C["new"][1], 1.6, 8)
    s.text(222, y1 + 10, "preprocess_conv0pack (CPU NEON, OpenMP: thread마다 packed 행)", 13.5, weight="bold", color="#2a6a24")
    sub = [("cpu", ["quantize", "vcvtaq + 포화 narrow", "(std::round와 같음)"], 160),
           ("cpu", ["표 [C][256]", "vqtbl4q / vqtbx4q", "= elemsub·elemdiv"], 160),
           ("cpu", ["padded 행 2개", "(2q, 2q+1)", "thread 지역 buffer"], 150),
           ("cpu", ["pack", "vqtbl4q_u8로 42채널 +0 6", "→ packed 행 q"], 190)]
    flow(s, 226, y1 + 30, sub, 150, 86, 22, 12)
    s.text(226, y1 + 150, "표는 compile time에 CPU elemsub / elemdiv와 같은 float 계산으로 만들어 weights 파일에 넣는다 (bit 단위로 같다).", 12)
    s.text(226, y1 + 170, "루프 값(d, vi[])을 지역 변수로 옮겨 aliasing 때문에 다시 읽던 것을 없앴다: pack 0.88 → 0.35 ms.", 12)
    s.box(1110, y1, 160, 90, "mem", ["packed EVTA layout", "115×112×48"], 12.5)
    s.box(1300, y1, 120, 90, "evta", ["conv0 + pool", "4×1"], 12.5)
    s.arrow(172, y1 + 45, 222, y1 + 70)
    s.arrow(1072, y1 + 72, 1108, y1 + 45)
    s.arrow(1272, y1 + 45, 1298, y1 + 45)
    s.lines(40, y1 + 250, [("결과", "bold"),
                           "• 1단계(§7.9, 0 채움판과 합침): 2.2 + 0.2 ms → 0.37 ms (OMP 4). Resnet18 single OMP 1 39.92 → 37.08 ms.",
                           "• 2단계(§7.12, packed 입력): 행 단위로 바꿔 OMP 4에서 0.58 → 0.44 ms, OMP 1 단독 1.69 ms (그중 약 1.35 ms가 quantize 계산).",
                           "• Resnet50, VGG16은 표 없이 quantize → transpose → 0 채움 경로를 같은 방식으로 한다."], 13)
    return s


# ---------------------------------------------------------------------------------------------------------------
@fig(2, "change_map")
def fig02():
    W = 1460
    s = Svg(W, 870, "그림 2. 무엇을 어디서 바꿨나: 컴파일러 앞단은 그대로, backend 마지막 단계·runtime·하드웨어를 바꿨다",
            ["초록 = 이번 작업에서 바꾼 곳, 회색 = 그대로. 화살표는 모델이 실행 파일과 bitstream이 되는 흐름."])
    y0 = 100
    s.section(y0, "컴파일 (보드의 simulator 빌드 Debug2_remote, model-compiler / VTASaverTest)")
    y1 = y0 + 50
    front = [("mem", ["모델", "ONNX / Caffe2"], 120), ("mem", ["Glow importer"], 120), ("mem", ["graph 최적화", "(NEST-C)"], 130),
             ("mem", ["양자화", "profile YAML"], 120), ("mem", ["lowering, IRGen"], 130)]
    xs = flow(s, 40, y1, front, 120, 70, 24, 12.5)
    s.text(40, y1 + 96, "바꾼 것 없음 (빌드 옵션 VTA_RESNET18_WITH_SKIPQUANT0 기본값만 OFF로: conv0을 int8로 EVTA에)", 12.5, color="#666666")
    bx = 40 + sum(w for _, w in xs) + 24 * 5
    s.arrow(bx - 22, y1 + 35, bx - 2, y1 + 35)
    s.rect(bx, y1 - 14, W - bx - 30, 225, C["new"][0], C["new"][1], 1.8, 8)
    s.text(bx + 12, y1 + 6, "VTA backend: VTASave.cpp (bundle .cpp + weights 생성)", 13.5, weight="bold", color="#2a6a24")
    s.lines(bx + 12, y1 + 30, ["findPackedActivations   -vta-keep-layout       층 사이 EVTA layout",
                               "findResidualFusions     -vta-fuse-residual     conv + add + relu",
                               "findEvtaMaxPools        -vta-evta-maxpool      maxpool EVTA",
                               "(fc)                    -vta-evta-fc           fc EVTA, weight packing",
                               "findInputPreprocess     -vta-fuse-preprocess   전처리 표 [C][256]",
                               "getConv0Pack            -vta-conv0-pack        conv0 weight 재배열",
                               "findPoolFusions         -vta-fuse-pool         conv0 + maxpool",
                               "flush / invalidate 호출, VTASchedules.h (conv0 schedule)"], 12, mono=True, lh=24)
    # runtime
    y2 = y1 + 270
    s.arrow(700, y2 - 46, 700, y2 - 2, "b", 1.6)
    s.text(712, y2 - 20, "생성된 bundle .cpp가 vtalib 함수를 부른다 (보드에서 함께 빌드)", 12, color="#2f5f98")
    s.section(y2, "runtime: vtalib (vta/vtalib 와 vtalib-zcu102-systolic, 보드 vtabundles_xcmpl에서 bundle과 함께 빌드)")
    y3 = y2 + 50
    rt = [("VTABundle.cpp", ["convolution_wo_tr_res / _pool", "maxpool_evta, fc_evta_packed",
                             "VTARequant, bias reset", "EVTA_DUMP_CONV, EVTA_RES_CHECK"]),
          ("CPUBundle_generic.cpp", ["transpose 16 B 복사", "cpuFullyConnected (NEON)", "preprocess_*, conv0pack_*"]),
          ("cmd_queue.h", ["record / replay (EVTA_REPLAY)", "EVTA_REPLAY_LOG, _DUMP", "멈춤 진단 옵션"]),
          ("api.cc", ["VTA buffer pool", "(EVTA_BUF_POOL_MB)"]),
          ("pynq_driver.cc", ["flush 뒤 dsb sy", "run 시간 제한 + hang dump", "bitstream EVTA 수 검사", "done 경쟁 수정"])]
    x = 40
    for name, lines in rt:
        w = 270 if name != "api.cc" else 190
        s.box(x, y3, w, 150, "new", lines, 12, title=name, tsize=13)
        x += w + 18
    # hardware
    y4 = y3 + 190
    s.section(y4, "하드웨어: bitstream (PC docker의 Vivado 2020.1)")
    y5 = y4 + 50
    hw = [("new", ["vivado.tcl", "Y219 결함 행 fence,", "FREQ, HP 333 MHz 분리,", "multi4 Y219 배선"], 260),
          ("new", ["HLS vta.cc", "systolic: GEMM은 uop만,", "ALU imm 루프 II = 1,", "ALU 결과에 requantize"], 270),
          ("new", ["RTL gemm_sa", "16×16 DSP48E2 WS 어레이,", "RMW, weight 채우기,", "reset에 bias, out에 requantize"], 270),
          ("new", ["ISA: 명령의 pad 비트", "GEMM bias_reset, rq_*,", "ALU rq_* (opcode 2비트,", "buffer 크기는 그대로)"], 250),
          ("mem", ["fetch / load / store", "그대로"], 240)]
    x = 40
    for kind, lines, w in hw:
        s.box(x, y5, w, 100, kind, lines, 12)
        x += w + 18
    s.arrow(700, y4 - 30, 700, y4 - 2, "b", 1.6)
    s.text(712, y4 - 12, "bundle의 EVTA 호출이 명령·uop을 만들어(또는 replay해) EVTA를 돌린다", 12, color="#2f5f98")
    s.legend(40, 850, [("new", "바꾼 곳"), ("mem", "그대로")])
    return s


# ---------------------------------------------------------------------------------------------------------------
@fig(8, "fc")
def fig08():
    W = 1460
    s = Svg(W, 1030, "그림 8. fc 512 → 1000: CPU 띄엄띄엄 읽기 → CPU NEON 순서대로 → EVTA GEMM (weight는 compile time에 EVTA layout)",
            ["Resnet18 마지막 층. weight int8 512 × 1000 = 512 KB, offset 0, 출력은 오른쪽 shift. 자세히: evta_runtime_compare.md §7.6, §7.8,"
             " vtalib cpuFullyConnected, fullyconnected_evta_packed."])
    ya = 92
    s.section(ya, "A1. 처음: 출력 하나씩, weight 열을 1000바이트 간격으로 (OMP 1 4.4 ms)")
    y0 = ya + 50
    gx, gy, cw, ch = 60, y0 + 10, 26, 16
    for r in range(10):
        for c in range(16):
            s.rect(gx + c * cw, gy + r * ch, cw, ch, "#f4f4f4", "#bbbbbb", 0.5)
    for r in range(10):
        s.rect(gx + 2 * cw, gy + r * ch, cw, ch, "#f3a65a", "#a0601a", 0.8)
    s.text(gx, gy + 10 * ch + 18, "weight [입력 512][출력 1000] (그림은 일부): 출력 o 하나에 열 o를 위에서 아래로", 12)
    s.lines(gx + 16 * cw + 30, gy + 20, ["• 앞 conv들이 지나가면 weight 512 KB는 cache에 없다.",
                                         "• 행 사이 간격 1000 B: cache line 64 B 중 1 B만 쓰고, prefetch도 안 맞는다.",
                                         "• 출력 1000개 × 입력 512개를 이렇게 읽는다."], 13)
    yb = ya + 270
    s.section(yb, "A2. 11-01: 행을 순서대로 8행씩, 출력 64개의 합을 NEON register에, 다음 8행 prefetch (0.74 ms, bit 단위로 같다)", color="#c06a10")
    y1 = yb + 50
    gy = y1 + 10
    for r in range(10):
        for c in range(16):
            fill = "#9bd18f" if r < 8 and c < 8 else ("#e0f0da" if r >= 8 and c < 8 else "#f4f4f4")
            s.rect(gx + c * cw, gy + r * ch, cw, ch, fill, "#bbbbbb", 0.5)
    s.text(gx, gy + 10 * ch + 18, "초록: 이번에 읽는 8행 × 64출력 (그림은 8열), 연한 초록: prefetch 중인 다음 8행", 12)
    s.lines(gx + 16 * cw + 30, gy + 20, ["• 연속 주소를 읽어 DRAM 대역을 다 쓴다.",
                                         "• 정수 합이라 순서가 바뀌어도 결과가 같다 (크기·offset·thread 576가지 비교).",
                                         "• OMP 1 4.43 → 0.74 ms, OMP 4 1.20 → 0.77 ms."], 13)
    yc = yb + 270
    s.section(yc, "B. 11-02 (-vta-evta-fc): EVTA GEMM. weight와 bias는 compile time에 EVTA layout으로 weights 파일에", color="#2f7a2a")
    y2 = yc + 50
    s.box(40, y2, 200, 90, "new", ["compile time (VTASave.cpp)", "weight → 16×16 블록들", "bias → acc layout"], 12.5)
    s.rect(280, y2 - 10, 1000, 200, C["evta"][0], C["evta"][1], 1.6, 8)
    s.text(292, y2 + 10, "EVTA 호출 fullyconnected_evta_packed (기록 / replay)", 13.5, weight="bold", color="#2f5f98")
    flow(s, 294, y2 + 30, [("cpu", ["입력 512 B", "복사 (CPU)"], 110), ("mem", ["LOAD", "입력, bias"], 100),
                           ("mem", ["LOAD weight", "덩어리 k+1", "(wgt buffer 반)"], 130), ("evta", ["GEMM", "덩어리 k", "→ acc"], 110),
                           ("evta", ["ALU MIN/MAX/", "ADD/SHR", "= std::round"], 130), ("mem", ["STORE", "1000 B"], 90),
                           ("cpu", ["출력 복사", "(CPU)"], 90)], 100, 80, 18, 11.5)
    s.text(294, y2 + 140, "weight LOAD와 GEMM이 덩어리마다 겹친다. ALU shift 인자는 −16..15라 SHR 31을 쓸 수 없어, 반올림에 쓰는 부호는 MIN / MAX로 구한다.", 12)
    s.text(294, y2 + 160, "batch 1, offset 0, scale 비가 2의 거듭제곱, 덩어리마다 출력 블록 ≥ 8일 때만 (Resnet50 2048→1000, VGG16 512→10은 CPU fc).", 12)
    s.arrow(242, y2 + 45, 292, y2 + 70)
    s.lines(40, y2 + 230, [("결과", "bold"), "fc 0.74 → 0.15 ms (입력 복사, EVTA 실행, 출력 복사 포함). Resnet18 single OMP 1 40.6 → 39.9 ms.",
                           "EVTA_RES_CHECK와 같은 실행 중 비교로 0 / 1000 불일치. 사용자 의도: CPU가 약한 보드(Kria, MicroBlaze)를 위해 CPU 함수를 줄인다."], 13)
    return s


# ---------------------------------------------------------------------------------------------------------------
@fig(9, "evta_call_runtime")
def fig09():
    W = 1460
    s = Svg(W, 770, "그림 9. EVTA 호출 하나의 CPU 쪽 일: 매번 할당·생성 → pool에서 재사용, 기록한 명령을 replay",
            ["conv 하나를 부를 때 CPU가 하는 일. 자세히: evta_runtime_compare.md §2.2, §7.5, §7.10, evta_runtime_aot.md §5.4,"
             " vtalib api.cc, cmd_queue.h ReplayBegin / RunReplay."])
    ya = 92
    s.section(ya, "A. 이전 (10-29): 호출마다 buffer를 새로 잡고, tile마다 명령과 uop을 CPU가 만든다 (JIT)")
    y0 = ya + 50
    items = [("cpu", ["VTABufferAlloc", "입력, 출력", "(cma_alloc ioctl)"], 140), ("cpu", ["transpose", "+ flush"], 100),
             ("cpu", ["conv_kernel × tile", "LOAD/GEMM/ALU/STORE", "명령과 uop 생성"], 170), ("cpu", ["명령·uop을", "queue buffer에", "복사 + flush"], 130),
             ("evta", ["MMIO 시작", "EVTA 실행"], 110), ("cpu", ["완료 폴링", "(done 레지스터)"], 120),
             ("cpu", ["invalidate", "+ transpose"], 110), ("cpu", ["VTABufferFree", "(cma_free ioctl)"], 130)]
    flow(s, 40, y0, items, 120, 80, 20, 12)
    s.lines(40, y0 + 120, ["• Resnet18 한 번(OMP 1): buffer 할당·해제·flush 4.5 ms, 명령 생성 2.5 ms (그 동안 EVTA는 쉰다).",
                           "• 입력이 같은 모양이면 매번 같은 명령이 나온다: 바뀌는 것은 buffer 주소뿐이다."], 13)
    yb = ya + 250
    s.section(yb, "B. 지금: buffer pool (§7.5) + record / replay (§7.10)", color="#2f7a2a")
    y1 = yb + 50
    s.box(40, y1, 200, 90, "new", ["VTABufferAlloc", "크기별 free list에서 꺼냄", "(첫 run만 ioctl)"], 12)
    s.box(280, y1, 230, 90, "new", ["ReplayBegin(key)", "key = 호출 종류와 스칼라 인자", "(모양, shift, EVTA 번호 등)"], 12)
    s.arrow(242, y1 + 45, 278, y1 + 45)
    # record path
    s.rect(560, y1 - 20, 860, 120, "#fff8ec", "#c06a10", 1.2, 8, "6 4")
    s.text(572, y1, "처음 본 key: 기록 (첫 추론에서, 호출 종류마다 한 번)", 12.5, weight="bold", color="#a0601a")
    flow(s, 572, y1 + 14, [("cpu", ["명령·uop 생성", "(A와 같음)"], 140), ("cpu", ["segment별로", "전용 buffer에 복사"], 160),
                           ("cpu", ["relocation 표", "LOAD/STORE마다", "어느 인자 buffer + offset"], 200), ("evta", ["실행"], 80)], 120, 70, 18, 11.5)
    s.arrow(512, y1 + 30, 570, y1 + 30, "o")
    # replay path
    y2 = y1 + 140
    s.rect(560, y2 - 20, 860, 120, "#eef7ea", "#3c8a35", 1.2, 8, "6 4")
    s.text(572, y2, "기록이 있는 key: replay (그 뒤 모든 호출)", 12.5, weight="bold", color="#2a6a24")
    flow(s, 572, y2 + 14, [("new", ["buffer 주소가", "바뀌었으면만", "dram_base 고침 + flush"], 190), ("evta", ["segment마다", "MMIO 시작"], 130),
                           ("cpu", ["완료 폴링"], 110)], 120, 70, 18, 11.5)
    s.arrow(512, y1 + 60, 570, y2 + 40, "g")
    s.box(280, y2 + 10, 230, 60, "new", ["VTABufferFree", "free list에 남김 (최대 32 MB)"], 12)
    s.lines(40, y2 + 140, [("결과", "bold"),
                           "• buffer pool: run당 Alloc 23번 + Free 23번 2.7 → 0.05 ms. Resnet18 single OMP 1 46.9 → 44.3 ms.",
                           "• replay: Resnet18 single OMP 1 37.1 → 34.5 ms, multi4 copy당 37.6 → 35.0 ms. bit 단위로 같다. EVTA_REPLAY=0으로 끈다.",
                           "• 재사용 buffer에는 이전 내용이 남아 있다(결과에는 영향 없음을 golden으로 확인)."], 13)
    return s


# ---------------------------------------------------------------------------------------------------------------
@fig(10, "multi4_hang_dsb")
def fig10():
    W = 1460
    s = Svg(W, 920, "그림 10. multi4 멈춤의 원인: cache flush가 끝나기 전에 EVTA 시작 MMIO가 나간다 → dsb sy로 막는다",
            ["EVTA 4개를 동시에 돌릴 때 copy당 약 400회 추론에 한 번 멈췄다(1×1 downsample 바로 다음 3×3 stride-2 conv)."
             " 자세히: evta_runtime_compare.md §7.7, vtalib pynq_driver.cc VTAFlushCache."])
    ya = 92
    s.section(ya, "A. 이전: libcma의 cma_flush_cache는 dc civac loop뿐, dsb가 없다")
    y0 = ya + 60
    lanes = [("CPU", y0), ("cache → DRAM", y0 + 70), ("EVTA fetch", y0 + 140)]
    for name, yy in lanes:
        s.text(150, yy + 24, name, 13, "end", "bold")
        s.line(165, yy + 20, 1400, yy + 20, "#dddddd", 1)
    s.box(180, y0, 220, 40, "cpu", ["명령·uop을 queue buffer에 씀"], 12)
    s.box(410, y0, 260, 40, "cpu", ["cma_flush_cache: dc civac × N"], 12)
    s.box(680, y0, 180, 40, "cpu", ["MMIO: EVTA 시작"], 12)
    s.box(420, y0 + 70, 560, 40, "mem", ["정리된 line이 DRAM에 닿는 중"], 12)
    s.text(990, y0 + 95, "DRAM이 4 EVTA로 바쁘면 길어진다", 12, color="#555555")
    s.box(880, y0 + 140, 330, 40, "gone", ["fetch가 덜 쓰인 명령(앞 run 것)을 읽음 → token 어긋남 → 멈춤"], 12)
    s.arrow(845, y0 + 42, 930, y0 + 140, "r", 1.6)
    s.text(600, y0 + 205, "Arm에서 dc civac는 dsb 뒤에야 끝났다고 보장된다. 그 전에 Device 쓰기(MMIO)가 먼저 나갈 수 있다.", 13, color="#c0392b")
    yb = ya + 300
    s.section(yb, "B. 지금: VTAFlushCache / VTAInvalidateCache가 libcma 다음에 dsb sy (EVTA_NO_DSB=1이면 뺀다)", color="#2f7a2a")
    y1 = yb + 60
    for name, yy in [("CPU", y1), ("cache → DRAM", y1 + 70), ("EVTA fetch", y1 + 140)]:
        s.text(150, yy + 24, name, 13, "end", "bold")
        s.line(165, yy + 20, 1400, yy + 20, "#dddddd", 1)
    s.box(180, y1, 220, 40, "cpu", ["명령·uop을 queue buffer에 씀"], 12)
    s.box(410, y1, 260, 40, "cpu", ["cma_flush_cache: dc civac × N"], 12)
    s.box(680, y1, 200, 40, "new", ["dsb sy: 정리 끝까지 대기"], 12)
    s.box(890, y1, 180, 40, "cpu", ["MMIO: EVTA 시작"], 12)
    s.box(420, y1 + 70, 450, 40, "mem", ["모든 line이 DRAM에 닿음"], 12)
    s.box(1080, y1 + 140, 300, 40, "new", ["fetch가 새 명령을 읽음"], 12)
    s.arrow(980, y1 + 42, 1100, y1 + 140, "g", 1.6)
    yt = yb + 290
    s.text(40, yt, "원인을 가른 실험 (systolic multi4 Resnet18, copy당 100회씩 반복)", 13.5, weight="bold")
    rows = [("run 앞에서 하는 일", "결과", "뜻"),
            ("없음", "약 400회 안에 멈춤", "대조"),
            ("flush 뒤 5~40 µs 바쁜 대기", "2000회 0번", "flush 뒤의 틈이 문제"),
            ("5 µs 대기를 flush 앞에", "멈춤", "EVTA 쪽 settle 문제가 아님"),
            ("명령 buffer를 반쪽씩 번갈아 (대기 없음)", "멈춤", "같은 주소 재사용 문제가 아님"),
            ("flush 뒤 dsb sy (대기 없음)", "4000회 0번", "고침 (비용은 측정 차이 안)")]
    for i, r in enumerate(rows):
        yy = yt + 28 + i * 24
        if i == 0:
            s.rect(36, yy - 17, 1000, 24, "#f3f6fa", "none", 0)
        for k, v in enumerate(r):
            s.text([40, 420, 640][k], yy, v, 12.5, weight="bold" if i == 0 else "normal",
                   color="#2a6a24" if i == 5 else "#222222")
    return s


# ---------------------------------------------------------------------------------------------------------------
@fig(11, "evta_block")
def fig11():
    W = 1460
    s = Svg(W, 1110, "그림 11. EVTA 한 개의 블록도: HLS GEMM, 클럭 하나 → systolic gemm_sa, EVTA 400 MHz / HP 333 MHz",
            ["VTA의 4모듈 DAE(fetch, load, compute, store)는 그대로다. 자세히: evta_systolic_design.md, evta_systolic.md §8, vta_synth_notes.md §5."])

    def evta(y, systolic):
        s.box(40, y + 40, 150, 330, "dram", ["DRAM", "", "명령", "uop", "inp / wgt", "bias", "out"], 12.5)
        s.box(220, y + 40, 120, 330, "mem", ["HP 포트", "S_AXI_HP0", "(AXI)", "", "333 MHz" if systolic else "EVTA와", "" if systolic else "같은 클럭"], 12)
        s.arrow(192, y + 205, 218, y + 205, "a", 1.4, both=True)
        mx = 370
        s.box(mx, y + 40, 140, 80, "hls", ["fetch", "명령을 읽어", "queue로 나눔"], 12)
        s.box(mx + 180, y + 140, 200, 120, "hls", ["load", "inp, wgt LOAD", "(pad 포함)"], 12)
        s.box(mx + 440, y + 40, 330 if systolic else 300, 320, "hls", [], 12)
        s.text(mx + 455, y + 62, "compute", 13, weight="bold")
        s.box(mx + 830 if systolic else mx + 800, y + 140, 160, 120, "hls", ["store", "out STORE"], 12)
        # queues
        s.arrow(mx + 140, y + 70, mx + 438, y + 70, "p", 1.2)
        s.path(f"M{mx + 70},{y + 122} L{mx + 70},{y + 200} L{mx + 178},{y + 200}", "p", 1.2, arrow="p")
        s.path(f"M{mx + 140},{y + 50} L{(mx + 910) if systolic else (mx + 880)},{y + 50} L{(mx + 910) if systolic else (mx + 880)},{y + 138}",
               "p", 1.2, arrow="p")
        s.text(mx + 150, y + 64, "명령 queue", 11, color="#6d4596")
        # memories
        s.box(mx + 180, y + 290, 95, 50, "mem", ["inp_mem"], 12)
        s.box(mx + 285, y + 290, 95, 50, "mem", ["wgt_mem"], 12)
        s.arrow(mx + 228, y + 262, mx + 228, y + 288, "a", 1.2)
        s.arrow(mx + 332, y + 262, mx + 332, y + 288, "a", 1.2)
        cx = mx + 455
        if not systolic:
            s.box(cx, y + 80, 270, 60, "hls", ["GEMM: 256 곱 + 덧셈 트리", "+ acc RMW (그림 12 A)"], 12)
            s.box(cx, y + 150, 270, 50, "hls", ["ALU (II = 2)"], 12)
            s.box(cx, y + 210, 125, 50, "mem", ["uop_mem"], 12)
            s.box(cx + 145, y + 210, 125, 50, "mem", ["acc_mem", "(compute 안)"], 11.5)
            s.box(cx, y + 280, 270, 50, "mem", ["out_mem"], 12)
        else:
            s.box(cx, y + 80, 300, 50, "hls", ["GEMM 명령 → uop 스트림 (II = 1)"], 12)
            s.box(cx, y + 140, 300, 50, "hls", ["ALU: imm II = 1, reg II = 2 (그림 13)"], 12)
            s.box(cx, y + 200, 140, 40, "mem", ["uop_mem"], 12)
            s.box(cx, y + 250, 300, 100, "rtl", ["gemm_sa (RTL, 그림 12 B)", "16×16 DSP48E2 + RMW", "acc_mem, out_mem은 BD BRAM:", "gemm_busy로 RTL / HLS가 나눠 씀"], 11.5)
        s.arrow(mx + 382, y + 315, mx + 452, y + 315 if not systolic else y + 300, "a", 1.2)
        # store reads out_mem
        sxx = (mx + 830) if systolic else (mx + 800)
        oy = y + 305 if not systolic else y + 320
        ox = cx + 270 if not systolic else cx + 300
        s.path(f"M{ox},{oy} L{sxx + 80},{oy} L{sxx + 80},{y + 262}", "a", 1.2, arrow="a")
        s.text(ox + 8, oy - 6, "out_mem 읽기", 11, color="#555555")
        s.text(mx + 390, y + 308, "읽기", 11, color="#555555")
        # dep tokens
        s.text(mx + 180, y + 125, "l2g / g2l token", 11, color="#3c8a35")
        s.text((mx + 780) if systolic else (mx + 750), y + 125, "g2s / s2g token", 11, color="#3c8a35")
        s.arrow(mx + 382, y + 160, mx + 438, y + 160, "g", 1.2, both=True)
        s.arrow((mx + 772) if systolic else (mx + 742), y + 160, (mx + 828) if systolic else (mx + 798), y + 160, "g", 1.2, both=True)
        # AXI to HP
        for xx, yy in [(mx, y + 100), (mx + 180, y + 230), (mx + 830 if systolic else mx + 800, y + 230)]:
            s.path(f"M{xx},{yy} L{xx - 10},{yy} L{xx - 10},{y + 385} L280,{y + 385} L280,{y + 372}", "k", 1, arrow="k", dash="3 3")
        s.text(400, y + 400, "모듈마다 AXI master → HP 포트 → DDR (fetch: 명령, load: inp/wgt, compute: uop/bias, store: out)", 11.5,
               color="#666666")
        return y

    ya = 92
    s.section(ya, "A. 이전 (10-26 자체 bitstream): compute 안에서 HLS가 GEMM, 모든 모듈과 HP 포트가 클럭 하나 (333 → 380 MHz가 한계)")
    evta(ya + 10, False)
    yb = ya + 470
    s.section(yb, "B. 지금 (SYSTOLIC=1, FREQ=400): GEMM은 RTL gemm_sa, EVTA 400 MHz와 HP 포트 333 MHz (hp_pll, axi_smc0가 클럭을 건넌다)",
              color="#2f7a2a")
    evta(yb + 10, True)
    s.lines(40, yb + 450, ["• PS의 레지스터 포트(M_AXI_HPM0)는 100 MHz pl_clk0. HP 포트는 PS8 규격상 333 MHz가 한계(최소 주기 3.0 ns)라 분리했다.",
                           "• multi4는 이 블록 4개가 HP0~3을 하나씩 쓴다(EVTA i 레지스터 0xA0000000 + i·0x10000). 이 보드는 SLICE Y219 결함 행을 피해 배치한다.",
                           "• load, store, fetch와 의존성 token 구조, ISA(256비트 명령)는 바뀌지 않았다."], 12.5)
    s.legend(40, 1095, [("hls", "HLS 모듈"), ("rtl", "RTL"), ("mem", "BRAM / 포트"), ("dram", "DRAM")])
    return s


# ---------------------------------------------------------------------------------------------------------------
@fig(13, "alu_ii")
def fig13():
    W = 1460
    s = Svg(W, 1010, "그림 13. ALU 한 반복의 acc_mem 포트 사용: II = 2 → imm 연산 II = 1 (11-03), RMW reset에 bias와 requantize (11-04)",
            ["ALU 반복 하나 = acc[dst] ← op(acc[dst], imm 또는 acc[src]) (16 lane int32). acc_mem은 포트 2개(A, B)짜리 BRAM."
             " 자세히: evta_runtime_compare.md §7.14, vta-hw-zcu102-systolic/hardware/xilinx/src/vta.cc alu()."])

    def ports(y, title, seq, n, note, color):
        s.text(40, y, title, 13.5, weight="bold", color=color)
        cw = 70
        x0 = 200
        for t in range(n):
            s.text(x0 + t * cw + cw / 2, y + 22, f"clk {t}", 11, "middle", color="#777777")
        for pi, pname in enumerate(["포트 A", "포트 B"]):
            yy = y + 30 + pi * 34
            s.text(x0 - 12, yy + 21, pname, 12.5, "end")
            for t in range(n):
                s.rect(x0 + t * cw, yy, cw, 30, "#fafafa", "#dddddd", 0.6)
        for (pi, t, txt, kind) in seq:
            fill, stroke = C[kind]
            s.rect(x0 + t * cw + 2, y + 32 + pi * 34, cw - 4, 26, fill, stroke, 1, 3)
            s.text(x0 + t * cw + cw / 2, y + 50 + pi * 34, txt, 11, "middle")
        s.text(x0 + n * cw + 20, y + 62, note, 12.5, color=color)

    ya = 92
    s.section(ya, "A. 이전 (모든 ALU 연산): 반복마다 src 읽기 + dst 읽기 + dst 쓰기 = 접근 3번 → 포트 2개로 2 clk")
    seq = []
    for k in range(4):
        t = 2 * k
        seq += [(0, t, f"src{k}", "hls"), (1, t, f"dst{k}", "hls"), (0, t + 1, f"쓰기{k - 2}" if k >= 2 else "", "new" if k >= 2 else "note")]
    ports(ya + 50, "loop EXE_OUT_LOOP_EXE_IN_LOOP_READ_ALU_UOP: II = 2, depth 11", [q for q in seq if q[2]], 8,
          "반복 4개에 8 clk", "#2f5f98")
    s.lines(40, ya + 170, ["imm 연산(SHR, clip MIN/MAX, relu, 반올림 ADD)은 src를 쓰지 않는데도 같은 루프라 2 clk였다.",
                           "Resnet18 ALU 4.63 ms 중 imm 연산이 69 %. (runbench: ALU 반복 2.0 clk, imm도 reg도)"], 12.5)
    yb = ya + 230
    s.section(yb, "B. 11-03 (systolic bitstream만): use_imm인 명령은 따로 루프 — dst 읽기(포트 A) + dst 쓰기(포트 B) = II = 1", color="#2f7a2a")
    seq = []
    for k in range(8):
        seq.append((0, k, f"dst{k}", "hls"))
        if k >= 4:
            seq.append((1, k, f"쓰기{k - 4}", "new"))
    ports(yb + 50, "loop ALU_IMM_OUT_LOOP..._UOP: II = 1, depth 9", seq, 8, "반복 8개에 8 clk (파이프라인이 차면)", "#2a6a24")
    s.lines(40, yb + 170, ["• 같은 반복 안에서 읽은 dst를 몇 clk 뒤 쓴다. 그 사이 같은 dst를 다시 읽는 반복이 없어야 한다(DEPENDENCE inter false):",
                           "  Resnet18의 ALU 명령 1,566개를 덤프(EVTA_REPLAY_DUMP)로 보면, imm 명령 중 16 반복 안에 같은 주소를 다시 쓰는 것은 없다.",
                           "• 첫 빌드는 uop 읽기 → 주소 덧셈 → acc_mem 주소(BRAM 58개)가 한 clk이라 WNS −0.819 ns. 그 둘 뒤에 register를 넣어 +0.001 ns.",
                           "• 보드: runbench aluimm 2.0 → 1.0 clk, Resnet18 EVTA 26.42 → 24.89 ms (−1.53, 예상 −1.5). ISA, vtalib, 컴파일러는 그대로 (§7.15)."], 12.5)
    yc = yb + 290
    s.section(yc, "C. 11-04: GEMM reset에서 acc에 0 대신 bias (ISA 비트 하나), out에 쓸 때 requantize (rq_* 필드) — ALU 명령이 pool MAX만 남는다",
              color="#9a7b00")
    y1 = yc + 50
    s.text(40, y1 + 14, "conv 출력 원소 하나의 requant (tile의 모든 원소에 대해, 원소당 clk)", 13, weight="bold")
    rowsA = [("ALU ADD bias (reg)", 2, "hls"), ("ALU SHR (imm)", 2, "hls"), ("ALU MAX (imm)", 2, "hls"), ("ALU MIN (imm)", 2, "hls")]
    rowsB = [("ALU ADD bias (reg)", 2, "hls"), ("SHR", 1, "new"), ("MAX", 1, "new"), ("MIN", 1, "new")]
    rowsC = [("bias는 reset 때 RMW가 씀", 0, "rtl"), ("SHR", 1, "new"), ("MAX", 1, "new"), ("MIN", 1, "new")]
    rowsD = [("bias는 reset 때 RMW가 씀", 0, "rtl"), ("shift · clip은 out 쓰기 길에서", 0, "rtl")]
    for i, (lbl, rows_) in enumerate([("11-02 bitstream", rowsA), ("ALU II = 1 (11-03)", rowsB), ("+ bias reset (11-04)", rowsC),
                                      ("+ requantize (11-04)", rowsD)]):
        yy = y1 + 36 + i * 50
        s.text(40, yy + 22, lbl, 12.5, weight="bold")
        x = 200
        tot = 0
        for name, clk, kind in rows_:
            w = max(clk, 0.0) * 120 if clk else 230
            fill, stroke = C[kind]
            s.rect(x, yy, w, 32, fill, stroke, 1, 3, dash="4 3" if clk == 0 else None)
            s.text(x + w / 2, yy + 21, f"{name} {clk} clk" if clk else name, 11.5, "middle")
            x += w + 6
            tot += clk
        s.text(x + 12, yy + 21, f"= {tot} clk", 13, weight="bold")
    s.lines(40, y1 + 250, ["• bias reset (§7.16): reset인 단계에서 읽는 주소를 dst 대신 uop의 src(= bias가 있는 acc entry)로, 쓰는 값을 0 대신 읽은 값으로."
                           " EVTA 24.89 → 24.15 ms.",
                           "• requantize (§7.18): out에 쓰는 값을 clip((acc + round) >> shift, lo, hi)로 (sa_rmw S4-S6, 추가 clk 없음). EVTA 24.15 → 23.05 ms.",
                           "• 예전 bitstream은 새 비트를 무시하므로 vtalib은 /tmp/evta_bitstream 이름(bias, _rq)을 보고 새 방식을 쓴다 (evta_isa_ext.md)."], 12.5)
    return s


# ---------------------------------------------------------------------------------------------------------------
@fig(15, "evta_time_now")
def fig15():
    W = 1460
    s = Svg(W, 850, "그림 15. EVTA 시간 26.47 ms(11-02)의 구성과 하드웨어 개선 세 단계 (11-03 ~ 11-04)",
            ["Resnet18 single, systolic 400 MHz. 11-02 막대는 측정(24 inference median, EVTA_REPLAY_LOG=2 + EVTA_REPLAY_DUMP, §7.14),"
             " 그 아래는 그 측정에서 뺀 추정이고 오른쪽 '측정'은 각 bitstream의 보드 _sysprof 값(§7.15, §7.16, §7.18)."])
    gemm = ("GEMM", 18.85, "#7aa6d6", None)
    rest = [("conv0 tile 고정", 1.08, "#d9d9d9", None), ("3×3 conv run당", 1.32, "#e6e6e6", None),
            ("1×1 conv", 0.46, "#cfcfcf", None), ("fc", 0.13, "#bdbdbd", None)]
    alu_now = [("bias ADD", 0.91, "#e98b8b", None), ("SHR", 0.90, "#f3a65a", None), ("MIN 127", 0.98, "#f2c38b", None),
               ("relu MAX 0", 0.69, "#f7d9b5", None), ("−128", 0.29, "#fbe6cc", None), ("pool MAX", 0.45, "#e9a0c0", None),
               ("기타", 0.41, "#f5ece0", None)]
    imm = {"SHR", "MIN 127", "relu MAX 0", "−128", "기타"}
    alu_ii1 = [(n, m / 2 if n in imm else m, c, "4 3" if n in imm else None) for n, m, c, _ in alu_now]
    alu_bias = [q for q in alu_ii1 if q[0] != "bias ADD"]
    alu_rq = [q for q in alu_bias if q[0] == "pool MAX"]
    cases = [("11-02 측정", alu_now, None), ("+ ALU II = 1", alu_ii1, 24.89), ("+ bias reset", alu_bias, 24.15),
             ("+ requantize", alu_rq, 23.05)]

    def stack(x0, scale, yy, label, segs, h=36, small=11, meas=None):
        s.text(x0 - 12, yy + h / 2 + 5, label, 13, "end", "bold")
        x = x0
        for name, ms, col, dash in segs:
            w = ms * scale
            s.rect(x, yy, w, h, col, "#555555", 0.8, 0, dash)
            if tw(name, small) < w - 6:
                s.text(x + w / 2, yy + h / 2 - 2, name, small, "middle")
                s.text(x + w / 2, yy + h / 2 + 13, f"{ms:.2f}", small, "middle", "bold")
            x += w
        tot = sum(m for _, m, _, _ in segs)
        s.text(x + 10, yy + h / 2 + 5, f"{tot:.2f} ms" + ("" if meas is None else f" 추정 · 측정 {meas:.2f}"), 13.5, weight="bold")

    s.section(92, "A. 전체 (1 ms = 42 px): 11-02에 GEMM이 71 %, requantize 뒤 82 %")
    for i, (lbl, alu, meas) in enumerate(cases):
        stack(220, 42, 140 + i * 48, lbl, [gemm] + alu + rest, meas=meas)
    s.section(350, "B. GEMM 밖만 크게 (1 ms = 140 px): ALU 연산별, 그 밖의 고정 비용")
    for i, (lbl, alu, _) in enumerate(cases):
        stack(220, 140, 400 + i * 62, lbl, alu + rest, 46, 11.5)
    s.lines(40, 670, [("읽는 법", "bold"),
                      "• GEMM 18.85 ms는 GEMM 반복 수 ÷ 400 MHz(1 반복 = 1 clk)로, 줄이기 어려운 하한이다.",
                      "• ALU 4.63 ms = ALU 반복 수 × 2 clk('−128'은 MAX −128, 아래쪽 clip). 연산별 몫(conv0과 다른 conv 합). '기타'는 반올림 ADD imm 0.16 ms와, 같은 기록을 두 번 쓰는 run의 몫.",
                      "• 나머지 2.99 ms: conv0 tile 98개의 고정 비용 1.08 ms, 3×3 conv run마다 60~130 µs(첫 tile LOAD, 마지막 ALU·STORE),",
                      "  1×1 downsample conv 3개는 GEMM이 작아 LOAD가 숨지 못한다(0.46 ms), fc는 weight 504 KB LOAD. run 하나의 시작·끝 비용은 4.7 µs뿐이다(runbench).",
                      "• 점선 = II = 1이 된 imm 연산(반복 시간 반). bias reset은 bias ADD를, requantize는 SHR · clip · 반올림 ADD를 없앤다(pool MAX만 남음).",
                      "• 측정이 추정보다 0.4~0.8 ms 크다: stage3/4에서는 ALU가 inp LOAD / STORE와 겹쳐 있어 없애도 그만큼 줄지 않는다(§7.16 EVTA_MEMEXP).",
                      "  추론(OMP 1)은 28.98 → 25.49 ms. 남은 큰 덩어리는 GEMM 18.85 ms(이론 최소의 94 %)와 고정 비용 약 3 ms(§7.17)."], 12.5)
    s.legend(40, 832, [("evta", "GEMM"), ("gone", "ALU (색은 연산별)"), ("mem", "그 밖의 고정 비용")])
    return s


def main():
    nums = [int(a) for a in sys.argv[1:]] or sorted(FIGS)
    for n in nums:
        name, f = FIGS[n]
        s = f()
        p = os.path.join(OUT, f"evta_fig{n:02d}_{name}.svg")
        s.save(p)
        print("wrote", os.path.relpath(p))


if __name__ == "__main__":
    main()
