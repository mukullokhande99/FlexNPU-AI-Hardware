// Regime-audit testbench for compute_tile_bram (int_pingpong flavor).
//
// Extends ctb_probe_tb (baseline) with:
//   - pass_end_pulse / drain_end_pulse counters
//   - peak per-row OUT_FIFO occupancy, max across all ARRAY_M rows
//   - r_pending occupancy histogram (values 0..7)
//   - credit-stall cycle count (S_NEXT stalled because r_pending>=D)
//   - overflow-hazard flag (recorded, not fatal — sweep needs to catalog)
//
// Emits ONE JSON line at end of run. Parsed by the sweep driver.

`timescale 1ns/1ps

module ctb_regime_tb;
    parameter integer DECOUPLE_MAX          = 3;
    parameter integer K_UNDER_TEST          = 1024;
    parameter integer DRAIN_ISSUE_ON_RETURN = 1;
    parameter integer SWAIT_EARLY_EXIT      = 1;
    parameter integer SWAIT_EARLY_THRESHOLD = 11;
    parameter integer ARRAY_M               = 16;
    parameter integer ARRAY_N               = 16;
    parameter integer K_TILE                = 16;
    parameter integer MAX_K                 = 4096;
    parameter integer ACT_WIDTH             = 8;
    parameter integer ACC_WIDTH             = 32;
    parameter integer ACT_FIFO_DEPTH        = 16;
    parameter integer OUT_FIFO_DEPTH        = 32;
    parameter integer PIPE_LAT              = 1;
    parameter integer OUTFIFO_RD_LAT        = 1;

    parameter integer IN_BUF_ADDR_WIDTH    = $clog2(MAX_K);
    parameter integer ACT_BUF_DATA_WIDTH   = ARRAY_M * ACT_WIDTH;
    parameter integer WGT_BUF_DATA_WIDTH   = ARRAY_N * ACT_WIDTH;
    parameter integer OUT_BUF_ADDR_WIDTH   = $clog2(ARRAY_M);
    parameter integer OUT_BUF_DATA_WIDTH   = ARRAY_N * ACC_WIDTH;
    parameter integer OUT_BUF_WE_WIDTH     = OUT_BUF_DATA_WIDTH / 8;

    // --- clock / reset ---
    reg clk = 0;
    reg rst_n = 0;
    always #5 clk = ~clk;   // 100 MHz

    // --- DUT inputs / outputs ---
    reg  start = 0;
    reg  [$clog2(MAX_K+1)-1:0] K_total = K_UNDER_TEST;
    reg  i_accumulate_in = 0;
    reg  i_writeback_en  = 1;

    wire busy, done, tile_done_pulse;
    wire [31:0] o_cyc_execute, o_cyc_wait, o_cyc_writeback, o_cyc_busy_total;
    wire [3:0]  o_perf_ctb_state;
    wire        o_perf_mm_active;

    // Perfect input buffers (as in baseline TB).
    wire [IN_BUF_ADDR_WIDTH-1:0] act_addrb, wgt_addrb;
    wire act_enb, wgt_enb;
    reg  act_buf_ready = 1;
    reg  wgt_buf_ready = 1;
    wire [ACT_BUF_DATA_WIDTH-1:0] act_doutb = '0;
    wire [WGT_BUF_DATA_WIDTH-1:0] wgt_doutb = '0;

    wire [OUT_BUF_ADDR_WIDTH-1:0] out_addrb;
    wire out_enb;
    wire [OUT_BUF_WE_WIDTH-1:0]   out_web;
    wire [OUT_BUF_DATA_WIDTH-1:0] out_dinb;

    compute_tile_bram #(
        .DECOUPLE_MAX(DECOUPLE_MAX),
        .DRAIN_ISSUE_ON_RETURN(DRAIN_ISSUE_ON_RETURN),
        .SWAIT_EARLY_EXIT(SWAIT_EARLY_EXIT),
        .SWAIT_EARLY_THRESHOLD(SWAIT_EARLY_THRESHOLD),
        .ACT_WIDTH(ACT_WIDTH),
        .ACC_WIDTH(ACC_WIDTH),
        .N(ARRAY_M),
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
        .clk(clk), .rst_n(rst_n), .start(start),
        .K_total(K_total),
        .i_accumulate_in(i_accumulate_in),
        .i_writeback_en(i_writeback_en),
        .busy(busy), .done(done), .tile_done_pulse(tile_done_pulse),
        .o_cyc_execute(o_cyc_execute),
        .o_cyc_wait(o_cyc_wait),
        .o_cyc_writeback(o_cyc_writeback),
        .o_cyc_busy_total(o_cyc_busy_total),
        .o_perf_ctb_state(o_perf_ctb_state),
        .o_perf_mm_active(o_perf_mm_active),
        .act_addrb(act_addrb), .act_enb(act_enb),
        .act_buf_ready(act_buf_ready), .act_doutb(act_doutb),
        .wgt_addrb(wgt_addrb), .wgt_enb(wgt_enb),
        .wgt_buf_ready(wgt_buf_ready), .wgt_doutb(wgt_doutb),
        .out_addrb(out_addrb), .out_enb(out_enb),
        .out_web(out_web), .out_dinb(out_dinb)
    );

    // ----------------------------------------------------------------
    // Kernel-scoped counters
    // ----------------------------------------------------------------
    integer cyc_total, cyc_idle, cyc_wait_tile, cyc_clear;
    integer cyc_execute, cyc_wait, cyc_next, cyc_writeback;
    integer cyc_mm_active;

    integer n_pass_end, n_drain_end;

    // Credit-stall = in S_NEXT, not final pass, r_pending==DECOUPLE_MAX
    integer cyc_credit_stall;

    // r_pending histogram (0..7)
    integer rp_hist [0:7];
    // Peak r_pending ever seen
    integer rp_peak;

    // Peak per-row FIFO occupancy across all rows and time
    integer fifo_peak_all;
    // Sum of (per-cycle max-across-rows occupancy) — enables avg
    integer fifo_maxrow_sum;
    // Overflow-hazard flag (soft; recorded to JSON)
    integer overflow_hazard;

    reg counting = 0;
    integer rr;

    // ----------------------------------------------------------------
    // Peak/hist tracking
    // ----------------------------------------------------------------
    // Snapshot per-row FIFO count via hierarchical reference. Because
    // per-row counts are only reachable through the OUT_FIFOS[i] genblk,
    // build a per-cycle max across the ARRAY_M rows.
    integer per_row_max_this_cycle;

    // Bring each row's FIFO count into a flat array by manual expansion.
    // This is the ugly-but-portable way in generate-block hierarchies.
`define ROW_COUNT(idx) DUT.u_mm.OUT_FIFOS[idx].out_fifo.count

    always @(posedge clk) begin
        if (counting) begin
            cyc_total = cyc_total + 1;

            // FSM residency
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

            // Pulses: hierarchical wires from DUT
            if (DUT.pass_end_pulse)  n_pass_end  = n_pass_end  + 1;
            if (DUT.drain_end_pulse) n_drain_end = n_drain_end + 1;

            // r_pending histogram
            if (DUT.r_pending <= 7) begin
                rp_hist[DUT.r_pending] = rp_hist[DUT.r_pending] + 1;
                if (DUT.r_pending > rp_peak)
                    rp_peak = DUT.r_pending;
            end

            // Credit stall witness
            if ((o_perf_ctb_state == 4'd5) &&
                (DUT.pass_cnt < DUT.num_pass - 1) &&
                (DUT.r_pending >= DECOUPLE_MAX))
                cyc_credit_stall = cyc_credit_stall + 1;

            // Per-row FIFO peak (all 16 rows).
            per_row_max_this_cycle = 0;
            if (`ROW_COUNT(0)  > per_row_max_this_cycle) per_row_max_this_cycle = `ROW_COUNT(0);
            if (`ROW_COUNT(1)  > per_row_max_this_cycle) per_row_max_this_cycle = `ROW_COUNT(1);
            if (`ROW_COUNT(2)  > per_row_max_this_cycle) per_row_max_this_cycle = `ROW_COUNT(2);
            if (`ROW_COUNT(3)  > per_row_max_this_cycle) per_row_max_this_cycle = `ROW_COUNT(3);
            if (`ROW_COUNT(4)  > per_row_max_this_cycle) per_row_max_this_cycle = `ROW_COUNT(4);
            if (`ROW_COUNT(5)  > per_row_max_this_cycle) per_row_max_this_cycle = `ROW_COUNT(5);
            if (`ROW_COUNT(6)  > per_row_max_this_cycle) per_row_max_this_cycle = `ROW_COUNT(6);
            if (`ROW_COUNT(7)  > per_row_max_this_cycle) per_row_max_this_cycle = `ROW_COUNT(7);
            if (`ROW_COUNT(8)  > per_row_max_this_cycle) per_row_max_this_cycle = `ROW_COUNT(8);
            if (`ROW_COUNT(9)  > per_row_max_this_cycle) per_row_max_this_cycle = `ROW_COUNT(9);
            if (`ROW_COUNT(10) > per_row_max_this_cycle) per_row_max_this_cycle = `ROW_COUNT(10);
            if (`ROW_COUNT(11) > per_row_max_this_cycle) per_row_max_this_cycle = `ROW_COUNT(11);
            if (`ROW_COUNT(12) > per_row_max_this_cycle) per_row_max_this_cycle = `ROW_COUNT(12);
            if (`ROW_COUNT(13) > per_row_max_this_cycle) per_row_max_this_cycle = `ROW_COUNT(13);
            if (`ROW_COUNT(14) > per_row_max_this_cycle) per_row_max_this_cycle = `ROW_COUNT(14);
            if (`ROW_COUNT(15) > per_row_max_this_cycle) per_row_max_this_cycle = `ROW_COUNT(15);

            if (per_row_max_this_cycle > fifo_peak_all)
                fifo_peak_all = per_row_max_this_cycle;
            fifo_maxrow_sum = fifo_maxrow_sum + per_row_max_this_cycle;

            // Overflow-hazard flag (mm.sv would silently mask a write when
            // stream_valid[r] & out_full[r]).
            if (|(DUT.u_mm.stream_valid & DUT.u_mm.out_full))
                overflow_hazard = 1;
        end
    end

`undef ROW_COUNT

    // ----------------------------------------------------------------
    // Test sequence
    // ----------------------------------------------------------------
    initial begin
        integer i;

        cyc_total      = 0;
        cyc_idle       = 0;
        cyc_wait_tile  = 0;
        cyc_clear      = 0;
        cyc_execute    = 0;
        cyc_wait       = 0;
        cyc_next       = 0;
        cyc_writeback  = 0;
        cyc_mm_active  = 0;
        n_pass_end     = 0;
        n_drain_end    = 0;
        cyc_credit_stall = 0;
        rp_peak        = 0;
        fifo_peak_all  = 0;
        fifo_maxrow_sum = 0;
        overflow_hazard = 0;
        for (i = 0; i < 8; i = i + 1) rp_hist[i] = 0;

        // Reset
        #25;
        rst_n = 1;

        @(posedge clk); #1;
        counting = 1;
        start = 1;
        @(posedge clk); #1;
        start = 0;

        while (!done) @(posedge clk);
        @(posedge clk); #1;
        counting = 0;

        // --- JSON one-liner ---
        // Emit a tag so the parser can find it, then a JSON object.
        $write("REGIME_JSON ");
        $write("{");
        $write("\"K\":%0d,",          K_UNDER_TEST);
        $write("\"D\":%0d,",          DECOUPLE_MAX);
        $write("\"drain_y2\":%0d,",   DRAIN_ISSUE_ON_RETURN);
        $write("\"swait_ee\":%0d,",   SWAIT_EARLY_EXIT);
        $write("\"num_pass\":%0d,",   K_UNDER_TEST / K_TILE);
        $write("\"cyc_total\":%0d,",  cyc_total);
        $write("\"cyc_idle\":%0d,",   cyc_idle);
        $write("\"service\":%0d,",    cyc_total - cyc_idle);
        $write("\"exec\":%0d,",       cyc_execute);
        $write("\"wait\":%0d,",       cyc_wait);
        $write("\"next\":%0d,",       cyc_next);
        $write("\"writeback\":%0d,",  cyc_writeback);
        $write("\"wait_tile\":%0d,",  cyc_wait_tile);
        $write("\"clear\":%0d,",      cyc_clear);
        $write("\"mm_active\":%0d,",  cyc_mm_active);
        $write("\"pass_end_n\":%0d,", n_pass_end);
        $write("\"drain_end_n\":%0d,",n_drain_end);
        $write("\"credit_stall_cyc\":%0d,", cyc_credit_stall);
        $write("\"rp_peak\":%0d,",    rp_peak);
        $write("\"rp_hist\":[%0d,%0d,%0d,%0d,%0d,%0d,%0d,%0d],",
                rp_hist[0], rp_hist[1], rp_hist[2], rp_hist[3],
                rp_hist[4], rp_hist[5], rp_hist[6], rp_hist[7]);
        $write("\"fifo_peak_all\":%0d,", fifo_peak_all);
        $write("\"fifo_avg_maxrow_x1000\":%0d,",
                (cyc_total > 0) ? (fifo_maxrow_sum * 1000 / cyc_total) : 0);
        $write("\"overflow_hazard\":%0d",   overflow_hazard);
        $write("}\n");

        $finish;
    end

    // Safety timeout — long enough for K=4096 with D=1 worst case.
    initial begin
        #100000000;
        $write("REGIME_JSON {\"K\":%0d,\"D\":%0d,\"TIMEOUT\":1}\n",
                K_UNDER_TEST, DECOUPLE_MAX);
        $finish;
    end

endmodule
