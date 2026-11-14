`timescale 1ns/1ps

// EdgeScope platformization Stage 3C (2026-08-28):
//   Independent ARRAY_M and ARRAY_N parameters. Legacy single-N is
//   retained as backward-compat default (both default to N) so the
//   trusted N=16 build remains byte-identical to the pre-Stage-3 source.
//
//   ACT_BUF_DATA_WIDTH = ARRAY_M * ACT_WIDTH  (A-side row lanes)
//   WGT_BUF_DATA_WIDTH = ARRAY_N * ACT_WIDTH  (B-side col lanes)
//   OUT_BUF_DATA_WIDTH = ARRAY_N * ACC_WIDTH  (one row of N accumulators per drain)
//   OUT_BUF_ADDR_WIDTH = $clog2(ARRAY_M)      (drain iterates over M rows)
//   out_mem            = ARRAY_M * ARRAY_N    row-major
//
//   For backward-compat, IN_BUF_DATA_WIDTH is retained as an alias for
//   ACT_BUF_DATA_WIDTH so external port-map connections that pass a
//   single "IN_BUF_DATA_WIDTH" continue to compile. Shells that want to
//   split ACT and WGT natively can pass ACT_BUF_DATA_WIDTH and
//   WGT_BUF_DATA_WIDTH separately.
module compute_tile_bram #(
    parameter integer ACT_WIDTH             = 8,
    parameter integer ACC_WIDTH             = 32,   // FROZEN: i8xi8->i32 (shell drives 32)
    parameter integer N                     = 8,

    // EdgeScope Stage 3C: split M and N. Default preserves legacy N.
    parameter integer ARRAY_M               = N,
    parameter integer ARRAY_N               = N,

    parameter integer ACT_FIFO_DEPTH        = 8,
    parameter integer OUT_FIFO_DEPTH        = 32,

    parameter integer K_TILE                = 8,
    parameter integer MAX_K                 = 1024,

    parameter integer PIPE_LAT              = 1,
    parameter integer OUTFIFO_RD_LAT        = 1,

    // input port parameters
    parameter integer IN_BUF_ADDR_WIDTH     = $clog2(MAX_K),      //BRAM_ADDR_R
    parameter integer ACT_BUF_DATA_WIDTH    = ARRAY_M * ACT_WIDTH, //A-side lanes
    parameter integer WGT_BUF_DATA_WIDTH    = ARRAY_N * ACT_WIDTH, //B-side lanes
    // Backward-compat alias: shells passing IN_BUF_DATA_WIDTH continue
    // to work when ARRAY_M == ARRAY_N. When they differ, external
    // shells should switch to the ACT_/WGT_ pair.
    parameter integer IN_BUF_DATA_WIDTH     = ACT_BUF_DATA_WIDTH,

    // output port parameters
    parameter integer OUT_BUF_ADDR_WIDTH    = $clog2(ARRAY_M),   //BRAM_ADDR_W — one entry per row
    parameter integer OUT_BUF_DATA_WIDTH    = ARRAY_N * ACC_WIDTH,//BRAM_DATA_W — one drained row (N cols)
    parameter integer OUT_BUF_WE_WIDTH      = OUT_BUF_DATA_WIDTH/8,//BRAM_WE_W

    // Decoupling / drain-issue knobs (moved into the header so `iverilog
    // -P<top>.DUT.<PARAM>=<val>` and named-parameter binding `.<PARAM>(x)`
    // from the shell work under Icarus 12.0's stricter parameter-scoping
    // rules. Defaults preserve the D=3 / Y2 / SWAIT-early-exit canonical
    // realization exactly. Full derivation comments retained below at
    // the original declaration sites.
    parameter integer DECOUPLE_MAX          = 3,
    parameter integer SWAIT_EARLY_EXIT      = 1,
    parameter integer SWAIT_EARLY_THRESHOLD = 11,
    parameter integer DRAIN_ISSUE_ON_RETURN = 1,
    parameter integer SQUARE_ENABLE         = 0,
    parameter integer SQUARE_V1             = 0
)(
    input  wire                             clk,
    input  wire                             rst_n,
    input  wire                             start,
    // Experimental low-precision format control, stable per dispatch.
    input  wire [1:0]                       i_square_mode,
    input  wire signed [5:0]                i_square_unit_exp,
    input  wire [$clog2(MAX_K+1)-1:0]       K_total,

    // Cross-dispatch K-accumulate control (sampled at start pulse).
    // Reset defaults (accumulate_in=0, writeback_en=1) preserve legacy
    // single-dispatch behavior: a shell that never drives these produces
    // byte-identical output to the pre-change compute_tile_bram.
    //   accumulate_in=1 : skip out_mem clear in S_CLEAR — the accumulator
    //                     is preserved from the previous dispatch, and the
    //                     current dispatch's partial sums are added on top.
    //   writeback_en=0  : skip S_WRITEBACK on the final pass; out_mem
    //                     retains the accumulated tile for the next
    //                     chained dispatch (with accumulate_in=1).
    // Correct usage for a K-chunked chain:
    //   chunk 0 (first)      : (accumulate_in=0, writeback_en=0)
    //   chunks 1..N-2 (mid)  : (accumulate_in=1, writeback_en=0)
    //   chunk N-1 (last)     : (accumulate_in=1, writeback_en=1)
    // Rescale (external to this module) is IMPLICITLY GATED on intermediate
    // chunks because writeback_en=0 skips S_WRITEBACK entirely — pack_row
    // never fires so the drain path never delivers partial sums to any
    // downstream rescale unit.
    input  wire                             i_accumulate_in,
    input  wire                             i_writeback_en,

    output reg                              busy,
    output reg                              done,

    output reg                              tile_done_pulse,

    // Diagnostic per-state cycle counters (2026-09-18). Accumulate across
    // dispatches since rst_n. Read via top-level AXI-Lite. Used to identify
    // the addressable fraction of the ~4-cyc/pass residual overhead.
    output wire [31:0]                      o_cyc_execute,
    output wire [31:0]                      o_cyc_wait,
    output wire [31:0]                      o_cyc_writeback,
    output wire [31:0]                      o_cyc_busy_total,

    // Passive observation ports (2026-09-19 diag build).
    //   o_perf_ctb_state : the CTB FSM state register, exported so the
    //                       perf monitor can produce a kernel-scoped
    //                       state-residency histogram. Values match the
    //                       localparams S_IDLE=0 .. S_WRITEBACK=6.
    //   o_perf_mm_active : mm.sv's truthful MM-datapath-active predicate
    //                       (see mm.sv port comment). Bubbled straight
    //                       through this module; no functional effect.
    output wire [3:0]                       o_perf_ctb_state,
    output wire                             o_perf_mm_active,

    // ACT read port
    output reg  [IN_BUF_ADDR_WIDTH-1:0]     act_addrb,
    output reg                              act_enb,
    input  wire                             act_buf_ready,      // ping pong buffer's data is ready
    input  wire [ACT_BUF_DATA_WIDTH-1:0]    act_doutb,

    // WGT read port
    output reg  [IN_BUF_ADDR_WIDTH-1:0]     wgt_addrb,
    output reg                              wgt_enb,
    input  wire                             wgt_buf_ready,
    input  wire [WGT_BUF_DATA_WIDTH-1:0]    wgt_doutb,

    // OUT write port
    output reg  [OUT_BUF_ADDR_WIDTH-1:0]    out_addrb,
    output reg                              out_enb,
    output reg  [OUT_BUF_WE_WIDTH-1:0]      out_web,
    output reg  [OUT_BUF_DATA_WIDTH-1:0]    out_dinb
);

    // out_mem[r*ARRAY_N + c] row-major; ARRAY_M rows × ARRAY_N cols.
    reg signed [ACC_WIDTH-1:0] out_mem [0:ARRAY_M*ARRAY_N-1];
    integer rr;

    reg  mm_clear_pulse;
    wire mm_rst_n = rst_n & ~mm_clear_pulse;

    reg                          mm_active;
    reg                          mm_wr_en_act, mm_wr_en_w;
    // A-side lanes = ARRAY_M rows; B-side lanes = ARRAY_N cols.
    reg  signed [ACT_WIDTH-1:0]  mm_ain [0:ARRAY_M-1];
    reg  signed [ACT_WIDTH-1:0]  mm_bin [0:ARRAY_N-1];

    wire                         mm_done;
    // Streaming-safety signal from mm.sv: high when it is safe to start
    // the next K-tile pass (i.e., all systolic accumulator reads have
    // been latched into mm output FIFOs). See mm.sv port comment.
    wire                         mm_all_streaming_done;
    // Output side: one per row (ARRAY_M) — read-enable, empty, full, per-row data.
    reg  [ARRAY_M-1:0]           out_rd_en;
    wire [ARRAY_M-1:0]           out_empty;
    wire [ARRAY_M-1:0]           out_full;
    wire signed [ACC_WIDTH-1:0]  out_dout [0:ARRAY_M-1];

    wire                         act_ready, wgt_ready, out_ready;

    reg [1:0] square_mode_q;
    reg signed [5:0] square_unit_exp_q;
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            square_mode_q <= 2'b00;
            square_unit_exp_q <= 6'sd0;
        end else if (start) begin
            square_mode_q <= i_square_mode;
            square_unit_exp_q <= i_square_unit_exp;
        end
    end

    mm #(
        .ACT_WIDTH      (ACT_WIDTH),
        .ACC_WIDTH      (ACC_WIDTH),
        .N              (N),
        .K              (K_TILE),
        .ACT_FIFO_DEPTH (ACT_FIFO_DEPTH),
        .OUT_FIFO_DEPTH (OUT_FIFO_DEPTH),
        .PIPE_LAT       (PIPE_LAT),
        .ARRAY_M        (ARRAY_M),
        .ARRAY_N        (ARRAY_N),
        .SQUARE_ENABLE   (SQUARE_ENABLE),
        .SQUARE_V1       (SQUARE_V1)
    ) u_mm (
        .clk       (clk),
        .rst       (mm_rst_n),
        .active    (mm_active),
        .square_mode(square_mode_q),
        .square_unit_exp(square_unit_exp_q),
        .ain       (mm_ain),
        .bin       (mm_bin),
        .wr_en_act (mm_wr_en_act),
        .wr_en_w   (mm_wr_en_w),
        .done      (mm_done),
        .acc_out   (),
        .out_rd_en (out_rd_en),
        .out_empty (out_empty),
        .out_full  (out_full),
        .out_dout  (out_dout),
        .act_ready (act_ready),
        .wgt_ready (wgt_ready),
        .out_ready (out_ready),
        .o_perf_mm_active (o_perf_mm_active),
        .o_all_streaming_done (mm_all_streaming_done)
    );

    // Main FSM States
    localparam [3:0]
        S_IDLE      = 4'd0,
        S_WAIT_TILE = 4'd1,
        S_CLEAR     = 4'd2,
        S_EXECUTE   = 4'd3,
        S_WAIT      = 4'd4,
        S_NEXT      = 4'd5,
        S_WRITEBACK = 4'd6;
    reg [3:0] state;

    // Drain FSM States
    localparam
        D_IDLE      = 1'b0,
        D_ACTIVE    = 1'b1;
    reg state_drain;

    // Drain FSM registers
    reg drain_busy;
    reg drain_start;
    reg drain_clear_mem;

    reg [$clog2(MAX_K+1)-1:0] k_base, pass_cnt;
    wire [$clog2(MAX_K+1)-1:0] num_pass = K_total / K_TILE;

    // Latched chain-flag state (sampled at start pulse to make mid-dispatch
    // register writes on OP_CHAIN_FLAGS a non-hazard).
    reg latched_accumulate_in;
    reg latched_writeback_en;

    reg [$clog2(K_TILE+1)-1:0] req_cnt, push_cnt, send_cnt;

    // Decoupled compute->drain pipeline. r_pending tracks passes that have
    // been launched/end-injected by compute but have not yet been fully drained
    // into out_mem. It is an outstanding-work credit, NOT a count of complete
    // pass outputs physically resident in the mm output FIFOs: results materialize
    // progressively through the pipeline. DECOUPLE_MAX therefore bounds producer
    // lead rather than directly mirroring FIFO capacity. D=3 is safe across the
    // validated K sweep; D=4 causes a real FIFO overflow at K=1024.
    // Drain FSM auto-restarts while outstanding work remains.
    // DECOUPLE_MAX is a module-header parameter (see the `#(...)` above).
    // Kept as a header parameter — not a body parameter — so external
    // named-binding `.DECOUPLE_MAX(x)` and iverilog `-P` overrides work
    // under Icarus 12.0's strict scope rules. Default 3 is the D=3
    // validated safe canonical value.
    reg [2:0] r_pending;                   // 0..7 with headroom

    // 2026-09-19 INTERVENTION-Y1-A: S_WAIT early exit.
    // The baseline schedule waits in S_WAIT for `mm_done` (systolic
    // pipeline drained + last row_done_pulse fired). Empirically this
    // takes 34 cycles per pass at K_TILE=16, ARRAY_M=ARRAY_N=16.
    // Legality analysis (HW_INTERVENTION_Y1_20260919.md §1) proves that
    // the next K-tile pass can safely begin operand injection at cycle
    // X_{p+1} = X_p + 33, where X_p is the current pass's first
    // systolic-visible __active cycle. Translating that lower bound
    // through the CTB fill and S_NEXT-transit stages gives an
    // S_WAIT_MIN of 11 cycles.
    // We implement this as a dead-reckoning counter (no dependence on
    // downstream signals — the safety window is fixed by array geometry
    // and K_TILE which are compile-time parameters). SWAIT_EARLY_EXIT is
    // a compile-time parameter (default 1); setting it 0 recovers the
    // baseline `mm_done`-gated behavior byte-for-byte.
    //
    // SWAIT_EARLY_EXIT, SWAIT_EARLY_THRESHOLD, DRAIN_ISSUE_ON_RETURN are
    // module-header parameters (see `#(...)` above) — not body parameters —
    // so external named-binding and iverilog `-P` overrides work under
    // Icarus 12.0's strict scope rules. Defaults 1 / 11 / 1 preserve the
    // Y2 + S_WAIT-early-exit canonical realization.
    reg [$clog2(SWAIT_EARLY_THRESHOLD+2)-1:0] wait_cnt;

    // Diagnostic per-state cycle counters (2026-09-18). Reset only on rst_n.
    reg [31:0] r_cyc_execute;    // cycles state == S_EXECUTE
    reg [31:0] r_cyc_wait;       // cycles state == S_WAIT (the key measurement)
    reg [31:0] r_cyc_writeback;  // cycles state == S_WRITEBACK
    reg [31:0] r_cyc_busy_total; // cycles state != S_IDLE (total compute service)

    assign o_cyc_execute    = r_cyc_execute;
    assign o_cyc_wait       = r_cyc_wait;
    assign o_cyc_writeback  = r_cyc_writeback;
    assign o_cyc_busy_total = r_cyc_busy_total;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            r_cyc_execute    <= 32'd0;
            r_cyc_wait       <= 32'd0;
            r_cyc_writeback  <= 32'd0;
            r_cyc_busy_total <= 32'd0;
        end else begin
            if (state != S_IDLE)      r_cyc_busy_total <= r_cyc_busy_total + 32'd1;
            if (state == S_EXECUTE)   r_cyc_execute    <= r_cyc_execute + 32'd1;
            if (state == S_WAIT)      r_cyc_wait       <= r_cyc_wait + 32'd1;
            if (state == S_WRITEBACK) r_cyc_writeback  <= r_cyc_writeback + 32'd1;
        end
    end

    // Passive observation: raw CTB state register (2026-09-19 diag).
    assign o_perf_ctb_state = state;

    // Input data read from input buffers — ACT lanes = M rows, WGT lanes = N cols.
    reg [ACT_BUF_DATA_WIDTH-1:0] act_q;
    reg [WGT_BUF_DATA_WIDTH-1:0] wgt_q;
    reg                     q_valid;
    reg                     rd_issued_d;

    // Column counters cycle 0..ARRAY_N-1 (drain iterates over N columns).
    reg [$clog2(ARRAY_N+1)-1:0]   col_pop;
    reg [$clog2(ARRAY_N+1)-1:0]   last_pop_col;
    reg [ARRAY_M-1:0]             out_rd_en_d;
    reg                           inflight;

    // q0/q1 hold per-drain-cycle values: ARRAY_M row values with their column index.
    reg                            q0_v, q1_v;
    reg [$clog2(ARRAY_N+1)-1:0]    q0_col, q1_col;
    reg signed [ACC_WIDTH-1:0]     q0_data [0:ARRAY_M-1];
    reg signed [ACC_WIDTH-1:0]     q1_data [0:ARRAY_M-1];

    wire buf_full = q0_v & q1_v;

    // 2026-09-19 INTERVENTION-Y1-A companion guard:
    // In baseline, the drain FSM implicitly relied on the FIFOs going empty
    // between passes (because pass P+1's data arrived AFTER pass P's drain
    // had finished). Under the S_WAIT early-exit intervention, pass P+1's
    // streaming overlaps pass P's drain, so `out_ready` can stay 1
    // continuously. This flag makes the "16 reads per drain" boundary
    // EXPLICIT: once we fire the read for col ARRAY_N-1, no more reads
    // this drain until drain_end_pulse fires and col_pop_saturated resets.
    // Required for correctness of the drain-per-pass accumulation.
    reg col_pop_saturated;

    // Writeback iterates over ARRAY_M rows.
    reg [$clog2(ARRAY_M+1)-1:0] wb_row;

    // pack_row packs one row of ARRAY_N accumulators into OUT_BUF_DATA_WIDTH.
    function [OUT_BUF_DATA_WIDTH-1:0] pack_row(input integer r);
        integer c;
        reg [OUT_BUF_DATA_WIDTH-1:0] tmp;
        begin
            tmp = '0;
            for (c = 0; c < ARRAY_N; c = c + 1)
                tmp[c*ACC_WIDTH +: ACC_WIDTH] = out_mem[r*ARRAY_N + c];
            pack_row = tmp;
        end
    endfunction

    // capture BRAM reads (1-cycle delayed model)
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            act_q <= '0;
            wgt_q <= '0;
            q_valid <= 1'b0;
            rd_issued_d <= 1'b0;
        end else begin
            rd_issued_d <= (act_enb && wgt_enb);
            q_valid <= rd_issued_d;
            if (rd_issued_d) begin
                act_q <= act_doutb;
                wgt_q <= wgt_doutb;
            end
        end
    end

    // Main FSM
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state <= S_IDLE;
            busy  <= 1'b0;
            done  <= 1'b0;

            mm_active <= 1'b0;
            mm_wr_en_act <= 1'b0;
            mm_wr_en_w <= 1'b0;
            mm_clear_pulse <= 1'b0;

            drain_clear_mem <= 1'b0;
            drain_start <= 1'b0;

            act_enb <= 1'b0;
            wgt_enb <= 1'b0;
            out_enb <= 1'b0;
            out_web <= '0;

            tile_done_pulse <= 1'b0;

            act_addrb <= '0;
            wgt_addrb <= '0;
            out_addrb <= '0;
            out_dinb  <= '0;

            k_base <= '0;
            pass_cnt <= '0;
            req_cnt <= '0;
            push_cnt <= '0;
            send_cnt <= '0;

            wb_row <= '0;

            latched_accumulate_in <= 1'b0;
            latched_writeback_en  <= 1'b1;

            wait_cnt <= '0;

        end else begin
            done <= 1'b0;
            act_enb <= 1'b0;
            wgt_enb <= 1'b0;
            out_enb <= 1'b0;
            out_web <= '0;

            tile_done_pulse <= 1'b0;

            mm_active <= 1'b0;
            mm_wr_en_act <= 1'b0;
            mm_wr_en_w <= 1'b0;
            mm_clear_pulse <= 1'b0;

            drain_clear_mem <= 1'b0;
            drain_start <= 1'b0;

            case (state)

            S_IDLE: begin
                busy <= 1'b0;
                if (start) begin
                    busy <= 1'b1;
                    k_base <= '0;
                    pass_cnt <= '0;
                    // Sample chain flags at start — must be held stable by
                    // the shell for the dispatch's duration.
                    latched_accumulate_in <= i_accumulate_in;
                    latched_writeback_en  <= i_writeback_en;
                    state <= S_WAIT_TILE;
                end
            end

            S_WAIT_TILE: begin
                if (act_buf_ready && wgt_buf_ready) begin
                    state <= S_CLEAR;
                end
            end

            S_CLEAR: begin
                req_cnt <= '0;
                push_cnt <= '0;
                send_cnt <= '0;
                // Skip out_mem clear on the very first pass when the shell
                // asked for accumulate-in: preserves the tile from the
                // previous dispatch so partial sums accumulate across
                // chained dispatches. When latched_accumulate_in=0 (reset
                // default), condition reduces to (pass_cnt == 0) — byte-
                // identical to the legacy behavior.
                if (pass_cnt == 0 && !latched_accumulate_in) begin
                    mm_clear_pulse <= 1'b1;
                    drain_clear_mem <= 1'b1;
                end
                state <= S_EXECUTE;
            end

            S_EXECUTE: begin
                if (req_cnt < K_TILE && act_ready && wgt_ready) begin // handshake to handle backpressure
                    act_enb <= 1'b1;
                    wgt_enb <= 1'b1;
                    act_addrb <= k_base + req_cnt;
                    wgt_addrb <= k_base + req_cnt;
                    req_cnt <= req_cnt + 1'b1;
                end

                if (q_valid && (push_cnt < K_TILE) && act_ready && wgt_ready) begin
                    // ACT unpack: ARRAY_M lanes from act_q (M-wide).
                    for (rr = 0; rr < ARRAY_M; rr = rr + 1) begin
                        mm_ain[rr] <= $signed(act_q[rr*ACT_WIDTH +: ACT_WIDTH]);
                    end
                    // WGT unpack: ARRAY_N lanes from wgt_q (N-wide).
                    for (rr = 0; rr < ARRAY_N; rr = rr + 1) begin
                        mm_bin[rr] <= $signed(wgt_q[rr*ACT_WIDTH +: ACT_WIDTH]);
                    end
                    mm_wr_en_act <= 1'b1;
                    mm_wr_en_w <= 1'b1;
                    push_cnt <= push_cnt + 1'b1;
                end

                if ((send_cnt < push_cnt) && (send_cnt < K_TILE)) begin
                    mm_active <= 1'b1;
                    send_cnt <= send_cnt + 1'b1;
                end

                if (send_cnt == K_TILE) begin
                    send_cnt <= '0;
                    tile_done_pulse <= 1'b1;
                    state <= S_WAIT;
                end
            end

            S_WAIT: begin
                tile_done_pulse <= 1'b0;
                drain_start <= 1'b1;
                // INTERVENTION-Y1-A (2026-09-19): advance to S_NEXT as
                // soon as the safety threshold has been reached. This
                // saves ~23 cyc/pass in the non-final case. Final-pass
                // total time is unchanged because S_NEXT then absorbs
                // the drain-completion wait (drain_busy=0).
                //
                // Legacy fallback: if SWAIT_EARLY_EXIT is 0, we still
                // gate on mm_done for byte-identical baseline behavior.
                if (SWAIT_EARLY_EXIT != 0) begin
                    if (wait_cnt >= SWAIT_EARLY_THRESHOLD[$bits(wait_cnt)-1:0]) begin
                        state <= S_NEXT;
                        wait_cnt <= '0;
                    end else begin
                        wait_cnt <= wait_cnt + 1'b1;
                    end
                end else begin
                    if (mm_done) begin
                        state <= S_NEXT;
                    end
                end
            end

            S_NEXT: begin
                drain_start <= 1'b0;
                if (pass_cnt == (num_pass - 1)) begin
                    if (!drain_busy) begin
                        // On the final intra-dispatch pass:
                        //   writeback_en=1 (default) → S_WRITEBACK drains
                        //     out_mem to output BRAM (rescale unit
                        //     downstream sees the fully-accumulated tile).
                        //   writeback_en=0            → skip S_WRITEBACK,
                        //     out_mem retained for the next chained
                        //     dispatch. Rescale is IMPLICITLY GATED here:
                        //     pack_row never fires so no partial sums leave
                        //     this module.
                        if (latched_writeback_en) begin
                            wb_row <= '0;
                            state <= S_WRITEBACK;
                        end else begin
                            done <= 1'b1;
                            busy <= 1'b0;
                            pass_cnt <= '0;
                            k_base <= '0;
                            state <= S_IDLE;
                        end
                    end
                end else begin
                    // D1 decoupled pipeline (2026-09-18): replace the
                    // `!drain_busy` wait with a FIFO-safety gate on
                    // r_pending. The drain FSM auto-restarts when
                    // r_pending>1 at its completion, so it stays busy across
                    // multiple queued passes. r_pending directly reflects
                    // how many pass outputs are in the mm output FIFOs.
                    // Compute may advance if r_pending < DECOUPLE_MAX
                    // (currently 2, matching FIFO capacity in passes).
                    //
                    // C1 intra-tile fast-path preserved: bypass S_WAIT_TILE
                    // and S_CLEAR since buffer_ready stays valid within a
                    // tile and S_CLEAR is a no-op for pass_cnt >= 1.
                    if (r_pending < DECOUPLE_MAX) begin
                        pass_cnt <= pass_cnt + 1'b1;
                        k_base   <= k_base + K_TILE;
                        req_cnt  <= '0;
                        push_cnt <= '0;
                        send_cnt <= '0;
                        state    <= S_EXECUTE;
                    end
                    // else: stall in S_NEXT until drain FSM frees room
                end
            end

            S_WRITEBACK: begin
                out_enb <= 1'b1;
                out_web <= {OUT_BUF_WE_WIDTH{1'b1}};
                out_addrb <= wb_row;
                out_dinb <= pack_row(wb_row);
                // Iterate ARRAY_M rows before signalling done.
                if (wb_row == ARRAY_M-1) begin
                    done <= 1'b1;
                    busy <= 1'b0;
                    pass_cnt <= '0;
                    k_base <= '0;
                    state <= S_IDLE;
                end else begin
                    wb_row <= wb_row + 1'b1;
                end
            end

            default: state <= S_IDLE;

            endcase
        end
    end

    // D1 decoupled pipeline pulses (2026-09-18) — combinational from state regs.
    // pass_end_pulse fires when compute FSM is transitioning out of S_EXECUTE
    // at end-of-pass; drain_end_pulse mirrors the drain FSM's own D_ACTIVE
    // completion condition.
    wire pass_end_pulse = (state == S_EXECUTE) && (send_cnt == K_TILE);
    wire drain_end_pulse = (state_drain == D_ACTIVE) &&
                          col_pop_saturated && !inflight && !q0_v && !q1_v;


    // Y2: allow a new drain read on the same cycle that the
    // previous registered FIFO response is being captured.
    wire drain_response_arriving = (out_rd_en_d != '0);

    wire drain_can_issue =
        !inflight ||
        ((DRAIN_ISSUE_ON_RETURN != 0) && drain_response_arriving);

    // Drain FSM
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state_drain <= D_IDLE;
            drain_busy <= 1'b0;

            out_rd_en <= '0;
            out_rd_en_d <= '0;

            col_pop <= '0;
            last_pop_col <= '0;
            inflight <= 1'b0;
            q0_v <= 1'b0;
            q1_v <= 1'b0;
            q0_col <= '0;
            col_pop_saturated <= 1'b0;
            q1_col <= '0;

            // D1 decoupled pipeline: reset pass-pending counter.
            r_pending <= 3'd0;

            // out_mem: ARRAY_M rows × ARRAY_N cols row-major.
            for (rr = 0; rr < ARRAY_M*ARRAY_N; rr = rr + 1)
                out_mem[rr] <= '0;

        end else begin
            // D1 decoupled pipeline: update r_pending based on pass_end and
            // drain_end pulses. Race-safe because both pulses can fire same
            // cycle (net zero) — 2'b11 case does nothing.
            if (state == S_IDLE) begin
                // Safety reset when compute FSM is between dispatches.
                r_pending <= 3'd0;
            end else begin
                case ({pass_end_pulse, drain_end_pulse})
                    2'b10: if (r_pending < 3'd7) r_pending <= r_pending + 3'd1;
                    2'b01: if (r_pending > 3'd0) r_pending <= r_pending - 3'd1;
                    default: ; // 2'b00 or 2'b11 (net zero)
                endcase
            end

            if (drain_clear_mem) begin
                for (rr = 0; rr < ARRAY_M*ARRAY_N; rr = rr + 1)
                    out_mem[rr] <= '0;
            end

            out_rd_en <= '0;
            out_rd_en_d <= out_rd_en;

            if (out_rd_en_d != '0) begin
                if (!q0_v) begin
                    q0_v <= 1'b1;
                    q0_col <= last_pop_col;
                    // ARRAY_M row values captured per drain cycle.
                    for (rr = 0; rr < ARRAY_M; rr = rr + 1)
                        q0_data[rr] <= out_dout[rr];
                end else if (!q1_v) begin
                    q1_v <= 1'b1;
                    q1_col <= last_pop_col;
                    for (rr = 0; rr < ARRAY_M; rr = rr + 1)
                        q1_data[rr] <= out_dout[rr];
                end
                inflight <= 1'b0;
            end

            if (q0_v && !drain_clear_mem) begin
                // Accumulate one drained column back into out_mem across all M rows.
                for (rr = 0; rr < ARRAY_M; rr = rr + 1)
                    out_mem[rr*ARRAY_N + q0_col] <= out_mem[rr*ARRAY_N + q0_col] + q0_data[rr];

                if (q1_v) begin
                    q0_col <= q1_col;
                    for (rr = 0; rr < ARRAY_M; rr = rr + 1)
                        q0_data[rr] <= q1_data[rr];
                    q1_v <= 1'b0;
                end else begin
                    q0_v <= 1'b0;
                end
            end

            case (state_drain)
                D_IDLE: begin
                    // D1 decoupled pipeline: auto-fire drain FSM when compute
                    // has queued a pass and no other drain is starting via
                    // drain_start pulse this cycle. Priority: drain_start
                    // pulse (backward compat) OR r_pending > 0 (queued drain).
                    if (drain_start || (r_pending > 3'd0)) begin
                        drain_busy <= 1'b1;
                        state_drain <= D_ACTIVE;
                        col_pop <= '0;
                        last_pop_col <= '0;
                        inflight <= 1'b0;
                    end
                end
                D_ACTIVE: begin
                    // INTERVENTION-Y1-A guard: only fire reads while there
                    // are columns remaining in THIS drain (col_pop_saturated=0).
                    // Once we've fired the read for col ARRAY_N-1, block
                    // further reads so any excess data in FIFOs (from an
                    // overlapping subsequent pass) stays there for the next
                    // auto-restart drain.
                    if (!buf_full && drain_can_issue && out_ready && !col_pop_saturated) begin
                        // Read all ARRAY_M row FIFOs simultaneously.
                        out_rd_en <= {ARRAY_M{1'b1}};
                        last_pop_col <= col_pop;
                        inflight <= 1'b1;
                        // Cycle through ARRAY_N columns.
                        if (col_pop != ARRAY_N-1)
                            col_pop <= col_pop + 1'b1;
                        else
                            col_pop_saturated <= 1'b1;   // 16th (=N) read fired
                    end

                    if (col_pop_saturated && !inflight && !q0_v && !q1_v) begin
                        // D1 decoupled pipeline: auto-restart if another pass
                        // is queued (r_pending decrements next cycle). If
                        // r_pending will still be > 0 after this drain
                        // completion decrement, immediately re-enter D_ACTIVE.
                        //
                        // The r_pending update logic elsewhere in this always
                        // block decrements by 1 this cycle. So checking
                        // r_pending > 1 tells us there is another pass queued
                        // beyond the one we're completing right now.
                        if (r_pending > 3'd1) begin
                            // Auto-restart for next queued pass.
                            state_drain <= D_ACTIVE;
                            drain_busy <= 1'b1;
                            col_pop <= '0;
                            last_pop_col <= '0;
                            inflight <= 1'b0;
                            col_pop_saturated <= 1'b0;
                        end else begin
                            state_drain <= D_IDLE;
                            drain_busy <= 1'b0;
                            col_pop_saturated <= 1'b0;
                        end
                    end
                end
            endcase
        end
    end

endmodule