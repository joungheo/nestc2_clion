// #### JO added (2026-10-27, systolic Phase D, docs/evta_systolic_design.md §6): uop FIFO, weight fill engine and issue.
// uop = {last, reset, wgt, src, dst} from the HLS GEMM loop (AXIS). A uop whose wgt differs from the uop enqueued before
// it starts a "run"; its wgt goes to the run queue. The fill engine fills runs in order (bank j address in cycle g+j),
// and the first uop of a run may issue (latch, flag = 1) only 17 cycles after its fill started (g + 17 <= issue).
// The next fill may start in the cycle of that latch (it overwrites B1 only after the latch has passed, sa_array).
// Times below are "presented" cycles (the cycle a registered address is on the memory port).
//   issue presented in P: inp_addr; x and the flag reach sa_array in P+L_RD; the tag reaches the RMW in P+L_RD+33.
// #### JO changed (2026-11-04, bias reset): uop bit DW+SW+WW+5 = bias (reset row that takes acc[src], sa_rmw); the tag
// carries it and src.
// #### JO changed (2026-11-04, requantize): uop bits DW+SW+WW+6 .. +29 = rq {hi, lo, shift, round, en} of the instruction
// (VTAGemInsn rq_*), carried in the FIFO and the tag to sa_rmw (the int8 written to out).
`timescale 1ns / 1ps
module sa_ctrl #(
  parameter N = 16,
  parameter DW = 12, SW = 12, WW = 11,   // acc/out, inp, wgt index widths
  parameter L_RD = 2,                   // inp/wgt BRAM read latency (address cycle -> data cycle)
  parameter FD = 5,                     // log2 uop FIFO depth (32)
  parameter RD = 3                      // log2 run queue depth (8)
) (
  input  wire                 clk,
  input  wire                 rst,
  // uop stream
  input  wire [DW+SW+WW+29:0] s_uop_tdata,   // {rq, bias, g2s, g2l, first, last, reset, wgt, src, dst}
  input  wire                 s_uop_tvalid,
  output wire                 s_uop_tready,
  // inp_mem read
  output reg                  inp_en,
  output reg  [SW-1:0]        inp_addr,
  // wgt_mem bank j read (bank j address presented one cycle after bank j-1)
  output reg  [N-1:0]         wgt_en,
  output reg  [WW*N-1:0]      wgt_addr,
  // to sa_array
  output wire                 arr_flag,      // with the inp data (P+L_RD)
  output wire                 arr_go,        // bank 0 data valid (g+L_RD)
  // tag to the RMW (P+L_RD+33)
  output wire                 tag_valid,
  output wire [DW-1:0]        tag_dst,
  output wire                 tag_reset,
  output wire                 tag_bias,       // #### JO added (2026-11-04)
  output wire [SW-1:0]        tag_src,
  output wire [23:0]          tag_rq,         // #### JO added (2026-11-04)
  output wire                 tag_last,
  output wire [1:0]           tag_tok,        // {g2s, g2l} of the instruction (on its last step)
  output wire                 idle           // nothing queued or in the array
);
  //#### JO changed (2026-11-04): + bias
  //localparam UW = DW + SW + WW + 4;          // FIFO entry: {g2s, g2l, last, reset, wgt, src, dst} (first only makes newrun)
  //localparam UW = DW + SW + WW + 5;          // FIFO entry: {bias, g2s, g2l, last, reset, wgt, src, dst} (first only makes newrun)
  //#### JO changed (2026-11-04): + rq on top; UB0 = the entry without it
  localparam UB0 = DW + SW + WW + 5;
  localparam UW = UB0 + 24;                  // FIFO entry: {rq, bias, g2s, g2l, last, reset, wgt, src, dst}
  localparam TD = L_RD + 33;                 // issue presented -> RMW

  // ---------------- uop FIFO (entry = {newrun, uop}) ----------------
  reg  [UW:0]   fifo [0:(1<<FD)-1];
  reg  [FD:0]   wp, rp;
  wire          f_empty = wp == rp;
  wire          f_full  = (wp[FD] != rp[FD]) && (wp[FD-1:0] == rp[FD-1:0]);
  // run queue
  reg  [WW-1:0] runq [0:(1<<RD)-1];
  reg  [RD:0]   qw, qr;
  wire          q_empty = qw == qr;
  wire          q_full  = (qw[RD] != qr[RD]) && (qw[RD-1:0] == qr[RD-1:0]);

  wire [WW-1:0] in_wgt = s_uop_tdata[DW+SW+WW-1:DW+SW];
  reg  [WW-1:0] prev_wgt;
  reg           have_prev;
  //#### JO changed (Phase D board fix): the first uop of each GEMM instruction always starts a run (refill): a LOAD may
  //#### have written new weights at the same address since the last fill (gemmprobe: reset GEMM, LOAD WGT, GEMM, same wgt)
  //wire          in_newrun = !have_prev || in_wgt != prev_wgt;
  wire          in_first = s_uop_tdata[DW+SW+WW+2];
  wire [1:0]    in_tok   = s_uop_tdata[DW+SW+WW+4:DW+SW+WW+3];
  wire          in_bias  = s_uop_tdata[DW+SW+WW+5];
  wire [23:0]   in_rq    = s_uop_tdata[DW+SW+WW+29:DW+SW+WW+6];
  wire          in_newrun = !have_prev || in_wgt != prev_wgt || in_first;
  assign s_uop_tready = !rst && !f_full && !(in_newrun && q_full);
  wire          enq = s_uop_tvalid && s_uop_tready;

  // head
  wire [UW:0]   head = fifo[rp[FD-1:0]];
  wire          h_newrun = head[UW];
  //#### JO changed (2026-11-04): the fields below rq at the positions of the entry without it (UB0)
  wire [23:0]   h_rq     = head[UW-1:UB0];
  wire          h_bias   = head[UB0-1];
  wire [1:0]    h_tok    = head[UB0-2:UB0-3];
  wire          h_last   = head[UB0-4];
  wire          h_reset  = head[UB0-5];
  wire [SW-1:0] h_src    = head[DW+SW-1:DW];
  wire [DW-1:0] h_dst    = head[DW-1:0];

  // ---------------- fill engine ----------------
  reg           pend;                         // the oldest unlatched run is being filled or is filled
  reg  [4:0]    cnt;                          // cycles since its fill was decided, saturating
  wire          ready = pend && cnt >= 5'd16; // issue decided in c >= fill decided + 17
  wire          issue = !f_empty && (!h_newrun || ready);
  wire          latch = issue && h_newrun;
  wire          start = !q_empty && (!pend || latch);

  always @(posedge clk) begin
    if (rst) begin
      wp <= 0; rp <= 0; qw <= 0; qr <= 0; have_prev <= 1'b0; pend <= 1'b0; cnt <= 0;
    end else begin
      if (enq) begin
        fifo[wp[FD-1:0]] <= {in_newrun, in_rq, in_bias, in_tok, s_uop_tdata[DW+SW+WW+1:0]};
        wp <= wp + 1'b1;
        prev_wgt <= in_wgt; have_prev <= 1'b1;
        if (in_newrun) begin runq[qw[RD-1:0]] <= in_wgt; qw <= qw + 1'b1; end
      end
      if (issue) rp <= rp + 1'b1;
      if (start) begin qr <= qr + 1'b1; pend <= 1'b1; cnt <= 0; end
      else begin
        if (latch) pend <= 1'b0;
        if (cnt != 5'd31) cnt <= cnt + 1'b1;
      end
    end
  end

  // ---------------- memory addresses (presented the cycle after the decision) ----------------
  integer j;
  always @(posedge clk) begin
    if (rst) begin
      inp_en <= 1'b0; wgt_en <= 0;
    end else begin
      inp_en <= issue;
      wgt_en <= {wgt_en[N-2:0], start};
    end
    inp_addr <= h_src;
    wgt_addr[WW-1:0] <= runq[qr[RD-1:0]];
    for (j = 1; j < N; j = j + 1) wgt_addr[WW*j +: WW] <= wgt_addr[WW*(j-1) +: WW];
  end

  // ---------------- flag / go pipes (L_RD) and the tag pipe (TD) ----------------
  reg [L_RD:0] flag_p, go_p;                   // [0] = presented cycle
  always @(posedge clk) begin
    if (rst) begin flag_p <= 0; go_p <= 0; end
    else begin
      flag_p <= {flag_p[L_RD-1:0], latch};
      go_p   <= {go_p[L_RD-1:0], start};
    end
  end
  assign arr_flag = flag_p[L_RD];
  assign arr_go   = go_p[L_RD];

  //#### JO changed (2026-11-04): + {src, bias} on top
  //#### JO changed (2026-11-04): + rq on top
  //reg [DW+SW+5:0] tagp [0:TD];                 // {src, bias, tok[1:0], valid, last, reset, dst}; [0] = presented cycle
  reg [DW+SW+29:0] tagp [0:TD];                // {rq, src, bias, tok[1:0], valid, last, reset, dst}; [0] = presented cycle
  integer t;
  always @(posedge clk) begin
    tagp[0] <= rst ? {(DW+SW+30){1'b0}} : {h_rq, h_src, h_bias, h_tok, issue, h_last, h_reset, h_dst};
    for (t = 1; t <= TD; t = t + 1) tagp[t] <= rst ? {(DW+SW+30){1'b0}} : tagp[t-1];
  end
  assign tag_rq    = tagp[TD][DW+SW+29:DW+SW+6];
  assign tag_bias  = tagp[TD][DW+5];
  assign tag_src   = tagp[TD][DW+SW+5:DW+6];
  assign tag_valid = tagp[TD][DW+2];
  assign tag_last  = tagp[TD][DW+1];
  assign tag_reset = tagp[TD][DW];
  assign tag_dst   = tagp[TD][DW-1:0];
  assign tag_tok   = tagp[TD][DW+4:DW+3];

  reg any_tag;
  always @(*) begin
    any_tag = 1'b0;
    for (t = 0; t <= TD; t = t + 1) any_tag = any_tag | tagp[t][DW+2];
  end
  assign idle = f_empty && !any_tag;
endmodule
