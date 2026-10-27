// #### JO added (2026-10-27, systolic Phase D, docs/evta_systolic_design.md §5, §6): 16 x 16 weight-stationary array.
// PE(i, j): row i = input channel ic, column j = output channel oc, one DSP48E2 column per j (sa_pe).
//
// Input x: x_in (one inp_mem row, x[i] = bits 8i+7:8i) and the latch flag f_in arrive together in cycle a. Row i is delayed
//   i cycles (skew), then moves one fabric FF per column, so PE(i, j) sees it at its A pin in cycle a+i+j. The flag rides
//   with every byte and is CEB2 of the PE it reaches: B2 <= B1 exactly when the first x of the new weight block arrives.
// Weight fill: go (cycle G) = bank 0 data valid. Bank j data is valid in G+j (the controller staggers the bank reads).
//   PISO j loads it in G+j and puts w[j][15], w[j][14], ... w[j][0] on the B pin of PE(0, j) in G+j+1 .. G+j+16.
//   CEB1(i, j) is 1 in [G+1+i+j, G+16+j] (start: diagonal wave from PE(0,0), end: per-column pulse), so after the
//   window B1(i, j) = w[j][i]. A latch (flag) at PE(i, j) must come at or after G+17+i+j, and the next go at or after
//   the latch of the block before it (the controller keeps both rules).
// Output: column j's dot product leaves PE(15, j) in cycle a+18+j; column j is delayed 15-j cycles, so the whole row
//   (16 x 20-bit signed, |sum| <= 16 * 128 * 128) is on row_out in cycle a+33.
`timescale 1ns / 1ps
module sa_array #(
  parameter N = 16,
  parameter PW = 20               // dot product width kept after the column
) (
  input  wire              clk,
  input  wire              rst,
  input  wire [8*N-1:0]    x_in,
  input  wire              f_in,
  input  wire              go,
  input  wire [128*N-1:0]  wd,     // bank j = bits 128j+127:128j = w[j][0..15] (w[j][i] in bits 8i+7:8i of the bank)
  output wire [PW*N-1:0]   row_out
);
  // ---------------- input skew + horizontal chain (data and flag) ----------------
  reg  [7:0] xa [0:N-1][0:N-1];   // xa[i][j] = A pin of PE(i, j) (j >= 1: registers)
  reg        fa [0:N-1][0:N-1];
  wire [7:0] xs [0:N-1];          // skewed row i (A pin of PE(i, 0))
  wire       fs [0:N-1];
  genvar i, j, k;
  generate
    for (i = 0; i < N; i = i + 1) begin : skew
      if (i == 0) begin : r0
        assign xs[0] = x_in[7:0];
        assign fs[0] = f_in;
      end else begin : ri
        reg [7:0] sx [0:i-1];
        reg       sf [0:i-1];
        integer s;
        always @(posedge clk) begin
          sx[0] <= x_in[8*i+7:8*i];
          sf[0] <= rst ? 1'b0 : f_in;
          for (s = 1; s < i; s = s + 1) begin
            sx[s] <= sx[s-1];
            sf[s] <= rst ? 1'b0 : sf[s-1];
          end
        end
        assign xs[i] = sx[i-1];
        assign fs[i] = sf[i-1];
      end
      for (j = 1; j < N; j = j + 1) begin : hz
        always @(posedge clk) begin
          xa[i][j] <= (j == 1) ? xs[i] : xa[i][j-1];
          fa[i][j] <= rst ? 1'b0 : ((j == 1) ? fs[i] : fa[i][j-1]);
        end
      end
    end
  endgenerate

  // ---------------- weight fill: column go chain, PISOs, CEB1 waves ----------------
  reg  [N-1:1] gcr;
  wire [N-1:0] gc = {gcr, go};    // gc[j] = go delayed j (PISO j load)
  reg  [15:0]  ed;                // ed[k] = go delayed k+1
  reg  [N-1:0] ec;                // ec[j] = go delayed 16+j (end of the CEB1 window of column j)
  reg  [127:0] sr [0:N-1];        // PISO j, top byte on the B pin
  reg          sw [0:N-1][0:N-1]; // sw[i][j] = go delayed i+j (CEB1(i, j) turns on next cycle)
  reg          ce1 [0:N-1][0:N-1];
  integer q;
  always @(posedge clk) begin
    if (rst) begin
      gcr <= 0; ed <= 0; ec <= 0;
    end else begin
      for (q = 1; q < N; q = q + 1) gcr[q] <= gc[q-1];
      ed <= {ed[14:0], go};
      ec[0] <= ed[14];               // go delayed 16
      for (q = 1; q < N; q = q + 1) ec[q] <= ec[q-1];
    end
  end
  generate
    for (j = 0; j < N; j = j + 1) begin : piso
      always @(posedge clk)
        sr[j] <= gc[j] ? wd[128*j+127:128*j] : {sr[j][119:0], 8'd0};
    end
    for (i = 0; i < N; i = i + 1) begin : cew
      for (j = 0; j < N; j = j + 1) begin : cec
        always @(posedge clk) begin
          if (rst) begin
            sw[i][j] <= 1'b0; ce1[i][j] <= 1'b0;
          end else begin
            sw[i][j] <= (i == 0) ? gc[j] : sw[i-1][j];
            ce1[i][j] <= ((i == 0) ? gc[j] : sw[i-1][j]) | (ce1[i][j] & ~ec[j]);
          end
        end
      end
    end
  endgenerate
  // note: ce1 is set by the registered copy of the wave (its D input is the same signal sw is loaded from), so
  // CEB1(i, j) = 1 from G+1+i+j; it is cleared after cycle G+16+j (ec[j] high in that cycle).

  // ---------------- PEs ----------------
  wire [17:0] bc [0:N][0:N-1];
  wire [47:0] pc [0:N][0:N-1];
  wire [47:0] pp [0:N-1][0:N-1];
  generate
    for (i = 0; i < N; i = i + 1) begin : row
      for (j = 0; j < N; j = j + 1) begin : col
        sa_pe #(.ROW0(i == 0)) pe (
          .clk(clk),
          .a(j == 0 ? xs[i] : xa[i][j]),
          .ceb1(ce1[i][j]),
          .ceb2(j == 0 ? fs[i] : fa[i][j]),
          .b(i == 0 ? sr[j][127:120] : 8'd0),
          .bcin(i == 0 ? 18'd0 : bc[i][j]),
          .bcout(bc[i+1][j]),
          .pcin(i == 0 ? 48'd0 : pc[i][j]),
          .pcout(pc[i+1][j]),
          .p(pp[i][j])
        );
      end
    end
  endgenerate

  // ---------------- deskew ----------------
  generate
    for (j = 0; j < N; j = j + 1) begin : dsk
      if (j == N - 1) begin : d0
        assign row_out[PW*j+PW-1:PW*j] = pp[N-1][j][PW-1:0];
      end else begin : dn
        reg [PW-1:0] d [0:N-2-j];
        integer s;
        always @(posedge clk) begin
          d[0] <= pp[N-1][j][PW-1:0];
          for (s = 1; s <= N - 2 - j; s = s + 1) d[s] <= d[s-1];
        end
        assign row_out[PW*j+PW-1:PW*j] = d[N-2-j];
      end
    end
  endgenerate
endmodule
