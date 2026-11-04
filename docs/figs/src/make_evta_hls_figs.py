#!/usr/bin/env python3
# Structure of the original HLS EVTA (single, vta-hw-zcu102-single: src/vta.cc fetch / load / compute / store, scripts/vivado.tcl
# block design, config 16/16/19/18) -> docs/figs/evta_hls_NN_*.svg. One overall figure, then one per part.
# usage: python3 docs/figs/src/make_evta_hls_figs.py [NN ...]
# Sizes from build/hardware/xilinx/include/zcu102_1x16_i8w8a32_16_16_19_18/vta_config.tcl and include/vta/hw_spec.h.
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


K = {"hls": C["hls"], "fifo": ("#fff3c4", "#9a7b00"), "bram": ("#e6eef7", "#4a6f9a"), "axi": ("#eeeeee", "#666666"),
     "ps": C["cpu"], "dram": C["dram"], "reg": ("#ffffff", "#888888"), "tok": ("#fde2e2", "#b03a2e")}


def blk(s, x, y, w, h, kind, lines, size=11, title=None, sw=1.4, rx=5, dash=None):
    s.box(x, y, w, h, K[kind] if kind in K else kind, ([(title, "bold")] if title else []) + list(lines), size, rx=rx, sw=sw, dash=dash)
    return (x, y, w, h)


def fifo(s, x, y, w, h, name, sub, vertical=False):
    """a FIFO drawn as a box with slots"""
    f, st = K["fifo"]
    s.rect(x, y, w, h, f, st, 1.3, 3)
    n = 6
    if vertical:
        for i in range(1, n):
            s.line(x + 3, y + i * h / n, x + w - 3, y + i * h / n, st, 0.6)
        s.text(x + w + 6, y + h / 2 - 2, name, 11, weight="bold")
        s.text(x + w + 6, y + h / 2 + 13, sub, 10, color="#555555")
    else:
        for i in range(1, n):
            s.line(x + i * w / n, y + 3, x + i * w / n, y + h - 3, st, 0.6)
        s.text(x + w / 2, y - 6, name, 11, "middle", "bold")
        s.text(x + w / 2, y + h + 13, sub, 10, "middle", color="#555555")


def bram(s, x, y, w, h, name, sub, pa=None, pb=None, banks=1):
    f, st = K["bram"]
    for b in range(min(banks, 4) - 1, 0, -1):
        s.rect(x + b * 5, y - b * 5, w, h, f, st, 0.8, 3)
    s.rect(x, y, w, h, f, st, 1.4, 3)
    s.text(x + w / 2, y + h / 2 - 3, name, 11.5, "middle", "bold")
    s.text(x + w / 2, y + h / 2 + 12, sub, 10, "middle")
    if pa:
        s.rect(x - 4, y + 4, 22, 16, "#ffffff", st, 0.8, 2)
        s.text(x + 7, y + 16, "A", 10, "middle", "bold")
        s.text(x - 8, y + 16, pa, 9.5, "end", color="#4a6f9a")
    if pb:
        s.rect(x + w - 18, y + h - 20, 22, 16, "#ffffff", st, 0.8, 2)
        s.text(x + w - 7, y + h - 8, "B", 10, "middle", "bold")
        s.text(x + w + 8, y + h - 8, pb, 9.5, color="#4a6f9a")


def arr(s, x1, y1, x2, y2, c="a", sw=1.5, label=None, lx=0, ly=-5, size=10, dash=None, anchor="middle"):
    s.arrow(x1, y1, x2, y2, c, sw, dash)
    if label:
        col = {"a": "#333333", "b": "#2f5f98", "o": "#c06a10", "g": "#3c8a35", "r": "#b03a2e", "p": "#6d4596", "y": "#9a7b00", "k": "#888888"}[c]
        s.text((x1 + x2) / 2 + lx, (y1 + y2) / 2 + ly, label, size, anchor, color=col)


def poly(s, pts, c="a", sw=1.5, dash=None, arrow=True):
    d = "M" + " L".join(f"{x},{y}" for x, y in pts)
    s.path(d, c, sw, arrow=c if arrow else None, dash=dash)


# ---------------------------------------------------------------------------------------------------------------
@fig(1, "overall")
def h1():
    s = Svg(1460, 940, "HLS EVTA 1. 전체 구조 (single, vta-hw-zcu102-single, config 16/16/19/18)",
            ["Vivado block design(scripts/vivado.tcl) 그대로: HLS IP 4개(vta.cc의 fetch / load / compute / store), 명령 큐 3개와 dependence 큐 4개(fifo_generator, AXI-Stream),",
             "SRAM 3종(blk_mem_gen, true dual port), compute 안의 uop_mem / acc_mem, DRAM 쪽 AXI(smartconnect → PS HP0), 레지스터 쪽 AXI(PS HPM0 → axi_xbar)."])
    # PS
    s.rect(30, 104, 1400, 128, *K["ps"], 1.6, 8)
    s.text(44, 124, "PS (Zynq UltraScale+ XCZU9EG): Cortex-A53 ×4, DDR4", 13, weight="bold")
    blk(s, 50, 136, 300, 84, "ps", ["vtalib (pynq_driver.cc): 명령 / uop / 데이터를", "CMA 버퍼에 쓰고, 레지스터로 시작, done 폴링"], 10.5, "CPU")
    blk(s, 370, 136, 380, 84, "dram", ["명령 256b × n, uop 64b, inp, wgt, bias int32, out", "명령의 dram_base = 물리 주소 / 원소 크기"], 10.5, "DRAM (CMA, 캐시 비일관)")
    blk(s, 770, 136, 250, 84, "axi", ["M_AXI_HPM0_FPD 128b", "pl_clk0 100 MHz"], 10.5, "PS → PL 레지스터")
    blk(s, 1040, 136, 370, 84, "axi", ["S_AXI_HP0_FPD (SAXIGP2) 128b", "PL이 DRAM 읽기 / 쓰기"], 10.5, "PL → DRAM")
    s.rect(30, 246, 1400, 680, "#f8fafd", "#2f5f98", 1.6, 8)
    s.text(44, 266, "PL: EVTA (clk_wiz pll_clk: pl_clk0 100 MHz → EVTA 클럭 FREQ; proc_sys_reset; 아래 모두 같은 클럭)", 12.5, weight="bold", color="#2f5f98")
    # AXI
    blk(s, 770, 280, 250, 44, "axi", ["axi_xbar (axi_interconnect 1 → 4)", "S00 100 MHz → M 쪽 EVTA 클럭"], 10)
    blk(s, 1100, 280, 300, 44, "axi", ["axi_smc0 (smartconnect 5 → 1)", "m_axi 5개 → HP0"], 10)
    arr(s, 895, 222, 895, 278, "k", 1.4, "AXI-Lite", 30, 0)
    arr(s, 1250, 278, 1250, 222, "b", 1.8, "AXI4 128b", 40, 0)
    # modules
    F = (50, 360, 200, 100)
    L = (290, 500, 250, 140)
    Cm = (650, 500, 320, 170)
    St = (1110, 500, 260, 140)
    blk(s, *F, "hls", ["insns[pc] 읽어 opcode로", "세 명령 큐에 나눔", "0xA0000000, START"], 10.5, "fetch", sw=2)
    blk(s, *L, "hls", ["DRAM → inp_mem / wgt_mem", "(pad는 0으로)", "0xA0001000, AUTORESTART"], 10.5, "load", sw=2)
    blk(s, *St, "hls", ["out_mem → DRAM", "", "0xA0003000, AUTORESTART"], 10.5, "store", sw=2)
    s.rect(*Cm, *K["hls"], 2, 5)
    s.text(Cm[0] + 160, Cm[1] + 20, "compute", 11.5, "middle", "bold")
    s.text(Cm[0] + 160, Cm[1] + 36, "LOAD uop / acc, GEMM, ALU, FINISH", 10.5, "middle")
    s.text(Cm[0] + 160, Cm[1] + 52, "0xA0002000, AUTORESTART, done", 10.5, "middle")
    bram(s, Cm[0] + 14, Cm[1] + 70, 140, 52, "uop_mem", "8192 × 64b")
    bram(s, Cm[0] + 168, Cm[1] + 70, 140, 52, "acc_mem", "4096 × 512b")
    s.text(Cm[0] + 160, Cm[1] + 150, "GEMM 16×16 (곱 256 + 덧셈 트리), ALU 16 lane", 10.5, "middle", color="#6d4596")
    # control bus (grey dashed) from xbar
    yb = 340
    poly(s, [(895, 326), (895, yb)], "k", 1.0, "3 2", arrow=False)
    s.line(150, yb, 1340, yb, "#888888", 1.0, "3 2")
    for x, top in [(150, F[1]), (520, L[1]), (950, Cm[1]), (1340, St[1])]:
        poly(s, [(x, yb), (x, top - 2)], "k", 1.0, "3 2")
    s.text(560, yb - 6, "s_axi_CONTROL_BUS (레지스터) ×4", 10, color="#777777")
    # AXI master bus (blue) at y=478
    ym = 478
    s.line(110, ym, 1415, ym, "#2f5f98", 1.4)
    poly(s, [(1415, ym), (1415, 302), (1402, 302)], "b", 1.4)
    for x, top, lab in [(110, F[1] + F[3], "ins_port"), (330, L[1], "data_port"), (690, Cm[1], "uop_port"), (760, Cm[1], "data_port(bias)"), (1150, St[1], "data_port")]:
        if top > ym:
            poly(s, [(x, top - 2), (x, ym)], "b", 1.3)
        else:
            poly(s, [(x, top + 2), (x, ym)], "b", 1.3)
        s.text(x + 4, ym - 4 if top > ym else ym + 12, lab, 9.5, color="#2f5f98")
    s.text(1300, ym - 6, "m_axi ×5", 10, color="#2f5f98", weight="bold")
    # instruction queues (purple) right of fetch, feeding down
    qy = 384
    Q = {"load_queue": (330, L), "gemm_queue": (800, Cm), "store_queue": (1180, St)}
    s.line(252, qy + 15, 1180, qy + 15, "#6d4596", 1.6)
    for nm, (qx, M) in Q.items():
        fifo(s, qx, qy, 110, 30, nm, "")
        s.text(qx + 55, qy + 44, "32B × 512", 9.5, "middle", color="#555555")
        poly(s, [(qx + 110, qy + 15), (qx + 130, qy + 15), (qx + 130, M[1] - 2)], "p", 1.6)
    s.text(256, qy + 8, "명령", 10, color="#6d4596", weight="bold")
    # dependence queues between modules
    def depq(x, y, nm, right):
        fifo(s, x, y, 76, 22, "", "")
        s.text(x + 38, y - 4, nm, 10.5, "middle", "bold", "#b03a2e")
    depq(552, 530, "l2g", True)
    arr(s, 542, 541, 550, 541, "r", 1.4)
    arr(s, 630, 541, 648, 541, "r", 1.4)
    depq(552, 600, "g2l", False)
    arr(s, 648, 611, 630, 611, "r", 1.4)
    arr(s, 550, 611, 542, 611, "r", 1.4)
    depq(1000, 530, "g2s", True)
    arr(s, 972, 541, 998, 541, "r", 1.4)
    arr(s, 1078, 541, 1108, 541, "r", 1.4)
    depq(1000, 600, "s2g", False)
    arr(s, 1108, 611, 1078, 611, "r", 1.4)
    arr(s, 998, 611, 972, 611, "r", 1.4)
    s.text(1000, 646, "1B × 1024 (token)", 9.5, color="#b03a2e")
    s.text(552, 646, "1B × 1024 (token)", 9.5, color="#b03a2e")
    # SRAMs
    by = 760
    bram(s, 300, by, 170, 62, "inp_mem", "4096 × 128b", "load", "compute")
    bram(s, 540, by, 190, 62, "wgt_mem ×16 bank", "2048 × 128b each", "load", "compute", banks=4)
    bram(s, 1040, by, 170, 62, "out_mem", "4096 × 128b", "compute", "store")
    arr(s, 360, L[1] + L[3] + 2, 360, by - 2, "b", 1.6, "A 쓰기", 26, 0)
    arr(s, 520, L[1] + L[3] + 2, 560, by - 18, "b", 1.6, "A 쓰기", 30, 10)
    poly(s, [(700, Cm[1] + Cm[3] + 2), (700, 712), (440, 712), (440, by - 2)], "g", 1.4)
    s.text(450, 708, "B 읽기", 10, color="#3c8a35")
    poly(s, [(760, Cm[1] + Cm[3] + 2), (760, by - 2)], "g", 1.4)
    s.text(766, 730, "B 읽기", 10, color="#3c8a35")
    poly(s, [(930, Cm[1] + Cm[3] + 2), (930, 730), (1080, 730), (1080, by - 2)], "b", 1.6)
    s.text(940, 726, "A 쓰기", 10, color="#2f5f98")
    poly(s, [(1180, by - 2), (1180, St[1] + St[3] + 2)], "g", 1.6)
    s.text(1186, 730, "B 읽기", 10, color="#3c8a35")
    s.lines(50, 870, [("읽는 법", "bold"),
                      "• 보라 = 명령(fetch가 opcode로 나눔), 빨강 = dependence token(같은 SRAM을 쓰는 두 모듈 사이 두 방향), 파랑 = DRAM 쪽 AXI와 SRAM 쓰기, 초록 = SRAM 읽기, 회색 점선 = 레지스터.",
                      "• SRAM은 쓰는 모듈 하나(포트 A)와 읽는 모듈 하나(포트 B). uop_mem과 acc_mem은 compute만 쓴다(HLS 내부 BRAM, acc_mem은 512비트 한 줄로 reshape).",
                      "• DRAM 크기(16/16/19/18 = log2 바이트): inp 64 KB, wgt 512 KB, acc 256 KB, out 64 KB, uop 64 KB."], 11.5)
    return s


# ---------------------------------------------------------------------------------------------------------------
@fig(2, "fetch")
def h2():
    s = Svg(1460, 760, "HLS EVTA 2. fetch: 명령을 DRAM에서 읽어 세 명령 큐로 나눈다 (vta.cc fetch())",
            ["run 하나 = 명령 insn_count개. vtalib이 insn_count(0x10)와 명령 버퍼 주소(0x18)를 쓰고 ap_start(0x00 = 1)를 쓰면 한 번 돈다.",
             "INSN_DECODE 루프(PIPELINE): 반복마다 명령 하나(256비트)를 m_axi ins_port로 읽고, opcode와 memory_type만 보고 보낸다."])
    blk(s, 40, 130, 220, 110, "dram", ["insns[0] ... insns[n−1]", "256비트 = 32바이트씩", "(vtalib 명령 큐)"], 11, "DRAM 명령 버퍼")
    blk(s, 40, 300, 220, 120, "reg", ["0x00 ap_ctrl: start / done / idle", "0x10 insn_count", "0x18 insns (물리 주소)", "(s_axi_CONTROL_BUS)"], 10.5, "fetch 레지스터")
    s.rect(330, 120, 640, 470, *K["hls"], 1.8, 8)
    s.text(344, 142, "fetch (HLS IP)", 13, weight="bold", color="#6d4596")
    blk(s, 360, 170, 200, 60, "axi", ["m_axi ins_port", "AXI4 burst 읽기"], 10.5)
    blk(s, 600, 170, 160, 60, "reg", ["pc 카운터", "0 … insn_count−1"], 10.5)
    blk(s, 360, 280, 400, 60, "reg", ["raw_insn (256비트 register)", "opcode = [2:0], memory_type = [8:7] (2비트)"], 10.5)
    blk(s, 360, 390, 400, 160, "hls", ["opcode == STORE → store_queue", "opcode == LOAD 이고 memory_type ∈ {INP, WGT} → load_queue",
                                       "그 밖(LOAD UOP / ACC, GEMM, ALU, FINISH) → gemm_queue", "", "비교기 두 개 + mux 셋 (명령은 그대로 보낸다)"], 10.5, "부분 해독")
    arr(s, 262, 185, 358, 200, "b", 1.6, "명령 읽기", 0, -8)
    arr(s, 460, 232, 460, 278, "a", 1.4)
    arr(s, 680, 232, 680, 278, "a", 1.4, "주소", 18, 0)
    arr(s, 560, 342, 560, 388, "a", 1.4)
    arr(s, 262, 360, 328, 360, "k", 1.2)
    fifo(s, 1050, 200, 170, 34, "load_queue", "256b × 512")
    fifo(s, 1050, 340, 170, 34, "gemm_queue", "256b × 512")
    fifo(s, 1050, 480, 170, 34, "store_queue", "256b × 512")
    arr(s, 762, 420, 1048, 217, "p", 1.8, "INP / WGT LOAD", 0, -6)
    arr(s, 762, 460, 1048, 357, "p", 1.8, "UOP / ACC LOAD, GEMM, ALU, FINISH", 0, -6)
    arr(s, 762, 500, 1048, 497, "p", 1.8, "STORE", 0, -6)
    for x, nm in [(1230, "→ load"), (1230, "→ compute"), (1230, "→ store")]:
        pass
    s.text(1230, 222, "→ load", 12, weight="bold")
    s.text(1230, 362, "→ compute", 12, weight="bold")
    s.text(1230, 502, "→ store", 12, weight="bold")
    s.lines(40, 630, ["• 큐가 꽉 차면(512개) fetch는 그 큐에 쓰는 데서 멈춘다(AXI-Stream back-pressure). 다른 모듈이 하나씩 꺼내 쓴다.",
                      "• fetch는 명령의 뜻(주소, 크기, 의존성)을 보지 않는다. 순서를 지키는 것은 각 큐 안의 순서와 dependence token이다(그림 7).",
                      "• 명령이 끝났다는 신호는 compute가 FINISH를 만나 done 레지스터를 1로 쓰는 것(vtalib이 0x18 읽기로 폴링)."], 12)
    return s


# ---------------------------------------------------------------------------------------------------------------
@fig(3, "load")
def h3():
    s = Svg(1460, 900, "HLS EVTA 3. load: DRAM의 2D 영역을 inp_mem / wgt_mem으로 (pad는 0으로 채움) (vta.cc load())",
            ["AUTORESTART로 계속 돈다: 한 번 호출 = load_queue에서 명령 하나. 필요하면 g2l token을 먼저 꺼내(compute가 버퍼를 다 썼다는 뜻) 쓰기 시작하고,",
             "끝나면 l2g token을 넣는다(compute가 읽어도 된다는 뜻). DRAM 읽기는 m_axi data_port(128비트) burst."])
    fifo(s, 40, 150, 160, 32, "load_queue", "명령 하나 꺼냄")
    fifo(s, 40, 260, 120, 28, "g2l_queue", "pop_next_dep면 꺼냄")
    fifo(s, 40, 360, 120, 28, "l2g_queue", "push_next_dep면 넣음")
    s.rect(250, 120, 760, 520, *K["hls"], 1.8, 8)
    s.text(264, 142, "load (HLS IP)", 13, weight="bold", color="#6d4596")
    blk(s, 280, 160, 330, 70, "reg", ["VTAMemInsn: sram_base, dram_base,", "y_size, x_size, x_stride, y/x_pad_0/1, memory_type"], 10.5, "명령 해독")
    blk(s, 640, 160, 340, 70, "hls", ["x_width = pad0 + x_size + pad1", "y_offset_0/1 = x_width × y_pad (Mul_LUT, 4단)"], 10.5, "pad 계산")
    blk(s, 280, 270, 700, 170, "hls", ["① reset_mem: 위쪽 pad y_offset_0줄을 0으로 (한 clk에 entry 하나)",
                                       "② 행 y = 0 … y_size−1 (PIPELINE):",
                                       "     왼쪽 pad x_pad_0개를 0 → memcpy(DRAM[dram_idx], x_size × 16 B) burst → 오른쪽 pad x_pad_1개를 0",
                                       "     sram_idx += x_width, dram_idx += x_stride",
                                       "③ reset_mem: 아래쪽 pad y_offset_1줄을 0으로",
                                       "wgt는 pad 없음(load_2d): 행마다 memcpy, entry 하나 = 16 bank × 128b = 16×16 int8"], 10.5, "load_pad_2d / load_2d")
    blk(s, 280, 470, 330, 60, "axi", ["m_axi data_port (128비트 = 16 B / beat)", "inp 1 entry = 1 beat, wgt 1 entry = 16 beat"], 10.5)
    blk(s, 640, 470, 340, 60, "reg", ["inp_mem / wgt_mem 포트 A", "주소 sram_idx, 쓰기 enable (byte)"], 10.5)
    arr(s, 202, 166, 278, 190, "p", 1.6)
    arr(s, 162, 274, 278, 230, "r", 1.4, "g2l pop", 0, 20)
    arr(s, 610, 195, 638, 195, "a", 1.2)
    arr(s, 445, 442, 445, 468, "a", 1.2)
    arr(s, 612, 500, 638, 500, "b", 1.6)
    arr(s, 278, 520, 162, 380, "r", 1.4, "l2g push", -10, 10)
    blk(s, 1060, 140, 360, 80, "dram", ["inputs (activations), weights", "dram_base × 16 B (inp) / × 256 B (wgt)"], 10.5, "DRAM")
    poly(s, [(1240, 222), (1240, 455), (560, 455), (560, 468)], "b", 1.8)
    s.text(900, 450, "DRAM burst 읽기 (128b beat)", 10.5, color="#2f5f98")
    bram(s, 1090, 470, 150, 60, "inp_mem", "4096 × 128b", "load", None)
    bram(s, 1260, 470, 150, 60, "wgt_mem ×16", "2048 × 128b", "load", None, banks=4)
    arr(s, 982, 500, 1086, 500, "b", 1.6, "A 쓰기", 0, -6)
    # 2D picture
    y0 = 670
    s.text(40, y0, "예: y_size 3, x_size 4, x_stride 9, x_pad 1/1, y_pad 1/1 → SRAM에 6 × 5 = 30 entry (회색은 0)", 12, weight="bold")
    for r in range(5):
        for c in range(6):
            pad = r in (0, 4) or c in (0, 5)
            s.rect(60 + c * 34, y0 + 16 + r * 30, 32, 28, "#e0e0e0" if pad else "#dce8f5", "#999999", 0.6)
            if not pad:
                s.text(60 + c * 34 + 16, y0 + 35 + r * 30, f"{(r - 1) * 9 + c - 1}", 9.5, "middle")
    s.text(290, y0 + 60, "← DRAM entry 번호 (dram_base 기준). 한 행 x_size개는 burst 하나.", 11.5)
    s.text(290, y0 + 80, "   행 사이는 x_stride만큼 건너뛴다(입력 tile을 잘라 올 때).", 11.5)
    s.text(290, y0 + 110, "• pad는 DRAM에서 읽지 않고 SRAM에 0을 쓴다: conv의 0 padding을 따로 메모리에 두지 않는다.", 11.5)
    s.text(290, y0 + 130, "• 한 번에 명령 하나만 처리 → 명령 사이에 HLS IP 재시작(AUTORESTART) 시간이 든다.", 11.5)
    return s


# ---------------------------------------------------------------------------------------------------------------
@fig(4, "compute_gemm")
def h4():
    s = Svg(1460, 1040, "HLS EVTA 4. compute: uop LOAD, bias LOAD, GEMM(16×16), ALU, FINISH (vta.cc compute(), gemm(), alu())",
            ["AUTORESTART: 한 번 = gemm_queue의 명령 하나. pop_prev_dep면 l2g(load가 inp / wgt를 다 씀), pop_next_dep면 s2g(store가 out을 다 읽음)를 먼저 꺼낸다.",
             "명령을 끝내고 push_prev_dep면 g2l, push_next_dep면 g2s에 token을 넣는다. uop_mem과 acc_mem은 compute 안의 static 배열."])
    fifo(s, 40, 140, 150, 30, "gemm_queue", "명령")
    fifo(s, 40, 220, 110, 26, "l2g / s2g", "pop")
    fifo(s, 40, 290, 110, 26, "g2l / g2s", "push")
    s.rect(240, 120, 1190, 300, *K["hls"], 1.6, 8)
    s.text(254, 142, "compute: 명령 종류별 처리", 13, weight="bold", color="#6d4596")
    blk(s, 260, 160, 260, 110, "hls", ["memory_type UOP: memcpy uops[dram] → uop_mem[sram]", "(m_axi uop_port, 64비트 uop)",
                                       "memory_type ACC: load_pad_2d biases → acc_mem", "(m_axi data_port, entry = 512비트)"], 10, "LOAD (compute 쪽)")
    blk(s, 540, 160, 280, 110, "hls", ["iter_out × iter_in × (uop_end − uop_bgn) 단계", "PIPELINE II = 1 (단계마다 1 clk)", "reset_reg면 acc ← 0"], 10.5, "GEMM")
    blk(s, 840, 160, 280, 110, "hls", ["같은 루프 구조, PIPELINE II = 2", "src, dst 두 번 읽기 + dst 쓰기", "MIN / MAX / ADD / SHR (imm 또는 reg)"], 10.5, "ALU")
    blk(s, 1140, 160, 270, 110, "hls", ["done 레지스터 ← 1", "(vtalib이 0x18을 폴링)", "load / store는 계속 대기"], 10.5, "FINISH")
    bram(s, 300, 310, 200, 60, "uop_mem", "8192 × 64b: dst | src | wgt", None, None)
    bram(s, 560, 310, 260, 60, "acc_mem", "4096 × 512b (int32 × 16 lane)", None, None)
    s.text(850, 345, "HLS 내부 BRAM. acc_mem은 ARRAY_RESHAPE로 한 entry = 512비트 한 줄,", 10.5)
    s.text(850, 362, "DEPENDENCE inter false(II = 1을 위해 단계 사이 의존성 무시)", 10.5)
    arr(s, 192, 155, 258, 180, "p", 1.5)
    # GEMM datapath
    s.section(440, "GEMM 한 단계의 datapath (한 clk에 시작, HLS가 몇 단 pipeline으로 나눔)")
    y = 500
    blk(s, 40, y, 150, 60, "bram", ["uop_mem[upc]", "64비트"], 10.5)
    blk(s, 230, y - 20, 210, 100, "hls", ["dst = uop[11:0] + dst_off", "src = uop[23:12] + src_off", "wgt = uop[34:24] + wgt_off",
                                          "(off: 루프 변수 × factor)"], 10, "주소 계산")
    arr(s, 192, y + 30, 228, y + 30, "a", 1.4)
    bram(s, 500, y - 30, 170, 50, "inp_mem 포트 B", "inp[src]: int8 × 16")
    bram(s, 500, y + 40, 170, 50, "wgt_mem ×16 포트 B", "wgt[wgt]: int8 × 16×16", banks=3)
    bram(s, 500, y + 110, 170, 50, "acc_mem 읽기", "acc[dst]: int32 × 16")
    for yy in (y - 5, y + 65, y + 135):
        arr(s, 442, y + 30, 498, yy, "a", 1.2)
    # MAC array
    mx, my = 730, y - 40
    s.rect(mx, my, 420, 240, "#f3eef8", "#6d4596", 1.4, 6)
    s.text(mx + 10, my + 18, "곱 256개(DSP) + 출력마다 16 → 1 덧셈 트리", 11.5, weight="bold", color="#6d4596")
    for oc in range(4):
        for ic in range(6):
            xx, yy = mx + 20 + ic * 44, my + 34 + oc * 44
            s.rect(xx, yy, 30, 24, "#ffffff", "#6d4596", 0.8, 2)
            s.text(xx + 15, yy + 16, "×", 11, "middle")
        s.rect(mx + 290, my + 34 + oc * 44, 50, 24, "#ece2f4", "#6d4596", 0.8, 3)
        s.text(mx + 315, my + 50 + oc * 44, "Σ16", 10, "middle")
        s.arrow(mx + 282, my + 46 + oc * 44, mx + 288, my + 46 + oc * 44, "p", 1)
        s.rect(mx + 350, my + 34 + oc * 44, 50, 24, "#ece2f4", "#6d4596", 0.8, 3)
        s.text(mx + 375, my + 50 + oc * 44, "+acc", 10, "middle")
    s.text(mx + 20, my + 222, "oc = 0 … 15 (16줄), ic = 0 … 15 (줄마다 16개): w[oc][ic] · x[ic]", 10.5)
    arr(s, 672, y - 5, mx - 2, y + 10, "b", 1.4)
    arr(s, 672, y + 65, mx - 2, y + 60, "b", 1.4)
    arr(s, 672, y + 135, mx - 2, y + 130, "b", 1.4)
    blk(s, 1190, y - 20, 230, 70, "bram", ["acc_mem[dst] 쓰기", "reset_reg ? 0 : acc + Σ"], 10.5)
    blk(s, 1190, y + 70, 230, 70, "bram", ["out_mem[dst] 쓰기 (포트 A)", "acc의 하위 8비트 (int8 × 16)"], 10.5)
    arr(s, mx + 422, y + 40, 1188, y + 15, "a", 1.4)
    arr(s, mx + 422, y + 80, 1188, y + 105, "a", 1.4)
    s.lines(40, 760, ["• 한 단계에 SRAM 넷을 읽고 둘을 쓴다: inp 128b, wgt 2048b(16 bank), acc 512b 읽기 / acc 512b, out 128b 쓰기.",
                      "• 연산 = int8 곱 256개(16 출력 × 16 입력)와 16개짜리 덧셈 트리 16개. 다음 단계가 같은 acc[dst]를 바로 읽어도 된다고 가정(DEPENDENCE false):",
                      "  vtalib의 uop 순서가 같은 dst를 몇 단계 안에 다시 쓰지 않게 만든다.",
                      "• 400 MHz에서는 이 곱 / 덧셈 트리와 inp 한 값이 16곳으로 퍼지는 배선이 timing을 못 맞췄다 → systolic(따로 그림)."], 12)
    # loop nest
    s.section(860, "GEMM 명령 하나 = 루프 3중 (uop은 주소 묶음, 명령의 factor가 주소를 움직인다)")
    s.lines(60, 920, ["for it_out in 0 … iter_out−1:                    # 예: conv의 출력 채널 블록",
                      "  for it_in in 0 … iter_in−1:                     # 예: kernel 위치",
                      "    for upc in uop_bgn … uop_end−1:              # 예: 출력 tile의 행 (uop마다 dst / src / wgt 시작 주소)",
                      "      dst = uop.dst + it_out·dst_factor_out + it_in·dst_factor_in",
                      "      src = uop.src + it_out·src_factor_out + it_in·src_factor_in",
                      "      wgt = uop.wgt + it_out·wgt_factor_out + it_in·wgt_factor_in",
                      "      acc[dst] (+)= inp[src] · wgt[wgt]ᵀ ;  out[dst] = acc[dst] 하위 8비트"], 12, mono=True)
    return s


# ---------------------------------------------------------------------------------------------------------------
@fig(5, "store")
def h5():
    s = Svg(1460, 640, "HLS EVTA 5. store: out_mem의 2D 영역을 DRAM으로 (vta.cc store())",
            ["AUTORESTART: 한 번 = store_queue의 명령 하나. pop_prev_dep면 g2s token(compute가 out을 다 씀)을 먼저 꺼내고, 끝나면 push_prev_dep면 s2g",
             "(store가 out을 다 읽어 compute가 덮어써도 됨). DRAM 쓰기는 m_axi data_port(128비트) burst. pad는 없다."])
    fifo(s, 40, 150, 160, 32, "store_queue", "명령 하나 꺼냄")
    fifo(s, 40, 250, 120, 28, "g2s_queue", "pop_prev_dep면 꺼냄")
    fifo(s, 40, 340, 120, 28, "s2g_queue", "push_prev_dep면 넣음")
    s.rect(250, 120, 740, 340, *K["hls"], 1.8, 8)
    s.text(264, 142, "store (HLS IP)", 13, weight="bold", color="#6d4596")
    blk(s, 280, 160, 330, 60, "reg", ["VTAMemInsn: sram_base, dram_base,", "y_size, x_size, x_stride"], 10.5, "명령 해독")
    blk(s, 280, 250, 680, 90, "hls", ["행 y = 0 … y_size−1 (PIPELINE):", "  memcpy(DRAM[dram_idx × 16 B] ← out_mem[sram_idx], x_size × 16 B) burst",
                                      "  sram_idx += x_size, dram_idx += x_stride"], 10.5, "2D 복사")
    blk(s, 280, 370, 330, 60, "reg", ["out_mem 포트 B 읽기", "128b = int8 × 16 / clk"], 10.5)
    blk(s, 640, 370, 320, 60, "axi", ["m_axi data_port (128비트)", "AXI4 burst 쓰기"], 10.5)
    arr(s, 202, 166, 278, 185, "p", 1.6)
    arr(s, 162, 264, 278, 210, "r", 1.4, "g2s pop", 0, 20)
    arr(s, 445, 342, 445, 368, "a", 1.2)
    arr(s, 800, 342, 800, 368, "a", 1.2)
    arr(s, 278, 410, 162, 360, "r", 1.4, "s2g push", -10, 14)
    bram(s, 1050, 160, 170, 60, "out_mem", "4096 × 128b", "compute", "store")
    blk(s, 1050, 340, 360, 80, "dram", ["outputs (다음 층 입력 / 결과)", "dram_base × 16 B"], 10.5, "DRAM")
    arr(s, 1048, 205, 612, 395, "g", 1.6, "B 읽기", 0, -8)
    arr(s, 962, 400, 1048, 380, "b", 1.8, "burst 쓰기", 0, -8)
    s.lines(40, 500, ["• out_mem에는 compute가 GEMM / ALU 단계마다 acc의 하위 8비트를 써 둔다. store는 그것을 옮기기만 한다(값을 바꾸지 않음).",
                      "• 한 행 x_size entry가 burst 하나. 출력 tile의 행마다 명령 하나를 쓰는 경우(vtalib의 outDummyW 경로)는 명령 수가 많아진다."], 12)
    return s


# ---------------------------------------------------------------------------------------------------------------
@fig(6, "interfaces")
def h6():
    s = Svg(1460, 1000, "HLS EVTA 6. 모듈 사이 interface와 버퍼 (주소, 폭, 깊이, 누가 쓰고 누가 읽나)",
            ["모든 모듈과 큐는 같은 EVTA 클럭(pll_clk). 레지스터 쪽만 PS 클럭 100 MHz(axi_xbar 안에서 클럭 변환). 숫자는 single 16/16/19/18 설정."])

    def table(x, y, cols, rows, size=11.5, rh=24):
        tot = sum(w for _, w in cols)
        s.rect(x, y, tot, rh, "#f3f6fa", "#c8d3e0", 1)
        cx = x
        for t, w in cols:
            s.text(cx + 6, y + 16, t, size, weight="bold")
            cx += w
        for r, row in enumerate(rows):
            yy = y + rh * (r + 1)
            s.rect(x, yy, tot, rh, "#ffffff" if r % 2 == 0 else "#fafafa", "#e2e2e2", 0.8)
            cx = x
            for (t, w), c in zip(cols, row):
                s.text(cx + 6, yy + 16, c, size)
                cx += w
        return y + rh * (len(rows) + 1)

    s.section(100, "A. 레지스터 (PS M_AXI_HPM0 → axi_xbar → s_axi_CONTROL_BUS, 모듈마다 0x1000)")
    y = table(30, 140, [("모듈", 120), ("주소", 140), ("0x00 ap_ctrl", 240), ("레지스터", 560), ("vtalib이 쓰는 값", 340)],
              [["fetch", "0xA0000000", "START (bit 0), run마다", "0x10 insn_count, 0x18 insns 물리 주소", "명령 수, 명령 버퍼 주소"],
               ["load", "0xA0001000", "AUTORESTART (0x81)", "0x10 inputs, 0x18 weights 기준 주소", "0 (명령의 dram_base가 물리 주소)"],
               ["compute", "0xA0002000", "AUTORESTART (0x81)", "0x10/0x18 done(쓰기/읽기), 0x20 uops, 0x28 biases", "0, done 폴링 (0x18)"],
               ["store", "0xA0003000", "AUTORESTART (0x81)", "0x10 outputs 기준 주소", "0"]])
    s.section(y + 20, "B. DRAM 쪽 AXI4 master (모두 axi_smc0 → PS S_AXI_HP0_FPD 128비트)")
    y = table(30, y + 60, [("포트", 260), ("모듈", 120), ("방향", 90), ("단위", 330), ("무엇", 600)],
              [["m_axi_ins_port", "fetch", "읽기", "명령 256비트(32 B)", "명령 버퍼"],
               ["m_axi_data_port", "load", "읽기", "128비트 beat, inp entry 16 B / wgt entry 256 B", "활성화, weight"],
               ["m_axi_uop_port", "compute", "읽기", "uop 64비트", "uop (VTAUopPush가 만든 것)"],
               ["m_axi_data_port", "compute", "읽기", "acc entry 64 B (int32 × 16)", "bias (LOAD ACC)"],
               ["m_axi_data_port", "store", "쓰기", "out entry 16 B", "출력 활성화"]])
    s.section(y + 20, "C. AXI-Stream 큐 (fifo_generator, Common Clock 분산 RAM)")
    y = table(30, y + 60, [("큐", 160), ("폭 × 깊이", 160), ("쓰는 모듈 → 읽는 모듈", 260), ("token / 명령의 뜻", 820)],
              [["load_queue", "32 B × 512", "fetch → load", "INP / WGT LOAD 명령"],
               ["gemm_queue", "32 B × 512", "fetch → compute", "UOP / ACC LOAD, GEMM, ALU, FINISH"],
               ["store_queue", "32 B × 512", "fetch → store", "STORE 명령"],
               ["l2g_queue", "1 B × 1024", "load → compute", "load가 inp / wgt 버퍼에 다 썼다 (compute가 읽어도 된다)"],
               ["g2l_queue", "1 B × 1024", "compute → load", "compute가 그 버퍼를 다 읽었다 (load가 덮어써도 된다)"],
               ["g2s_queue", "1 B × 1024", "compute → store", "compute가 out을 다 썼다 (store가 읽어도 된다)"],
               ["s2g_queue", "1 B × 1024", "store → compute", "store가 out을 다 읽었다 (compute가 덮어써도 된다)"]])
    s.section(y + 20, "D. SRAM")
    table(30, y + 60, [("SRAM", 170), ("구현", 260), ("크기", 300), ("포트 A", 220), ("포트 B", 220), ("한 entry", 230)],
          [["inp_mem", "blk_mem_gen TDP", "4096 × 128b = 64 KB", "load 쓰기", "compute 읽기", "int8 × 16 (입력 16채널)"],
           ["wgt_mem_0..15", "blk_mem_gen TDP × 16", "16 × 2048 × 128b = 512 KB", "load 쓰기", "compute 읽기", "16 bank 합쳐 int8 16×16"],
           ["out_mem", "blk_mem_gen TDP", "4096 × 128b = 64 KB", "compute 쓰기", "store 읽기", "int8 × 16"],
           ["acc_mem", "compute 안 (HLS)", "4096 × 512b = 256 KB", "compute", "compute", "int32 × 16"],
           ["uop_mem", "compute 안 (HLS)", "8192 × 64b = 64 KB", "compute", "compute", "uop {wgt, src, dst}"]])
    return s


# ---------------------------------------------------------------------------------------------------------------
@fig(7, "dependence")
def h7():
    s = Svg(1460, 1080, "HLS EVTA 7. dependence token: load / compute / store가 같은 버퍼를 겹치지 않게 쓰는 방법",
            ["세 모듈은 따로 돈다(각자 자기 명령 큐). 같은 SRAM을 쓰는 두 모듈 사이에 token 큐가 두 방향으로 있고, 명령의 비트 4개가 token을 꺼내고(pop) 넣는다(push).",
             "prev / next는 파이프라인 순서 load → compute → store에서 앞 / 뒤 모듈이다."])
    # modules in a row
    y = 140
    blk(s, 80, y, 260, 120, "hls", ["inp_mem, wgt_mem에 씀"], 12, "load")
    blk(s, 600, y, 260, 120, "hls", ["inp / wgt 읽음, out에 씀"], 12, "compute")
    blk(s, 1120, y, 260, 120, "hls", ["out_mem 읽음"], 12, "store")
    fifo(s, 400, y + 10, 140, 30, "l2g_queue", "")
    fifo(s, 400, y + 80, 140, 30, "g2l_queue", "")
    fifo(s, 920, y + 10, 140, 30, "g2s_queue", "")
    fifo(s, 920, y + 80, 140, 30, "s2g_queue", "")
    arr(s, 342, y + 25, 398, y + 25, "r", 1.8, "push_next", 0, -12)
    arr(s, 542, y + 25, 598, y + 25, "r", 1.8, "pop_prev", 0, -12)
    arr(s, 598, y + 95, 542, y + 95, "r", 1.8, "push_prev", 0, 22)
    arr(s, 398, y + 95, 342, y + 95, "r", 1.8, "pop_next", 0, 22)
    arr(s, 862, y + 25, 918, y + 25, "r", 1.8, "push_next", 0, -12)
    arr(s, 1062, y + 25, 1118, y + 25, "r", 1.8, "pop_prev", 0, -12)
    arr(s, 1118, y + 95, 1062, y + 95, "r", 1.8, "push_prev", 0, 22)
    arr(s, 918, y + 95, 862, y + 95, "r", 1.8, "pop_next", 0, 22)
    s.section(310, "A. 명령의 비트 (모든 명령 공통, opcode 바로 뒤 4비트)")
    rows = [("pop_prev_dep", "앞 모듈의 token을 기다렸다 꺼낸 뒤 시작", "load: 없음 / compute: l2g / store: g2s"),
            ("pop_next_dep", "뒤 모듈의 token을 기다렸다 꺼낸 뒤 시작", "load: g2l / compute: s2g / store: 없음"),
            ("push_prev_dep", "끝나면 앞 모듈에 token", "load: 없음 / compute: g2l / store: s2g"),
            ("push_next_dep", "끝나면 뒤 모듈에 token", "load: l2g / compute: g2s / store: 없음")]
    for i, (a, b, c) in enumerate(rows):
        yy = 360 + i * 28
        s.text(60, yy, a, 12.5, weight="bold", mono=True)
        s.text(260, yy, b, 12.5)
        s.text(720, yy, c, 12.5, color="#b03a2e")
    s.lines(60, 490, ["vtalib: VTADepPush(from, to) = 직전 명령에 push 비트(이미 있으면 빈 명령을 하나 더), VTADepPop(from, to) = 다음 명령에 pop 비트."], 12)
    # timeline
    s.section(520, "B. 예: conv tile 둘을 virtual thread 2개로 (inp / wgt / acc를 반씩 나눠 번갈아) — load가 다음 tile을 읽는 동안 compute가 앞 tile을 계산")
    lanes = [("load", 80), ("compute", 140), ("store", 200)]
    base = 580
    for nm, dy in lanes:
        s.text(60, base + dy - 50 + 18, nm, 12.5, "end" if False else "start", "bold")
    ev = [("load", 0, 120, "LOAD inp/wgt A", "evta"), ("load", 130, 120, "LOAD inp/wgt B", "evta"), ("compute", 130, 150, "GEMM A", "hls"),
          ("load", 290, 120, "LOAD A' (A 다 읽은 뒤)", "evta"), ("compute", 290, 150, "GEMM B", "hls"), ("store", 290, 110, "STORE A", "mem"),
          ("compute", 450, 150, "GEMM A'", "hls"), ("store", 450, 110, "STORE B", "mem"), ("load", 450, 120, "LOAD B'", "evta")]
    for m, t, w, lab, k in ev:
        dy = dict(lanes)[m]
        s.box(160 + t * 2, base + dy - 50, w * 2 - 10, 32, k, [lab], 10.5)
    # token arrows
    def tok(m1, t1, m2, t2, lab):
        y1 = base + dict(lanes)[m1] - 50 + (32 if dict(lanes)[m2] > dict(lanes)[m1] else 0)
        y2 = base + dict(lanes)[m2] - 50 + (0 if dict(lanes)[m2] > dict(lanes)[m1] else 32)
        s.arrow(160 + t1 * 2, y1, 160 + t2 * 2, y2, "r", 1.4)
        s.text(160 + (t1 + t2), (y1 + y2) / 2 - 2, lab, 9.5, "middle", color="#b03a2e")
    tok("load", 120, "compute", 132, "l2g")
    tok("load", 250, "compute", 292, "l2g")
    tok("compute", 280, "store", 292, "g2s")
    tok("compute", 280, "load", 292, "g2l")
    tok("compute", 440, "store", 452, "g2s")
    s.text(160, base + 190, "시간 →", 11, color="#777777")
    s.lines(60, base + 220, ["• load와 compute가 같은 반쪽(A)을 동시에 쓰지 않는다: load의 다음 A 쓰기는 g2l token(compute가 A를 다 읽음)을 꺼낸 뒤에 시작.",
                             "• store는 g2s(compute가 out을 다 씀)를 꺼낸 뒤 읽고, 다 읽으면 s2g를 넣어 compute가 그 out 자리를 다시 쓰게 한다.",
                             "• token은 개수만 센다(값 없음). 큐 깊이 1024라 앞서 쌓아 둘 수 있다. 짝이 안 맞으면(push 없이 pop) 모듈이 영원히 기다린다 = deadlock(멈춘 run).",
                             "• run 끝: vtalib Synchronize가 store → compute, load → compute token을 넣고 compute의 FINISH가 그 둘을 꺼낸 뒤 done = 1."], 12)
    return s


def main():
    nums = [int(a) for a in sys.argv[1:]] or sorted(FIGS)
    for n in nums:
        name, f = FIGS[n]
        s = f()
        p = os.path.join(OUT, f"evta_hls_{n:02d}_{name}.svg")
        s.save(p)
        print("wrote", os.path.relpath(p))


if __name__ == "__main__":
    main()
