// #### JO added (2026-10-27, systolic Phase D, docs/evta_systolic_design.md §7): accumulator read-modify-write.
// S0: row R + tag {dst, reset, last} arrive, acc[dst] read address presented.  S2: acc data D (L_ACC = 2).
//     old = the newest in-flight write to dst (S3 now, written 1 or 2 cycles ago) else D; sum = old + R (16 lanes int32).
// S3: acc[dst] <= reset ? 0 : sum, out[dst] <= low 8 bits of each lane of sum (as vta.cc gemm(), also on reset).
// The acc BRAM must read the old value when a read and a write hit the same address in one cycle (READ_FIRST); a write
// is visible to reads presented two cycles later, which is why three in-flight writes are compared.
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
  input  wire              tag_last,
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
  output wire              busy
);
  assign acc_rd_en   = tag_valid;
  assign acc_rd_addr = tag_dst;

  reg [PW*N-1:0] r1, r2;
  reg            v1, v2, v3, vw1, vw2;
  reg [DW-1:0]   d1, d2, d3, dw1, dw2;
  reg            z1, z2, z3, zw1, zw2;       // reset
  reg            l1, l2, l3;
  reg [32*N-1:0] s3, sw1, sw2;               // sums

  //#### JO changed (2026-10-27, Phase D4): the dst compares are done one stage early (S1, against the rows that will be in
  //#### S3 / written 1 / written 2 cycles ago when this row is in S2) and registered, so S2 is only the select and the add
  //#### (at 400 MHz OOC the compare -> 512-bit select -> add path was the worst one).
  //wire m3 = v3  && d3  == d2;
  //wire m4 = vw1 && dw1 == d2;
  //wire m5 = vw2 && dw2 == d2;
  (* max_fanout = 32 *) reg f3, f4, f5;   // S2 takes: S3's sum, the sum written 1 / 2 cycles ago, the BRAM data
  (* max_fanout = 32 *) reg f3z, f4z, f5z;   // that row was a reset row: the value is 0
  wire [32*N-1:0] old = f3 ? (f3z ? {32*N{1'b0}} : s3) :
                        f4 ? (f4z ? {32*N{1'b0}} : sw1) :
                        f5 ? (f5z ? {32*N{1'b0}} : sw2) : acc_rd_data;
  wire [32*N-1:0] sum;
  genvar k;
  generate
    for (k = 0; k < N; k = k + 1) begin : lane
      assign sum[32*k+31:32*k] = old[32*k+31:32*k] + {{(32-PW){r2[PW*k+PW-1]}}, r2[PW*k+PW-1:PW*k]};
      assign out_wr_data[8*k+7:8*k] = s3[32*k+7:32*k];
    end
  endgenerate

  always @(posedge clk) begin
    // compare in S1: when this row (d1) is in S2, S3 holds the row now in S2 (d2), written-1 the row now in S3 (d3),
    // written-2 the row written last cycle (dw1)
    f3 <= v2 && d2 == d1;   f3z <= z2;
    f4 <= v3 && d3 == d1;   f4z <= z3;
    f5 <= vw1 && dw1 == d1; f5z <= zw1;
    r1 <= row; d1 <= tag_dst; z1 <= tag_reset; l1 <= tag_last;
    r2 <= r1;  d2 <= d1;      z2 <= z1;        l2 <= l1;
    s3 <= sum; d3 <= d2;      z3 <= z2;        l3 <= l2;
    sw1 <= s3; dw1 <= d3;     zw1 <= z3;
    sw2 <= sw1; dw2 <= dw1;   zw2 <= zw1;
    if (rst) begin v1 <= 0; v2 <= 0; v3 <= 0; vw1 <= 0; vw2 <= 0; end
    else begin v1 <= tag_valid; v2 <= v1; v3 <= v2; vw1 <= v3; vw2 <= vw1; end
  end

  assign acc_wr_en   = v3;
  assign acc_wr_addr = d3;
  assign acc_wr_data = z3 ? {32*N{1'b0}} : s3;
  assign out_wr_en   = v3;
  assign out_wr_addr = d3;
  assign done        = v3 && l3;
  assign busy        = v1 || v2 || v3;
endmodule
