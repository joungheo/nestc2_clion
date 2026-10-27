// #### JO added (2026-10-27, systolic Phase D, docs/evta_systolic_design.md): RTL GEMM unit of the EVTA compute module.
// The HLS compute streams one uop per GEMM step ({last, reset, wgt, src, dst}, final addresses) and keeps going; this
// unit computes acc[dst] += inp[src] . wgt[wgt]^T on a 16 x 16 weight-stationary DSP48E2 array and writes acc and out
// exactly as vta.cc gemm() does. One done token per GEMM instruction (uop with last) goes back on m_done.
// busy: uops queued or in flight (acc_mem / out_mem port muxes select this unit while it is 1).
`timescale 1ns / 1ps
module gemm_sa #(
  parameter N = 16,
  parameter DW = 12, SW = 12, WW = 11,
  parameter L_RD = 2,                 // inp/wgt read latency; acc read latency is 2
  parameter PW = 20
) (
  input  wire                 clk,
  input  wire                 rstn,
  input  wire [DW+SW+WW+1:0]  s_uop_tdata,
  input  wire                 s_uop_tvalid,
  output wire                 s_uop_tready,
  output wire [7:0]           m_done_tdata,
  output wire                 m_done_tvalid,
  input  wire                 m_done_tready,
  output wire                 inp_en,
  output wire [SW-1:0]        inp_addr,
  input  wire [8*N-1:0]       inp_data,
  output wire [N-1:0]         wgt_en,
  output wire [WW*N-1:0]      wgt_addr,
  input  wire [128*N-1:0]     wgt_data,
  output wire                 acc_rd_en,
  output wire [DW-1:0]        acc_rd_addr,
  input  wire [32*N-1:0]      acc_rd_data,
  output wire                 acc_wr_en,
  output wire [DW-1:0]        acc_wr_addr,
  output wire [32*N-1:0]      acc_wr_data,
  output wire                 out_wr_en,
  output wire [DW-1:0]        out_wr_addr,
  output wire [8*N-1:0]       out_wr_data,
  output wire                 busy
);
  reg rst;
  always @(posedge clk) rst <= !rstn;

  wire flag, go, tv, tz, tl, idle, done, rbusy;
  wire [DW-1:0] td;
  wire [PW*N-1:0] row;

  sa_ctrl #(.N(N), .DW(DW), .SW(SW), .WW(WW), .L_RD(L_RD)) ctrl (
    .clk(clk), .rst(rst),
    .s_uop_tdata(s_uop_tdata), .s_uop_tvalid(s_uop_tvalid), .s_uop_tready(s_uop_tready),
    .inp_en(inp_en), .inp_addr(inp_addr), .wgt_en(wgt_en), .wgt_addr(wgt_addr),
    .arr_flag(flag), .arr_go(go),
    .tag_valid(tv), .tag_dst(td), .tag_reset(tz), .tag_last(tl), .idle(idle));

  sa_array #(.N(N), .PW(PW)) array (
    .clk(clk), .rst(rst), .x_in(inp_data), .f_in(flag), .go(go), .wd(wgt_data), .row_out(row));

  sa_rmw #(.N(N), .PW(PW), .DW(DW)) rmw (
    .clk(clk), .rst(rst), .row(row), .tag_valid(tv), .tag_dst(td), .tag_reset(tz), .tag_last(tl),
    .acc_rd_en(acc_rd_en), .acc_rd_addr(acc_rd_addr), .acc_rd_data(acc_rd_data),
    .acc_wr_en(acc_wr_en), .acc_wr_addr(acc_wr_addr), .acc_wr_data(acc_wr_data),
    .out_wr_en(out_wr_en), .out_wr_addr(out_wr_addr), .out_wr_data(out_wr_data),
    .done(done), .busy(rbusy));

  // done tokens (the HLS side may read them late)
  reg [7:0] ndone;
  always @(posedge clk)
    if (rst) ndone <= 0;
    else ndone <= ndone + done - (m_done_tvalid && m_done_tready);
  assign m_done_tvalid = ndone != 0;
  assign m_done_tdata  = 8'd1;
  assign busy = !idle || rbusy;
endmodule
