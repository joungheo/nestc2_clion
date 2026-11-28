// #### JO added (2026-11-28, DSP packing experiment, pixel pairs): sa_array with sa_pe_px: every PE multiplies its
// weight with two pixels (x_in = {pixel 1, pixel 0}, 8N bits each).  Row i packs once at the input:
// hp = x1[i] - (x0[i] < 0), lo = x0[i]; {hp, lo} (16 bits) goes through the skew and the horizontal chain.
// Weights, fill and flag rules as sa_array.v.  Column bottom: pixel 0 = P[18:0], pixel 1 = P[38:19] + P[18], one register,
// then the deskew.  row_out = {pixel 1 row, pixel 0 row} (N x PW each), valid in cycle a+34.
`timescale 1ns / 1ps
module sa_array_px #(
  parameter N = 16,
  parameter PW = 20               // dot product width kept after the column
) (
  input  wire              clk,
  input  wire              rst,
  input  wire [16*N-1:0]   x_in,
  input  wire              f_in,
  input  wire              go,
  input  wire [128*N-1:0]  wd,     // bank j = bits 128j+127:128j = w[j][0..15] (w[j][i] in bits 8i+7:8i of the bank)
  output wire [2*PW*N-1:0] row_out
);
  // ---------------- input skew + horizontal chain (data and flag) ----------------
  reg  [15:0] xa [0:N-1][0:N-1];   // xa[i][j] = A pin of PE(i, j) (j >= 1: registers)
  reg        fa [0:N-1][0:N-1];
  wire [15:0] xs [0:N-1];          // skewed row i (A pin of PE(i, 0))
  wire       fs [0:N-1];
  genvar i, j, k;
  wire [15:0] xp [0:N-1];          // packed pair of row i
  generate
    for (i = 0; i < N; i = i + 1) begin : xpk
      wire [7:0] lo = x_in[8*i+7:8*i];
      wire [7:0] hi = x_in[8*N+8*i+7:8*N+8*i];
      assign xp[i] = {hi - {7'd0, lo[7]}, lo};
    end
  endgenerate
  generate
    for (i = 0; i < N; i = i + 1) begin : skew
      if (i == 0) begin : r0
        assign xs[0] = xp[0];
        assign fs[0] = f_in;
      end else begin : ri
        reg [15:0] sx [0:i-1];
        reg       sf [0:i-1];
        integer s;
        always @(posedge clk) begin
          sx[0] <= xp[i];
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
        sa_pe_px #(.ROW0(i == 0)) pe (
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

  // ---------------- lane split + deskew ----------------
  generate
    for (j = 0; j < N; j = j + 1) begin : dsk
      wire [47:0] pb = pp[N-1][j];
      reg  [2*PW-1:0] sp;           // {pixel 1, pixel 0}
      always @(posedge clk)
        sp <= {pb[38:19] + {19'd0, pb[18]}, {(PW-19){pb[18]}}, pb[18:0]};
      wire [2*PW-1:0] q;
      if (j == N - 1) begin : d0
        assign q = sp;
      end else begin : dn
        reg [2*PW-1:0] d [0:N-2-j];
        integer s;
        always @(posedge clk) begin
          d[0] <= sp;
          for (s = 1; s <= N - 2 - j; s = s + 1) d[s] <= d[s-1];
        end
        assign q = d[N-2-j];
      end
      assign row_out[PW*j+PW-1:PW*j]           = q[PW-1:0];
      assign row_out[PW*N+PW*j+PW-1:PW*N+PW*j] = q[2*PW-1:PW];
    end
  endgenerate
endmodule
