// #### JO added (2026-11-29, pixel pair experiment): gemm_sa (PX 0) or gemm_sa_px (PX 1) with its memories, for an
// out-of-context place and route with the BRAM paths timed (ooc.tcl leaves the memory ports as unconstrained ports, so
// acc_mem -> compute is not checked there).  As gemm_sa_bd: inp / wgt port A = LOAD write, port B = GEMM read; acc port
// A = GEMM read / HLS port A, port B = GEMM write / HLS port B, out port A = GEMM write / HLS port; the muxes select on
// sel (registered busy).  PX 1: inp / acc / out are two banks by entry parity; an HLS access goes to bank addr[0] and its
// read data comes back through a mux on the registered addr[0].  Memories inferred (READ_FIRST, latency 2 = BRAM output
// register).  Every top port is registered.  Sizes as 16/16/19/18: inp 4096 x 128, wgt 16 x 2048 x 128, acc 4096 x 512,
// out 4096 x 128.
`timescale 1ns / 1ps
module ooc_bram #(parameter AW = 12, parameter W = 128) (
  input  wire          clk,
  input  wire          a_en, a_we, input wire [AW-1:0] a_addr, input wire [W-1:0] a_din, output reg [W-1:0] a_dout,
  input  wire          b_en, b_we, input wire [AW-1:0] b_addr, input wire [W-1:0] b_din, output reg [W-1:0] b_dout
);
  (* ram_style = "block" *) reg [W-1:0] m [0:(1<<AW)-1];
  reg [W-1:0] ra, rb;
  always @(posedge clk) begin
    if (a_en) begin ra <= m[a_addr]; if (a_we) m[a_addr] <= a_din; end
    a_dout <= ra;
  end
  always @(posedge clk) begin
    if (b_en) begin rb <= m[b_addr]; if (b_we) m[b_addr] <= b_din; end
    b_dout <= rb;
  end
endmodule

module ooc_gemm_mem #(
  parameter PX = 0,
  parameter N = 16, DW = 12, SW = 12, WW = 11, L_RD = 2,
  parameter UWD = (PX ? DW + SW + WW + 33 : DW + SW + WW + 32)
) (
  input  wire              clk,
  input  wire              rstn,
  input  wire [UWD-1:0]    s_uop_tdata,
  input  wire              s_uop_tvalid,
  output reg               s_uop_tready,
  output reg               m_done_tvalid, m_g2l_tvalid, m_g2s_tvalid,
  input  wire              li_en, input wire [SW-1:0] li_addr, input wire [127:0] li_data,                 // LOAD inp
  input  wire [N-1:0]      lw_en, input wire [WW-1:0] lw_addr, input wire [127:0] lw_data,                 // LOAD wgt
  input  wire              ha_en, ha_we, input wire [DW-1:0] ha_addr, input wire [511:0] ha_din, output reg [511:0] ha_dout,
  input  wire              hb_en, hb_we, input wire [DW-1:0] hb_addr, input wire [511:0] hb_din, output reg [511:0] hb_dout,
  input  wire              ho_en, ho_we, input wire [DW-1:0] ho_addr, input wire [127:0] ho_din, output reg [127:0] ho_dout
);
  localparam NB = PX ? 2 : 1, IA = PX ? SW - 1 : SW, AA = PX ? DW - 1 : DW;
  // registered top inputs
  reg [UWD-1:0] u_d; reg u_v, rn;
  reg li_e; reg [SW-1:0] li_a; reg [127:0] li_d;
  reg [N-1:0] lw_e; reg [WW-1:0] lw_a; reg [127:0] lw_d;
  reg ha_e, ha_w, hb_e, hb_w, ho_e, ho_w; reg [DW-1:0] ha_a, hb_a, ho_a; reg [511:0] ha_di, hb_di; reg [127:0] ho_di;
  always @(posedge clk) begin
    u_d <= s_uop_tdata; u_v <= s_uop_tvalid; rn <= rstn;
    li_e <= li_en; li_a <= li_addr; li_d <= li_data; lw_e <= lw_en; lw_a <= lw_addr; lw_d <= lw_data;
    ha_e <= ha_en; ha_w <= ha_we; ha_a <= ha_addr; ha_di <= ha_din;
    hb_e <= hb_en; hb_w <= hb_we; hb_a <= hb_addr; hb_di <= hb_din;
    ho_e <= ho_en; ho_w <= ho_we; ho_a <= ho_addr; ho_di <= ho_din;
  end

  wire u_r, d_v, g2l_v, g2s_v, busy;
  wire [7:0] d_d;
  wire [NB-1:0] inp_en, acc_rd_en, acc_wr_en, out_wr_en;
  wire [NB*IA-1:0] inp_addr;
  wire [NB*AA-1:0] acc_rd_addr, acc_wr_addr, out_wr_addr;
  wire [NB*128-1:0] inp_data, out_wr_data;
  wire [NB*512-1:0] acc_rd_data, acc_wr_data;
  wire [N-1:0] wgt_en; wire [WW*N-1:0] wgt_addr; wire [128*N-1:0] wgt_data;

  generate
    if (PX) begin : g_px
      gemm_sa_px #(.N(N), .DW(DW), .SW(SW), .WW(WW), .L_RD(L_RD)) sa (
        .clk(clk), .rstn(rn), .s_uop_tdata(u_d), .s_uop_tvalid(u_v), .s_uop_tready(u_r),
        .m_done_tdata(d_d), .m_done_tvalid(d_v), .m_done_tready(1'b1),
        .m_g2l_tvalid(g2l_v), .m_g2l_tready(1'b1), .m_g2s_tvalid(g2s_v), .m_g2s_tready(1'b1),
        .inp_en(inp_en), .inp_addr(inp_addr), .inp_data(inp_data), .wgt_en(wgt_en), .wgt_addr(wgt_addr), .wgt_data(wgt_data),
        .acc_rd_en(acc_rd_en), .acc_rd_addr(acc_rd_addr), .acc_rd_data(acc_rd_data),
        .acc_wr_en(acc_wr_en), .acc_wr_addr(acc_wr_addr), .acc_wr_data(acc_wr_data),
        .out_wr_en(out_wr_en), .out_wr_addr(out_wr_addr), .out_wr_data(out_wr_data), .busy(busy));
    end else begin : g_sa
      gemm_sa #(.N(N), .DW(DW), .SW(SW), .WW(WW), .L_RD(L_RD), .DWU(0)) sa (
        .clk(clk), .rstn(rn), .s_uop_tdata(u_d), .s_uop_tvalid(u_v), .s_uop_tready(u_r),
        .m_done_tdata(d_d), .m_done_tvalid(d_v), .m_done_tready(1'b1),
        .m_g2l_tvalid(g2l_v), .m_g2l_tready(1'b1), .m_g2s_tvalid(g2s_v), .m_g2s_tready(1'b1),
        .inp_en(inp_en), .inp_addr(inp_addr), .inp_data(inp_data), .wgt_en(wgt_en), .wgt_addr(wgt_addr), .wgt_data(wgt_data),
        .acc_rd_en(acc_rd_en), .acc_rd_addr(acc_rd_addr), .acc_rd_data(acc_rd_data),
        .acc_wr_en(acc_wr_en), .acc_wr_addr(acc_wr_addr), .acc_wr_data(acc_wr_data),
        .out_wr_en(out_wr_en), .out_wr_addr(out_wr_addr), .out_wr_data(out_wr_data), .busy(busy));
    end
  endgenerate
  always @(posedge clk) begin
    s_uop_tready <= u_r; m_done_tvalid <= d_v; m_g2l_tvalid <= g2l_v; m_g2s_tvalid <= g2s_v;
  end

  (* max_fanout = 64 *) reg sel;
  always @(posedge clk) sel <= rn ? busy : 1'b0;

  // wgt: 16 banks
  genvar j, b;
  generate
    for (j = 0; j < N; j = j + 1) begin : wgt
      ooc_bram #(.AW(WW), .W(128)) m (.clk(clk),
        .a_en(lw_e[j]), .a_we(1'b1), .a_addr(lw_a), .a_din(lw_d), .a_dout(),
        .b_en(1'b1), .b_we(1'b0), .b_addr(wgt_addr[WW*j +: WW]), .b_din(128'd0), .b_dout(wgt_data[128*j +: 128]));
    end
  endgenerate

  // HLS-side bank of each access and its registered copy for the read data (latency 2)
  wire [NB-1:0] hab, hbb, hob, lib;
  reg  [1:0] has, hbs, hos;
  wire [NB*512-1:0] ha_do, hb_do;
  wire [NB*128-1:0] ho_do;
  generate
    if (PX) begin : g_bs
      assign hab = {ha_a[0], !ha_a[0]}; assign hbb = {hb_a[0], !hb_a[0]}; assign hob = {ho_a[0], !ho_a[0]};
      assign lib = {li_a[0], !li_a[0]};
    end else begin : g_bs1
      assign hab = 1'b1; assign hbb = 1'b1; assign hob = 1'b1; assign lib = 1'b1;
    end
  endgenerate
  always @(posedge clk) begin
    has <= {has[0], PX ? ha_a[0] : 1'b0}; hbs <= {hbs[0], PX ? hb_a[0] : 1'b0}; hos <= {hos[0], PX ? ho_a[0] : 1'b0};
    ha_dout <= ha_do[512*has[1] +: 512]; hb_dout <= hb_do[512*hbs[1] +: 512]; ho_dout <= ho_do[128*hos[1] +: 128];
  end
  generate
    for (b = 0; b < NB; b = b + 1) begin : bank
      wire [IA-1:0] lia = PX ? li_a[SW-1:1] : li_a[IA-1:0];
      wire [AA-1:0] haa = PX ? ha_a[DW-1:1] : ha_a[AA-1:0];
      wire [AA-1:0] hba = PX ? hb_a[DW-1:1] : hb_a[AA-1:0];
      wire [AA-1:0] hoa = PX ? ho_a[DW-1:1] : ho_a[AA-1:0];
      ooc_bram #(.AW(IA), .W(128)) inp (.clk(clk),
        .a_en(li_e && lib[b]), .a_we(1'b1), .a_addr(lia), .a_din(li_d), .a_dout(),
        .b_en(1'b1), .b_we(1'b0), .b_addr(inp_addr[IA*b +: IA]), .b_din(128'd0), .b_dout(inp_data[128*b +: 128]));
      ooc_bram #(.AW(AA), .W(512)) acc (.clk(clk),
        .a_en(sel ? 1'b1 : ha_e && hab[b]), .a_we(sel ? 1'b0 : ha_w), .a_addr(sel ? acc_rd_addr[AA*b +: AA] : haa),
        .a_din(ha_di), .a_dout(ha_do[512*b +: 512]),
        .b_en(sel ? acc_wr_en[b] : hb_e && hbb[b]), .b_we(sel ? 1'b1 : hb_w), .b_addr(sel ? acc_wr_addr[AA*b +: AA] : hba),
        .b_din(sel ? acc_wr_data[512*b +: 512] : hb_di), .b_dout(hb_do[512*b +: 512]));
      assign acc_rd_data[512*b +: 512] = ha_do[512*b +: 512];
      ooc_bram #(.AW(AA), .W(128)) out (.clk(clk),
        .a_en(sel ? out_wr_en[b] : ho_e && hob[b]), .a_we(sel ? 1'b1 : ho_w), .a_addr(sel ? out_wr_addr[AA*b +: AA] : hoa),
        .a_din(sel ? out_wr_data[128*b +: 128] : ho_di), .a_dout(ho_do[128*b +: 128]),
        .b_en(1'b0), .b_we(1'b0), .b_addr({AA{1'b0}}), .b_din(128'd0), .b_dout());
    end
  endgenerate
endmodule
