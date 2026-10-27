// #### JO added (2026-10-27, systolic Phase D2/D3): gemm_sa against the vectors of sim/systolic_model.cc (dump mode).
// plusargs: +dir=<vector dir>  +gap=<0..99, % of cycles the uop stream is idle>  +bp=<0..99, % of cycles done is not taken>
// Prints "RESULT <dir>: PASS|FAIL acc_wrong out_wrong cycles uops".
`timescale 1ns / 1ps
module tb_gemm_sa;
  localparam N = 16, DW = 12, SW = 12, WW = 11, L_RD = 2;
  localparam NI = 256, NW = 64, NA = 256;

  reg clk = 0, rstn = 0;
  always #1.25 clk = ~clk;

  reg  [DW+SW+WW+4:0] uop_d;
  wire                g2l_v, g2s_v;
  integer             ng2l, ng2s, eg2l, eg2s;
  reg                 uop_v;
  wire                uop_r;
  wire [7:0]          done_d;
  wire                done_v;
  reg                 done_r;
  wire                inp_en;
  wire [SW-1:0]       inp_addr;
  wire [N-1:0]        wgt_en;
  wire [WW*N-1:0]     wgt_addr;
  wire                acc_rd_en, acc_wr_en, out_wr_en, busy;
  wire [DW-1:0]       acc_rd_addr, acc_wr_addr, out_wr_addr;
  wire [32*N-1:0]     acc_wr_data;
  wire [8*N-1:0]      out_wr_data;

  // ---------------- memories ----------------
  reg [127:0] inp_mem [0:NI-1];
  reg [127:0] wgt_all [0:NW*N-1];
  reg [127:0] wgt_bank [0:N-1][0:NW-1];
  reg [511:0] acc_mem [0:NA-1];
  reg [127:0] out_mem [0:NA-1];
  reg [511:0] acc_exp [0:NA-1];
  reg [127:0] out_exp [0:NA-1];
  reg [39:0]  uops [0:65535];

  reg [127:0]  inp_p [0:L_RD-1];
  reg [127:0]  wgt_p [0:N-1][0:L_RD-1];
  reg [511:0]  acc_p0, acc_p1;
  integer      j, s;
  always @(posedge clk) begin
    inp_p[0] <= inp_en ? inp_mem[inp_addr] : 128'bx;
    for (s = 1; s < L_RD; s = s + 1) inp_p[s] <= inp_p[s-1];
    for (j = 0; j < N; j = j + 1) begin
      wgt_p[j][0] <= wgt_en[j] ? wgt_bank[j][wgt_addr[WW*j +: WW]] : 128'bx;
      for (s = 1; s < L_RD; s = s + 1) wgt_p[j][s] <= wgt_p[j][s-1];
    end
    acc_p0 <= acc_rd_en ? acc_mem[acc_rd_addr] : 512'bx;     // READ_FIRST: old value on a same-cycle write
    acc_p1 <= acc_p0;
    if (acc_wr_en) acc_mem[acc_wr_addr] <= acc_wr_data;
    if (out_wr_en) out_mem[out_wr_addr] <= out_wr_data;
  end
  wire [128*N-1:0] wgt_data;
  genvar g;
  generate for (g = 0; g < N; g = g + 1) begin : wd
    assign wgt_data[128*g +: 128] = wgt_p[g][L_RD-1];
  end endgenerate

  gemm_sa #(.N(N), .DW(DW), .SW(SW), .WW(WW), .L_RD(L_RD)) dut (
    .clk(clk), .rstn(rstn),
    .s_uop_tdata(uop_d), .s_uop_tvalid(uop_v), .s_uop_tready(uop_r),
    .m_done_tdata(done_d), .m_done_tvalid(done_v), .m_done_tready(done_r),
    .m_g2l_tvalid(g2l_v), .m_g2l_tready(1'b1), .m_g2s_tvalid(g2s_v), .m_g2s_tready(1'b1),
    .inp_en(inp_en), .inp_addr(inp_addr), .inp_data(inp_p[L_RD-1]),
    .wgt_en(wgt_en), .wgt_addr(wgt_addr), .wgt_data(wgt_data),
    .acc_rd_en(acc_rd_en), .acc_rd_addr(acc_rd_addr), .acc_rd_data(acc_p1),
    .acc_wr_en(acc_wr_en), .acc_wr_addr(acc_wr_addr), .acc_wr_data(acc_wr_data),
    .out_wr_en(out_wr_en), .out_wr_addr(out_wr_addr), .out_wr_data(out_wr_data),
    .busy(busy));

  // ---------------- debug trace (+dbg=<cycles>) ----------------
  integer dbg, dc;
  initial begin if (!$value$plusargs("dbg=%d", dbg)) dbg = 0; dc = 0; if (dbg) $display("trace %0d cycles", dbg); end
  always @(posedge clk) if (rstn && dc < dbg) begin
    dc = dc + 1;
    $display("c%0d enq %b iss %b latch %b start %b pend %b cnt %0d | go %b flag %b x0 %h | ce1 00 %b 0f %b f0 %b ff %b | f 00 %b 0f %b f0 %b ff %b | tag %b",
      dc, uop_v && uop_r, dut.ctrl.issue, dut.ctrl.latch, dut.ctrl.start, dut.ctrl.pend, dut.ctrl.cnt,
      dut.go, dut.flag, inp_p[L_RD-1][7:0],
      dut.array.ce1[0][0], dut.array.ce1[0][15], dut.array.ce1[15][0], dut.array.ce1[15][15],
      dut.array.fs[0], dut.array.fa[0][15], dut.array.fs[15], dut.array.fa[15][15], dut.tv);
    $display("     P00 %0d P0f %0d P40f %0d P50f %0d Pf0 %0d Pff %0d | A00 %h A0f %h",
      $signed(dut.array.pp[0][0][19:0]), $signed(dut.array.pp[0][15][19:0]), $signed(dut.array.pp[4][15][19:0]),
      $signed(dut.array.pp[5][15][19:0]), $signed(dut.array.pp[15][0][19:0]), $signed(dut.array.pp[15][15][19:0]),
      dut.array.xs[0], dut.array.xa[0][15]);
  end

  // ---------------- stimulus ----------------
  reg [1023:0] dir;
  integer nu, ni, nw, na, nl, fd, r, gap, bp, k, sent, ndone, cyc, cyc0, cyc1, bad_acc, bad_out, b, reload, swapped, n0;
  reg [127:0] wgt2_all [0:NW*N-1];
  reg [39:0] nofirst;
  initial begin
    if (!$value$plusargs("dir=%s", dir)) begin $display("need +dir="); $finish; end
    if (!$value$plusargs("gap=%d", gap)) gap = 0;
    if (!$value$plusargs("bp=%d", bp)) bp = 0;
    fd = $fopen($sformatf("%0s/meta.txt", dir), "r"); r = $fscanf(fd, "%d %d %d %d %d %d", nu, ni, nw, na, nl, reload); $fclose(fd);
    if (r < 6) reload = 0;
    if (reload) $readmemh($sformatf("%0s/wgt2.hex", dir), wgt2_all);
    swapped = 0;
    if (!$value$plusargs("nofirst=%d", nofirst)) nofirst = 0;   // 1: clear the first bit (shows why it is needed)
    $readmemh($sformatf("%0s/uops.hex", dir), uops);
    $readmemh($sformatf("%0s/inp.hex", dir), inp_mem);
    $readmemh($sformatf("%0s/wgt.hex", dir), wgt_all);
    $readmemh($sformatf("%0s/acc0.hex", dir), acc_mem);
    $readmemh($sformatf("%0s/acc.hex", dir), acc_exp);
    $readmemh($sformatf("%0s/out.hex", dir), out_exp);
    for (b = 0; b < NW; b = b + 1) for (j = 0; j < N; j = j + 1) wgt_bank[j][b] = wgt_all[b * N + j];
    n0 = 0; while (n0 < nu && !uops[n0][36]) n0 = n0 + 1; n0 = n0 + 1;   // uops of instruction 0 (up to its last)
    for (k = 0; k < NA; k = k + 1) out_mem[k] = 0;
    uop_v = 0; done_r = 0; sent = 0; ndone = 0; cyc = 0; cyc0 = -1; cyc1 = 0; ng2l = 0; ng2s = 0;
    eg2l = 0; eg2s = 0; for (k = 0; k < nu; k = k + 1) begin eg2l = eg2l + uops[k][38]; eg2s = eg2s + uops[k][39]; end
    // glbl.v holds GSR for the first 100 ns: DSP48E2 registers ignore their CEs until then (simulation only)
    #200;
    @(posedge clk);
    rstn <= 1;
    repeat (4) @(posedge clk);
    while (ndone < nl || busy) begin
      @(posedge clk);
      cyc = cyc + 1;
      if (uop_v && uop_r) begin sent = sent + 1; if (cyc0 < 0) cyc0 = cyc; end
      if (done_v && done_r) begin ndone = ndone + 1; cyc1 = cyc; end
      if (g2l_v) ng2l = ng2l + 1;
      if (g2s_v) ng2s = ng2s + 1;
      // reload: instruction 1 waits for instruction 0's done and the weight swap (LOAD WGT in between)
      if (reload && !swapped && ndone >= 1) begin
        for (b = 0; b < NW; b = b + 1) for (j = 0; j < N; j = j + 1) wgt_bank[j][b] = wgt2_all[b * N + j];
        swapped = 1;
      end
      if (sent < nu && !(reload && !swapped && sent >= n0) && ($urandom % 100) >= gap) begin uop_v <= 1; uop_d <= uops[sent] & ~(nofirst << 37); end
      else uop_v <= 0;
      if (uop_v && uop_r && sent >= nu) uop_v <= 0;
      done_r <= ($urandom % 100) >= bp;
      if (cyc > 2000000) begin $display("RESULT %0s: FAIL timeout sent %0d/%0d done %0d/%0d", dir, sent, nu, ndone, nl); $finish; end
    end
    repeat (8) @(posedge clk);
    bad_acc = 0; bad_out = 0;
    for (k = 0; k < NA; k = k + 1) begin
      if (acc_mem[k] !== acc_exp[k]) begin
        bad_acc = bad_acc + 1;
        if (bad_acc <= 3) $display("acc[%0d]\n  got %h\n  exp %h", k, acc_mem[k], acc_exp[k]);
      end
      if (out_mem[k] !== out_exp[k]) begin
        bad_out = bad_out + 1;
        if (bad_out <= 3) $display("out[%0d] got %h exp %h", k, out_mem[k], out_exp[k]);
      end
    end
    $display("RESULT %0s: %0s acc_wrong %0d out_wrong %0d cycles %0d uops %0d tokens g2l %0d/%0d g2s %0d/%0d", dir,
             (bad_acc == 0 && bad_out == 0 && ng2l == eg2l && ng2s == eg2s) ? "PASS" : "FAIL", bad_acc, bad_out, cyc1 - cyc0 + 1, nu,
             ng2l, eg2l, ng2s, eg2s);
    $finish;
  end
endmodule
