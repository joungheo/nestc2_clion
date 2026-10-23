// #### JO added (2026-10-23): BRAM self test for every RAMB36 site of the ZCU102 PL (xczu9eg, 912 sites).
// The vtahw EVTA bitstreams return wrong data from BRAMs in the upper half of the device (sramscan); this
// tests each RAMB36 on its own: build.tcl LOCs instance i to the i-th RAMB36 site (sites.txt).
// Per phase (4 patterns, incl. inverted and address-in-data) a common controller writes all 1024 x 36-bit
// words through port A, then reads them through port B (the EVTA access pattern: load writes A, compute/store
// read B); every BRAM compares against the broadcast expected value and keeps an error count and the OR of
// the flipped bits. Read the results over AXI-lite (brambist.py):
//   0x0000 CTRL  write bit0 = start, bit1 = loop (repeat the 4 phases until CTRL is written with bit1 = 0;
//                counts accumulate); read: bit0 busy, bit1 done, bit2 loop, [11:8] phase, [31:16] passes
//   0x000C EN_LO / 0x0010 EN_HI  only BRAMs EN_LO <= i < EN_HI are written/read/checked (default all)
//   0x0004 N     number of BRAMs (912)
//   0x0008 ID    0xB1A57002
//   0x4000 + 8*i  {flipped parity bits[3:0], 12'b0, error count[15:0] (saturating)}
//   0x4004 + 8*i  flipped data bits[31:0]
module brambist_axi #(parameter N = 912) (
  input  wire        aclk,
  input  wire        aresetn,
  input  wire [15:0] s_axi_awaddr,
  input  wire        s_axi_awvalid,
  output reg         s_axi_awready,
  input  wire [31:0] s_axi_wdata,
  input  wire [3:0]  s_axi_wstrb,
  input  wire        s_axi_wvalid,
  output reg         s_axi_wready,
  output wire [1:0]  s_axi_bresp,
  output reg         s_axi_bvalid,
  input  wire        s_axi_bready,
  input  wire [15:0] s_axi_araddr,
  input  wire        s_axi_arvalid,
  output reg         s_axi_arready,
  output reg  [31:0] s_axi_rdata,
  output wire [1:0]  s_axi_rresp,
  output reg         s_axi_rvalid,
  input  wire        s_axi_rready
);
  assign s_axi_bresp = 2'b00;
  assign s_axi_rresp = 2'b00;

  // ---------------- controller ----------------
  localparam S_IDLE = 2'd0, S_WR = 2'd1, S_RD = 2'd2, S_DONE = 2'd3;
  reg [1:0]  state;
  reg [1:0]  ph;
  reg [9:0]  addr;
  reg [1:0]  tail;
  reg        start, loop;
  reg [15:0] en_lo, en_hi, passes;
  reg        clr;
  reg        we, en_b, v1, v2, v3;
  reg [9:0]  a_q;
  reg [35:0] din, exp1, exp2, exp3;
  // E0: a_q/en_b/v1/exp1 set; E1: BRAM reads a_q; E2: dq <= DOUT; E3: compare dq with exp3 when v3

  function [35:0] pat(input [1:0] p, input [9:0] a);
    reg [35:0] base;
    begin
      base = {a, a, a, a[5:0]};
      case (p)
        2'd0: pat = base;
        2'd1: pat = ~base;
        2'd2: pat = 36'hA5A5A5A5A ^ {a[5:0], a, a, a};
        default: pat = ~(36'hA5A5A5A5A ^ {a[5:0], a, a, a});
      endcase
    end
  endfunction

  always @(posedge aclk) begin
    if (!aresetn) begin
      state <= S_IDLE; ph <= 0; addr <= 0; tail <= 0; clr <= 0; passes <= 0;
      we <= 0; en_b <= 0; v1 <= 0; v2 <= 0; v3 <= 0;
    end else begin
      clr <= 0; we <= 0; en_b <= 0;
      v3 <= v2; exp3 <= exp2; v2 <= v1; exp2 <= exp1; v1 <= 0;
      case (state)
        S_IDLE, S_DONE: if (start) begin state <= S_WR; ph <= 0; addr <= 0; clr <= 1; passes <= 0; end
        S_WR: begin
          we <= 1; a_q <= addr; din <= pat(ph, addr);
          addr <= addr + 1;
          if (addr == 10'd1023) begin state <= S_RD; tail <= 0; end
        end
        S_RD: begin
          if (tail == 0) begin
            en_b <= 1; a_q <= addr; v1 <= 1; exp1 <= pat(ph, addr);
            addr <= addr + 1;
            if (addr == 10'd1023) tail <= 1;
          end else if (tail == 2'd3) begin
            tail <= 0;
            if (ph == 2'd3) begin passes <= passes + 1; ph <= 0; state <= loop ? S_WR : S_DONE; end
            else begin ph <= ph + 1; state <= S_WR; end
          end else tail <= tail + 1;
        end
      endcase
    end
  end

  // ---------------- BRAMs under test ----------------
  wire [15:0] cnt  [0:N-1];
  wire [35:0] ebit [0:N-1];
  genvar i;
  generate
    for (i = 0; i < N; i = i + 1) begin : gen_bram
      wire [31:0] dob;
      wire [3:0]  dopb;
      reg  [35:0] dq;
      reg  [15:0] c;
      reg  [35:0] e;
      reg         on;
      always @(posedge aclk) on <= (i >= en_lo) && (i < en_hi);
      (* DONT_TOUCH = "TRUE" *)
      RAMB36E2 #(
        .READ_WIDTH_A(36), .WRITE_WIDTH_A(36), .READ_WIDTH_B(36), .WRITE_WIDTH_B(36),
        .DOA_REG(0), .DOB_REG(0), .WRITE_MODE_A("WRITE_FIRST"), .WRITE_MODE_B("WRITE_FIRST"),
        .CLOCK_DOMAINS("COMMON")
      ) u_ram (
        .CLKARDCLK(aclk), .CLKBWRCLK(aclk),
        .ADDRARDADDR({a_q, 5'b0}), .ADDRBWRADDR({a_q, 5'b0}), .ADDRENA(1'b1), .ADDRENB(1'b1),
        .ENARDEN(we & on), .ENBWREN(en_b & on), .WEA({4{we & on}}), .WEBWE(8'b0),
        .DINADIN(din[31:0]), .DINPADINP(din[35:32]), .DINBDIN(32'b0), .DINPBDINP(4'b0),
        .DOUTADOUT(), .DOUTPADOUTP(), .DOUTBDOUT(dob), .DOUTPBDOUTP(dopb),
        .REGCEAREGCE(1'b1), .REGCEB(1'b1),
        .RSTRAMARSTRAM(1'b0), .RSTRAMB(!aresetn), .RSTREGARSTREG(1'b0), .RSTREGB(1'b0),
        .SLEEP(1'b0), .ECCPIPECE(1'b0), .INJECTDBITERR(1'b0), .INJECTSBITERR(1'b0),
        .CASDIMUXA(1'b0), .CASDIMUXB(1'b0), .CASDINA(32'b0), .CASDINB(32'b0), .CASDINPA(4'b0), .CASDINPB(4'b0),
        .CASDOMUXA(1'b0), .CASDOMUXB(1'b0), .CASDOMUXEN_A(1'b1), .CASDOMUXEN_B(1'b1),
        .CASINDBITERR(1'b0), .CASINSBITERR(1'b0), .CASOREGIMUXA(1'b0), .CASOREGIMUXB(1'b0),
        .CASOREGIMUXEN_A(1'b1), .CASOREGIMUXEN_B(1'b1),
        .CASDOUTA(), .CASDOUTB(), .CASDOUTPA(), .CASDOUTPB(), .CASOUTDBITERR(), .CASOUTSBITERR(),
        .DBITERR(), .ECCPARITY(), .RDADDRECC(), .SBITERR()
      );
      always @(posedge aclk) begin
        dq <= {dopb, dob};
        if (clr) begin c <= 0; e <= 0; end
        else if (v3 && on && dq != exp3) begin
          if (c != 16'hFFFF) c <= c + 1;
          e <= e | (dq ^ exp3);
        end
      end
      assign cnt[i] = c;
      assign ebit[i] = e;
    end
  endgenerate

  // ---------------- AXI-lite ----------------
  reg [15:0] waddr;
  reg        aw_got, w_got;
  reg [31:0] wdat;
  always @(posedge aclk) begin
    if (!aresetn) begin
      s_axi_awready <= 0; s_axi_wready <= 0; s_axi_bvalid <= 0; aw_got <= 0; w_got <= 0; start <= 0;
      loop <= 0; en_lo <= 0; en_hi <= N;
    end else begin
      start <= 0;
      s_axi_awready <= !aw_got && s_axi_awvalid && !s_axi_awready;
      s_axi_wready  <= !w_got && s_axi_wvalid && !s_axi_wready;
      if (s_axi_awvalid && s_axi_awready) begin aw_got <= 1; waddr <= s_axi_awaddr; end
      if (s_axi_wvalid && s_axi_wready) begin w_got <= 1; wdat <= s_axi_wdata; end
      if (aw_got && w_got && !s_axi_bvalid) begin
        if (waddr[15:2] == 0) begin start <= wdat[0]; loop <= wdat[1]; end
        if (waddr[15:2] == 3) en_lo <= wdat[15:0];
        if (waddr[15:2] == 4) en_hi <= wdat[15:0];
        s_axi_bvalid <= 1; aw_got <= 0; w_got <= 0;
      end
      if (s_axi_bvalid && s_axi_bready) s_axi_bvalid <= 0;
    end
  end

  reg [15:0] raddr;
  reg        rpend;
  wire [13:0] ri = (raddr - 16'h4000) >> 3;
  always @(posedge aclk) begin
    if (!aresetn) begin
      s_axi_arready <= 0; s_axi_rvalid <= 0; rpend <= 0;
    end else begin
      s_axi_arready <= !rpend && !s_axi_rvalid && s_axi_arvalid && !s_axi_arready;
      if (s_axi_arvalid && s_axi_arready) begin raddr <= s_axi_araddr; rpend <= 1; end
      if (rpend) begin
        rpend <= 0; s_axi_rvalid <= 1;
        if (raddr[15:2] == 0) s_axi_rdata <= {passes, 4'b0, 2'b0, ph, 5'b0, loop, state == S_DONE, state == S_WR || state == S_RD};
        else if (raddr[15:2] == 1) s_axi_rdata <= N;
        else if (raddr[15:2] == 2) s_axi_rdata <= 32'hB1A57002;
        else if (raddr[15:2] == 3) s_axi_rdata <= en_lo;
        else if (raddr[15:2] == 4) s_axi_rdata <= en_hi;
        else if (raddr >= 16'h4000 && ri < N)
          s_axi_rdata <= raddr[2] ? ebit[ri][31:0] : {ebit[ri][35:32], 12'b0, cnt[ri]};
        else s_axi_rdata <= 32'hDEADBEEF;
      end
      if (s_axi_rvalid && s_axi_rready) s_axi_rvalid <= 0;
    end
  end
endmodule
