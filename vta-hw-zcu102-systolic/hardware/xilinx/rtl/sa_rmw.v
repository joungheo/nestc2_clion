// #### JO added (2026-10-27, systolic Phase D, docs/evta_systolic_design.md §7): accumulator read-modify-write.
// S0: row R + tag {dst, reset, last} arrive, acc[dst] read address presented.  S2: acc data D (L_ACC = 2).
//     old = the newest in-flight write to dst (S3 now, written 1 or 2 cycles ago) else D; sum = old + R (16 lanes int32).
// S3: acc[dst] <= reset ? 0 : sum, out[dst] <= low 8 bits of each lane of sum (as vta.cc gemm(), also on reset).
// The acc BRAM must read the old value when a read and a write hit the same address in one cycle (READ_FIRST); a write
// is visible to reads presented two cycles later, which is why three in-flight writes are compared.
// #### JO changed (2026-11-04, bias reset): a row with tag_bias (a reset uop of a GEMM instruction with bias_reset) reads
// acc[tag_src] instead of acc[dst] and writes that value to acc[dst] (the row from the array is not added): the conv bias,
// LOADed into acc before the reset GEMM, becomes the start of the accumulation and the ALU ADD of the bias goes away.
// The forwarding compares are against the address read (src for a bias row), so a bias entry still in flight is forwarded.
// #### JO changed (2026-11-04, requantize): the out write is no longer the low 8 bits of the sum in S3 but, with rq_en of
// the instruction (tag_rq = {hi, lo, shift, round, en}, VTAGemInsn rq_*), clip((sum + round) >> shift, lo, hi), in three
// more stages: S4 sum + round (round 1: 2^(shift-1), 2: 2^(shift-1) - (sum < 0)), S5 arithmetic shift, S6 clip; out is
// written in S6 (without rq_en the low 8 bits, also in S6). acc is written in S3 as before (the forwarding is unchanged);
// done / tokens / busy come with the out write in S6, so nothing reads out (store) or uses the ports before it.
// #### JO changed (2026-11-14, 400 MHz: acc_mem output register -> 4:1 select -> 32-bit add -> s3 was the worst GEMM path,
// -0.050 ns): one more stage (the version before: git e1a7924). S2 registers the BRAM data already merged with the older
// in-flight writes (p3 = the sum of the row 2 ahead (S4), 3 ahead (written 1 cycle ago) or 4 ahead (2 ago), else the BRAM
// data), S3 takes the row 1 ahead (S4 now) or p3 and adds R, S4 writes acc. A read presented in S0 misses the writes of the
// 4 rows ahead (the 4th writes on the same clock edge: READ_FIRST), so there are 4 forwarding sources instead of 3, all
// compares in S1 / S2. rq / out stages S5..S7; done / tokens / busy with the out write in S7. No change for the programs
// (forwarded: any dst order is right); the instruction ends one cycle later.
`timescale 1ns / 1ps
module sa_rmw #(
  parameter N = 16,
  parameter PW = 20,
  parameter DW = 12
) (
  input  wire              clk,
  input  wire              rst,
  input  wire [PW*N-1:0]   row,
  input  wire              tag_valid,
  input  wire [DW-1:0]     tag_dst,
  input  wire              tag_reset,
  input  wire              tag_bias,     // #### JO added (2026-11-04): reset row that takes acc[tag_src] (with tag_reset)
  input  wire [DW-1:0]     tag_src,
  input  wire [23:0]       tag_rq,       // #### JO added (2026-11-04): {hi, lo, shift, round, en}
  input  wire              tag_last,
  input  wire [1:0]        tag_tok,
  // acc_mem: read port (address cycle -> data two cycles later) and write port
  output wire              acc_rd_en,
  output wire [DW-1:0]     acc_rd_addr,
  input  wire [32*N-1:0]   acc_rd_data,
  output wire              acc_wr_en,
  output wire [DW-1:0]     acc_wr_addr,
  output wire [32*N-1:0]   acc_wr_data,
  // out_mem write port
  output wire              out_wr_en,
  output wire [DW-1:0]     out_wr_addr,
  output wire [8*N-1:0]    out_wr_data,
  output wire              done,         // pulse: the last uop of a GEMM instruction is written
  output wire [1:0]        done_tok,     // its {g2s, g2l} pushes, with done
  output wire              busy
);
  assign acc_rd_en   = tag_valid;
  // read address = src for a bias row
  wire [DW-1:0] ra = tag_bias ? tag_src : tag_dst;
  assign acc_rd_addr = ra;

  // row pipeline: S1 r1.., S2 r2.., S3 r3.. (add), S4 s4 (acc write), W1 / W2 the sums written 1 / 2 cycles ago
  reg [PW*N-1:0] r1, r2, r3;
  reg            v1, v2, v3, v4, vw1;
  reg [DW-1:0]   d1, d2, d3, d4, dw1;
  reg [DW-1:0]   a1, a2;                     // address read (dst, or src for a bias row)
  reg            z1, z2, z3, z4, zw1, zw2;   // zero write (reset row that is not a bias row)
  reg            l1, l2, l3, l4;
  reg [1:0]      k1, k2, k3, k4;             // tokens
  reg [32*N-1:0] p3, s4, sw1, sw2;
  // rq with the row (q1..q4), the out stages S5..S7
  reg [23:0]     q1, q2, q3, q4;
  reg            v5, v6, v7, l5, l6, l7, e5, e6;
  reg [DW-1:0]   d5, d6, d7;
  reg [1:0]      k5, k6, k7;
  (* max_fanout = 64 *) reg [4:0] sh5;
  reg [7:0]      lo5, lo6, hi5, hi6;
  reg [31:0]     hb4, hm4;                   // 2^(shift-1) (0 without round / shift 0), and minus 1 for round 2
  reg            m4;                         // round 2: a negative sum takes hm4
  reg [32*N-1:0] t5, h6;
  reg [8*N-1:0]  o7;

  // forwarding flags. S1 (for p3 in S2): the rows 2 / 3 / 4 ahead are now in S3 / S4 / W1. S2 (for the add in S3): the row
  // 1 ahead is now in S3. g*z / f1z: that row writes 0.
  (* max_fanout = 32 *) reg g2, g3, g4, g2z, g3z, g4z;
  (* max_fanout = 32 *) reg f1, f1z;
  wire [32*N-1:0] pre = g2 ? (g2z ? {32*N{1'b0}} : s4) :
                        g3 ? (g3z ? {32*N{1'b0}} : sw1) :
                        g4 ? (g4z ? {32*N{1'b0}} : sw2) : acc_rd_data;
  wire [32*N-1:0] old = f1 ? (f1z ? {32*N{1'b0}} : s4) : p3;
  wire [32*N-1:0] sum;
  genvar k;
  generate
    for (k = 0; k < N; k = k + 1) begin : lane
      assign sum[32*k+31:32*k] = old[32*k+31:32*k] + {{(32-PW){r3[PW*k+PW-1]}}, r3[PW*k+PW-1:PW*k]};
      assign out_wr_data[8*k+7:8*k] = o7[8*k+7:8*k];
      wire [31:0] sk = s4[32*k+31:32*k];
      wire [31:0] tk = t5[32*k+31:32*k];
      wire signed [31:0] hk = h6[32*k+31:32*k];
      always @(posedge clk) begin
        t5[32*k+31:32*k] <= sk + ((m4 && sk[31]) ? hm4 : hb4);
        h6[32*k+31:32*k] <= $signed(tk) >>> sh5;
        o7[8*k+7:8*k]    <= !e6 ? hk[7:0] :
                            hk < $signed({{24{lo6[7]}}, lo6}) ? lo6 :
                            hk > $signed({{24{hi6[7]}}, hi6}) ? hi6 : hk[7:0];
      end
    end
  endgenerate

  // rq fields: en q[0], round q[2:1], shift q[7:3], lo q[15:8], hi q[23:16]
  wire       qe = q3[0];
  wire [1:0] qr = q3[2:1];
  wire [4:0] qs = q3[7:3];
  always @(posedge clk) begin
    q1 <= tag_rq; q2 <= q1; q3 <= q2; q4 <= q3;
    // S4 (with s4): the round constants of the row now in S3
    hb4 <= (qe && qr != 2'd0 && qs != 5'd0) ? (32'd1 << (qs - 5'd1)) : 32'd0;
    hm4 <= (qe && qr != 2'd0 && qs != 5'd0) ? (32'd1 << (qs - 5'd1)) - 32'd1 : 32'd0;
    m4  <= qe && qr == 2'd2 && qs != 5'd0;
    // S5
    sh5 <= q4[0] ? q4[7:3] : 5'd0; e5 <= q4[0]; lo5 <= q4[15:8]; hi5 <= q4[23:16];
    d5 <= d4; l5 <= l4; k5 <= k4;
    // S6
    e6 <= e5; lo6 <= lo5; hi6 <= hi5; d6 <= d5; l6 <= l5; k6 <= k5;
    // S7
    d7 <= d6; l7 <= l6; k7 <= k6;
    if (rst) begin v5 <= 0; v6 <= 0; v7 <= 0; end
    else begin v5 <= v4; v6 <= v5; v7 <= v6; end
  end

  always @(posedge clk) begin
    g2 <= v3  && d3  == a1;   g2z <= z3;
    g3 <= v4  && d4  == a1;   g3z <= z4;
    g4 <= vw1 && dw1 == a1;   g4z <= zw1;
    f1 <= v3  && d3  == a2;   f1z <= z3;
    p3 <= pre;
    r1 <= tag_bias ? {PW*N{1'b0}} : row; d1 <= tag_dst; a1 <= ra; z1 <= tag_reset && !tag_bias; l1 <= tag_last; k1 <= tag_tok;
    r2 <= r1;  d2 <= d1;  a2 <= a1;  z2 <= z1;  l2 <= l1;  k2 <= k1;
    r3 <= r2;  d3 <= d2;             z3 <= z2;  l3 <= l2;  k3 <= k2;
    s4 <= sum; d4 <= d3;             z4 <= z3;  l4 <= l3;  k4 <= k3;
    sw1 <= s4; dw1 <= d4;            zw1 <= z4;
    sw2 <= sw1;                      zw2 <= zw1;
    if (rst) begin v1 <= 0; v2 <= 0; v3 <= 0; v4 <= 0; vw1 <= 0; end
    else begin v1 <= tag_valid; v2 <= v1; v3 <= v2; v4 <= v3; vw1 <= v4; end
  end

  assign acc_wr_en   = v4;
  assign acc_wr_addr = d4;
  assign acc_wr_data = z4 ? {32*N{1'b0}} : s4;
  assign out_wr_en   = v7;
  assign out_wr_addr = d7;
  assign done        = v7 && l7;
  assign done_tok    = (v7 && l7) ? k7 : 2'b00;
  assign busy        = v1 || v2 || v3 || v4 || v5 || v6 || v7;
endmodule
