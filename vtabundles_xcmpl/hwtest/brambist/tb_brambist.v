`timescale 1ns/1ps
module tb;
  reg clk = 0, rstn = 0;
  always #5 clk = ~clk;
  reg [15:0] awaddr = 0, araddr = 0; reg awvalid = 0, wvalid = 0, bready = 1, arvalid = 0, rready = 1;
  reg [31:0] wdata = 0;
  wire awready, wready, bvalid, arready, rvalid; wire [1:0] bresp, rresp; wire [31:0] rdata;
  brambist_axi #(.N(4)) dut (.aclk(clk), .aresetn(rstn),
    .s_axi_awaddr(awaddr), .s_axi_awvalid(awvalid), .s_axi_awready(awready), .s_axi_wdata(wdata), .s_axi_wstrb(4'hf),
    .s_axi_wvalid(wvalid), .s_axi_wready(wready), .s_axi_bresp(bresp), .s_axi_bvalid(bvalid), .s_axi_bready(bready),
    .s_axi_araddr(araddr), .s_axi_arvalid(arvalid), .s_axi_arready(arready), .s_axi_rdata(rdata), .s_axi_rresp(rresp),
    .s_axi_rvalid(rvalid), .s_axi_rready(rready));
  task wr(input [15:0] a, input [31:0] d); begin
    @(posedge clk); awaddr <= a; awvalid <= 1; wdata <= d; wvalid <= 1;
    wait (awready); @(posedge clk); awvalid <= 0; wait (wready); @(posedge clk); wvalid <= 0; wait (bvalid); @(posedge clk); end endtask
  task rd(input [15:0] a, output [31:0] d); begin
    @(posedge clk); araddr <= a; arvalid <= 1; wait (arready); @(posedge clk); arvalid <= 0; wait (rvalid); d = rdata; @(posedge clk); end endtask
  reg [31:0] v; integer i, t;
  task runbist(input integer inj); begin
    wr(16'h0, 1);
    t = 0;
    if (inj) begin
      // flip data bit 3 of BRAM 2 while phase 1 reads
      wait (dut.ph == 1 && dut.state == 2); force dut.gen_bram[2].dob[3] = 1'b1; #200; release dut.gen_bram[2].dob[3];
    end
    rd(16'h0, v); while (!v[1]) begin rd(16'h0, v); t = t + 1; end
    $display("ctrl=0x%h after %0d polls", v, t);
    for (i = 0; i < 4; i = i + 1) begin
      rd(16'h4000 + 8*i, v); $write("bram %0d: cnt=%0d par=%h ", i, v[15:0], v[31:28]); rd(16'h4004 + 8*i, v); $display("bits=%h", v);
    end
  end endtask
  initial begin
    #100 rstn = 1; #100;
    rd(16'h8, v); $display("ID=%h", v); rd(16'h4, v); $display("N=%0d", v);
    runbist(0);
    runbist(1);
    // v2: only BRAM 1..2, BRAM 0 and 2 get a stuck bit -> only BRAM 2 may count
    wr(16'hC, 1); wr(16'h10, 3);
    wr(16'h0, 1);
    wait (dut.ph == 1 && dut.state == 2); force dut.gen_bram[2].dob[3] = 1'b1; force dut.gen_bram[0].dob[5] = 1'b1; #200; release dut.gen_bram[2].dob[3]; release dut.gen_bram[0].dob[5];
    rd(16'h0, v); while (!v[1]) rd(16'h0, v);
    for (i = 0; i < 4; i = i + 1) begin rd(16'h4000 + 8*i, v); $display("range bram %0d: cnt=%0d", i, v[15:0]); end
    // loop mode: 3 passes then stop
    wr(16'hC, 0); wr(16'h10, 4); wr(16'h0, 3);
    wait (dut.passes == 3); wr(16'h0, 0);
    rd(16'h0, v); while (!v[1]) rd(16'h0, v);
    $display("loop: ctrl=%h passes=%0d", v, v[31:16]);
    $display("SIMDONE"); $finish;
  end
endmodule
