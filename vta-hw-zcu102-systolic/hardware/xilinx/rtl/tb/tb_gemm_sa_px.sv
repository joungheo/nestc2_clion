// #### JO added (2026-11-29, pixel pair experiment): gemm_sa_px against gemm_sa on the same random program.
// gemm_sa gets the program with every pair uop as two uops (src, dst), (src+1, dst+1); gemm_sa_px gets the pair uops
// (pair bit 67) on two-bank inp / acc / out memories (entry e = bank e[0], address e >> 1).  Instructions: random weight
// block, rq, reset / bias reset, steps with repeated destinations (forwarding inside a bank and across pairs), single
// steps mixed in, random gaps.  Final acc and out of both must be equal; done / token counts too.
// plusargs: +seed=<n> +gap=<0..99>.   usage: rtl/tb/run_gemm_sa_px.sh [seeds]
`timescale 1ns / 1ps
module tb_gemm_sa_px;
  localparam N = 16, DW = 12, SW = 12, WW = 11, L_RD = 2;
  localparam NI = 256, NWB = 8, NA = 256, NINSN = 16, MAXU = 4096;

  reg clk = 0, rstn = 0;
  always #1.25 clk = ~clk;

  // ---------------- shared initial memories ----------------
  reg [127:0] inp_mem [0:NI-1];
  reg [127:0] wgt_bank [0:N-1][0:NWB-1];
  reg [511:0] acc_init [0:NA-1];

  // ---------------- reference: gemm_sa ----------------
  reg  [67:0] ru [0:MAXU-1];  integer nru;      // unpaired program
  reg  [67:0] pu [0:MAXU-1];  integer npu;      // paired program
  reg  [66:0] r_d;  reg r_v;  wire r_r;
  wire [7:0] r_dd; wire r_dv; wire r_g2l, r_g2s;
  wire r_inp_en; wire [SW-1:0] r_inp_addr; wire [N-1:0] r_wgt_en; wire [WW*N-1:0] r_wgt_addr;
  wire r_acc_rd_en, r_acc_wr_en, r_out_wr_en, r_busy;
  wire [DW-1:0] r_acc_rd_addr, r_acc_wr_addr, r_out_wr_addr;
  wire [32*N-1:0] r_acc_wr_data; wire [8*N-1:0] r_out_wr_data;
  reg [511:0] r_acc [0:NA-1]; reg [127:0] r_out [0:NA-1];
  reg [127:0] r_inp_p [0:L_RD-1]; reg [127:0] r_wgt_p [0:N-1][0:L_RD-1]; reg [511:0] r_acc_p0, r_acc_p1;

  // ---------------- DUT: gemm_sa_px ----------------
  reg  [67:0] p_d;  reg p_v;  wire p_r;
  wire [7:0] p_dd; wire p_dv; wire p_g2l, p_g2s;
  wire [1:0] p_inp_en; wire [2*SW-3:0] p_inp_addr; wire [N-1:0] p_wgt_en; wire [WW*N-1:0] p_wgt_addr;
  wire [1:0] p_acc_rd_en, p_acc_wr_en, p_out_wr_en; wire p_busy;
  wire [2*DW-3:0] p_acc_rd_addr, p_acc_wr_addr, p_out_wr_addr;
  wire [2*32*N-1:0] p_acc_wr_data; wire [2*8*N-1:0] p_out_wr_data;
  reg [511:0] p_acc [0:NA-1]; reg [127:0] p_out [0:NA-1];
  reg [255:0] p_inp_p [0:L_RD-1]; reg [127:0] p_wgt_p [0:N-1][0:L_RD-1]; reg [1023:0] p_acc_p0, p_acc_p1;

  integer j, s, bk;
  always @(posedge clk) begin
    // reference memories
    r_inp_p[0] <= r_inp_en ? inp_mem[r_inp_addr] : 128'bx;
    for (s = 1; s < L_RD; s = s + 1) r_inp_p[s] <= r_inp_p[s-1];
    for (j = 0; j < N; j = j + 1) begin
      r_wgt_p[j][0] <= r_wgt_en[j] ? wgt_bank[j][r_wgt_addr[WW*j +: WW]] : 128'bx;
      for (s = 1; s < L_RD; s = s + 1) r_wgt_p[j][s] <= r_wgt_p[j][s-1];
    end
    r_acc_p0 <= r_acc_rd_en ? r_acc[r_acc_rd_addr] : 512'bx; r_acc_p1 <= r_acc_p0;
    if (r_acc_wr_en) r_acc[r_acc_wr_addr] <= r_acc_wr_data;
    if (r_out_wr_en) r_out[r_out_wr_addr] <= r_out_wr_data;
    // banked memories of gemm_sa_px (entry = 2 * address + bank)
    for (bk = 0; bk < 2; bk = bk + 1) begin
      p_inp_p[0][128*bk +: 128] <= inp_mem[2 * p_inp_addr[(SW-1)*bk +: SW-1] + bk];   // EN stays 1 (gemm_sa_bd)
      p_acc_p0[512*bk +: 512] <= p_acc_rd_en[bk] ? p_acc[2 * p_acc_rd_addr[(DW-1)*bk +: DW-1] + bk] : 512'bx;
      if (p_acc_wr_en[bk]) p_acc[2 * p_acc_wr_addr[(DW-1)*bk +: DW-1] + bk] <= p_acc_wr_data[512*bk +: 512];
      if (p_out_wr_en[bk]) p_out[2 * p_out_wr_addr[(DW-1)*bk +: DW-1] + bk] <= p_out_wr_data[128*bk +: 128];
    end
    for (s = 1; s < L_RD; s = s + 1) p_inp_p[s] <= p_inp_p[s-1];
    p_acc_p1 <= p_acc_p0;
    for (j = 0; j < N; j = j + 1) begin
      p_wgt_p[j][0] <= p_wgt_en[j] ? wgt_bank[j][p_wgt_addr[WW*j +: WW]] : 128'bx;
      for (s = 1; s < L_RD; s = s + 1) p_wgt_p[j][s] <= p_wgt_p[j][s-1];
    end
  end
  wire [128*N-1:0] r_wgt_data, p_wgt_data;
  genvar g;
  generate for (g = 0; g < N; g = g + 1) begin : wd
    assign r_wgt_data[128*g +: 128] = r_wgt_p[g][L_RD-1];
    assign p_wgt_data[128*g +: 128] = p_wgt_p[g][L_RD-1];
  end endgenerate

  gemm_sa #(.N(N), .DW(DW), .SW(SW), .WW(WW), .L_RD(L_RD), .DWU(0)) ref_sa (
    .clk(clk), .rstn(rstn), .s_uop_tdata(r_d), .s_uop_tvalid(r_v), .s_uop_tready(r_r),
    .m_done_tdata(r_dd), .m_done_tvalid(r_dv), .m_done_tready(1'b1),
    .m_g2l_tvalid(r_g2l), .m_g2l_tready(1'b1), .m_g2s_tvalid(r_g2s), .m_g2s_tready(1'b1),
    .inp_en(r_inp_en), .inp_addr(r_inp_addr), .inp_data(r_inp_p[L_RD-1]),
    .wgt_en(r_wgt_en), .wgt_addr(r_wgt_addr), .wgt_data(r_wgt_data),
    .acc_rd_en(r_acc_rd_en), .acc_rd_addr(r_acc_rd_addr), .acc_rd_data(r_acc_p1),
    .acc_wr_en(r_acc_wr_en), .acc_wr_addr(r_acc_wr_addr), .acc_wr_data(r_acc_wr_data),
    .out_wr_en(r_out_wr_en), .out_wr_addr(r_out_wr_addr), .out_wr_data(r_out_wr_data), .busy(r_busy));

  gemm_sa_px #(.N(N), .DW(DW), .SW(SW), .WW(WW), .L_RD(L_RD)) dut (
    .clk(clk), .rstn(rstn), .s_uop_tdata(p_d), .s_uop_tvalid(p_v), .s_uop_tready(p_r),
    .m_done_tdata(p_dd), .m_done_tvalid(p_dv), .m_done_tready(1'b1),
    .m_g2l_tvalid(p_g2l), .m_g2l_tready(1'b1), .m_g2s_tvalid(p_g2s), .m_g2s_tready(1'b1),
    .inp_en(p_inp_en), .inp_addr(p_inp_addr), .inp_data(p_inp_p[L_RD-1]),
    .wgt_en(p_wgt_en), .wgt_addr(p_wgt_addr), .wgt_data(p_wgt_data),
    .acc_rd_en(p_acc_rd_en), .acc_rd_addr(p_acc_rd_addr), .acc_rd_data(p_acc_p1),
    .acc_wr_en(p_acc_wr_en), .acc_wr_addr(p_acc_wr_addr), .acc_wr_data(p_acc_wr_data),
    .out_wr_en(p_out_wr_en), .out_wr_addr(p_out_wr_addr), .out_wr_data(p_out_wr_data), .busy(p_busy));

  // ---------------- program ----------------
  integer seed0, seed, gap, k, i, n, L, w, rs, bi, dst, src, pr, nl, b;
  reg [23:0] rq;
  function integer rnd(input integer lo, input integer hi);
    rnd = lo + ($unsigned($random(seed)) % (hi - lo + 1));
  endfunction
  // uop = {pair, emit, dw, rq, bias, g2s, g2l, first, last, reset, wgt, src, dst}
  function [67:0] mk(input pair, input [23:0] q, input bias, input [1:0] tok, input first, input last, input reset,
                     input [WW-1:0] wg, input [SW-1:0] sr, input [DW-1:0] ds);
    mk = {pair, 2'b00, q, bias, tok, first, last, reset, wg, sr, ds};
  endfunction

  integer r_sent, p_sent, r_done, p_done, r_ng2l, r_ng2s, p_ng2l, p_ng2s, cyc, bad_acc, bad_out, cpx0, cpx1, cr0, cr1;
  initial begin
    if (!$value$plusargs("seed=%d", seed)) seed = 1;
    seed0 = seed;
    if (!$value$plusargs("gap=%d", gap)) gap = 0;
    for (k = 0; k < NI; k = k + 1)
      for (i = 0; i < N; i = i + 1) inp_mem[k][8*i +: 8] = rnd(-127, 127);
    for (b = 0; b < NWB; b = b + 1)
      for (j = 0; j < N; j = j + 1)
        for (i = 0; i < N; i = i + 1) wgt_bank[j][b][8*i +: 8] = rnd(-128, 127);
    for (k = 0; k < NA; k = k + 1) begin
      for (i = 0; i < N; i = i + 1) acc_init[k][32*i +: 32] = rnd(-100000, 100000);
      r_acc[k] = acc_init[k]; p_acc[k] = acc_init[k]; r_out[k] = 0; p_out[k] = 0;
    end
    nru = 0; npu = 0; nl = 0;
    for (n = 0; n < NINSN; n = n + 1) begin
      w  = rnd(0, NWB - 1);
      rs = (rnd(0, 3) == 0);                      // reset instruction
      bi = rs && rnd(0, 1);                       // bias reset
      rq = 0;
      if (rnd(0, 1)) begin
        rq[0] = 1; rq[2:1] = rnd(0, 2); rq[7:3] = rnd(0, 12);
        rq[15:8] = rnd(-128, -1); rq[23:16] = rnd(0, 127);
      end
      L = rnd(1, 40);
      for (k = 0; k < L; k = k + 1) begin
        pr  = (rnd(0, 4) != 0);
        dst = (rnd(0, 2) == 0) ? rnd(0, 9) : rnd(0, NA - 2);     // a third into a small pool: forwarding
        if (bi) begin
          src = rnd(0, NA / 2 - 2) * 2 + (dst % 2);              // bias rows: src[0] == dst[0]
        end else src = rnd(0, NI - 2);
        pu[npu] = mk(pr, rq, bi, (k == L - 1) ? rnd(0, 3) : 2'd0, k == 0, k == L - 1, rs, w, src, dst);
        ru[nru] = mk(0, rq, bi, (pr || k != L - 1) ? 2'd0 : pu[npu][39:38], k == 0, !pr && k == L - 1, rs, w, src, dst);
        nru = nru + 1;
        if (pr) begin
          ru[nru] = mk(0, rq, bi, (k == L - 1) ? pu[npu][39:38] : 2'd0, 1'b0, k == L - 1, rs, w, src + 1, dst + 1);
          nru = nru + 1;
        end
        npu = npu + 1;
      end
      nl = nl + 1;
    end

    r_v = 0; p_v = 0; r_sent = 0; p_sent = 0; r_done = 0; p_done = 0; cyc = 0;
    r_ng2l = 0; r_ng2s = 0; p_ng2l = 0; p_ng2s = 0; cr0 = -1; cpx0 = -1; cr1 = 0; cpx1 = 0;
    #200;                                       // glbl GSR
    @(posedge clk); rstn <= 1;
    repeat (4) @(posedge clk);
    while (r_done < nl || p_done < nl || r_busy || p_busy) begin
      @(posedge clk);
      cyc = cyc + 1;
      if (r_v && r_r) begin r_sent = r_sent + 1; if (cr0 < 0) cr0 = cyc; end
      if (p_v && p_r) begin p_sent = p_sent + 1; if (cpx0 < 0) cpx0 = cyc; end
      if (r_dv) begin r_done = r_done + 1; cr1 = cyc; end
      if (p_dv) begin p_done = p_done + 1; cpx1 = cyc; end
      if (r_g2l) r_ng2l = r_ng2l + 1;  if (r_g2s) r_ng2s = r_ng2s + 1;
      if (p_g2l) p_ng2l = p_ng2l + 1;  if (p_g2s) p_ng2s = p_ng2s + 1;
      if (r_sent < nru && ($urandom % 100) >= gap) begin r_v <= 1; r_d <= ru[r_sent][66:0]; end else r_v <= 0;
      if (r_v && r_r && r_sent >= nru) r_v <= 0;
      if (p_sent < npu && ($urandom % 100) >= gap) begin p_v <= 1; p_d <= pu[p_sent]; end else p_v <= 0;
      if (p_v && p_r && p_sent >= npu) p_v <= 0;
      if (cyc > 200000) begin $display("RESULT FAIL timeout seed %0d: ref %0d/%0d px %0d/%0d", seed0, r_done, nl, p_done, nl); $finish; end
    end
    repeat (8) @(posedge clk);
    bad_acc = 0; bad_out = 0;
    for (k = 0; k < NA; k = k + 1) begin
      if (p_acc[k] !== r_acc[k]) begin
        bad_acc = bad_acc + 1;
        if (bad_acc <= 3) $display("acc[%0d]\n  px  %h\n  ref %h", k, p_acc[k], r_acc[k]);
      end
      if (p_out[k] !== r_out[k]) begin
        bad_out = bad_out + 1;
        if (bad_out <= 3) $display("out[%0d] px %h ref %h", k, p_out[k], r_out[k]);
      end
    end
    $display("RESULT %0s seed %0d: acc_wrong %0d out_wrong %0d | uops ref %0d px %0d, cycles ref %0d px %0d | done %0d/%0d tokens g2l %0d/%0d g2s %0d/%0d",
      (bad_acc == 0 && bad_out == 0 && r_done == p_done && r_ng2l == p_ng2l && r_ng2s == p_ng2s) ? "PASS" : "FAIL",
      seed0, bad_acc, bad_out, nru, npu, cr1 - cr0 + 1, cpx1 - cpx0 + 1, p_done, r_done, p_ng2l, r_ng2l, p_ng2s, r_ng2s);
    $finish;
  end
endmodule
