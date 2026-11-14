// mm — MAC matmul engine
//
// EdgeScope platformization Stage 3C (2026-08-28):
//   Independent ARRAY_M (rows) and ARRAY_N (cols) parameters. The
//   legacy single-N parameter is retained as backward-compat default
//   (ARRAY_M and ARRAY_N both default to N) so the trusted N=16 build
//   compiles byte-identical to the pre-Stage-3 source.
//
//   Structural changes vs single-N source:
//     - A-input FIFOs: count = ARRAY_M (one per row of A)
//     - B-input FIFOs: count = ARRAY_N (one per col of B)
//     - Output side (per-row FIFOs, streaming counters): count = ARRAY_M
//     - Streaming column counter: max value = ARRAY_N-1
//     - systolic_array port dimensions carry the split (see systolic.sv)

module mm #(
    parameter ACT_WIDTH = 4,
    parameter ACC_WIDTH = 16,
    parameter N = 8,
    parameter K = 8,
    parameter ACT_FIFO_DEPTH = 8,
    parameter OUT_FIFO_DEPTH = 32,
    parameter PIPE_LAT = 1,   // latency from row_done_pulse -> first valid acc_stream_out
    // EdgeScope Stage 3C: split M and N. Default preserves legacy N.
    parameter ARRAY_M = N,
    parameter ARRAY_N = N,
    parameter SQUARE_ENABLE = 0,
    parameter SQUARE_V1 = 0
)(
    input  wire                         clk,
    input  wire                         rst,
    input  wire                         active,
    input  wire [1:0]                   square_mode,
    input  wire signed [5:0]            square_unit_exp,

    // signed inputs into mm — ain has ARRAY_M rows, bin has ARRAY_N cols
    input  wire signed [ACT_WIDTH-1:0]  ain [0:ARRAY_M-1],
    input  wire signed [ACT_WIDTH-1:0]  bin [0:ARRAY_N-1],

    input  wire                         wr_en_act,
    input  wire                         wr_en_w,

    output wire                         done,
    output wire signed [ACC_WIDTH-1:0]  acc_out [0:ARRAY_M*ARRAY_N-1],

    // per-row output FIFO — one per systolic row (ARRAY_M)
    input  wire [ARRAY_M-1:0]           out_rd_en,
    output wire [ARRAY_M-1:0]           out_empty,
    output wire [ARRAY_M-1:0]           out_full,

    // signed outputs from output FIFOs
    output wire signed [ACC_WIDTH-1:0]  out_dout [0:ARRAY_M-1],
    output wire                         act_ready,
    output wire                         wgt_ready,
    output wire                         out_ready,

    // ------------------------------------------------------------
    // Passive observation port (2026-09-19 diag build). Truthful
    // "MM datapath is doing useful work THIS cycle" predicate.
    //   __active     : 2-cycle-delayed `active`; drives systolic.en
    //                  (MAC pipeline is clocking operands forward).
    //   |streaming   : at least one row is in output-drain window
    //                  (row_done_pulse triggered; ARRAY_N cycles
    //                  of accumulator readout + PIPE_LAT wait).
    //   |stream_valid: at least one row is actively writing to its
    //                  output FIFO (subset of streaming).
    // Any of these three => real datapath activity, either at the
    // multiply stage or the accumulator-drain stage. Purely passive;
    // NO functional effect on mm behavior.
    output wire                         o_perf_mm_active,

    // Streaming-safety port (2026-09-19 optimization).
    // High when every row's output stream has completed writing to the
    // per-row output FIFOs (equivalently: after the last mm.streaming[r]
    // deasserts). This is a stricter (later) condition than the systolic
    // pipeline's `done`, because it also guarantees that the systolic
    // per-PE accumulator has been fully read by the streaming FSM.
    //
    // Consumer contract: this is the moment when it is SAFE for the
    // caller (compute_tile_bram) to begin the next K-tile pass without
    // corrupting the current pass's not-yet-latched accumulator reads.
    // See §1 of HW_INTERVENTION_Y1_20260919.md for the legality proof.
    output wire                         o_all_streaming_done
);

    // ============================================================
    // Input FIFOs — separate loops for M (A-side) and N (B-side)
    // ============================================================
    wire [ACT_WIDTH-1:0] act_fifo_a_u [0:ARRAY_M-1];
    wire [ACT_WIDTH-1:0] act_fifo_b_u [0:ARRAY_N-1];

    wire signed [ACT_WIDTH-1:0] act_fifo_a_out [0:ARRAY_M-1];
    wire signed [ACT_WIDTH-1:0] act_fifo_b_out [0:ARRAY_N-1];

    // active_row drives A-FIFO reads (ARRAY_M); active_column drives B-FIFO reads (ARRAY_N)
    wire [ARRAY_M-1:0] active_row;
    wire [ARRAY_N-1:0] active_column;

    wire [ARRAY_M-1:0] act_full;
    wire [ARRAY_N-1:0] wgt_full;

    assign act_ready = ~|act_full;
    assign wgt_ready = ~|wgt_full;

    genvar i;
    generate
        // A-side FIFOs: ARRAY_M
        for (i = 0; i < ARRAY_M; i = i + 1) begin : A_IN_FIFOS
            fifo #(.WIDTH(ACT_WIDTH), .DEPTH(ACT_FIFO_DEPTH)) fifo_a (
                .clk   (clk),
                .rst   (rst),
                .wr_en (wr_en_act),
                .rd_en (active_row[i]),
                .din   (ain[i][ACT_WIDTH-1:0]),
                .dout  (act_fifo_a_u[i]),
                .full  (act_full[i]),
                .empty ()
            );
            assign act_fifo_a_out[i] = $signed(act_fifo_a_u[i]);
        end

        // B-side FIFOs: ARRAY_N
        for (i = 0; i < ARRAY_N; i = i + 1) begin : B_IN_FIFOS
            fifo #(.WIDTH(ACT_WIDTH), .DEPTH(ACT_FIFO_DEPTH)) fifo_b (
                .clk   (clk),
                .rst   (rst),
                .wr_en (wr_en_w),
                .rd_en (active_column[i]),
                .din   (bin[i][ACT_WIDTH-1:0]),
                .dout  (act_fifo_b_u[i]),
                .full  (wgt_full[i]),
                .empty ()
            );
            assign act_fifo_b_out[i] = $signed(act_fifo_b_u[i]);
        end
    endgenerate

    // ============================================================
    // Active pipeline (unchanged)
    // ============================================================
    reg _active, __active;
    always @(posedge clk or negedge rst) begin
        if (!rst) begin
            _active  <= 1'b0;
            __active <= 1'b0;
        end else begin
            _active  <= active;
            __active <= _active;
        end
    end

    // ============================================================
    // Systolic array — carries ARRAY_M / ARRAY_N through
    // ============================================================
    wire signed [ACC_WIDTH-1:0] acc_stream_out [0:ARRAY_M-1];
    wire [ARRAY_M-1:0]          row_done_pulse;

    systolic_array #(
        .D_W(ACT_WIDTH),
        .ACC_WIDTH(ACC_WIDTH),
        .N(N),
        .ARRAY_M(ARRAY_M),
        .ARRAY_N(ARRAY_N),
        .SQUARE_ENABLE(SQUARE_ENABLE),
        .SQUARE_V1(SQUARE_V1)
    ) systolic_inst (
        .clk            (clk),
        .rst            (rst),
        .en             (__active),
        .square_mode    (square_mode),
        .square_unit_exp(square_unit_exp),

        .a_in            (act_fifo_a_out),
        .b_in            (act_fifo_b_out),

        .done           (done),

        .acc_out         (acc_out),

        .acc_stream_out  (acc_stream_out),
        .row_done_pulse  (row_done_pulse),

        .active_row      (active_row),
        .active_column   (active_column)
    );

    // ============================================================
    // Stream control — per-row (ARRAY_M) streaming of ARRAY_N cols
    // ============================================================
    reg [ARRAY_M-1:0] streaming;
    reg [ARRAY_M-1:0] stream_valid;
    reg [$clog2(ARRAY_N):0]      cnt [0:ARRAY_M-1];
    reg [$clog2(PIPE_LAT+1):0]   lat_cnt [0:ARRAY_M-1];

    integer r;
    always @(posedge clk or negedge rst) begin
        if (!rst) begin
            streaming    <= '0;
            stream_valid <= '0;
            for (r = 0; r < ARRAY_M; r = r + 1) begin
                cnt[r]     <= '0;
                lat_cnt[r] <= '0;
            end
        end else begin
            for (r = 0; r < ARRAY_M; r = r + 1) begin
                // start stream
                if (row_done_pulse[r]) begin
                    streaming[r]    <= 1'b1;
                    stream_valid[r] <= 1'b0;
                    cnt[r]          <= '0;
                    lat_cnt[r]      <= '0;
                end

                // wait for pipeline latency
                if (streaming[r] && !stream_valid[r]) begin
                    if (PIPE_LAT == 0) begin
                        stream_valid[r] <= 1'b1;
                    end else if (lat_cnt[r] == PIPE_LAT-1) begin
                        stream_valid[r] <= 1'b1;
                    end else begin
                        lat_cnt[r] <= lat_cnt[r] + 1'b1;
                    end
                end

                // stream data for ARRAY_N cycles per row
                if (stream_valid[r]) begin
                    if (cnt[r] == ARRAY_N-1) begin
                        streaming[r]    <= 1'b0;
                        stream_valid[r] <= 1'b0;
                        cnt[r]          <= '0;
                    end else begin
                        cnt[r] <= cnt[r] + 1'b1;
                    end
                end
            end
        end
    end

    // ============================================================
    // Output FIFOs: one per row (ARRAY_M)
    // ============================================================
    wire [ARRAY_M-1:0] out_wr_en;
    generate
        for (i = 0; i < ARRAY_M; i = i + 1) begin : OUT_WR
            assign out_wr_en[i] = stream_valid[i] & ~out_full[i];
        end
    endgenerate

    wire [ACC_WIDTH-1:0] out_dout_u [0:ARRAY_M-1];
    assign out_ready = &(~out_empty);

    // Passive MM-active observation (see port comment above).
    assign o_perf_mm_active = __active | (|streaming) | (|stream_valid);

    // Streaming-safety observation: high when NO row is currently in
    // its output-drain window (streaming[r]==0 for all r AND
    // stream_valid[r]==0 for all r). Purely combinational; passive.
    assign o_all_streaming_done = ~(|streaming) & ~(|stream_valid);

    generate
        for (i = 0; i < ARRAY_M; i = i + 1) begin : OUT_FIFOS
            fifo #(
                .WIDTH(ACC_WIDTH),
                .DEPTH(OUT_FIFO_DEPTH)
            ) out_fifo (
                .clk   (clk),
                .rst   (rst),
                .wr_en (out_wr_en[i]),
                .rd_en (out_rd_en[i]),

                .din   (acc_stream_out[i][ACC_WIDTH-1:0]),
                .dout  (out_dout_u[i]),

                .full  (out_full[i]),
                .empty (out_empty[i])
            );

            assign out_dout[i] = $signed(out_dout_u[i]);
        end
    endgenerate

endmodule
