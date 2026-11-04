#!/usr/bin/env python3
# Structure of the systolic EVTA (vta-hw-zcu102-systolic: src/vta.cc with VTA_SYSTOLIC, rtl/gemm_sa_bd.v / gemm_sa.v / sa_ctrl.v /
# sa_array.v / sa_pe.v / sa_rmw.v, scripts/vivado.tcl with SYSTOLIC=1, FREQ=400) -> docs/figs/evta_sa_NN_*.svg, in the order of
# the HLS figures (make_evta_hls_figs.py), plus the array, the PE, sa_ctrl and sa_rmw.
# usage: python3 docs/figs/src/make_evta_sa_figs.py [NN ...]
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from svglib import Svg, C  # noqa: E402
import make_evta_hls_figs as H  # noqa: E402
from make_evta_hls_figs import K, blk, fifo, bram, arr, poly  # noqa: E402
from make_evta_struct_figs import rmw_pipe, reg  # noqa: E402

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
FIGS = {}
NEW = C["new"]


def fig(num, name):
    def deco(f):
        FIGS[num] = (name, f)
        return f
    return deco


def retitle(s, title, extra_h, notes):
    """take an HLS figure: new title, a green box at the bottom with what differs in the systolic build"""
    s.el[1] = ""
    s.text(24, 38, title, 22, weight="bold")
    y = s.h + 6
    s.h += extra_h
    s.el[0] = f'<rect x="0" y="0" width="{s.w}" height="{s.h}" fill="#ffffff" stroke="none"/>'
    s.rect(30, y, s.w - 60, extra_h - 20, NEW[0], NEW[1], 1.4, 6)
    s.lines(46, y + 24, notes, 12, lh=19)
    return s


# ---------------------------------------------------------------------------------------------------------------
@fig(1, "overall")
def s1():
    s = Svg(1460, 1010, "systolic EVTA 1. 전체 구조 (vta-hw-zcu102-systolic, SYSTOLIC=1, FREQ=400, config 16/16/19/18)",
            ["HLS EVTA(그림 HLS 1)와 같은 HLS IP 4개에 RTL gemm_sa(gemm_sa_bd)가 더해졌다. GEMM 계산은 gemm_sa가 하고, HLS compute는 GEMM 단계마다 uop 하나를 보낸다.",
             "초록 = HLS EVTA에서 바뀌거나 새로 생긴 것. acc_mem은 compute 밖 BRAM이 되어 compute와 gemm_sa가 mux로 나눠 쓴다. HP 쪽은 333 MHz 클럭으로 분리."])
    s.rect(30, 104, 1400, 128, *K["ps"], 1.6, 8)
    s.text(44, 124, "PS (Zynq UltraScale+ XCZU9EG): Cortex-A53 ×4, DDR4", 13, weight="bold")
    blk(s, 50, 136, 300, 84, "ps", ["vtalib-zcu102-systolic: 같은 API,", "GEMM uop 순서만 다름(weight가 오래 머물게)"], 10.5, "CPU")
    blk(s, 370, 136, 380, 84, "dram", ["명령 256b × n, uop 64b, inp, wgt, bias int32, out", "(HLS EVTA와 같음)"], 10.5, "DRAM (CMA)")
    blk(s, 770, 136, 250, 84, NEW, ["M_AXI_HPM0_FPD 128b, 100 MHz", "→ reg_dwc 128 → 32b (새)"], 10.5, "PS → PL 레지스터")
    blk(s, 1040, 136, 370, 84, NEW, ["S_AXI_HP0_FPD 128b", "hp_pll 333 MHz (PS8 HP 한계 3.0 ns)"], 10.5, "PL → DRAM")
    s.rect(30, 246, 1400, 750, "#f8fafd", "#2f5f98", 1.6, 8)
    s.text(44, 266, "PL: EVTA 클럭 400 MHz (pll_clk) — HP 쪽만 333 MHz (hp_pll, smartconnect 안에서 클럭 변환)", 12.5, weight="bold", color="#2f5f98")
    blk(s, 770, 280, 250, 44, "axi", ["axi_xbar (1 → 4)", "S00 100 MHz → M 400 MHz"], 10)
    blk(s, 1100, 280, 300, 44, NEW, ["axi_smc0 (5 → 1), 클럭 2개", "S 쪽 400 MHz, M 쪽 333 MHz"], 10)
    arr(s, 895, 222, 895, 278, "k", 1.4, "AXI-Lite", 30, 0)
    arr(s, 1250, 278, 1250, 222, "b", 1.8, "AXI4 128b", 40, 0)
    F = (50, 360, 200, 100)
    L = (290, 500, 230, 130)
    Cm = (600, 500, 230, 130)
    G = (880, 500, 230, 150)
    St = (1170, 500, 230, 130)
    blk(s, *F, "hls", ["(HLS EVTA와 같음)", "0xA0000000, START"], 10.5, "fetch", sw=2)
    blk(s, *L, "hls", ["DRAM → inp / wgt (pad 0)", "reset_mem pipeline (새)", "0xA0001000"], 10.5, "load", sw=2)
    blk(s, *Cm, NEW, ["GEMM: 단계마다 uop 전송", "ALU imm II = 1, LOAD, FINISH", "uop_mem 8192×64b (안)"], 10.5, "compute (HLS)", sw=2)
    blk(s, *G, NEW, ["sa_ctrl: uop FIFO, weight fill", "sa_array: 16×16 DSP48E2", "sa_rmw: acc RMW, out 쓰기"], 10.5, "gemm_sa (RTL, 새)", sw=2.4)
    blk(s, *St, "hls", ["out_mem → DRAM", "(HLS EVTA와 같음)", "0xA0003000"], 10.5, "store", sw=2)
    # control
    yb = 340
    poly(s, [(895, 326), (895, yb)], "k", 1.0, "3 2", arrow=False)
    s.line(150, yb, 1380, yb, "#888888", 1.0, "3 2")
    for x, top in [(150, F[1]), (500, L[1]), (810, Cm[1]), (1380, St[1])]:
        poly(s, [(x, yb), (x, top - 2)], "k", 1.0, "3 2")
    s.text(470, yb - 6, "s_axi_CONTROL_BUS ×4 (gemm_sa에는 레지스터 없음)", 10, color="#777777")
    # m_axi
    ym = 478
    s.line(110, ym, 1415, ym, "#2f5f98", 1.4)
    poly(s, [(1415, ym), (1415, 302), (1402, 302)], "b", 1.4)
    for x, top, lab in [(110, F[1] + F[3], "ins_port"), (320, L[1], "data_port"), (630, Cm[1], "uop_port"), (700, Cm[1], "data(bias)"), (1200, St[1], "data_port")]:
        poly(s, [(x, top - 2 if top > ym else top + 2), (x, ym)], "b", 1.3)
        s.text(x + 4, ym - 4, lab, 9.5, color="#2f5f98")
    # instruction queues
    qy = 384
    Q = {"load_queue": (330, L), "gemm_queue": (620, Cm), "store_queue": (1220, St)}
    s.line(252, qy + 15, 1220, qy + 15, "#6d4596", 1.6)
    for nm, (qx, M) in Q.items():
        fifo(s, qx, qy, 100, 30, nm, "")
        poly(s, [(qx + 100, qy + 15), (qx + 116, qy + 15), (qx + 116, M[1] - 2)], "p", 1.6)
    # compute <-> gemm_sa streams
    arr(s, Cm[0] + Cm[2] + 2, 525, G[0] - 2, 525, "g", 2.2, "uop stream (65b / 단계)", 0, -6, 10)
    arr(s, G[0] - 2, 560, Cm[0] + Cm[2] + 2, 560, "g", 1.6, "done (명령마다 1)", 0, 14, 10)
    # dep queues
    def depq(x, y, nm):
        fifo(s, x, y, 56, 20, "", "")
        s.text(x + 28, y - 4, nm, 10, "middle", "bold", "#b03a2e")
    depq(530, 515, "l2g")
    arr(s, 522, 525, 528, 525, "r", 1.2)
    arr(s, 588, 525, 598, 525, "r", 1.2)
    depq(530, 590, "g2l")
    arr(s, 598, 600, 588, 600, "r", 1.2)
    arr(s, 528, 600, 522, 600, "r", 1.2)
    depq(1112, 520, "g2s")
    arr(s, 1104, 530, 1110, 530, "r", 1.2)
    arr(s, 1170, 530, 1168, 530, "r", 1.2)
    depq(1112, 600, "s2g")
    poly(s, [(1168, 610), (1150, 610)], "r", 1.2)
    poly(s, [(1112, 610), (1100, 610), (1100, 680), (720, 680), (720, Cm[1] + Cm[3] + 2)], "r", 1.2)
    poly(s, [(G[0] + 60, G[1] + G[3] + 2), (G[0] + 60, 670), (560, 670), (560, 612)], "r", 1.4)
    s.text(G[0] + 66, 664, "g2l / g2s: GEMM 명령의 token은 gemm_sa가 마지막 단계를 쓴 뒤 넣는다(bd에서 compute 것과 합침)", 10, color="#b03a2e")
    # SRAMs
    by = 760
    bram(s, 280, by, 160, 62, "inp_mem", "4096 × 128b", "load", "gemm_sa")
    bram(s, 500, by, 180, 62, "wgt_mem ×16", "2048 × 128b", "load", "gemm_sa", banks=4)
    bram(s, 760, by, 200, 62, "acc_mem (BD BRAM, 새)", "4096 × 512b, A 읽기 B 쓰기", "mux", "mux")
    bram(s, 1060, by, 160, 62, "out_mem", "4096 × 128b", "mux", "store")
    arr(s, 340, L[1] + L[3] + 2, 340, by - 2, "b", 1.5)
    arr(s, 480, L[1] + L[3] + 2, 540, by - 18, "b", 1.5)
    poly(s, [(G[0] + 20, G[1] + G[3] + 2), (G[0] + 20, 720), (420, 720), (420, by - 2)], "g", 1.4)
    poly(s, [(G[0] + 40, G[1] + G[3] + 2), (G[0] + 40, 735), (650, 735), (650, by - 12)], "g", 1.4)
    s.text(430, 716, "B 읽기 (출력 register, 2 clk)", 10, color="#3c8a35")
    s.rect(800, 700, 140, 34, NEW[0], NEW[1], 1.2, 4)
    s.text(870, 714, "port mux", 10.5, "middle", "bold")
    s.text(870, 728, "sel = busy (1 clk 늦게)", 9.5, "middle")
    poly(s, [(G[0] + 120, G[1] + G[3] + 2), (G[0] + 120, 717), (942, 717)], "g", 1.4)
    poly(s, [(Cm[0] + 180, Cm[1] + Cm[3] + 2), (Cm[0] + 180, 717), (798, 717)], "p", 1.4)
    arr(s, 860, 736, 860, by - 2, "a", 1.4)
    poly(s, [(940, 734), (940, 745), (1100, 745), (1100, by - 2)], "a", 1.4)
    poly(s, [(1270, by - 2), (1270, St[1] + St[3] + 2)], "g", 1.5)
    s.lines(50, 870, [("HLS EVTA와 다른 점", "bold"),
                      "• GEMM: compute의 곱 256개 + 덧셈 트리 → gemm_sa의 weight-stationary 어레이(PE마다 DSP48E2 하나, PE 사이 register). 400 MHz를 맞춤.",
                      "• acc_mem / out_mem 포트 A: gemm_sa가 일하는 동안(busy)은 gemm_sa, 아니면 HLS compute(ALU, acc LOAD). compute는 GEMM 뒤 다른 명령 전에 done을 다 받는다.",
                      "• inp / wgt 포트 B를 gemm_sa가 읽는다(16 bank는 한 clk씩 어긋나게). HLS compute는 inp / wgt를 읽지 않는다."], 11.5)
    return s


@fig(2, "fetch")
def s2():
    return retitle(H.h2(), "systolic EVTA 2. fetch: HLS EVTA와 같다", 110,
                   [("바뀐 것", "bold"), "• fetch.vta.cc 코드는 그대로. 클럭만 400 MHz(레지스터 쪽 AXI-Lite는 axi_xbar에서 100 MHz → 400 MHz, PS 쪽에 reg_dwc 128 → 32b).",
                    "• gemm_queue에 들어간 GEMM 명령은 compute가 꺼내 gemm_sa에 단계별로 보낸다(그림 4, 5)."])


@fig(3, "load")
def s3():
    return retitle(H.h3(), "systolic EVTA 3. load: 같은 구조, reset_mem(pad 0 쓰기)을 pipeline으로", 130,
                   [("바뀐 것", "bold"), "• reset_mem: 루프 제어(i++ 비교)가 쓰기 enable(BRAM 58개)까지 한 clk이던 것을, 주소 = base + i를 register(alu_reg)에 담아 II = 1 pipeline으로",
                    "  (multi4 400 MHz에서 WNS −0.343 ns였던 경로; acc LOAD의 pad에도 같은 함수를 쓴다).",
                    "• inp_mem / wgt_mem 포트 B(읽는 쪽 = gemm_sa)에 출력 register(MEM_OREG): 읽기 지연 2 clk. load가 쓰는 포트 A는 그대로."])


@fig(4, "compute")
def s4():
    s = Svg(1460, 1080, "systolic EVTA 4. compute (HLS): GEMM은 uop을 보내기만, ALU / LOAD / FINISH는 그대로 (vta.cc, VTA_SYSTOLIC)",
            ["gemm_queue의 명령 하나씩. GEMM이면 HLS가 GEMM의 3중 루프를 돌며 단계마다 uop(최종 주소 + 비트)을 gemm_sa로 보내고, 기다리지 않고 다음 명령으로 간다.",
             "GEMM이 아닌 명령(ALU, acc LOAD, token이 있는 uop LOAD, FINISH) 앞에서는 보낸 GEMM의 done을 모두 받는다(acc / out 포트를 넘겨받기 위해)."])
    fifo(s, 40, 140, 150, 30, "gemm_queue", "명령")
    s.rect(240, 110, 1190, 340, *K["hls"], 1.6, 8)
    s.text(254, 132, "compute (HLS IP)", 13, weight="bold", color="#6d4596")
    blk(s, 260, 150, 300, 120, NEW, ["for it_out, it_in, upc (PIPELINE II = 1):", "  주소 = uop + offset (HLS EVTA와 같음)",
                                      "  sa_uop = {rq, bias, g2s, g2l, first, last,", "           reset, wgt, src, dst}", "  gemm_uop.write(sa_uop)"], 10, "GEMM → gemm_sa_issue")
    blk(s, 580, 150, 260, 120, NEW, ["sa_out = 보내고 done 안 받은 GEMM 수", "다음 명령이 GEMM이면 그대로 보냄",
                                      "아니면 SA_DRAIN: sa_out이 0이 될 때까지", "gemm_done 읽기"], 10, "보낸 GEMM 세기")
    blk(s, 860, 150, 270, 120, NEW, ["imm 연산: II = 1 루프 (A 읽기, B 쓰기)", "reg 연산: II = 2 루프", "쓰기 앞 register (alu_reg)",
                                      "out ← requant(결과) (rq_en)"], 10, "ALU")
    blk(s, 1150, 150, 260, 120, "hls", ["LOAD UOP → uop_mem (안)", "LOAD ACC → acc_mem (BD BRAM, mux)", "FINISH → done = 1"], 10, "LOAD / FINISH")
    bram(s, 300, 300, 200, 56, "uop_mem", "8192 × 64b (compute 안)")
    blk(s, 560, 300, 400, 120, NEW, ["GEMM 명령의 push_prev / push_next를 마지막 단계(last)의 비트로 실어 보냄",
                                      "→ gemm_sa가 그 단계를 acc / out에 쓴 뒤 g2l / g2s에 넣는다", "(compute가 done을 읽을 때까지 load / store가 기다리지 않게)",
                                      "단계가 없는 GEMM은 compute가 바로 push"], 10, "token")
    blk(s, 990, 300, 420, 120, "axi", ["gemm_uop: AXI-Stream, compute → gemm_sa (65b, 8b 단위로 72b)", "gemm_done: AXI-Stream, gemm_sa → compute (명령마다 1)",
                                       "acc_mem 포트 A / B, out_mem 포트 A: gemm_sa_bd의 mux를 거쳐"], 10, "compute의 새 포트")
    arr(s, 192, 155, 258, 180, "p", 1.5)
    # uop bit layout
    s.section(470, "gemm_sa로 가는 uop 한 단계 (VTA_SA_UOP_B = 12 + 12 + 11 = 35, 폭 65비트)")
    fields = [("dst", 12, "#dce8f5"), ("src", 12, "#dce8f5"), ("wgt", 11, "#dce8f5"), ("reset", 1, "#fde5cc"), ("last", 1, "#fde5cc"),
              ("first", 1, "#fde5cc"), ("g2l", 1, "#fde2e2"), ("g2s", 1, "#fde2e2"), ("bias", 1, "#dcefd6"), ("rq (en, round, shift, lo, hi)", 24, "#dcefd6")]
    x = 60
    bit = 0
    for nm, w, col in fields:
        ww = max(w * 16, 40)
        s.rect(x, 520, ww, 40, col, "#666666", 1, 0)
        s.text(x + ww / 2, 545, nm, 10.5, "middle", "bold")
        s.text(x + 2, 576, str(bit), 9.5, color="#777777")
        bit += w
        x += ww
    s.text(x + 4, 576, str(bit), 9.5, color="#777777")
    s.lines(60, 610, ["• dst / src / wgt는 루프 offset을 더한 최종 주소. reset: reset_reg. last: 명령의 마지막 단계(done과 token). first: 명령의 첫 단계(weight를 다시 채움).",
                      "• bias(11-04): reset 단계가 acc[src]를 쓴다(bias reset). rq(11-04): out에 쓸 requantize (evta_isa_ext.md)."], 11.5)
    s.section(680, "compute와 gemm_sa가 나눠 쓰는 것")
    s.lines(60, 730, ["• acc_mem: gemm_sa는 포트 A로 읽고(2 clk) 포트 B로 쓴다. compute(ALU, acc LOAD)도 같은 두 포트를 쓴다 → gemm_sa_bd가 sel(busy를 1 clk 늦춘 것)로 고른다.",
                      "  gemm_sa의 첫 acc 접근은 첫 uop 뒤 35 clk 이상이고, compute는 마지막 쓰기 1 clk 뒤의 done을 받은 다음에만 acc / out을 건드린다 → 1 clk 늦은 sel도 안전.",
                      "• out_mem 포트 A: GEMM 단계의 out 쓰기는 gemm_sa(sa_rmw), ALU의 out 쓰기는 compute. 같은 mux.",
                      "• uop_mem은 compute만 읽는다(gemm_sa는 uop을 stream으로 받는다)."], 11.5)
    return s


@fig(5, "gemm_sa")
def s5():
    s = Svg(1460, 900, "systolic EVTA 5. gemm_sa (RTL): sa_ctrl → sa_array → sa_rmw, 그리고 SRAM 포트 (gemm_sa.v, gemm_sa_bd.v)",
            ["uop 한 단계 = acc[dst] (+)= inp[src] · wgt[wgt]ᵀ. weight 블록(16×16)은 같은 wgt 주소가 이어지는 동안(run) 어레이에 머문다.",
             "단계 사이 간격 1 clk, 결과 행은 issue 뒤 L_RD + 33 clk에 sa_rmw에 도착한다. 이 시간 동안 다음 단계들이 줄줄이 들어간다(파이프라인)."])
    s.rect(220, 110, 1000, 520, NEW[0], NEW[1], 1.8, 8)
    s.text(234, 132, "gemm_sa (gemm_sa_bd 안)", 13, weight="bold", color="#245e1f")
    fifo(s, 40, 170, 140, 30, "uop stream", "compute에서 (65b)")
    blk(s, 250, 150, 280, 150, "rtl", ["uop FIFO 32 entry", "run queue 8 (새 weight 주소)", "fill engine: bank j 주소를 g+j clk에",
                                       "issue: inp 주소, flag(latch)", "tag pipe 35단 → sa_rmw"], 10.5, "sa_ctrl")
    blk(s, 600, 150, 300, 300, "rtl", ["PE 16 × 16 = DSP48E2 256개", "행 i = 입력 채널, 열 j = 출력 채널", "x: 왼쪽에서 오른쪽, 한 clk 한 PE",
                                       "부분합: 위에서 아래(PCIN → PCOUT)", "weight: 열마다 PISO → B1 채움 사슬", "B2 = 쓰는 weight (flag 때 B1 → B2)",
                                       "skew / deskew register"], 10.5, "sa_array")
    blk(s, 960, 150, 240, 300, "rtl", ["S0 tag + 행, acc 읽기 주소", "S1 forwarding 비교", "S2 old + 행 (int32 × 16)", "S3 acc 쓰기 (포트 B)",
                                       "S4~S6 requantize → out 쓰기", "done / token / busy (S6)"], 10.5, "sa_rmw")
    arr(s, 182, 185, 248, 200, "g", 1.8)
    arr(s, 532, 200, 598, 200, "a", 1.6, "x 주소 → inp", 0, -6, 10)
    arr(s, 532, 260, 598, 260, "a", 1.6, "go, flag", 0, -6, 10)
    arr(s, 902, 300, 958, 300, "y", 2, "행 (20b × 16)", 0, -6, 10)
    poly(s, [(390, 302), (390, 480), (1080, 480), (1080, 452)], "k", 1.4, "4 3")
    s.text(560, 474, "tag: dst, reset, last, tok, bias, src, rq (35단 늦춰 행과 같이 도착)", 10.5, color="#666666")
    # srams
    bram(s, 260, 670, 170, 56, "inp_mem 포트 B", "128b, 2 clk")
    bram(s, 480, 670, 200, 56, "wgt_mem ×16 포트 B", "bank j 주소 g+j clk", banks=4)
    bram(s, 900, 670, 160, 56, "acc_mem A", "읽기 512b")
    bram(s, 1080, 670, 160, 56, "acc_mem B", "쓰기 512b")
    bram(s, 1260, 670, 160, 56, "out_mem A", "쓰기 128b")
    arr(s, 345, 668, 420, 302, "b", 1.4, "inp", -10, 0)
    arr(s, 580, 660, 700, 452, "b", 1.4, "wgt (2048b)", 30, 0)
    arr(s, 980, 668, 1000, 452, "b", 1.4, "읽기", -14, 0)
    arr(s, 1100, 452, 1150, 668, "g", 1.4, "쓰기", 22, 0)
    arr(s, 1180, 452, 1330, 668, "g", 1.4, "out", 22, 0)
    blk(s, 1240, 160, 200, 150, "rtl", ["done: 명령의 last 단계가", "쓰이면 1 (counter)", "g2l / g2s: 그 단계의 tok", "(counter, bd에서", "compute 것과 합침)"], 10, "gemm_sa 출력")
    arr(s, 1202, 300, 1238, 260, "r", 1.4)
    s.lines(40, 770, ["• 같은 wgt 주소가 이어지면(run) weight를 다시 채우지 않는다. 새 run은 weight 채우기(16 clk) 뒤에만 시작 가능: fill engine이 다음 run의 weight를 지금 run 동안 B1에 미리 채운다.",
                      "• vtalib-zcu102-systolic의 uop 순서는 이 run이 길도록(같은 weight로 출력 위치를 쭉 도는 순서) 바꿨다(evta_systolic.md §4).",
                      "• 명령의 첫 단계(first)는 같은 wgt 주소라도 새 run: 그 사이 LOAD가 같은 주소에 새 weight를 썼을 수 있다(보드에서 찾은 버그)."], 11.5)
    return s


@fig(6, "array_pe")
def s6():
    s = Svg(1460, 1020, "systolic EVTA 6. sa_array와 PE 하나 (sa_array.v, sa_pe.v): weight-stationary 16 × 16, DSP48E2",
            ["PE(i, j) = DSP48E2 하나: P = x[i] · w[j][i] + (위 PE의 P). x[i]는 a + i + j clk에 PE(i, j)에 도착하고, 열 j의 합은 a + 18 + j clk에 맨 아래에서 나온다.",
             "열마다 15 − j clk 늦춰(deskew) 16개가 a + 33 clk에 한 행으로 나온다."])
    x0, y0 = 220, 140
    n = 5
    cw, ch = 150, 110
    # skew registers
    for i in range(n):
        yy = y0 + i * ch
        lab = f"x[{i}]" if i < 4 else "x[15]"
        s.text(40, yy + 50, lab, 12, weight="bold")
        for k in range(min(i, 3)):
            reg(s, 90 + k * 22, yy + 32, 26)
        if i >= 1:
            s.text(170, yy + 26, f"{i if i < 4 else 15} clk skew", 9.5, "middle", color="#2a6a24")
        s.arrow(160, yy + 45, x0 - 2, yy + 45, "b", 1.2)
    for i in range(n):
        for j in range(n):
            px, py = x0 + j * cw, y0 + i * ch
            s.rect(px, py, 110, 80, "#fff3c4", "#9a7b00", 1.2, 4)
            s.text(px + 55, py + 16, f"PE({i if i < 4 else 15},{j if j < 4 else 15})", 10, "middle", "bold")
            s.rect(px + 8, py + 24, 40, 18, "#ffffff", "#9a7b00", 0.8, 2)
            s.text(px + 28, py + 37, "B1", 9.5, "middle")
            s.rect(px + 8, py + 46, 40, 18, "#dcefd6", "#3c8a35", 0.8, 2)
            s.text(px + 28, py + 59, "B2 w", 9.5, "middle")
            s.rect(px + 58, py + 30, 44, 30, "#ffffff", "#9a7b00", 0.8, 2)
            s.text(px + 80, py + 49, "× +", 11, "middle")
            if j < n - 1:
                reg(s, px + 114, py + 32, 22)
                s.arrow(px + 126, py + 45, px + cw - 2, py + 45, "b", 1)
            if i < n - 1:
                s.arrow(px + 80, py + 82, px + 80, py + ch - 2, "y", 1.4)
                s.arrow(px + 28, py + 82, px + 28, py + ch - 2, "k", 0.8, dash="2 2")
    s.text(x0 + 4 * cw + 115, y0 + 45, "x →", 11, color="#2f5f98")
    s.text(x0 + 3.5 * cw, y0 + 4.5 * ch + 70, "⋮ 실제는 16 × 16", 11, color="#9a7b00")
    # PISO
    for j in range(n):
        px = x0 + j * cw
        s.rect(px + 4, y0 - 34, 60, 20, "#e6eef7", "#4a6f9a", 0.8, 2)
        s.text(px + 34, y0 - 20, f"PISO {j if j < 4 else 15}", 9.5, "middle")
        s.arrow(px + 28, y0 - 12, px + 28, y0 + 22, "k", 1)
    s.text(x0 + 5 * cw - 20, y0 - 20, "← wgt bank j (128b = w[j][0..15]) 를 g+j clk에 받아 1바이트씩", 10, color="#4a6f9a")
    # deskew
    yo = y0 + n * ch + 10
    for j in range(n):
        px = x0 + j * cw
        k = 3 if j < 4 else 0
        for q in range(k):
            reg(s, px + 70 + q * 14, yo + 4, 22)
        s.text(px + 55, yo + 44, f"deskew {15 - (j if j < 4 else 15)}", 9.5, "middle", color="#2a6a24")
        s.arrow(px + 80, yo + 30, px + 80, yo + 56, "y", 1.4)
    s.box(x0, yo + 58, 4 * cw + 110, 30, "rtl", ["row_out: 16 × 20비트 (a + 33 clk) → sa_rmw"], 10.5)
    # right: PE detail
    rx = 1030
    s.rect(rx, 140, 410, 520, "#fffdf3", "#9a7b00", 1.4, 6)
    s.text(rx + 12, 162, "PE 하나 = DSP48E2 (sa_pe.v)", 12.5, weight="bold", color="#9a7b00")
    items = [("A2 (AREG 1)", "x[i] (int8 → 30b 부호 확장)"), ("B1 (BREG 2의 1단)", "weight 채움 사슬: CEB1, BCIN → BCOUT"),
             ("B2 (BREG 2의 2단)", "쓰는 weight: CEB2 = flag (새 run의 첫 x와 함께)"), ("M (MREG 1)", "A2 × B2 곱 register"),
             ("P (PREG 1)", "M + PCIN (행 0은 + 0)"), ("PCOUT → 아래 PE PCIN", "48비트 전용 cascade 배선")]
    for k, (a, b) in enumerate(items):
        yy = 190 + k * 52
        s.rect(rx + 14, yy, 160, 36, "#fff3c4", "#9a7b00", 1, 3)
        s.text(rx + 94, yy + 23, a, 10.5, "middle", "bold")
        s.text(rx + 184, yy + 23, b, 10.5)
    s.lines(rx + 14, 520, ["OPMODE: W = 0, Z = PCIN(행 0은 0), X = Y = M", "지연: x가 A에 온 clk t → P는 t + 3",
                           "곱셈기, 덧셈기, register가 모두 DSP 안:", "fabric 배선이 거의 없어 400 MHz"], 10.5)
    s.lines(40, 940, ["• weight 채우기: go(cycle G)에 bank 0 데이터, bank j는 G + j. PISO j가 w[j][15] … w[j][0]을 PE(0, j)의 B에 하나씩, B1이 아래로 흘러 16 clk 뒤 B1(i, j) = w[j][i].",
                      "• flag(새 run의 첫 x)가 x와 함께 오른쪽으로 가며 각 PE의 CEB2가 된다: 그 PE에 새 run의 첫 x가 오는 바로 그 clk에 B2 ← B1. 앞 run의 x는 이전 B2를 쓴다."], 11.5)
    return s


@fig(7, "sa_ctrl")
def s7():
    s = Svg(1460, 860, "systolic EVTA 7. sa_ctrl (sa_ctrl.v): uop FIFO, run queue, weight fill engine, issue, tag pipe",
            ["uop이 들어올 때 wgt 주소가 앞 uop과 다르거나 명령의 첫 단계면 새 run: 그 wgt 주소를 run queue에 넣는다. fill engine은 run queue 순서로 weight를 어레이에 채우고,",
             "run의 첫 uop은 그 fill이 시작된 뒤 17 clk이 지나야 issue한다(B1이 다 찰 때까지). 같은 run의 나머지 uop은 매 clk 하나씩."])
    fifo(s, 40, 170, 140, 30, "s_uop (AXIS)", "65b, tready = 자리 있음")
    blk(s, 240, 140, 300, 110, "rtl", ["newrun = 처음 || wgt ≠ prev_wgt || first", "entry = {newrun, rq, bias, tok, last, reset, wgt, src, dst}",
                                       "FIFO 32 (분산 RAM), run queue가 차면 tready 0"], 10, "enqueue")
    blk(s, 240, 290, 300, 90, "bram", ["uop FIFO (깊이 32)", "head = 다음에 issue할 uop"], 10.5)
    blk(s, 600, 290, 260, 90, "bram", ["run queue (깊이 8)", "새 run의 wgt 주소"], 10.5)
    blk(s, 600, 140, 260, 110, "rtl", ["start = run queue 있음 && (!pend || latch)", "pend: 채우는 중 / 다 채움", "cnt: fill 시작 뒤 clk (16에서 멈춤)",
                                       "ready = pend && cnt ≥ 16"], 10, "fill engine")
    blk(s, 920, 140, 260, 110, "rtl", ["issue = FIFO 있음 && (!newrun || ready)", "latch = issue && newrun (B1 → B2)", "inp_addr ← head.src (다음 clk)"], 10, "issue")
    blk(s, 920, 290, 260, 90, "rtl", ["wgt_addr[0] ← run queue 머리", "wgt_addr[j] ← wgt_addr[j−1] (1 clk씩 밀림)"], 10, "wgt bank 주소")
    blk(s, 1220, 140, 210, 110, "rtl", ["flag_p[L_RD]: latch", "go_p[L_RD]: start", "(SRAM 읽기 지연만큼)"], 10, "→ sa_array")
    blk(s, 1220, 290, 210, 90, "rtl", ["tagp[0..35]", "→ sa_rmw (행과 같이)"], 10, "tag pipe")
    arr(s, 182, 185, 238, 190, "g", 1.6)
    arr(s, 390, 252, 390, 288, "a", 1.3)
    arr(s, 542, 230, 598, 320, "a", 1.3, "newrun이면 wgt", 0, 0, 9.5)
    arr(s, 730, 288, 730, 252, "a", 1.3)
    arr(s, 542, 335, 918, 200, "a", 1.3, "head", 0, -4, 9.5)
    arr(s, 862, 195, 918, 195, "a", 1.3, "ready", 0, -6, 9.5)
    arr(s, 862, 335, 918, 335, "a", 1.3)
    arr(s, 1182, 195, 1218, 195, "a", 1.3)
    arr(s, 1182, 230, 1218, 320, "a", 1.3)
    # timing
    s.section(420, "시간표: run 둘 (A: wgt 5, uop 3개 / B: wgt 9, uop 2개), L_RD = 2")
    cols = 26
    cw = 44
    x0 = 200
    for t in range(cols):
        s.text(x0 + t * cw + cw / 2, 470, str(t), 9.5, "middle", color="#777777")
    rows = [("fill (start)", [(0, 1, "A", "#fde5cc"), (17, 1, "B", "#fde5cc")]),
            ("cnt ≥ 16", [(16, 1, "rdy", "#dcefd6"), (33 - 0, 0, "", "#ffffff")]),
            ("issue", [(17, 1, "A0", "#dce8f5"), (18, 1, "A1", "#dce8f5"), (19, 1, "A2", "#dce8f5"), (34 - 0, 0, "", "#fff")]),
            ("latch (B1→B2)", [(17, 1, "A", "#dcefd6")]),
            ("bank j 주소", [(1, 16, "A의 bank 0 … 15 (j clk 늦게)", "#e6eef7"), (18, 8, "B의 bank 0 …", "#e6eef7")])]
    for r, (lab, ev) in enumerate(rows):
        yy = 480 + r * 34
        s.text(x0 - 10, yy + 18, lab, 11, "end", "bold")
        s.rect(x0, yy, cols * cw, 26, "#fafafa", "#e0e0e0", 0.6)
        for t, w, txt, col in ev:
            if w <= 0 or t >= cols:
                continue
            w = min(w, cols - t)
            s.rect(x0 + t * cw + 1, yy + 2, w * cw - 2, 22, col, "#888888", 0.8, 3)
            s.text(x0 + t * cw + w * cw / 2, yy + 17, txt, 10, "middle")
    s.lines(40, 680, ["• B의 첫 uop은 B의 fill(clk 17 시작)이 끝나는 clk 34부터 issue할 수 있다: run 하나가 17 clk보다 짧으면 어레이가 논다(fill에 묶임).",
                      "  vtalib-zcu102-systolic의 uop 순서는 run을 길게 만든다(같은 weight로 tile의 출력 위치를 쭉). 1×1 conv처럼 짧은 run이 많으면 가동률이 떨어진다.",
                      "• 다음 fill은 앞 run의 latch 때 시작할 수 있다(B1을 덮어써도 B2는 이미 앞 weight를 쥐고 있다): fill과 계산이 겹친다.",
                      "• idle = FIFO 비었고 tag pipe에 유효한 단계 없음 → gemm_sa busy의 일부(acc / out mux)."], 11.5)
    return s


@fig(8, "sa_rmw")
def s8():
    s = Svg(1460, 760, "systolic EVTA 8. sa_rmw (sa_rmw.v): acc read-modify-write와 out 쓰기 (11-04 bias reset, requantize 포함)",
            ["행(16 × 20비트 부분합)과 tag가 같은 clk에 온다. acc[dst]를 읽어(포트 A, 2 clk) 더하고 S3에 쓴다(포트 B). 앞 행이 같은 dst를 아직 쓰는 중이면 그 값을 가져온다(forwarding).",
             "out에는 S6에서 requantize한 int8을 쓴다(rq_en = 0이면 하위 8비트). done / token / busy는 out 쓰기에 맞춘다."])
    rmw_pipe(s, 40, 130, bias=True, rq=True)
    s.lines(40, 420, [("forwarding이 필요한 이유", "bold"),
                      "• acc를 읽고(S0 주소 → S2 데이터) 쓰기(S3)까지 3 clk. 같은 dst가 1~3 clk 안에 다시 오면 BRAM에는 아직 옛 값이 있다.",
                      "  sa_rmw는 S3의 sum과 1, 2 clk 전에 쓴 sum(w1, w2)을 들고 있다가 주소가 같으면 BRAM 대신 그것을 old로 쓴다(S1에서 비교해 S2에서 선택).",
                      "• 이 때문에 HLS EVTA의 'DEPENDENCE false' 가정(같은 dst를 몇 단계 안에 다시 읽지 않음) 없이도 결과가 맞다(RTL testbench hazard / same 모드).",
                      ("acc_mem 포트", "bold"),
                      "• 포트 A = 읽기만(EN 항상 1, 출력 register), 포트 B = 쓰기만. 한 clk에 읽기 하나 + 쓰기 하나 → 단계마다 1 clk.",
                      ("400 MHz", "bold"),
                      "• S2(선택 + 32비트 덧셈 16 lane)가 가장 빡빡한 경로라 비교는 S1으로, bias 행의 '행 = 0'도 S1에서. requantize는 S4~S6로 따로."], 11.5)
    return s


@fig(9, "store")
def s9():
    return retitle(H.h5(), "systolic EVTA 9. store: HLS EVTA와 같다 (out_mem 포트 A 쪽이 gemm_sa / compute mux)", 110,
                   [("바뀐 것", "bold"), "• store.vta.cc 코드는 그대로. out_mem 포트 A(쓰는 쪽)는 gemm_sa_bd의 mux: GEMM 단계는 gemm_sa, ALU는 compute.",
                    "• DRAM 쓰기(m_axi data_port)는 smartconnect에서 333 MHz HP 클럭으로 넘어간다(HP 포트 한계 때문에 EVTA 400 MHz와 분리)."])


@fig(10, "interfaces")
def s10():
    s = Svg(1460, 760, "systolic EVTA 10. interface와 버퍼: HLS EVTA(그림 HLS 6)에서 바뀐 것만",
            ["레지스터 주소, 명령 큐, dependence 큐, inp / wgt / out SRAM 크기는 같다. 아래는 새로 생기거나 바뀐 것."])

    def table(x, y, cols, rows, size=11.5, rh=26):
        tot = sum(w for _, w in cols)
        s.rect(x, y, tot, rh, "#f3f6fa", "#c8d3e0", 1)
        cx = x
        for t, w in cols:
            s.text(cx + 6, y + 17, t, size, weight="bold")
            cx += w
        for r, row in enumerate(rows):
            yy = y + rh * (r + 1)
            s.rect(x, yy, tot, rh, "#ffffff" if r % 2 == 0 else "#fafafa", "#e2e2e2", 0.8)
            cx = x
            for (t, w), c in zip(cols, row):
                s.text(cx + 6, yy + 17, c, size)
                cx += w
        return y + rh * (len(rows) + 1)
    y = table(30, 110, [("무엇", 230), ("종류", 230), ("폭 / 크기", 260), ("쓰는 쪽 → 읽는 쪽", 300), ("비고", 380)],
              [["gemm_uop", "AXI-Stream (HLS ↔ RTL)", "65b (72b로 전송)", "compute → gemm_sa", "GEMM 단계 하나 = 한 beat"],
               ["gemm_done", "AXI-Stream", "8b", "gemm_sa → compute", "GEMM 명령 하나 = 하나 (counter)"],
               ["g2l / g2s (gemm_sa 쪽)", "AXI-Stream 합류", "8b", "gemm_sa + compute → 큐", "gemm_sa_bd: gemm_sa token 우선"],
               ["acc_mem", "blk_mem_gen TDP (BD)", "4096 × 512b = 256 KB", "A: 읽기, B: 쓰기", "compute / gemm_sa mux, A 출력 register"],
               ["out_mem 포트 A", "기존 BRAM", "4096 × 128b", "compute / gemm_sa mux → store", "포트 B(store)는 그대로"],
               ["inp_mem / wgt_mem 포트 B", "기존 BRAM", "", "load → gemm_sa", "출력 register(MEM_OREG), 읽기 2 clk"],
               ["uop FIFO, run queue", "sa_ctrl 안 (분산 RAM)", "32 × 65b, 8 × 11b", "sa_ctrl", "uop 저장, 새 run의 wgt 주소"],
               ["tag pipe", "sa_ctrl 안 register", "36단 × 54b", "sa_ctrl → sa_rmw", "dst, reset, last, tok, bias, src, rq"],
               ["hp_pll, hp_rst", "clk_wiz, proc_sys_reset", "333 MHz", "smartconnect M 쪽, HP0", "FREQ > 333일 때"],
               ["reg_dwc", "axi_dwidth_converter", "128 → 32b", "PS HPM0 → axi_xbar", "PS 클럭 100 MHz"]])
    s.lines(30, y + 30, ["• DSP: 어레이 256 + HLS ALU 등. LUT(single 400 MHz): HLS GEMM 대비 늘고 줄어든 것은 evta_systolic.md, 최신 requantize 빌드 42.1K.",
                         "• acc_mem이 HLS 내부 배열에서 BD BRAM이 된 이유: HLS compute와 RTL gemm_sa가 같은 acc를 써야 해서(포트를 밖으로 꺼냄)."], 11.5)
    return s


@fig(11, "dependence")
def s11():
    s = Svg(1460, 760, "systolic EVTA 11. dependence: 같은 token 규칙, GEMM의 push는 gemm_sa가",
            ["명령의 비트 4개와 큐 4개는 HLS EVTA와 같다(그림 HLS 7). 다른 점은 GEMM 명령의 push(g2l, g2s)를 언제 누가 넣느냐다."])
    y = 130
    blk(s, 60, y, 220, 90, "hls", ["inp / wgt 씀"], 12, "load")
    blk(s, 420, y, 240, 90, NEW, ["GEMM: uop 보냄", "push 비트는 last uop에"], 11, "compute")
    blk(s, 760, y, 240, 90, NEW, ["last 단계를 acc / out에", "쓴 clk에 token"], 11, "gemm_sa")
    blk(s, 1120, y, 220, 90, "hls", ["out 읽음"], 12, "store")
    arr(s, 662, y + 30, 758, y + 30, "g", 1.8, "uop (g2l, g2s 비트)", 0, -6, 10)
    poly(s, [(820, y + 92), (820, y + 150), (300, y + 150), (300, y + 70), (282, y + 70)], "r", 1.8)
    s.text(500, y + 145, "g2l token (load가 inp / wgt를 덮어써도 됨)", 10.5, color="#b03a2e")
    arr(s, 1002, y + 60, 1118, y + 60, "r", 1.8, "g2s token", 0, -6, 10)
    s.section(300, "HLS EVTA와 비교: GEMM 명령 X 뒤의 load / store가 시작하는 때")
    s.lines(60, 350, ["HLS EVTA: compute가 GEMM X의 모든 단계를 끝내고(II = 1, 같은 IP 안) push → load / store 시작.",
                      "systolic, 처음 생각: compute가 uop을 다 보낸 뒤 done을 기다렸다 push → 다음 GEMM의 II = 1 루프 안에 있는 compute가 done을 늦게 읽어",
                      "    load가 입력 블록마다 GEMM 명령 하나만큼 더 기다렸다(Phase D 측정).",
                      "systolic, 지금: push 비트를 X의 last uop에 실어 보내고, gemm_sa가 그 단계를 acc / out에 쓴 clk에 바로 token. compute는 done을 세기만 한다.",
                      "    gemm_sa_bd가 gemm_sa의 token과 compute의 token(ALU 등)을 같은 큐에 합친다(동시에 오면 gemm_sa 먼저, 한 번에 하나)."], 12)
    s.section(500, "규칙 (vta.cc compute, VTA_SYSTOLIC)")
    s.lines(60, 550, ["• GEMM이 아닌 명령(ALU, acc LOAD, token이 있는 uop LOAD, FINISH)은 시작 전에 sa_out(보낸 GEMM 수)이 0이 될 때까지 done을 읽는다:",
                      "  acc / out 포트를 gemm_sa에게서 넘겨받고, token 순서(GEMM의 push가 뒤 명령의 push보다 먼저)를 지키기 위해.",
                      "• token이 없는 uop LOAD는 기다리지 않는다(uop_mem은 compute만 쓰고, gemm_sa는 uop을 이미 stream으로 받았다).",
                      "• 단계가 0개인 GEMM은 gemm_sa로 가지 않아 compute가 직접 push한다. vtalib의 no-op(DepPush가 만드는 빈 명령)은 token이 있는 uop LOAD라 기다린다."], 12)
    return s


def main():
    nums = [int(a) for a in sys.argv[1:]] or sorted(FIGS)
    for n in nums:
        name, f = FIGS[n]
        s = f()
        p = os.path.join(OUT, f"evta_sa_{n:02d}_{name}.svg")
        s.save(p)
        print("wrote", os.path.relpath(p))


if __name__ == "__main__":
    main()
