#!/usr/bin/env python3
# EVTA structure figures, one per speed-up step (docs/figs/evta_stNN_*.svg): before / after on the same machine drawing
# (CPU, DRAM buffers, EVTA load / compute / store with its SRAMs, GEMM array and ALU), the changed parts in colour, the
# data paths as arrows, and the measured times with the change in %. Hardware steps get their own micro-architecture
# drawings (acc_mem ports, pipeline stages, registers, muxes).
# usage: python3 docs/figs/src/make_evta_struct_figs.py [NN ...]   (no argument: every figure)
# Numbers: docs/evta_runtime_compare.md §7.x, docs/evta_systolic_design.md (named in each figure).
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from svglib import Svg, C, tw  # noqa: E402

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
FIGS = {}

COL = {"a": "#333333", "b": "#2f5f98", "o": "#c06a10", "g": "#3c8a35", "r": "#c0392b", "p": "#6d4596", "y": "#9a7b00",
       "k": "#999999"}


def fig(num, name):
    def deco(f):
        FIGS[num] = (name, f)
        return f
    return deco


def pct(a, b):
    return f"{b - a:+.2f} ms ({(b - a) / a * 100:+.1f} %)".replace("+", "+").replace("-", "−")


def edge(r, px, py):
    """point on the border of rect r = (x, y, w, h) toward (px, py)"""
    x, y, w, h = r
    cx, cy = x + w / 2, y + h / 2
    dx, dy = px - cx, py - cy
    if dx == 0 and dy == 0:
        return cx, cy
    sx = (w / 2) / abs(dx) if dx else 1e9
    sy = (h / 2) / abs(dy) if dy else 1e9
    t = min(sx, sy)
    return cx + dx * t, cy + dy * t


def link(s, ra, rb, color="k", sw=1.4, dash=None, label=None, lsize=10.5, off=(0, 0), both=False):
    ax, ay = ra[0] + ra[2] / 2 + off[0], ra[1] + ra[3] / 2 + off[1]
    bx, by = rb[0] + rb[2] / 2 + off[0], rb[1] + rb[3] / 2 + off[1]
    x1, y1 = edge(ra, bx, by)
    x2, y2 = edge(rb, ax, ay)
    s.arrow(x1, y1, x2, y2, color, sw, dash, both)
    if label:
        mx, my = (x1 + x2) / 2, (y1 + y2) / 2
        s.text(mx + 5, my - 4, label, lsize, color=COL[color], weight="bold")


# ---------------------------------------------------------------------------------------------------------------
# the machine: CPU, DRAM, EVTA (positions relative to the panel)
LAYOUT = {
    "cpu": (0, 0, 250, 176), "dram": (0, 196, 250, 324),
    "fetch": (292, 34, 118, 40), "load": (292, 112, 118, 50), "uop": (292, 200, 118, 44), "alu": (292, 282, 118, 62),
    "store": (292, 420, 118, 50),
    "inp": (430, 30, 118, 62), "wgt": (562, 30, 118, 62), "gemm": (430, 128, 250, 92), "acc": (430, 252, 250, 62),
    "out": (430, 348, 250, 56), "note": (430, 420, 250, 96),
}
LABEL = {
    "fetch": ["fetch", "명령 256비트"], "load": ["LOAD 모듈", "DRAM → SRAM (pad)"], "uop": ["uop_mem (SRAM)", "주소 묶음"],
    "alu": ["ALU 16 lane", "MIN MAX ADD SHR"], "store": ["STORE 모듈", "out_mem → DRAM"],
    "inp": ["inp_mem (SRAM)", "4096×16 int8"], "wgt": ["wgt_mem (SRAM)", "2048×16×16 int8"],
    "gemm": ["GEMM 16×16 MAC (DSP 256)", "acc[d] += inp[s] · wgt[w]ᵀ"], "acc": ["acc_mem (SRAM)", "4096×16 int32"],
    "out": ["out_mem (SRAM) 4096×16 int8", "= acc의 하위 8비트"],
}
KIND = {"fetch": "evta", "load": "evta", "uop": "mem", "alu": "hls", "store": "evta", "inp": "mem", "wgt": "mem",
        "gemm": "evta", "acc": "mem", "out": "mem"}
STATE_KIND = {"new": "new", "gone": "gone", "hot": None, "": None}
DEFAULT_FLOWS = [("dram", "fetch"), ("dram", "load"), ("load", "inp"), ("load", "wgt"), ("inp", "gemm"), ("wgt", "gemm"),
                 ("gemm", "acc"), ("acc", "alu"), ("acc", "out"), ("out", "store"), ("store", "dram"), ("uop", "gemm")]


def machine(s, px, py, title, cpu=(), dram=(), st=None, label=None, flows=(), note=None, cpu_title=None, dim_default=True):
    """cpu / dram: [(text, state)], state '' / 'new' / 'gone' / 'hot'; st: {component: state}; flows: (a, b, color, label[, dash])"""
    st = st or {}
    label = label or {}
    R = {k: (px + x, py + y, w, h) for k, (x, y, w, h) in LAYOUT.items()}
    s.text(px, py - 12, title, 15, weight="bold", color="#2f5f98")
    # CPU
    x, y, w, h = R["cpu"]
    s.rect(x, y, w, h, *C["cpu"], 1.4, 6)
    s.text(x + 10, y + 20, cpu_title or "CPU  Cortex-A53 ×4 (NEON)", 12.5, weight="bold")
    for i, (t, sta) in enumerate(cpu):
        yy = y + 42 + i * 21
        if sta == "gone":
            s.rect(x + 6, yy - 14, w - 12, 19, "#fbdcdc", "#c0392b", 0.8, 3, dash="3 2")
            s.text(x + 12, yy, t, 11.5, color="#a02a20")
            s.line(x + 10, yy - 4, x + 12 + tw(t, 11.5), yy - 4, "#c0392b", 1.2)
        elif sta == "new":
            s.rect(x + 6, yy - 14, w - 12, 19, *C["new"], 0.8, 3)
            s.text(x + 12, yy, t, 11.5, weight="bold", color="#245e1f")
        else:
            s.text(x + 12, yy, t, 11.5, weight="bold" if sta == "hot" else "normal")
    # DRAM
    x, y, w, h = R["dram"]
    s.rect(x, y, w, h, *C["dram"], 1.4, 6)
    s.text(x + 10, y + 20, "DRAM (CMA, 캐시 비일관)", 12.5, weight="bold")
    for i, (t, sta) in enumerate(dram):
        yy = y + 32 + i * 30
        kind = {"new": C["new"], "gone": C["gone"]}.get(sta, ("#fbf6ec", "#8a6d3b"))
        s.rect(x + 8, yy, w - 16, 25, kind[0], kind[1], 1, 3, dash="3 2" if sta == "gone" else None)
        s.text(x + 14, yy + 17, t, 11, weight="bold" if sta in ("new", "hot") else "normal",
               color="#a02a20" if sta == "gone" else "#222222")
    # EVTA frame
    s.rect(px + 280, py, 410, 520, "#f6f9fd", "#2f5f98", 1.6, 8)
    s.text(px + 292, py + 20, "EVTA (PL, 400 MHz)", 12.5, weight="bold", color="#2f5f98")
    s.rect(px + 286, py + 192, 400, 216, "none", "#9db3cf", 1, 6, dash="5 3")
    s.text(px + 680, py + 405, "compute 모듈", 10.5, "end", color="#5f7fa8")
    for k in ["fetch", "load", "uop", "alu", "store", "inp", "wgt", "gemm", "acc", "out"]:
        sta = st.get(k, "")
        kind = STATE_KIND.get(sta) or KIND[k]
        x, y, w, h = R[k]
        lines = label.get(k, LABEL[k])
        s.box(x, y, w, h, kind, [(lines[0], "bold")] + list(lines[1:]), 11, dash="4 3" if sta == "gone" else None,
              sw=2.6 if sta == "hot" else 1.3)
        if sta == "gone":
            s.line(x + 4, y + 4, x + w - 4, y + h - 4, "#c0392b", 1.2)
    # default data paths (light), then the figure's
    drawn = {(a, b) for a, b, *_ in flows}
    if dim_default:
        for a, b in DEFAULT_FLOWS:
            if (a, b) not in drawn:
                link(s, R[a], R[b], "k", 1.0, "3 3")
    for f in flows:
        a, b, col = f[0], f[1], f[2]
        lab = f[3] if len(f) > 3 else None
        dash = f[4] if len(f) > 4 else None
        link(s, R[a], R[b], col, 2.2, dash, lab)
    if note:
        x, y, w, h = R["note"]
        s.rect(x, y, w, h, "#ffffff", "#bbbbbb", 1, 4)
        s.lines(x + 8, y + 18, note, 10.8, lh=15)
    return R


def times(s, x, y, rows, size=12.5):
    """rows: (label, before, after, unit) -> a table with the change and %"""
    cols = [("측정", 560), ("이전", 190), ("이후", 190), ("차이", 300)]
    s.rect(x, y, sum(c[1] for c in cols), 26, "#f3f6fa", "#c8d3e0", 1)
    cx = x
    for t, w in cols:
        s.text(cx + 8, y + 18, t, size, weight="bold")
        cx += w
    for r, (lab, a, b, u) in enumerate(rows):
        yy = y + 26 * (r + 1)
        s.rect(x, yy, sum(c[1] for c in cols), 26, "#ffffff" if r % 2 == 0 else "#fafafa", "#e2e2e2", 0.8)
        d = b - a
        cells = [lab, f"{a:g} {u}", f"{b:g} {u}", f"{d:+.2f} {u} ({d / a * 100:+.1f} %)".replace("-", "−")]
        cx = x
        for (t, w), c in zip(cols, cells):
            s.text(cx + 8, yy + 18, c, size, weight="bold" if c is cells[2] or c is cells[3] else "normal",
                   color="#2a6a24" if c is cells[3] and d < 0 else ("#c0392b" if c is cells[3] else "#222222"))
            cx += w
    return y + 26 * (len(rows) + 1)


def step(num, name, title, sub, before, after, rows, notes, H=980):
    """a standard before / after figure"""
    @fig(num, name)
    def f():
        s = Svg(1460, H, title, sub)
        machine(s, 30, 128, "이전", **before)
        machine(s, 750, 128, "이후", **after)
        s.legend(30, 676, [("new", "새로 / 옮겨 온 것"), ("gone", "없어진 것"), ("cpu", "CPU"), ("dram", "DRAM 버퍼")])
        s.text(800, 676, "굵은 화살표 = 이 단계에서 보는 데이터 길, 회색 점선 = 그 밖의 평소 길", 12, color="#555555")
        y = times(s, 30, 696, rows)
        s.lines(30, y + 26, notes, 12.5, lh=19)
        s.h = y + 30 + 19 * len(notes)
        return s
    return f


# ---------------------------------------------------------------------------------------------------------------
# software / runtime steps (single EVTA, Resnet18, inference = one image end to end)
CONV_FLOWS = [("dram", "load", "b", "inp, wgt"), ("inp", "gemm", "b"), ("wgt", "gemm", "b"), ("gemm", "acc", "b"),
              ("acc", "alu", "b"), ("acc", "out", "b"), ("out", "store", "b"), ("store", "dram", "b", "출력")]

step(1, "conv0_to_evta",
     "1. conv0(3채널)을 CPU float에서 EVTA로: 입력을 16채널로 0 채워 EVTA에 보냄 (10-28)",
     ["추론 131.8 → 78.6 ms (OMP 4, −40.4 %). 이전에는 첫 conv가 3채널이라(EVTA는 16채널 단위) CPU가 float로 계산했다.",
      "자세히: evta_runtime_aot.md §5.3, CMake VTA_RESNET18_WITH_SKIPQUANT0=OFF."],
     dict(cpu=[("conv0 7×7×3→64 float (대부분의 시간)", "gone"), ("양자화 / 역양자화", "gone"), ("나머지 층의 transpose 등", "")],
          dram=[("입력 224×224×3 float", ""), ("conv0 출력 float", "gone"), ("conv1~ 활성화 int8", ""), ("weight, bias", "")],
          st={}, flows=[]),
     dict(cpu=[("입력 0 채움: 3 → 16채널 (zerofill)", "new"), ("나머지 층의 transpose 등", "")],
          dram=[("입력 224×224×16 int8 (13채널은 0)", "new"), ("conv0 weight 16채널로", "new"), ("conv1~ 활성화 int8", ""),
                ("weight, bias", "")],
          st={"gemm": "hot"}, flows=CONV_FLOWS,
          note=["conv0도 GEMM 16×16에서: 입력 16채널 중", "3채널만 값이 있어 GEMM의 13/16은 0을 곱한다", "(그래도 CPU float보다 빠름; 11번 그림에서 고침)"]),
     [("Resnet18 추론 (single, OMP 4, _timed run 2)", 131.8, 78.6, "ms")],
     ["• conv0은 Resnet18 연산의 6.5 %지만 CPU float로는 가장 큰 덩어리였다. 0 채움(zerofill) 복사는 CPU에 남는다(5번 그림에서 6.8 → 0.2 ms)."])

step(2, "keep_layout",
     "2. 층 사이 활성화를 EVTA layout 그대로: conv마다 CPU transpose 두 번과 버퍼 할당이 없어짐 (10-31)",
     ["EVTA는 N{C/16}HW16(16채널 묶음) layout을 읽고 쓴다. 이전에는 conv마다 NHWC → EVTA → NHWC로 바꿨다. VTASave.cpp -vta-keep-layout.",
      "자세히: evta_runtime_compare.md §7.1."],
     dict(cpu=[("transpose NHWC → EVTA (conv 앞)", "gone"), ("transpose EVTA → NHWC (conv 뒤)", "gone"),
               ("VTABufferAlloc / Free (conv마다)", "gone"), ("elemadd, relu, maxpool (NHWC)", "")],
          dram=[("활성화 NHWC", ""), ("conv 입력 사본 (EVTA layout)", "gone"), ("conv 출력 사본 (EVTA layout)", "gone"),
                ("weight, bias", "")],
          flows=CONV_FLOWS),
     dict(cpu=[("elemadd, relu, maxpool: EVTA layout 그대로", "new"), ("(16채널 블록을 batch로)", "")],
          dram=[("활성화 N{C/16}HW16 (EVTA layout)", "new"), ("weight, bias", "")],
          flows=CONV_FLOWS,
          note=["EVTA 쪽은 그대로. STORE가 쓴 출력을", "다음 conv의 LOAD가 바로 읽는다.", "입구(conv0 앞)와 출구(fc 앞)에서만 변환."]),
     [("Resnet18 추론 (single, OMP 1, _systimed)", 148.7, 91.1, "ms"), ("Resnet18 추론 (single, OMP 4)", 73.6, 56.4, "ms")],
     ["• 없어진 CPU 일: transpose 약 15.8 ms + conv마다 버퍼 할당 약 4.5 ms(OMP 4 기준 추정, §7 표)."])

step(3, "residual_fusion",
     "3. residual(elemadd + relu)을 conv 호출 안의 EVTA로: residual을 inp_mem에 LOAD, identity GEMM으로 acc에 더함 (10-31)",
     ["conv 출력 + 다른 경로(skip)의 활성화를 더하고 relu. 이전에는 CPU가 DRAM의 두 텐서를 읽어 더했다. VTASave.cpp -vta-fuse-residual.",
      "자세히: evta_runtime_compare.md §7.2. 2의 거듭제곱 scale만(정수 shift로 맞춤), bit 단위로 CPU와 같다(EVTA_RES_CHECK=1)."],
     dict(cpu=[("elemadd: conv 출력 + skip (int8, 두 번 읽음)", "gone"), ("relu", "gone")],
          dram=[("conv 출력 (STORE)", "gone"), ("skip 활성화", ""), ("합 (CPU가 씀)", "gone"), ("weight, bias", "")],
          flows=CONV_FLOWS),
     dict(cpu=[("(없음: EVTA 호출 하나)", "")],
          dram=[("skip 활성화 → LOAD", "hot"), ("identity weight (16×16, 2^shl)", "new"), ("합 (STORE)", "new"), ("weight, bias", "")],
          st={"inp": "new", "gemm": "hot", "alu": "hot"},
          label={"inp": ["inp_mem", "+ skip tile (int8)"], "gemm": ["GEMM: conv + identity GEMM", "acc += skip · I·2^shl"],
                 "alu": ["ALU", "SHR, MAX, MIN ×2"]},
          flows=CONV_FLOWS + [("dram", "inp", "g", "skip")],
          note=["tile마다: conv GEMM → requant(SHR/MAX/MIN)", "→ ADD ×shl → skip을 identity GEMM으로 더함", "→ 반올림 SHR, relu MAX, MIN → STORE"]),
     [("Resnet18 추론 (single, OMP 1)", 90.93, 67.13, "ms"), ("EVTA 하드웨어 시간 (늘어난 ALU / GEMM)", 29.68, 30.80, "ms")],
     ["• CPU 일 약 25 ms가 EVTA 일 1.1 ms로 바뀌었다. 같은 skip 텐서를 CPU가 따로 읽고 쓰던 DRAM 왕복도 없어졌다."])

step(4, "maxpool_evta",
     "4. max pool을 EVTA ALU로: 16채널 블록을 batch로 LOAD, 창마다 ALU MAX (10-31)",
     ["입력이 0 이상(relu 뒤)일 때만: LOAD가 pad를 0으로 채우면 CPU의 max와 같다. VTASave.cpp -vta-evta-maxpool, vtalib maxpool_evta.",
      "자세히: evta_runtime_compare.md §7.3."],
     dict(cpu=[("maxpool 3×3 / 2 (int8)", "gone")], dram=[("conv0 출력", ""), ("pool 출력 (CPU가 씀)", "gone"), ("weight, bias", "")],
          flows=[]),
     dict(cpu=[("(없음)", "")], dram=[("conv0 출력 → LOAD (pad 0)", "hot"), ("identity weight", "new"), ("pool 출력 (STORE)", "new")],
          st={"alu": "new", "gemm": "hot"}, label={"alu": ["ALU MAX", "창 위치마다 1 명령"], "gemm": ["identity GEMM", "inp → acc 옮기기"]},
          flows=[("dram", "load", "b"), ("load", "inp", "b"), ("inp", "gemm", "b"), ("gemm", "acc", "b"), ("acc", "alu", "g", "MAX"),
                 ("acc", "out", "b"), ("out", "store", "b"), ("store", "dram", "b")],
          note=["출력 tile마다 입력 halo를 다시 읽고,", "identity GEMM으로 acc에 올린 뒤", "ALU MAX를 창의 9 위치만큼"]),
     [("Resnet18 추론 (single, OMP 1)", 67.13, 52.48, "ms"), ("EVTA 하드웨어 시간", 30.80, 31.98, "ms")],
     ["• CPU maxpool 약 16 ms → EVTA 1.2 ms. 12번 그림에서 이 pool이 conv0 호출 안으로 들어간다."])

step(5, "transpose_neon",
     "5. 남은 CPU layout 변환을 16바이트 복사로 (입구 / 출구, conv0 0 채움) (11-01)",
     ["EVTA layout의 한 원소 = 16채널 = 16바이트. 바이트마다 주소를 계산하던 루프를 16바이트 단위 복사로. vtalib CPUBundle_generic.cpp.",
      "자세히: evta_runtime_compare.md §7.4."],
     dict(cpu=[("transpose 바이트 단위 (index 계산)", "gone"), ("conv0 zerofill 3 → 16 (6.8 ms, OMP 4)", "gone")],
          dram=[("입력 NHWC", ""), ("EVTA layout 입력", "")], flows=[]),
     dict(cpu=[("transpose: 16바이트 복사 (NEON)", "new"), ("zerofill: 3바이트 + 13바이트 0 (0.2 ms)", "new")],
          dram=[("입력 NHWC", ""), ("EVTA layout 입력", "")], flows=[],
          note=["EVTA는 그대로. CPU 일만 빨라졌다."]),
     [("Resnet18 추론 (single, OMP 1)", 52.48, 46.90, "ms"), ("conv0 zerofill (OMP 4)", 6.8, 0.2, "ms"),
      ("transpose_vtaio2nhwc 56×56×64 (OMP 1)", 3.31, 0.26, "ms")],
     [])

step(6, "buffer_pool",
     "6. VTA 버퍼 pool: 같은 크기 버퍼를 다시 써서 cma_alloc / cma_free ioctl이 첫 추론에만 (11-01)",
     ["bundle은 추론마다 활성화 버퍼 23개를 할당 / 해제한다. VTABufferFree가 크기별 free list에 두고 VTABufferAlloc이 꺼내 쓴다(EVTA_BUF_POOL_MB, 기본 32).",
      "자세히: evta_runtime_compare.md §7.5."],
     dict(cpu=[("cma_alloc ioctl ×23 (커널, 페이지 고정)", "gone"), ("cma_free ioctl ×23", "gone")],
          dram=[("활성화 버퍼: 매 추론 새로 할당", "gone"), ("weight, bias (한 번)", "")], flows=[]),
     dict(cpu=[("free list에서 꺼냄 / 돌려놓음", "new")], dram=[("활성화 버퍼: 첫 추론 뒤 재사용", "new"), ("weight, bias (한 번)", "")],
          flows=[], note=["EVTA는 그대로. 재사용한 버퍼는 이전 내용을", "갖고 있다(cma_alloc도 지우지 않는다)."]),
     [("Resnet18 추론 (single, OMP 1)", 46.97, 44.27, "ms"), ("추론당 VTABufferAlloc + Free", 2.7, 0.05, "ms")],
     [])

step(7, "cpu_fc_neon",
     "7. CPU fc를 다시 씀: weight를 저장된 순서대로 읽고 NEON으로 8행 × 64출력씩 (11-01)",
     ["Resnet18 fc 512 → 1000. 이전 코드는 출력마다 weight 열을 건너뛰며 읽었다(캐시 줄당 1바이트). vtalib cpuFullyConnected, bit 단위 같음.",
      "자세히: evta_runtime_compare.md §7.6."],
     dict(cpu=[("fc: 출력마다 weight 열 (stride 1000)", "gone")], dram=[("fc weight 512×1000 int8", "")], flows=[]),
     dict(cpu=[("fc: weight 행 순서, NEON 8행×64출력, prefetch", "new")], dram=[("fc weight 512×1000 int8", "")], flows=[]),
     [("Resnet18 추론 (single, OMP 1)", 44.27, 40.56, "ms"), ("fc (OMP 1)", 4.43, 0.74, "ms")],
     ["• 8번 그림에서 fc는 EVTA로 간다."])

step(8, "fc_evta",
     "8. fc를 EVTA GEMM으로: weight와 bias를 컴파일 때 EVTA layout으로, chunk마다 wgt_mem 반쪽씩 (11-02)",
     ["batch 1이라 inp는 512 / 16 = 32 entry 하나, weight 16×16 블록 2,016개를 wgt_mem 반쪽씩 번갈아 LOAD(다른 반쪽은 GEMM 중). VTASave.cpp -vta-evta-fc.",
      "자세히: evta_runtime_compare.md §7.8. 반올림(half away from zero)과 clip을 ALU MIN/MAX/ADD/SHR로 맞춰 bit 단위로 CPU와 같다."],
     dict(cpu=[("fc 512→1000 (NEON)", "gone")], dram=[("fc weight (Glow 순서)", ""), ("fc 출력 (CPU가 씀)", "gone")], flows=[]),
     dict(cpu=[("입력 512바이트 복사만", "")],
          dram=[("fc weight EVTA layout (컴파일 때)", "new"), ("bias int32 (반올림 반영)", "new"), ("fc 출력 (STORE)", "new")],
          st={"wgt": "hot", "gemm": "hot", "alu": "hot"}, label={"wgt": ["wgt_mem", "반쪽씩 번갈아"], "alu": ["ALU", "반올림, SHR, clip"]},
          flows=[("dram", "load", "b", "weight 504 KB"), ("load", "wgt", "b"), ("wgt", "gemm", "b"), ("inp", "gemm", "b"),
                 ("gemm", "acc", "b"), ("acc", "alu", "b"), ("acc", "out", "b"), ("out", "store", "b"), ("store", "dram", "b")],
          note=["acc[o] = bias, 이어서 chunk마다", "acc[o] += Σc inp[c] · W[o][c]", "(18번 그림에서 ALU 부분은 out 쓰기로)"]),
     [("Resnet18 추론 (single, OMP 1)", 40.56, 39.92, "ms"), ("fc 한 번", 0.74, 0.152, "ms")],
     [])

step(9, "preprocess_fuse",
     "9. 입력 전처리를 conv0 입력 복사 하나로: 양자화, 채널별 빼기 / 나누기, NCHW → NHWC를 한 번에 (11-02)",
     ["이전에는 CPU가 224×224×3 이미지를 네 번 훑었다(DRAM 왕복 네 번). [C][256] 표를 컴파일 때 만들어 원소당 표 한 번 읽기로. VTASave.cpp -vta-fuse-preprocess.",
      "자세히: evta_runtime_compare.md §7.9. NEON으로 16 화소씩, bit 단위 같음."],
     dict(cpu=[("float 양자화", "gone"), ("채널별 elemsub (평균)", "gone"), ("채널별 elemdiv (표준편차)", "gone"),
               ("NCHW → NHWC transpose", "gone"), ("zerofill → EVTA layout", "gone")],
          dram=[("이미지 float NCHW", ""), ("중간 텐서 ×3", "gone"), ("conv0 입력 EVTA layout", "")], flows=[]),
     dict(cpu=[("표[C][256] 한 번 읽기 + EVTA layout 쓰기", "new"), ("(NEON 16 화소씩)", "")],
          dram=[("이미지 float NCHW", ""), ("[C][256] 표 (컴파일 때)", "new"), ("conv0 입력 EVTA layout", "")], flows=[]),
     [("Resnet18 추론 (single, OMP 1)", 39.92, 37.08, "ms"), ("입력 전처리 + 0 채움", 2.4, 0.37, "ms")],
     [])

step(10, "replay",
     "10. EVTA 명령 기록 / 재생 (AOT 0단계): 같은 인자의 호출은 명령과 uop를 다시 만들지 않고 복사 (11-02)",
     ["conv 호출마다 CPU가 VTAUopPush / VTALoadBuffer2D 등으로 명령 수백~수천 개와 uop를 만들었다. 첫 호출에 기록하고(버퍼 주소는 relocation 표),",
      "다음 호출은 memcpy + 주소 고치기만. vtalib cmd_queue.h ReplayBegin (EVTA_REPLAY=0으로 끔). 자세히: evta_runtime_aot.md §5.4."],
     dict(cpu=[("명령 생성 (호출마다, 함수 호출 수천 번)", "gone"), ("uop 생성 / 캐시 검색", "gone"), ("cache flush, 실행, 폴링", "")],
          dram=[("명령 큐 (매번 새로 씀)", "hot"), ("uop (매번 새로 씀)", "hot"), ("활성화, weight", "")],
          flows=[("cpu", "dram", "o", "명령 생성"), ("dram", "fetch", "b")]),
     dict(cpu=[("기록 복사 + relocation (호출마다)", "new"), ("cache flush, 실행, 폴링", "")],
          dram=[("기록 사본 (첫 호출)", "new"), ("명령 큐 ← 복사", "new"), ("uop ← 복사", "new"), ("활성화, weight", "")],
          flows=[("cpu", "dram", "g", "memcpy"), ("dram", "fetch", "b")], note=["EVTA가 받는 명령은 바이트 단위로 같다."]),
     [("Resnet18 추론 (single, OMP 1)", 37.08, 34.50, "ms")],
     [])

step(11, "conv0_pack",
     "11. conv0을 packed 입력의 4×1 conv로: 7×7 / stride 2 / 3채널 → 4×1 / stride 1 / 48채널 (11-02)",
     ["stride 2의 두 행과 7개 tap을 채널로 접는다(s · KW · 3 = 2 · 7 · 3 → 48 = 16의 배수, 빈 채널 없음). 1번 그림에서는 16채널 중 13이 0이었다.",
      "자세히: evta_runtime_compare.md §7.12. VTASave.cpp -vta-conv0-pack, vtalib preprocess_conv0pack. 모든 conv 출력이 byte 단위로 같다."],
     dict(cpu=[("zerofill 3 → 16채널 입력", "")], dram=[("conv0 입력 224×224×16 (13/16이 0)", "hot"), ("weight 7×7×16 → 64", "")],
          st={"gemm": "hot"}, label={"inp": ["inp_mem", "16채널 중 3만 값"], "gemm": ["GEMM 7×7 tap × 1 블록", "곱의 13/16이 0"]},
          flows=CONV_FLOWS, note=["GEMM 반복 = 49 tap × 1 입력 블록", "(GEMM 하한 6.27 ms)"]),
     dict(cpu=[("두 행씩 접어 48채널로 복사", "new")], dram=[("conv0 입력 115×112×48 (packed)", "new"), ("weight 4×1×48 → 64", "new")],
          st={"gemm": "hot", "inp": "new"}, label={"inp": ["inp_mem", "48채널 = 3 블록, 0 없음"], "gemm": ["GEMM 4 tap × 3 블록", "곱이 모두 유효"]},
          flows=CONV_FLOWS, note=["GEMM 반복 = 4 tap × 3 블록 = 12", "(이전 49의 1/4, GEMM 하한 1.63 ms)"]),
     [("Resnet18 추론 (single, OMP 1)", 34.48, 29.19, "ms"), ("conv0 EVTA 시간", 9.13, 3.64, "ms")],
     [])

step(12, "conv0_pool",
     "12. conv0 + max pool을 EVTA 호출 하나로: pool을 acc 안에서 ALU MAX 두 번(행, 열), pool 결과만 STORE (11-02)",
     ["conv tile에 pool의 halo 행 / 열을 다시 계산해 넣고, acc에서 MAX. conv0 출력 112×112×64를 DRAM에 썼다가 다시 읽던 왕복이 없어졌다.",
      "자세히: evta_runtime_compare.md §7.13. VTASave.cpp -vta-fuse-pool, vtalib convolution_wo_tr_pool."],
     dict(cpu=[("EVTA 호출 두 번 (conv0, maxpool)", "")], dram=[("conv0 출력 112×112×64", "hot"), ("pool 출력", "")],
          flows=CONV_FLOWS + [("dram", "load", "o", "다시 LOAD")], note=["호출 1: conv0 → STORE 전체", "호출 2: LOAD → identity GEMM → MAX → STORE"]),
     dict(cpu=[("EVTA 호출 한 번", "new")], dram=[("conv0 출력", "gone"), ("pool 출력 56×56×64", "")],
          st={"alu": "new"}, label={"alu": ["ALU MAX", "acc 안에서 2단계"], "out": ["out_mem", "pool 영역만 STORE"]},
          flows=CONV_FLOWS[:-1] + [("store", "dram", "b", "pool만")], note=["conv tile + halo → acc", "→ 행 방향 MAX, 열 방향 MAX", "→ pool 영역만 STORE"]),
     [("Resnet18 추론 (single, OMP 1)", 29.19, 28.83, "ms"), ("conv0 + maxpool EVTA", 4.83, 4.48, "ms")],
     [])


# ---------------------------------------------------------------------------------------------------------------
# hardware steps
def reg(s, x, y, h=30, label=None):
    s.rect(x, y, 10, h, "#3c8a35", "#245e1f", 1, 1)
    s.path(f"M{x},{y + h - 8} L{x + 6},{y + h - 4} L{x},{y + h}", "#ffffff", 1)
    if label:
        s.text(x + 5, y - 4, label, 9.5, "middle", color="#245e1f")


def sram(s, x, y, w, h, title, sub=None, ports=(), kind="mem"):
    s.box(x, y, w, h, kind, [(title, "bold")] + ([sub] if sub else []), 11)
    for side, yy, name in ports:
        px = x - 2 if side == "L" else x + w - 26
        s.rect(px, y + yy - 9, 28, 18, "#ffffff", "#777777", 0.8, 2)
        s.text(px + 14, y + yy + 4, name, 9.5, "middle")


@fig(13, "systolic_gemm")
def st13():
    s = Svg(1460, 1000, "13. GEMM을 HLS broadcast에서 RTL systolic(weight-stationary) 어레이로: 333 → 400 MHz (10-27~28)",
            ["GEMM 한 단계 = acc[d] (16 int32) += inp[s] (16 int8) · wgt[w]ᵀ (16×16 int8). 이전에는 HLS가 이것을 한 clk의 곱 256개 + 덧셈 트리로 만들었고,",
             "400 MHz 실패 경로 상위 200개 중 169개가 그 어레이였다. 자세히: evta_systolic_design.md, evta_systolic.md."])
    # before
    s.section(110, "이전: HLS GEMM (broadcast + 덧셈 트리, compute 안)")
    x0, y0 = 40, 166
    sram(s, x0, y0, 130, 50, "inp_mem", "inp[s] 16 int8")
    sram(s, x0, y0 + 90, 130, 50, "wgt_mem", "wgt[w] 16×16")
    s.rect(x0 + 180, y0 - 10, 330, 200, "#f3eef8", "#6d4596", 1.2, 6)
    s.text(x0 + 190, y0 + 8, "같은 clk 안: 곱 256개 → 행마다 16→1 덧셈 트리", 11.5, weight="bold", color="#6d4596")
    for r in range(4):
        for c in range(6):
            xx, yy = x0 + 200 + c * 40, y0 + 22 + r * 36
            s.rect(xx, yy, 26, 22, "#ffffff", "#6d4596", 0.8, 2)
            s.text(xx + 13, yy + 15, "×", 12, "middle")
            s.line(x0 + 130, y0 + 25, xx, yy + 11, "#c9b6dc", 0.5)
    s.text(x0 + 440, y0 + 90, "⋯ 256", 12, color="#6d4596")
    s.box(x0 + 200, y0 + 160, 290, 26, "hls", ["덧셈 트리 16 → 1 (×16 행)"], 10.5)
    sram(s, x0 + 560, y0 + 40, 130, 60, "acc_mem", "acc[d] += 합")
    s.arrow(x0 + 490, y0 + 172, x0 + 558, y0 + 80, "p", 1.6)
    s.lines(x0, y0 + 230, ["• inp 한 값이 16곳으로, wgt 256 값이 곱셈기로 한꺼번에 퍼지고(broadcast), 덧셈 트리가 4단 깊이:",
                           "  배선이 길어 380 MHz가 한계(333 MHz HLS 기준 bitstream)."], 12)
    # after
    s.section(470, "이후: RTL gemm_sa (16×16 DSP48E2 PE, PE 사이마다 register, weight는 PE 안에 머무름)", color="#2f7a2a")
    x0, y0 = 40, 530
    sram(s, x0, y0 + 30, 120, 50, "wgt_mem", "16 bank")
    s.text(x0, y0 + 110, "weight는 run마다 한 번", 11, color="#555555")
    s.text(x0, y0 + 126, "PE의 B1/B2 register로", 11, color="#555555")
    sram(s, x0, y0 + 160, 120, 50, "inp_mem", "inp[s] 16 int8")
    gx, gy = x0 + 220, y0
    n = 6
    for r in range(n):
        for c in range(n):
            xx, yy = gx + c * 62, gy + r * 50
            s.rect(xx, yy, 44, 34, "#fff3c4", "#9a7b00", 1, 3)
            s.text(xx + 22, yy + 14, "w", 10, "middle", color="#9a7b00")
            s.text(xx + 22, yy + 28, "MAC", 9.5, "middle")
            reg(s, xx + 46, yy + 6, 20)
            if r < n - 1:
                s.arrow(xx + 22, yy + 35, xx + 22, yy + 49, "y", 1)
    s.text(gx + n * 62 + 4, gy + 100, "⋯ 16×16", 12, color="#9a7b00")
    s.arrow(x0 + 122, y0 + 185, gx - 4, gy + 20, "b", 1.6)
    s.text(x0 + 130, y0 + 230, "inp 값이 한 clk씩 옆 PE로 (skew)", 11, color="#2f5f98")
    s.arrow(x0 + 122, y0 + 55, gx - 4, gy + 8, "y", 1.4)
    s.text(gx, gy + n * 50 + 12, "부분합이 한 clk씩 아래 PE로 → 16 clk 뒤 행 하나", 11.5, color="#9a7b00")
    rx = gx + n * 62 + 80
    s.box(rx, gy + 60, 230, 120, "rtl", [("sa_rmw (acc read-modify-write)", "bold"), "S0 tag, 읽기 주소", "S1 forwarding 비교", "S2 old + 행", "S3 acc 쓰기, out 쓰기"], 11)
    sram(s, rx + 260, gy + 60, 120, 54, "acc_mem", "포트 A 읽기")
    sram(s, rx + 260, gy + 126, 120, 54, "out_mem", "하위 8비트")
    s.arrow(gx + n * 62 + 10, gy + 150, rx - 2, gy + 120, "y", 1.6)
    s.arrow(rx + 232, gy + 90, rx + 258, gy + 87, "a", 1.4)
    s.arrow(rx + 232, gy + 150, rx + 258, gy + 153, "a", 1.4)
    s.lines(rx, gy + 210, ["• 한 clk에 PE 하나가 곱 하나 + 덧셈 하나만:", "  긴 배선과 덧셈 트리가 없어짐 → 400 MHz.",
                           "• HLS compute는 GEMM 단계마다 uop 하나를", "  AXI stream으로 보내기만 한다(sa_ctrl FIFO)."], 11.5)
    y = times(s, 40, 880, [("Resnet18 EVTA 시간 (_sysprof, 333 MHz HLS → 400 MHz systolic)", 48.54, 41.10, "ms"),
                            ("ResConv10 EVTA 시간", 2.893, 2.435, "ms")])
    return s


@fig(14, "alu_ii1_regs")
def st14():
    s = Svg(1460, 1000, "14. ALU imm 연산을 II = 2 → 1로, acc / out 쓰기 앞에 register (11-03 single, 11-04 multi4)",
            ["ALU 한 반복 = 16 lane에 acc[dst] ← op(acc[dst], imm 또는 acc[src]). acc_mem은 포트가 A, B 둘인 BRAM(읽기 2 clk 지연).",
             "자세히: evta_runtime_compare.md §7.15. HLS vta.cc alu() — ISA, vtalib, 컴파일러는 그대로."])
    s.section(110, "A. acc_mem 포트 사용: 이전에는 imm 연산도 src를 읽는 루프 → 반복마다 접근 3번 / 포트 2개 = 2 clk")
    cw = 62

    def lanes(y, title, seq, n, col):
        s.text(40, y + 14, title, 12.5, weight="bold", color=col)
        for t in range(n):
            s.text(230 + t * cw + cw / 2, y + 14, f"clk {t}", 10.5, "middle", color="#777777")
        for pi, pn in enumerate(["포트 A", "포트 B"]):
            yy = y + 22 + pi * 30
            s.text(222, yy + 19, pn, 11.5, "end")
            for t in range(n):
                s.rect(230 + t * cw, yy, cw, 26, "#fafafa", "#dddddd", 0.6)
        for pi, t, txt, k in seq:
            f, st_ = C[k]
            s.rect(232 + t * cw, y + 24 + pi * 30, cw - 4, 22, f, st_, 1, 3)
            s.text(230 + t * cw + cw / 2, y + 40 + pi * 30, txt, 10.5, "middle")
    seq = []
    for k in range(5):
        seq += [(0, 2 * k, f"src{k}", "hls"), (1, 2 * k, f"dst{k}", "hls")]
        if k >= 1:
            seq.append((0, 2 * k + 1, f"쓰기{k - 1}", "gone"))
    lanes(164, "이전 (II = 2)", [q for q in seq if q[1] < 12], 12, "#c0392b")
    s.text(1000, 200, "반복 6개에 12 clk", 12.5, color="#c0392b", weight="bold")
    seq = []
    for k in range(12):
        seq.append((0, k, f"dst{k}", "hls"))
        if k >= 4:
            seq.append((1, k, f"쓰기{k - 4}", "new"))
    lanes(250, "이후 imm (II = 1)", seq, 12, "#2a6a24")
    s.text(1000, 286, "반복 12개에 12 clk", 12.5, color="#2a6a24", weight="bold")
    s.lines(40, 350, ["• imm 연산(SHR, MIN/MAX imm, ADD imm)은 src를 읽지 않는다 → dst 읽기는 포트 A, 쓰기는 포트 B로 매 clk.",
                      "• reg-reg 연산(bias ADD, pool MAX, residual ADD)은 acc를 두 번 읽어야 해서 II = 2 루프에 남는다."], 12.5)
    s.section(410, "B. imm 루프의 pipeline: 한 clk에 다 들어가던 경로를 register로 끊음 (초록 = 넣은 register)")
    y = 470

    def stage_row(y, items, regs_after, label, wns, col):
        s.text(40, y + 22, label, 12.5, weight="bold")
        s.text(40, y + 40, wns, 11.5, color=col)
        x = 200
        for i, (k, ln, w) in enumerate(items):
            s.box(x, y, w, 48, k, ln, 10.5)
            nx = x + w + 30
            if i in regs_after:
                reg(s, x + w + 10, y + 9, 30)
                s.arrow(x + w + 1, y + 24, x + w + 9, y + 24, "a", 1)
                s.arrow(x + w + 21, y + 24, nx - 1, y + 24, "a", 1)
            elif i + 1 < len(items):
                s.arrow(x + w + 1, y + 24, nx - 1, y + 24, "a", 1.2)
            x = nx
    items = [("mem", ["uop_mem 읽기"], 110), ("hls", ["dst = uop", "+ offset"], 100), ("mem", ["acc_mem 주소", "(BRAM 58개)"], 120),
             ("mem", ["acc 읽기", "2 clk"], 90), ("hls", ["ALU 16 lane", "op(dst, imm)"], 110), ("mem", ["acc 쓰기 (B)", "out 쓰기"], 120)]
    stage_row(y, items, (), "single 첫 빌드", "WNS −0.819 ns", "#c0392b")
    stage_row(y + 76, items, (0, 1), "single (alu1_epr)", "WNS +0.001 ns", "#2a6a24")
    stage_row(y + 152, items, (0, 1, 4), "multi4, 이후 single", "WNS +0.001 / +0.000", "#2a6a24")
    s.text(1090, y + 28, "← 주소 계산이 BRAM 58개로", 11.5, color="#c0392b")
    s.text(1090, y + 104, "← uop 뒤, 주소 뒤 register", 11.5, color="#2a6a24")
    s.text(1090, y + 180, "← ALU 결과 뒤(쓰기 데이터 앞)도", 11.5, color="#2a6a24")
    y2 = y + 230
    s.text(40, y2 + 22, "RESET_MEM (acc LOAD pad / reset)", 12.5, weight="bold")
    x = 320
    s.box(x, y2, 150, 44, "gone", ["루프 i++ 비교 →", "쓰기 enable 58개"], 10.5, dash="4 3")
    s.text(x + 170, y2 + 26, "→", 16)
    s.box(x + 200, y2, 160, 44, "new", ["주소 = base + i", "(register, II = 1)"], 10.5)
    reg(s, x + 370, y2 + 7, 30)
    s.box(x + 400, y2, 120, 44, "mem", ["acc 쓰기"], 10.5)
    s.text(x + 540, y2 + 26, "multi4 WNS −0.343 → +0.001 (hold 한 net은 holdpin.tcl)", 11.5, color="#555555")
    s.lines(40, y2 + 76, ["• register는 깊이(latency)만 늘리고 매 clk 한 반복은 그대로다(imm 루프 깊이 10, II = 2 루프 11).",
                          "• 같은 dst를 몇 clk 안에 다시 읽는 명령은 없어야 한다(DEPENDENCE inter false): Resnet18 ALU 명령 1,566개를 덤프해 확인."], 12.5)
    times(s, 40, y2 + 130, [("runbench aluimm 16 2048 (반복당 clk)", 2.0, 1.0, "clk"),
                            ("Resnet18 EVTA 시간 (single, 같은 실행 파일)", 26.42, 24.89, "ms"),
                            ("Resnet18 추론 (single, OMP 1)", 28.98, 27.32, "ms"),
                            ("multi4 4 copy 동시, EVTA마다 (가운데 값)", 26.79, 25.30, "ms")])
    return s


def rmw_pipe(s, x0, y0, bias, rq):
    """sa_rmw.v stages S0..S3 (+ S4..S6 with rq): muxes, adders, registers"""
    sw_ = 150
    names = ["S0", "S1", "S2", "S3"] + (["S4", "S5", "S6"] if rq else [])
    for i, nme in enumerate(names):
        x = x0 + i * (sw_ + 18)
        s.rect(x, y0, sw_, 250, "#fffbea" if i < 4 else "#eef8ea", "#d9c98a" if i < 4 else "#9fcf93", 1, 4)
        s.text(x + sw_ / 2, y0 + 16, nme, 12.5, "middle", "bold")
    X = [x0 + i * (sw_ + 18) for i in range(len(names))]
    # S0
    s.box(X[0] + 8, y0 + 30, sw_ - 16, 40, "rtl", ["tag: dst, reset", "+ bias, src" if bias else "(last, tok)"], 10)
    s.box(X[0] + 8, y0 + 86, sw_ - 16, 44, "new" if bias else "rtl", ["읽기 주소", "bias ? src : dst" if bias else "= dst"], 10)
    s.box(X[0] + 8, y0 + 150, sw_ - 16, 36, "rtl", ["행 (어레이 출력)"], 10)
    # S1
    s.box(X[1] + 8, y0 + 30, sw_ - 16, 50, "rtl", ["forwarding 비교", "진행 중 쓰기 3개와", "읽은 주소 비교" if bias else "dst 비교"], 9.5)
    s.box(X[1] + 8, y0 + 150, sw_ - 16, 36, "new" if bias else "rtl", ["행 = bias ? 0 : 행" if bias else "행 register"], 10)
    reg(s, X[1] + sw_ - 6, y0 + 152, 30)
    # S2
    s.box(X[2] + 8, y0 + 30, sw_ - 16, 44, "mem", ["acc_mem 데이터", "(포트 A, 2 clk)"], 10)
    s.box(X[2] + 8, y0 + 90, sw_ - 16, 44, "rtl", ["old = forward", "? S3/w1/w2 : 데이터"], 9.5)
    s.box(X[2] + 8, y0 + 150, sw_ - 16, 36, "rtl", ["sum = old + 행", "(16 lane int32)"], 9.5)
    reg(s, X[2] + sw_ - 6, y0 + 152, 30)
    # S3
    s.box(X[3] + 8, y0 + 30, sw_ - 16, 48, "mem", ["acc 쓰기 (포트 B)", "reset && !bias ? 0" if bias else "reset ? 0 : sum", ": sum" if bias else ""], 9.5)
    s.box(X[3] + 8, y0 + 96, sw_ - 16, 40, "rtl", ["forwarding 원천", "s3, w1, w2 register"], 9.5)
    if rq:
        s.box(X[3] + 8, y0 + 150, sw_ - 16, 36, "gone", ["out ← sum 하위 8비트"], 9.5, dash="4 3")
        s.box(X[4] + 8, y0 + 60, sw_ - 16, 60, "new", ["t = sum + r", "r = 2^(s−1)", "[− (sum<0)]"], 9.5)
        reg(s, X[4] + sw_ - 6, y0 + 74, 30)
        s.box(X[5] + 8, y0 + 60, sw_ - 16, 60, "new", ["h = t >>> shift", "(산술, 5단 mux)"], 9.5)
        reg(s, X[5] + sw_ - 6, y0 + 74, 30)
        s.box(X[6] + 8, y0 + 60, sw_ - 16, 60, "new", ["h < lo ? lo :", "h > hi ? hi :", "h[7:0]"], 9.5)
        s.box(X[6] + 8, y0 + 150, sw_ - 16, 36, "mem", ["out 쓰기 (int8)"], 10)
        s.box(X[6] + 8, y0 + 200, sw_ - 16, 36, "new", ["done, token, busy"], 10)
        s.arrow(X[3] + sw_ - 8, y0 + 168, X[4] + 8, y0 + 100, "g", 1.4)
        s.arrow(X[4] + sw_ + 6, y0 + 90, X[5] + 6, y0 + 90, "g", 1.4)
        s.arrow(X[5] + sw_ + 6, y0 + 90, X[6] + 6, y0 + 90, "g", 1.4)
        s.arrow(X[6] + sw_ / 2, y0 + 122, X[6] + sw_ / 2, y0 + 148, "g", 1.4)
    else:
        s.box(X[3] + 8, y0 + 150, sw_ - 16, 36, "mem", ["out ← sum 하위 8비트"], 9.5)
        s.box(X[3] + 8, y0 + 200, sw_ - 16, 36, "rtl", ["done, token"], 10)
    # main arrows
    s.arrow(X[0] + sw_ - 8, y0 + 108, X[1] + 8, y0 + 56, "a", 1.2)
    s.arrow(X[0] + sw_ - 8, y0 + 168, X[1] + 8, y0 + 168, "a", 1.2)
    s.arrow(X[1] + sw_ + 6, y0 + 168, X[2] + 8, y0 + 168, "a", 1.2)
    s.arrow(X[2] + sw_ / 2, y0 + 76, X[2] + sw_ / 2, y0 + 88, "a", 1.2)
    s.arrow(X[2] + sw_ / 2, y0 + 136, X[2] + sw_ / 2, y0 + 148, "a", 1.2)
    s.arrow(X[2] + sw_ + 6, y0 + 168, X[3] + 8, y0 + 60, "a", 1.2)
    s.arrow(X[2] + sw_ + 6, y0 + 172, X[3] + 8, y0 + 168, "a", 1.2)
    s.path(f"M{X[3] + 20},{y0 + 138} L{X[3] + 20},{y0 + 244} L{X[2] + 30},{y0 + 244} L{X[2] + 30},{y0 + 136}", "k", 1.1, arrow="k", dash="3 2")
    s.text(X[2] + 40, y0 + 240, "forwarding (1~3 clk 전 쓰기)", 9.5, color="#777777")
    return X


@fig(15, "bias_reset")
def st15():
    s = Svg(1460, 1080, "15. bias reset: GEMM의 reset 단계가 acc를 0 대신 bias로 시작 → GEMM 뒤의 ALU bias ADD(reg, II = 2)가 없어짐 (11-04)",
            ["conv 출력 = Σ x·w + bias. 이전: reset이 acc ← 0, GEMM 누산, 그 뒤 bias를 acc 빈 자리에 LOAD하고 ALU ADD로 tile 전체에 더함.",
             "이후: bias를 먼저 LOAD, reset 단계가 acc ← acc[bias]. ISA VTAGemInsn 비트 140 bias_reset. 자세히: evta_runtime_compare.md §7.16, evta_isa_ext.md."])
    s.section(110, "A. acc_mem 안 (tile 하나, 출력 채널 블록 nF개 × 출력 위치 T개, bias entry nF개는 tile 바로 뒤)")
    y = 160
    for row, (lbl, cells, col) in enumerate([("이전", ["reset: 0", "GEMM: += x·w", "LOAD bias → 뒤쪽", "ALU ADD: += bias"], "#c0392b"),
                                               ("이후", ["LOAD bias → 뒤쪽", "reset: ← acc[bias + f]", "GEMM: += x·w", ""], "#2a6a24")]):
        yy = y + row * 120
        s.text(40, yy + 40, lbl, 14, weight="bold", color=col)
        s.rect(130, yy, 760, 80, "#f5f5f5", "#999999", 1, 3)
        for f in range(4):
            for p in range(8):
                s.rect(140 + f * 150 + p * 17, yy + 10, 15, 30, "#dce8f5", "#2f5f98", 0.6)
            s.text(140 + f * 150 + 66, yy + 58, f"f = {f} (T개)" if f < 3 else "⋯", 10.5, "middle")
        for f in range(4):
            s.rect(750 + f * 30, yy + 10, 26, 30, "#efe6d6", "#8a6d3b", 0.8)
        s.text(800, yy + 58, "bias[0..nF)", 10.5, "middle")
        x = 920
        for i, c in enumerate(cells):
            if not c:
                continue
            k = "gone" if (row == 0 and "ADD" in c) else ("new" if (row == 1 and "acc[bias" in c) else "evta")
            s.box(x, yy + 6 + (i % 2) * 36, 255, 30, k, [f"{i + 1}. {c}"], 10.5)
            if i % 2 == 1:
                x += 268
        if row == 1:
            s.path(f"M{780},{yy + 10} C{700},{yy - 18} {300},{yy - 18} {200},{yy + 10}", "g", 1.6, arrow="g")
            s.text(430, yy - 12, "reset 단계: 각 위치 p에 bias[f]를 복사 (uop src = bias + f)", 11, color="#2a6a24")
    s.section(380, "B. sa_rmw.v (RTL, 16 lane): 바뀐 곳은 초록 — 읽기 주소 mux, 행 0 mux(S1), 쓰기 값 조건")
    rmw_pipe(s, 40, 430, bias=True, rq=False)
    s.lines(760, 450, ["• S2(forwarding 선택 + 32비트 덧셈)가 400 MHz에서 가장", "  빡빡해, bias 행의 '행 = 0'은 한 단계 앞 S1에서 만든다.",
                       "• 읽는 주소가 src로 바뀌므로 forwarding 비교도 '읽은", "  주소'로 한다: 방금 LOAD한 bias entry도 맞게 읽는다.",
                       "• sa_ctrl의 uop FIFO / tag에 bias 1비트와 src를 더 싣는다.", "• HLS compute: uop 비트 VTA_SA_UOP_B + 5 = reset && bias_reset."], 12)
    s.section(710, "C. 시간: ALU ADD(reg 연산, 2 clk / 반복)가 빠짐. conv별로는 작은 층에서 덜 줄었다", color="#2f7a2a")
    y = 760
    rows = [("conv0 (+pool)", 0.316, 0.292), ("stage1 conv (개당)", 0.066, 0.063), ("stage2 conv", 0.028, 0.033),
            ("stage3 conv", 0.007, 0.016), ("stage4 conv", 0.002, 0.008)]
    for i, (lbl, a, b) in enumerate(rows):
        yy = y + i * 30
        s.text(250, yy + 16, lbl, 12, "end")
        s.rect(262, yy + 2, a * 900, 10, "#7aa6d6", "#2f5f98", 0.8)
        s.rect(262, yy + 13, b * 900, 10, "#a8d49e", "#3c8a35", 0.8)
        s.text(270 + max(a, b) * 900, yy + 17, f"−{a:.3f} / −{b:.3f} ms", 11.5)
    s.text(262, y + 166, "파랑: 그대로   초록: DRAM 전송을 뺀 실험(EVTA_MEMEXP=15) — stage3/4는 ADD가 inp LOAD / STORE와 겹쳐 있었다", 11.5, color="#555555")
    times(s, 40, y + 190, [("Resnet18 EVTA 시간 (single, 같은 실행 파일)", 24.889, 24.154, "ms"), ("Resnet18 추론 (single, OMP 1)", 27.303, 26.592, "ms"),
                           ("multi4 4 copy 동시, EVTA마다 (가운데 값)", 25.30, 24.555, "ms")])
    return s


@fig(16, "requantize")
def st16():
    s = Svg(1460, 1280, "16. requantize를 acc → out 쓰기 길에: ALU SHR / MAX / MIN 명령 대신 out에 쓸 때 반올림 · shift · clip (11-04)",
            ["out_mem은 STORE가 DRAM으로 옮기는 int8이고, 원래는 acc의 하위 8비트를 배선으로 받기만 했다(logic 없음). 그래서 requantize를",
             "acc 값 자체에 ALU 명령으로 했다. 이제 GEMM / ALU 명령의 rq_* 필드로 out 쓰기 길에서 한다. 자세히: evta_runtime_compare.md §7.18, evta_isa_ext.md."])
    s.section(110, "A. GEMM의 sa_rmw.v: acc 쪽(S0~S3)은 그대로, out 쪽에 S4~S6 (lane 16개마다 덧셈기, barrel shifter, 비교기 2개)")
    rmw_pipe(s, 30, 160, bias=True, rq=True)
    s.lines(30, 440, ["• rq_en = 0이면 r = 0, shift = 0, clip 대신 하위 8비트 → 이전과 bit 단위로 같다. out 쓰기 / done이 3 clk 늦어지는 것만 남는다.",
                      "• rq 24비트(en, round, shift, lo, hi)는 명령 → HLS가 uop마다 → sa_ctrl FIFO와 tag pipe → S4~S6 (uop stream 41 → 65비트)."], 12)
    s.section(490, "B. ALU (HLS vta.cc alu()): 연산 결과를 acc에 쓰고, out에는 requant(결과)")
    y = 545
    x = 40
    for i, (k, ln, w) in enumerate([("mem", ["acc 읽기 (A)"], 110), ("hls", ["ALU op", "(MIN/MAX/ADD/SHR)"], 130),
                                    ("mem", ["acc 쓰기 (B)"], 110)]):
        s.box(x, y, w, 46, k, ln, 10.5)
        if i < 2:
            s.arrow(x + w + 2, y + 23, x + w + 28, y + 23, "a", 1.2)
        x += w + 30
    s.box(320, y + 70, 220, 46, "new", ["requant(결과): + r, >>> s, clip"], 10.5)
    s.box(580, y + 70, 130, 46, "mem", ["out 쓰기"], 10.5)
    s.arrow(290, y + 48, 360, y + 68, "g", 1.4)
    s.arrow(542, y + 93, 578, y + 93, "g", 1.4)
    s.box(760, y + 70, 200, 46, "gone", ["이전: out ← 결과 하위 8비트"], 10.5, dash="4 3")
    s.lines(1000, y + 10, ["• imm 루프(II = 1)와 reg 루프(II = 2) 둘 다.", "• HLS 추정 clock / critical path는 그대로", "  (기존 MIN/MAX 비교가 최장)."], 12)
    s.section(690, "C. tile 하나의 compute 명령: 빨강이 빠진 ALU 명령 (반복마다 1~2 clk × tile 원소 수)")
    y = 742
    rows = [("일반 conv", [("evta", "LOAD bias", 90), ("evta", "GEMM reset + GEMM ×C/16", 210), ("gone", "SHR", 60), ("gone", "MAX", 60),
                          ("gone", "MIN", 60), ("mem", "STORE", 80)], "GEMM 명령이 rq를 갖는다"),
            ("conv + residual", [("evta", "conv + SHR/MAX/MIN + ADD×shl", 250), ("evta", "skip identity GEMM", 150), ("gone", "ADD r", 60),
                                ("gone", "SHR", 50), ("gone", "MAX", 50), ("gone", "MIN", 50), ("mem", "STORE", 70)], "skip GEMM이 rq(round 1)"),
            ("conv0 + pool", [("evta", "GEMM", 70), ("gone", "SHR, MAX, MIN", 130), ("evta", "MAX 행 방향", 110), ("new", "MAX 열 방향 (rq)", 150),
                             ("mem", "STORE", 70)], "max와 requantize는 순서를 바꿔도 같다"),
            ("fc", [("evta", "LOAD bias", 90), ("evta", "fc GEMM chunk들", 150), ("gone", "reset, ADD, MIN, MAX, ADD, ADD, SHR, MAX, MIN", 360),
                    ("mem", "STORE", 70)], "GEMM이 rq(round 2: half away from 0)")]
    for i, (lbl, items, note) in enumerate(rows):
        yy = y + i * 56
        s.text(40, yy + 24, lbl, 12.5, weight="bold")
        x = 190
        for k, t, w in items:
            s.box(x, yy, w, 36, k, [t], 10.5, dash="4 3" if k == "gone" else None)
            x += w + 8
        s.text(x + 10, yy + 23, note, 11.5, color="#2a6a24")
    times(s, 40, 990, [("Resnet18 EVTA 시간 (single, bias → _rq bitstream)", 24.153, 23.053, "ms"),
                       ("Resnet18 추론 (single, OMP 1)", 26.611, 25.493, "ms"),
                       ("conv0 + pool EVTA", 3.761, 3.295, "ms"), ("stage1 conv1 EVTA", 1.586, 1.454, "ms"),
                       ("multi4 4 copy 동시, EVTA마다 (가운데 값)", 24.555, 23.50, "ms")])
    s.lines(40, 1180, ["• 비용: LUT 34.3K → 42.1K(single), 133.3K → 164.1K(multi4), BRAM / DSP 그대로. single 400 MHz WNS +0.039 ns, multi4 400 MHz +0.002 ns.",
                       "• 정확도: golden 27 PASS + 분류 4개 같음, 층 출력 Resnet18 40개 · Resnet50 106개 byte 동일, EVTA_RES_CHECK 0, EVTA_REQUANT=0 우회도 동일."], 12.5)
    return s


@fig(17, "summary")
def st17():
    s = Svg(1460, 1000, "17. Resnet18 추론 시간을 줄인 단계 (single EVTA, 각 단계는 같은 날 바로 앞과 비교)",
            ["막대 = 그 단계 뒤의 추론 시간(ms, 0부터). 오른쪽 = 바로 앞 단계 대비 줄어든 시간과 %. 초록 = CPU 일을 EVTA로, 파랑 = CPU / runtime, 노랑 = 하드웨어.",
             "OMP 1 기준(1번만 OMP 4). 하드웨어 단계(13~16)는 EVTA 시간도 함께. 각 숫자의 출처는 그림 번호의 문서 절."])
    st = [("1", "conv0 → EVTA (OMP 4)", 131.8, 78.6, "g"), ("2", "EVTA layout 유지", 148.7, 91.1, "b"),
          ("3", "residual → EVTA", 90.93, 67.13, "g"), ("4", "maxpool → EVTA", 67.13, 52.48, "g"), ("5", "transpose 16 B 복사", 52.48, 46.90, "b"),
          ("6", "버퍼 pool", 46.97, 44.27, "b"), ("7", "CPU fc NEON", 44.27, 40.56, "b"), ("8", "fc → EVTA", 40.56, 39.92, "g"),
          ("9", "전처리 합치기", 39.92, 37.08, "b"), ("10", "명령 기록 / 재생", 37.08, 34.50, "b"), ("11", "conv0 4×1 pack", 34.48, 29.19, "g"),
          ("12", "conv0 + pool 한 호출", 29.19, 28.83, "g"), ("14", "ALU imm II = 1 (HW)", 28.98, 27.32, "y"),
          ("15", "bias reset (HW)", 27.30, 26.59, "y"), ("16", "requantize (HW)", 26.61, 25.49, "y")]
    sc = 5.2
    y = 120
    fills = {"g": ("#a8d49e", "#3c8a35"), "b": ("#b9d0ea", "#2f5f98"), "y": ("#ffe58f", "#9a7b00")}
    for i, (n, lbl, a, b, k) in enumerate(st):
        yy = y + i * 50
        s.text(300, yy + 22, f"{n}. {lbl}", 13, "end", "bold")
        s.rect(312, yy + 4, a * sc, 14, "#eeeeee", "#bbbbbb", 0.6)
        s.rect(312, yy + 20, b * sc, 16, *fills[k], 1)
        s.text(312 + a * sc + 8, yy + 16, f"{a:g}", 10.5, color="#777777")
        s.text(312 + b * sc + 8, yy + 33, f"{b:g} ms", 11.5, weight="bold")
        d = b - a
        s.text(1440, yy + 26, f"{d:+.2f} ms ({d / a * 100:+.1f} %)".replace("-", "−"), 13, "end", "bold", "#2a6a24")
    s.text(312, y + len(st) * 50 + 10, "13. GEMM systolic 400 MHz (HW): Resnet18 EVTA 시간 48.54 → 41.10 ms (−15.3 %), 위 줄들과 다른 날·다른 runtime이라 막대에서 뺐다.", 12, color="#555555")
    s.text(312, y + len(st) * 50 + 32, "single OMP 1 전체: 148.7 ms(10-30) → 25.49 ms(11-04), −83 %. EVTA 시간: 32.18(11-02 conv0 pack 전) → 23.05 ms.", 13, weight="bold")
    return s


def main():
    nums = [int(a) for a in sys.argv[1:]] or sorted(FIGS)
    for n in nums:
        name, f = FIGS[n]
        s = f()
        p = os.path.join(OUT, f"evta_st{n:02d}_{name}.svg")
        s.save(p)
        print("wrote", os.path.relpath(p))


if __name__ == "__main__":
    main()
