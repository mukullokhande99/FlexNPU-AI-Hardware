// Baseline cycle-probe TB for compute_tile_bram (int_pingpong flavor).
// Drives one dispatch with configurable K and counts CTB state cycles.

`timescale 1ns/1ps

module ctb_probe_tb;
    parameter integer DECOUPLE_MAX = 2;
    parameter integer K_UNDER_TEST = 16;
    parameter integer DRAIN_ISSUE_ON_RETURN = 0;
    parameter integer ARRAY_M = 16;
    parameter integer ARRAY_N = 16;
    parameter integer K_TILE = 16;
    parameter integer MAX_K = 1024;
    parameter integer ACT_WIDTH = 8;
    parameter integer ACC_WIDTH = 32;
    parameter integer ACT_FIFO_DEPTH = 16;
    parameter integer OUT_FIFO_DEPTH = 32;
    parameter integer PIPE_LAT = 1;
    parameter integer OUTFIFO_RD_LAT = 1;

    parameter integer IN_BUF_ADDR_WIDTH = $clog2(MAX_K);
    parameter integer ACT_BUF_DATA_WIDTH = ARRAY_M * ACT_WIDTH;
    parameter integer WGT_BUF_DATA_WIDTH = ARRAY_N * ACT_WIDTH;
    parameter integer OUT_BUF_ADDR_WIDTH = $clog2(ARRAY_M);
    parameter integer OUT_BUF_DATA_WIDTH = ARRAY_N * ACC_WIDTH;
    parameter integer OUT_BUF_WE_WIDTH = OUT_BUF_DATA_WIDTH / 8;

    // ----------------------------------------------------------------
    // Clock / reset
    // ----------------------------------------------------------------

    reg clk = 0;
    reg rst_n = 0;

    always #5 clk = ~clk;   // 100 MHz

    // ----------------------------------------------------------------
    // DUT inputs
    // ----------------------------------------------------------------

    reg start = 0;

    reg [$clog2(MAX_K+1)-1:0] K_total = K_UNDER_TEST;

    reg i_accumulate_in = 0;
    reg i_writeback_en = 1;

    // ----------------------------------------------------------------
    // DUT status / observability
    // ----------------------------------------------------------------

    wire busy;
    wire done;
    wire tile_done_pulse;

    wire [31:0] o_cyc_execute;
    wire [31:0] o_cyc_wait;
    wire [31:0] o_cyc_writeback;
    wire [31:0] o_cyc_busy_total;

    wire [3:0] o_perf_ctb_state;
    wire       o_perf_mm_active;

    // ----------------------------------------------------------------
    // ACT / WGT buffer interface
    //
    // Perfect input buffers:
    //   - always ready
    //   - always return zero data
    //
    // This removes upstream starvation so we can study intrinsic
    // CTB control / compute / drain timing.
    // ----------------------------------------------------------------

    wire [IN_BUF_ADDR_WIDTH-1:0] act_addrb;
    wire [IN_BUF_ADDR_WIDTH-1:0] wgt_addrb;

    wire act_enb;
    wire wgt_enb;

    reg act_buf_ready = 1;
    reg wgt_buf_ready = 1;

    wire [ACT_BUF_DATA_WIDTH-1:0] act_doutb = '0;
    wire [WGT_BUF_DATA_WIDTH-1:0] wgt_doutb = '0;

    // ----------------------------------------------------------------
    // Output-buffer interface
    // ----------------------------------------------------------------

    wire [OUT_BUF_ADDR_WIDTH-1:0] out_addrb;
    wire out_enb;

    wire [OUT_BUF_WE_WIDTH-1:0] out_web;
    wire [OUT_BUF_DATA_WIDTH-1:0] out_dinb;

    // ----------------------------------------------------------------
    // DUT
    //
    // IMPORTANT:
    // This baseline RTL predates SWAIT_EARLY_EXIT, so there is
    // intentionally NO SWAIT_EARLY_EXIT parameter here.
    // ----------------------------------------------------------------

    compute_tile_bram #(
        .DECOUPLE_MAX(DECOUPLE_MAX),
        .DRAIN_ISSUE_ON_RETURN(DRAIN_ISSUE_ON_RETURN),
        .ACT_WIDTH(ACT_WIDTH),
        .ACC_WIDTH(ACC_WIDTH),

        .N(ARRAY_M),              // legacy square parameter
        .ARRAY_M(ARRAY_M),
        .ARRAY_N(ARRAY_N),

        .ACT_FIFO_DEPTH(ACT_FIFO_DEPTH),
        .OUT_FIFO_DEPTH(OUT_FIFO_DEPTH),

        .K_TILE(K_TILE),
        .MAX_K(MAX_K),

        .PIPE_LAT(PIPE_LAT),
        .OUTFIFO_RD_LAT(OUTFIFO_RD_LAT),

        .IN_BUF_ADDR_WIDTH(IN_BUF_ADDR_WIDTH),
        .ACT_BUF_DATA_WIDTH(ACT_BUF_DATA_WIDTH),
        .WGT_BUF_DATA_WIDTH(WGT_BUF_DATA_WIDTH),

        .OUT_BUF_ADDR_WIDTH(OUT_BUF_ADDR_WIDTH),
        .OUT_BUF_DATA_WIDTH(OUT_BUF_DATA_WIDTH),
        .OUT_BUF_WE_WIDTH(OUT_BUF_WE_WIDTH)
    ) DUT (
        .clk(clk),
        .rst_n(rst_n),
        .start(start),

        .K_total(K_total),

        .i_accumulate_in(i_accumulate_in),
        .i_writeback_en(i_writeback_en),

        .busy(busy),
        .done(done),
        .tile_done_pulse(tile_done_pulse),

        .o_cyc_execute(o_cyc_execute),
        .o_cyc_wait(o_cyc_wait),
        .o_cyc_writeback(o_cyc_writeback),
        .o_cyc_busy_total(o_cyc_busy_total),

        .o_perf_ctb_state(o_perf_ctb_state),
        .o_perf_mm_active(o_perf_mm_active),

        .act_addrb(act_addrb),
        .act_enb(act_enb),
        .act_buf_ready(act_buf_ready),
        .act_doutb(act_doutb),

        .wgt_addrb(wgt_addrb),
        .wgt_enb(wgt_enb),
        .wgt_buf_ready(wgt_buf_ready),
        .wgt_doutb(wgt_doutb),

        .out_addrb(out_addrb),
        .out_enb(out_enb),
        .out_web(out_web),
        .out_dinb(out_dinb)
    );



// Fail loudly if MM wants to write a result into a full output FIFO.
// mm.sv otherwise masks the write with ~out_full, which could silently
// drop a result while its streaming counter continues advancing.
always @(posedge clk) begin
    if (counting &&
        |(DUT.u_mm.stream_valid & DUT.u_mm.out_full)) begin

        $display(
            "FIFO OVERFLOW HAZARD: stream_valid=%h out_full=%h",
            DUT.u_mm.stream_valid,
            DUT.u_mm.out_full
        );

        $fatal;
    end
end

    // ----------------------------------------------------------------
    // Kernel-scoped state counters
    // ----------------------------------------------------------------

    integer cyc_total;
    integer cyc_idle;
    integer cyc_wait_tile;
    integer cyc_clear;
    integer cyc_execute;
    integer cyc_wait;
    integer cyc_next;
    integer cyc_writeback;
    integer cyc_mm_active;

    reg counting = 0;

    always @(posedge clk) begin
        if (counting) begin

            cyc_total = cyc_total + 1;

            case (o_perf_ctb_state)
                4'd0: cyc_idle      = cyc_idle      + 1;
                4'd1: cyc_wait_tile = cyc_wait_tile + 1;
                4'd2: cyc_clear     = cyc_clear     + 1;
                4'd3: cyc_execute   = cyc_execute   + 1;
                4'd4: cyc_wait      = cyc_wait      + 1;
                4'd5: cyc_next      = cyc_next      + 1;
                4'd6: cyc_writeback = cyc_writeback + 1;
                default: ;
            endcase

            if (o_perf_mm_active)
                cyc_mm_active = cyc_mm_active + 1;
        end
    end

    // ----------------------------------------------------------------
    // Test sequence
    // ----------------------------------------------------------------

    initial begin

        cyc_total      = 0;
        cyc_idle       = 0;
        cyc_wait_tile  = 0;
        cyc_clear      = 0;
        cyc_execute    = 0;
        cyc_wait       = 0;
        cyc_next       = 0;
        cyc_writeback  = 0;
        cyc_mm_active  = 0;

        // Reset
        #25;
        rst_n = 1;

        @(posedge clk);
        #1;

        // Begin measurement window
        counting = 1;

        // One-cycle start pulse
        start = 1;

        @(posedge clk);
        #1;

        start = 0;

        // Wait for CTB completion
        while (!done)
            @(posedge clk);

        // Capture trailing transition
        @(posedge clk);
        #1;

        counting = 0;

        // ------------------------------------------------------------
        // Report
        // ------------------------------------------------------------

        $display(
            "K=%0d ARRAY=%0dx%0d K_TILE=%0d num_pass=%0d",
            K_UNDER_TEST,
            ARRAY_M,
            ARRAY_N,
            K_TILE,
            K_UNDER_TEST / K_TILE
        );

        $display("  ctb_total      %0d", cyc_total);
        $display("  ctb_idle       %0d", cyc_idle);
        $display("  ctb_wait_tile  %0d", cyc_wait_tile);
        $display("  ctb_clear      %0d", cyc_clear);
        $display("  ctb_execute    %0d", cyc_execute);
        $display("  ctb_wait       %0d", cyc_wait);
        $display("  ctb_next       %0d", cyc_next);
        $display("  ctb_writeback  %0d", cyc_writeback);

        $display(
            "  service        %0d",
            cyc_total - cyc_idle
        );

        $display(
            "  mm_active      %0d",
            cyc_mm_active
        );

        $display(
            "  mm/service     %.4f",
            (cyc_total - cyc_idle) > 0
                ? cyc_mm_active * 1.0 / (cyc_total - cyc_idle)
                : 0
        );

        $finish;
    end



wire [$clog2(OUT_FIFO_DEPTH):0] fifo_count_r0 =
    DUT.u_mm.OUT_FIFOS[0].out_fifo.count;

wire [$clog2(OUT_FIFO_DEPTH):0] fifo_count_r15 =
    DUT.u_mm.OUT_FIFOS[15].out_fifo.count;



// Drain request timing trace.
integer trace_cycle = 0;

always @(negedge clk) begin
    if (counting) begin
        $display(
            "C=%0d state=%0d pending=%0d drain=%0d col=%0d f0=%0d f15=%0d out_ready=%0b rd=%0b rd_d=%0b inflight=%0b q0=%0b q1=%0b pass_end=%0b drain_end=%0b",
            trace_cycle,
            o_perf_ctb_state,
            DUT.r_pending,
            DUT.state_drain,
            DUT.col_pop,
            fifo_count_r0,
            fifo_count_r15,
            DUT.out_ready,
            |DUT.out_rd_en,
            |DUT.out_rd_en_d,
            DUT.inflight,
            DUT.q0_v,
            DUT.q1_v,
            DUT.pass_end_pulse,
            DUT.drain_end_pulse
        );

        trace_cycle = trace_cycle + 1;
    end
end
    // ----------------------------------------------------------------
    // Safety timeout
    // ----------------------------------------------------------------

    initial begin
        #50000000;
        $display("TIMEOUT");
        $finish;
    end

endmodule