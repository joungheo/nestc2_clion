// #### JO added (2026-11-28, DSP packing experiment): sa_array (PK = 0), sa_array_pk (PK = 1, two output channels per DSP)
// or sa_array_px (PK = 2, two pixels per DSP: x in -127..127) alone against a reference.
// NB weight blocks of M rows each, back to back at the fill rules of sa_array.v (go at G, first latch at G+17, next go
// at the latch before); random x in -128..127, weights in -127..127.  The latency is found on row 0 and then every row
// of every block is compared.  usage: rtl/tb/run_sa_array.sh
`timescale 1ns / 1ps
module tb_sa_array;
  parameter PK = 0;
  parameter SEED = 1;
  localparam N = 16, PW = 20, NO = PK ? 2 * N : N, NW = (PK == 1) ? 2 * N : N, NX = (PK == 2) ? 2 : 1, NB = 6, M = 24, R = NB * M, T = 100 + 17 * NB + R + 60;
  reg clk = 0;
  always #1 clk = ~clk;
  reg              rst = 1;
  reg [8*N*NX-1:0] x_in = 0;
  reg              f_in = 0, go = 0;
  reg [128*NW-1:0] wd = 0;
  wire [PW*NO-1:0] row_out;
  generate
    if (PK == 2) begin : g_px
      sa_array_px #(.N(N), .PW(PW)) dut(.clk(clk), .rst(rst), .x_in(x_in), .f_in(f_in), .go(go), .wd(wd), .row_out(row_out));
    end else if (PK) begin : g_pk
      sa_array_pk #(.N(N), .PW(PW)) dut(.clk(clk), .rst(rst), .x_in(x_in), .f_in(f_in), .go(go), .wd(wd), .row_out(row_out));
    end else begin : g_sa
      sa_array #(.N(N), .PW(PW)) dut(.clk(clk), .rst(rst), .x_in(x_in), .f_in(f_in), .go(go), .wd(wd), .row_out(row_out));
    end
  endgenerate

  integer W [0:NB-1][0:NW-1][0:N-1];
  integer X [0:R-1][0:NX*N-1];
  integer E [0:R-1][0:NO-1];
  integer G [0:NB-1], A [0:NB-1];
  reg [PW*NO-1:0] H [0:T-1];
  integer b, o, i, r, c, s, lat, bad, seed;

  function integer rnd(input integer lo, input integer hi);
    rnd = lo + ($unsigned($random(seed)) % (hi - lo + 1));
  endfunction

  initial begin
    seed = SEED;
    for (b = 0; b < NB; b = b + 1)
      for (o = 0; o < NW; o = o + 1)
        for (i = 0; i < N; i = i + 1)
          W[b][o][i] = (b == 0 && o < 4) ? ((o == 0 || o == 3) ? -127 : 127) : rnd(-127, 127);   // block 0: both lane signs at max
    for (r = 0; r < R; r = r + 1)
      for (i = 0; i < NX * N; i = i + 1)
        if (PK == 2) X[r][i] = (r < 2) ? ((r == 0) ? ((i < N) ? -127 : 127) : ((i % 3) ? 127 : -127)) : rnd(-127, 127);
        else         X[r][i] = (r < 2) ? ((r == 0) ? -128 : ((i % 2) ? 127 : -128)) : rnd(-128, 127);  // rows 0, 1: extreme x
    for (r = 0; r < R; r = r + 1)
      for (o = 0; o < NO; o = o + 1) begin
        s = 0;
        for (i = 0; i < N; i = i + 1)
          s = s + ((PK == 2) ? X[r][(o / N) * N + i] * W[r / M][o % N][i] : X[r][i] * W[r / M][o][i]);
        E[r][o] = s;
      end
    G[0] = 70;                               // after glbl GSR (100 ns = 50 cycles)
    for (b = 0; b < NB; b = b + 1) begin
      if (b > 0) G[b] = A[b-1];
      A[b] = G[b] + 17;
      if (b > 0 && A[b] < A[b-1] + M) A[b] = A[b-1] + M;
    end

    for (c = 0; c < T; c = c + 1) begin
      @(negedge clk);
      if (c > 0) H[c-1] = row_out;          // value after the posedge of cycle c-1
      rst = (c < 4);
      go = 0; f_in = 0; x_in = 0;
      for (b = 0; b < NB; b = b + 1) begin
        if (c == G[b]) begin
          go = 1;
          for (o = 0; o < NW; o = o + 1)
            for (i = 0; i < N; i = i + 1) wd[128*o+8*i +: 8] = W[b][o][i];
        end
        if (c >= A[b] && c < A[b] + M) begin
          r = b * M + (c - A[b]);
          f_in = (c == A[b]);
          for (i = 0; i < NX * N; i = i + 1) x_in[8*i +: 8] = X[r][i];
        end
      end
    end

    lat = -1;
    for (c = 20; c < 50 && lat < 0; c = c + 1) begin
      bad = 0;
      for (o = 0; o < NO; o = o + 1) if ($signed(H[A[0] + c][PW*o +: PW]) != E[0][o]) bad = 1;
      if (!bad) lat = c;
    end
    if (lat < 0) begin
      $display("RESULT FAIL PK=%0d: row 0 never matched", PK);
      for (o = 0; o < 4; o = o + 1) $display("  exp[%0d] %0d  got@33 %0d @34 %0d", o, E[0][o],
        $signed(H[A[0] + 33][PW*o +: PW]), $signed(H[A[0] + 34][PW*o +: PW]));
      $finish;
    end
    bad = 0;
    for (b = 0; b < NB; b = b + 1)
      for (r = 0; r < M; r = r + 1)
        for (o = 0; o < NO; o = o + 1)
          if ($signed(H[A[b] + r + lat][PW*o +: PW]) != E[b*M + r][o]) begin
            if (bad < 8) $display("  block %0d row %0d oc %0d: exp %0d got %0d", b, r, o, E[b*M + r][o],
              $signed(H[A[b] + r + lat][PW*o +: PW]));
            bad = bad + 1;
          end
    $display("RESULT %s PK=%0d SEED=%0d latency %0d, %0d blocks x %0d rows x %0d oc, %0d mismatches",
      bad ? "FAIL" : "PASS", PK, SEED, lat, NB, M, NO, bad);
    $finish;
  end
endmodule
