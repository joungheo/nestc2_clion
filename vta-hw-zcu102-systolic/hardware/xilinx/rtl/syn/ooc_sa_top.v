// #### JO added (2026-11-28, DSP packing experiment): registered ports around sa_array (MODE 0), sa_array_pk (1: two
// output channels per DSP) or sa_array_px (2: two pixels per DSP) for an out-of-context place and route of the array
// alone (ooc_array.tcl); every path inside is register to register.
`timescale 1ns / 1ps
module ooc_sa_top #(
  parameter MODE = 0,
  parameter N = 16,
  parameter PW = 20,
  parameter XW = (MODE == 2) ? 16 * N : 8 * N,
  parameter WDW = (MODE == 1) ? 256 * N : 128 * N,
  parameter OW = (MODE == 0) ? PW * N : 2 * PW * N
) (
  input  wire           clk,
  input  wire           rst,
  input  wire [XW-1:0]  x_in,
  input  wire           f_in,
  input  wire           go,
  input  wire [WDW-1:0] wd,
  output reg  [OW-1:0]  row_out
);
  reg [XW-1:0]  x_r;
  reg           f_r, go_r, rst_r;
  reg [WDW-1:0] wd_r;
  wire [OW-1:0] ro;
  always @(posedge clk) begin
    x_r <= x_in; f_r <= f_in; go_r <= go; rst_r <= rst; wd_r <= wd; row_out <= ro;
  end
  generate
    if (MODE == 1) begin : g_pk
      sa_array_pk #(.N(N), .PW(PW)) a(.clk(clk), .rst(rst_r), .x_in(x_r), .f_in(f_r), .go(go_r), .wd(wd_r), .row_out(ro));
    end else if (MODE == 2) begin : g_px
      sa_array_px #(.N(N), .PW(PW)) a(.clk(clk), .rst(rst_r), .x_in(x_r), .f_in(f_r), .go(go_r), .wd(wd_r), .row_out(ro));
    end else begin : g_sa
      sa_array #(.N(N), .PW(PW)) a(.clk(clk), .rst(rst_r), .x_in(x_r), .f_in(f_r), .go(go_r), .wd(wd_r), .row_out(ro));
    end
  endgenerate
endmodule
