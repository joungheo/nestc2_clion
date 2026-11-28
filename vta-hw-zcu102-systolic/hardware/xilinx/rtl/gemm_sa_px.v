// #### JO added (2026-11-29, pixel pair experiment): gemm_sa with two pixels per step (docs: the OOC note in rtl/syn/
// ooc_gemm_mem.tcl).  A uop with pair = 1 (bit DW+SW+WW+32) computes acc[dst] += inp[src] . W^T and
// acc[dst+1] += inp[src+1] . W^T in one cycle on sa_array_px (two pixels per DSP48E2, x in -127..127).
// inp_mem, acc_mem and out_mem are two banks each by entry parity (entry e in bank e[0] at e >> 1), so the two entries of
// a pair are always in different banks: bank 0 holds (e+1) >> 1, bank 1 e >> 1 of the pair starting at e.  One sa_rmw
// per bank (its forwarding is per bank: an entry is always in the same bank).  pair = 0: one pixel, its bank only.
// Bias rows (read acc[src], write acc[dst]) need src[0] == dst[0] (vtalib keeps the bias at two entries b, b+1).
// No depthwise unit here (DWU 0).
`timescale 1ns / 1ps
module gemm_sa_px #(
  parameter N = 16,
  parameter DW = 12, SW = 12, WW = 11,
  parameter L_RD = 2,
  parameter PW = 20
) (
  input  wire                  clk,
  input  wire                  rstn,
  input  wire [DW+SW+WW+32:0]  s_uop_tdata,   // {pair, emit, dw, rq, bias, g2s, g2l, first, last, reset, wgt, src, dst}
  input  wire                  s_uop_tvalid,
  output wire                  s_uop_tready,
  output wire [7:0]            m_done_tdata,
  output wire                  m_done_tvalid,
  input  wire                  m_done_tready,
  output wire                  m_g2l_tvalid,
  input  wire                  m_g2l_tready,
  output wire                  m_g2s_tvalid,
  input  wire                  m_g2s_tready,
  output wire [1:0]            inp_en,         // per bank
  output wire [2*SW-3:0]       inp_addr,       // {bank 1, bank 0}
  input  wire [2*8*N-1:0]      inp_data,       // {bank 1, bank 0}
  output wire [N-1:0]          wgt_en,
  output wire [WW*N-1:0]       wgt_addr,
  input  wire [128*N-1:0]      wgt_data,
  output wire [1:0]            acc_rd_en,
  output wire [2*DW-3:0]       acc_rd_addr,
  input  wire [2*32*N-1:0]     acc_rd_data,
  output wire [1:0]            acc_wr_en,
  output wire [2*DW-3:0]       acc_wr_addr,
  output wire [2*32*N-1:0]     acc_wr_data,
  output wire [1:0]            out_wr_en,
  output wire [2*DW-3:0]       out_wr_addr,
  output wire [2*8*N-1:0]      out_wr_data,
  output wire                  busy
);
  reg rst;
  always @(posedge clk) rst <= !rstn;

  wire flag, go, tv, tz, tl, idle, tb, tp, swap, dv, tdw;
  wire [SW-1:0] ts;
  wire [23:0] trq;
  wire [1:0] tk;
  wire [DW-1:0] td;
  wire [2*PW*N-1:0] arow;                      // {pixel 1, pixel 0}

  sa_ctrl_px #(.N(N), .DW(DW), .SW(SW), .WW(WW), .L_RD(L_RD), .DWU(0)) ctrl (
    .clk(clk), .rst(rst),
    .s_uop_tdata(s_uop_tdata), .s_uop_tvalid(s_uop_tvalid), .s_uop_tready(s_uop_tready),
    .inp_en(inp_en), .inp_addr(inp_addr), .arr_swap(swap), .wgt_en(wgt_en), .wgt_addr(wgt_addr),
    .arr_flag(flag), .arr_go(go), .arr_dv(dv), .tag_dw(tdw), .tag_pair(tp),
    .tag_valid(tv), .tag_dst(td), .tag_reset(tz), .tag_bias(tb), .tag_src(ts), .tag_rq(trq), .tag_last(tl), .tag_tok(tk), .idle(idle));

  // pixel 0 = the bank of src, pixel 1 = the other one
  wire [8*N-1:0] ib0 = inp_data[8*N-1:0], ib1 = inp_data[16*N-1:8*N];
  wire [16*N-1:0] xpair = swap ? {ib0, ib1} : {ib1, ib0};

  sa_array_px #(.N(N), .PW(PW)) array (
    .clk(clk), .rst(rst), .x_in(xpair), .f_in(flag), .go(go), .wd(wgt_data), .row_out(arow));

  // tag split: bank b takes pixel 0 if dst[0] == b, else pixel 1 (pair only)
  wire [PW*N-1:0] px0 = arow[PW*N-1:0], px1 = arow[2*PW*N-1:PW*N];
  wire [DW-1:0] td1 = td + 1'b1;
  wire [DW-1:0] tsd = ts[DW-1:0], ts1 = ts[DW-1:0] + 1'b1;
  wire [1:0] done_b;
  wire [3:0] tok_b;
  wire [1:0] busy_b;
  genvar b;
  generate
    for (b = 0; b < 2; b = b + 1) begin : bank
      wire own = (td[0] == b);                // pixel 0 is in this bank
      wire [PW*N-1:0] row_b = own ? px0 : px1;
      wire vb = tv && (own || tp);
      wire [DW-2:0] dst_b = (b == 0) ? td1[DW-1:1] : td[DW-1:1];
      wire [DW-2:0] src_b = (b == 0) ? ts1[DW-1:1] : tsd[DW-1:1];
      sa_rmw #(.N(N), .PW(PW), .DW(DW-1)) rmw (
        .clk(clk), .rst(rst), .row(row_b), .tag_valid(vb), .tag_dst(dst_b), .tag_reset(tz), .tag_bias(tb), .tag_src(src_b),
        .tag_rq(trq), .tag_last(tl), .tag_tok(tk),
        .acc_rd_en(acc_rd_en[b]), .acc_rd_addr(acc_rd_addr[(DW-1)*b +: DW-1]), .acc_rd_data(acc_rd_data[32*N*b +: 32*N]),
        .acc_wr_en(acc_wr_en[b]), .acc_wr_addr(acc_wr_addr[(DW-1)*b +: DW-1]), .acc_wr_data(acc_wr_data[32*N*b +: 32*N]),
        .out_wr_en(out_wr_en[b]), .out_wr_addr(out_wr_addr[(DW-1)*b +: DW-1]), .out_wr_data(out_wr_data[8*N*b +: 8*N]),
        .done(done_b[b]), .done_tok(tok_b[2*b +: 2]), .busy(busy_b[b]));
    end
  endgenerate
  // both banks of a pair finish in the same cycle: one done / token per instruction
  wire done = |done_b;
  wire [1:0] dtok = tok_b[1:0] | tok_b[3:2];

  reg [7:0] ndone;
  always @(posedge clk)
    if (rst) ndone <= 0;
    else ndone <= ndone + done - (m_done_tvalid && m_done_tready);
  assign m_done_tvalid = ndone != 0;
  assign m_done_tdata  = 8'd1;
  reg [7:0] ng2l, ng2s;
  always @(posedge clk)
    if (rst) begin ng2l <= 0; ng2s <= 0; end
    else begin
      ng2l <= ng2l + dtok[0] - (m_g2l_tvalid && m_g2l_tready);
      ng2s <= ng2s + dtok[1] - (m_g2s_tvalid && m_g2s_tready);
    end
  assign m_g2l_tvalid = ng2l != 0;
  assign m_g2s_tvalid = ng2s != 0;
  assign busy = !idle || |busy_b;
endmodule
