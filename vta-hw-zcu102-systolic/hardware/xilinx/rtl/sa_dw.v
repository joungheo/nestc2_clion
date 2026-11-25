// #### JO added (2026-11-24, docs/evta_dw_unit.md 4): the depthwise window unit of gemm_sa (DWU = 1). Beside sa_array, with
// the same inputs and the same latency, so the tag from sa_ctrl and sa_rmw (acc read-modify-write, requantize, out, tokens)
// are unchanged.
//   x_in / f_in / dv: one inp row (16 x int8), the latch flag of a new weight block and "a depthwise step" arrive together
//   in cycle a (as sa_array's x_in / f_in). Each depthwise step shifts the window: win = {win1, win2, x}.
//   Weights: go (cycle G) as sa_array's; bank j data (wd bits 128j +: 128, w[j][k] in bits 8k +: 8) is valid in G+j and
//   bytes k = 0..2 go to B1[j][k]; the flag moves B1 to B2 for the step it comes with (the controller's fill / latch rules
//   keep the next fill off B1 until then, as for the PEs).
//   row_out lane j = sum_k B2[j][k] * win_k[j] (signed, |sum| <= 3 * 128 * 128 < 2^19) in cycle a+33, as sa_array's.
//   Only rows of emit steps are written (tag valid); the others are don't care.
`timescale 1ns / 1ps
module sa_dw #(
  parameter N = 16,
  parameter PW = 20
) (
  input  wire              clk,
  input  wire              rst,
  input  wire [8*N-1:0]    x_in,
  input  wire              f_in,
  input  wire              dv,
  input  wire              go,
  input  wire [128*N-1:0]  wd,
  output wire [PW*N-1:0]   row_out
);
  // #### JO changed (2026-11-25): every input is registered once per lane (cycle a -> a+1; the 400 MHz build missed timing on
  // the flag / dv fanout into all lanes' B2 / window registers, -0.112 ns); all inputs move together, so the order is unchanged
  // and the delay line is one shorter.
  reg  [N-1:1] gcr;
  wire [N-1:0] gc = {gcr, go};
  integer q;
  always @(posedge clk)
    if (rst) gcr <= 0;
    else for (q = 1; q < N; q = q + 1) gcr[q] <= gc[q-1];
  reg  [23:0] b1 [0:N-1];
  reg  [23:0] b2 [0:N-1];
  reg  [7:0]  w0 [0:N-1];
  reg  [7:0]  w1 [0:N-1];
  reg  [7:0]  w2 [0:N-1];
  genvar j;
  generate
    for (j = 0; j < N; j = j + 1) begin : wb
      (* keep = "true" *) reg fr, dr, gr;
      reg [7:0]  xr;
      reg [23:0] wr;
      always @(posedge clk) begin
        fr <= f_in; dr <= dv; gr <= gc[j]; xr <= x_in[8*j +: 8]; wr <= wd[128*j +: 24];
        if (gr) b1[j] <= wr;
        if (fr) b2[j] <= b1[j];
        if (dr) begin w0[j] <= w1[j]; w1[j] <= w2[j]; w2[j] <= xr; end   // window: a+1 -> visible a+2
      end
    end
  endgenerate

  // ---------------- products (a+3), sums (a+4) ----------------
  localparam D = 29;                 // a+4 -> a+33
  reg  signed [15:0] p0 [0:N-1];
  reg  signed [15:0] p1 [0:N-1];
  reg  signed [15:0] p2 [0:N-1];
  reg  [PW-1:0]      sm [0:N-1];
  generate
    for (j = 0; j < N; j = j + 1) begin : ln
      always @(posedge clk) begin
        p0[j] <= $signed(w0[j]) * $signed(b2[j][7:0]);
        p1[j] <= $signed(w1[j]) * $signed(b2[j][15:8]);
        p2[j] <= $signed(w2[j]) * $signed(b2[j][23:16]);
        sm[j] <= {{(PW-16){p0[j][15]}}, p0[j]} + {{(PW-16){p1[j][15]}}, p1[j]} + {{(PW-16){p2[j][15]}}, p2[j]};
      end
      // delay line (SRL: no reset)
      reg [PW-1:0] dl [0:D-1];
      integer s;
      always @(posedge clk) begin
        dl[0] <= sm[j];
        for (s = 1; s < D; s = s + 1) dl[s] <= dl[s-1];
      end
      assign row_out[PW*j +: PW] = dl[D-1];
    end
  endgenerate
endmodule
