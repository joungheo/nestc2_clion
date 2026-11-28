// #### JO added (2026-11-28, DSP packing experiment step 1): sa_array with sa_pe_pk, two output channels per column:
// column j computes oc 2j (low lane) and 2j+1 (high lane) from the same x.  Same skew, fill timing and flag rules as
// sa_array.v (see there); differences:
//   wd has 2N banks (bank k = w[k][0..15]); PISO j packs banks 2j / 2j+1 at load into 16 words w_hi * 2^19 + w_lo
//   (27-bit) and shifts them down the A cascade.  Weights must be in -127..127 (the low lane holds 19 signed bits).
//   Column bottom: lo = P[18:0], hi = P[38:19] + P[18] (the low lane's sign borrows one from the high lane), one
//   register, then the deskew.  row_out (2N x PW) is valid in cycle a+34 (one more than sa_array).
`timescale 1ns / 1ps
module sa_array_pk #(
  parameter N = 16,
  parameter PW = 20               // dot product width kept after the column
) (
  input  wire              clk,
  input  wire              rst,
  input  wire [8*N-1:0]    x_in,
  input  wire              f_in,
  input  wire              go,
  input  wire [256*N-1:0]  wd,     // bank k = bits 128k+127:128k = w[k][0..15] (w[k][i] in bits 8i+7:8i of the bank)
  output wire [2*PW*N-1:0] row_out
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
  reg  [431:0] sr [0:N-1];        // PISO j, 16 packed words, top word on the A pin
  reg          sw [0:N-1][0:N-1]; // sw[i][j] = go delayed i+j (CEB1(i, j) turns on next cycle)
  reg          ce1 [0:N-1][0:N-1];
  wire [431:0] pk [0:N-1];
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
    for (j = 0; j < N; j = j + 1) begin : pack
      for (k = 0; k < N; k = k + 1) begin : w
        wire signed [7:0] lo = wd[256*j+8*k+7:256*j+8*k];
        wire signed [7:0] hi = wd[256*j+128+8*k+7:256*j+128+8*k];
        assign pk[j][27*k+26:27*k] = {hi, 19'd0} + {{19{lo[7]}}, lo};
      end
    end
    for (j = 0; j < N; j = j + 1) begin : piso
      always @(posedge clk)
        sr[j] <= gc[j] ? pk[j] : {sr[j][404:0], 27'd0};
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
  wire [29:0] ac [0:N][0:N-1];
  wire [47:0] pc [0:N][0:N-1];
  wire [47:0] pp [0:N-1][0:N-1];
  generate
    for (i = 0; i < N; i = i + 1) begin : row
      for (j = 0; j < N; j = j + 1) begin : col
        sa_pe_pk #(.ROW0(i == 0)) pe (
          .clk(clk),
          .x(j == 0 ? xs[i] : xa[i][j]),
          .cea1(ce1[i][j]),
          .cea2(j == 0 ? fs[i] : fa[i][j]),
          .w(i == 0 ? sr[j][431:405] : 27'd0),
          .acin(i == 0 ? 30'd0 : ac[i][j]),
          .acout(ac[i+1][j]),
          .pcin(i == 0 ? 48'd0 : pc[i][j]),
          .pcout(pc[i+1][j]),
          .p(pp[i][j])
        );
      end
    end
  endgenerate

  // ---------------- lane split + deskew ----------------
  generate
    for (j = 0; j < N; j = j + 1) begin : dsk
      wire [47:0] pb = pp[N-1][j];
      reg  [2*PW-1:0] sp;           // {hi, lo}
      always @(posedge clk)
        sp <= {pb[38:19] + {19'd0, pb[18]}, {(PW-19){pb[18]}}, pb[18:0]};
      if (j == N - 1) begin : d0
        assign row_out[2*PW*j+PW-1:2*PW*j]          = sp[PW-1:0];
        assign row_out[2*PW*j+2*PW-1:2*PW*j+PW]     = sp[2*PW-1:PW];
      end else begin : dn
        reg [2*PW-1:0] d [0:N-2-j];
        integer s;
        always @(posedge clk) begin
          d[0] <= sp;
          for (s = 1; s <= N - 2 - j; s = s + 1) d[s] <= d[s-1];
        end
        assign row_out[2*PW*j+2*PW-1:2*PW*j] = d[N-2-j];
      end
    end
  endgenerate
endmodule
