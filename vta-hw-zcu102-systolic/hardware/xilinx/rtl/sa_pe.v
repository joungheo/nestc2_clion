// #### JO added (2026-10-27, systolic Phase D, docs/evta_systolic_design.md §5): one PE of the weight-stationary array = one
// DSP48E2.  P = A2 * B2 + (ROW0 ? 0 : PCIN)
//   A: x (int8) from the fabric, AREG = 1 (A2).  B: weight (int8), BREG = 2: B1 = fill chain (CEB1, BCOUT from B1 with
//   BCASCREG = 1), B2 = weight in use (CEB2, INMODE[4] = 0).  MREG = 1, PREG = 1, partial sums down the column on PCIN/PCOUT.
// Latency: x at the A pin in cycle t -> P (with PCIN of the row above) visible in cycle t+3.
`timescale 1ns / 1ps
module sa_pe #(
  parameter ROW0 = 0              // 1: B from the B pin (column PISO), Z = 0; 0: B from BCIN, Z = PCIN
) (
  input  wire        clk,
  input  wire [7:0]  a,           // x[ic], signed
  input  wire        ceb1,        // B1 <= B (row 0) or BCIN
  input  wire        ceb2,        // B2 <= B1
  input  wire [7:0]  b,           // row 0 only: next weight byte from the PISO
  input  wire [17:0] bcin,
  output wire [17:0] bcout,
  input  wire [47:0] pcin,
  output wire [47:0] pcout,
  output wire [47:0] p
);
  DSP48E2 #(
    .A_INPUT("DIRECT"), .B_INPUT(ROW0 ? "DIRECT" : "CASCADE"),
    .AMULTSEL("A"), .BMULTSEL("B"), .PREADDINSEL("A"), .USE_MULT("MULTIPLY"), .USE_SIMD("ONE48"),
    .AREG(1), .ACASCREG(1), .BREG(2), .BCASCREG(1), .CREG(0), .DREG(0), .ADREG(0), .MREG(1), .PREG(1),
    .INMODEREG(0), .OPMODEREG(0), .ALUMODEREG(0), .CARRYINREG(0), .CARRYINSELREG(0),
    .USE_PATTERN_DETECT("NO_PATDET"), .AUTORESET_PATDET("NO_RESET")
  ) dsp (
    .CLK(clk),
    .A({{22{a[7]}}, a}), .B({{10{b[7]}}, b}), .C(48'd0), .D(27'd0), .CARRYIN(1'b0),
    .ACIN(30'd0), .BCIN(bcin), .PCIN(ROW0 ? 48'd0 : pcin), .CARRYCASCIN(1'b0), .MULTSIGNIN(1'b0),
    .ACOUT(), .BCOUT(bcout), .PCOUT(pcout), .CARRYCASCOUT(), .MULTSIGNOUT(),
    .P(p), .CARRYOUT(), .XOROUT(), .OVERFLOW(), .UNDERFLOW(), .PATTERNDETECT(), .PATTERNBDETECT(),
    // OPMODE {W[8:7], Z[6:4], Y[3:2], X[1:0]}: W = 0, Z = PCIN (001) or 0, Y = X = M (01)
    .OPMODE(ROW0 ? 9'b00_000_01_01 : 9'b00_001_01_01), .ALUMODE(4'b0000), .INMODE(5'b00000), .CARRYINSEL(3'b000),
    .CEA1(1'b0), .CEA2(1'b1), .CEB1(ceb1), .CEB2(ceb2), .CEC(1'b0), .CED(1'b0), .CEAD(1'b0), .CEM(1'b1), .CEP(1'b1),
    .CECTRL(1'b0), .CEINMODE(1'b0), .CEALUMODE(1'b0), .CECARRYIN(1'b0),
    .RSTA(1'b0), .RSTB(1'b0), .RSTC(1'b0), .RSTD(1'b0), .RSTM(1'b0), .RSTP(1'b0), .RSTCTRL(1'b0), .RSTINMODE(1'b0),
    .RSTALLCARRYIN(1'b0), .RSTALUMODE(1'b0)
  );
endmodule
