// #### JO added (2026-11-28, DSP packing experiment step 1): one PE with two int8 products per DSP48E2 (INT8 packing,
// Xilinx WP486 style, both products share the input x).  P = A2 * B2 + (ROW0 ? 0 : PCIN)
//   A: packed weight pair w_hi * 2^19 + w_lo (27-bit signed, made once at the column top), AREG = 2: A1 = fill chain
//      (CEA1, ACOUT from A1 with ACASCREG = 1), A2 = pair in use (CEA2, INMODE[0] = 0).
//   B: x (int8) from the fabric, BREG = 1 (B2).  MREG = 1, PREG = 1, partial sums down the column on PCIN/PCOUT.
//   The lanes are 19 bits apart: a column sum of 16 products with |w| <= 127 fits 19 signed bits (|sum| <= 260096),
//   so the 16-deep cascade needs no cut; the column bottom splits P (sa_array_pk.v).
// Same latency and pins as sa_pe (x at the B pin in cycle t -> P in cycle t+3); only the weight path moved to A.
`timescale 1ns / 1ps
module sa_pe_pk #(
  parameter ROW0 = 0              // 1: A from the A pin (column PISO), Z = 0; 0: A from ACIN, Z = PCIN
) (
  input  wire        clk,
  input  wire [7:0]  x,           // x[ic], signed
  input  wire        cea1,        // A1 <= A (row 0) or ACIN
  input  wire        cea2,        // A2 <= A1
  input  wire [26:0] w,           // row 0 only: next packed weight pair from the PISO
  input  wire [29:0] acin,
  output wire [29:0] acout,
  input  wire [47:0] pcin,
  output wire [47:0] pcout,
  output wire [47:0] p
);
  DSP48E2 #(
    .A_INPUT(ROW0 ? "DIRECT" : "CASCADE"), .B_INPUT("DIRECT"),
    .AMULTSEL("A"), .BMULTSEL("B"), .PREADDINSEL("A"), .USE_MULT("MULTIPLY"), .USE_SIMD("ONE48"),
    .AREG(2), .ACASCREG(1), .BREG(1), .BCASCREG(1), .CREG(0), .DREG(0), .ADREG(0), .MREG(1), .PREG(1),
    .INMODEREG(0), .OPMODEREG(0), .ALUMODEREG(0), .CARRYINREG(0), .CARRYINSELREG(0),
    .USE_PATTERN_DETECT("NO_PATDET"), .AUTORESET_PATDET("NO_RESET")
  ) dsp (
    .CLK(clk),
    .A({{3{w[26]}}, w}), .B({{10{x[7]}}, x}), .C(48'd0), .D(27'd0), .CARRYIN(1'b0),
    .ACIN(acin), .BCIN(18'd0), .PCIN(ROW0 ? 48'd0 : pcin), .CARRYCASCIN(1'b0), .MULTSIGNIN(1'b0),
    .ACOUT(acout), .BCOUT(), .PCOUT(pcout), .CARRYCASCOUT(), .MULTSIGNOUT(),
    .P(p), .CARRYOUT(), .XOROUT(), .OVERFLOW(), .UNDERFLOW(), .PATTERNDETECT(), .PATTERNBDETECT(),
    .OPMODE(ROW0 ? 9'b00_000_01_01 : 9'b00_001_01_01), .ALUMODE(4'b0000), .INMODE(5'b00000), .CARRYINSEL(3'b000),
    .CEA1(cea1), .CEA2(cea2), .CEB1(1'b0), .CEB2(1'b1), .CEC(1'b0), .CED(1'b0), .CEAD(1'b0), .CEM(1'b1), .CEP(1'b1),
    .CECTRL(1'b0), .CEINMODE(1'b0), .CEALUMODE(1'b0), .CECARRYIN(1'b0),
    .RSTA(1'b0), .RSTB(1'b0), .RSTC(1'b0), .RSTD(1'b0), .RSTM(1'b0), .RSTP(1'b0), .RSTCTRL(1'b0), .RSTINMODE(1'b0),
    .RSTALLCARRYIN(1'b0), .RSTALUMODE(1'b0)
  );
endmodule
