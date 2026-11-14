// systolic_array
//
// EdgeScope platformization Stage 3C (2026-08-28):
//   Independent ARRAY_M (rows) and ARRAY_N (cols) parameters. The
//   legacy single-N parameter is retained as backward-compat default
//   (ARRAY_M and ARRAY_N both default to N) so the trusted N=16 build
//   compiles byte-identical to the pre-Stage-3 source.
//
//   Signal semantics:
//     a_in [0:ARRAY_M-1]     — one A operand per row
//     b_in [0:ARRAY_N-1]     — one B operand per col
//     c_out[0:ARRAY_M-1]     — one accumulator per (row,col) MAC
//          [0:ARRAY_N-1]
//     acc_out[0:ARRAY_M*ARRAY_N-1]  flattened, row-major
//     active_row[ARRAY_M-1:0]       — one per systolic row
//     active_column[ARRAY_N-1:0]    — one per systolic col
//     row_done_pulse[ARRAY_M-1:0]   — one per row (drain trigger)
//     acc_stream_out[0:ARRAY_M-1]   — per-row stream engine (each
//                                     row streams ARRAY_N columns)
module systolic_array #(
    parameter D_W        = 4,
    parameter ACC_WIDTH  = 16,
    parameter N          = 2,
    // EdgeScope Stage 3C: split M and N. Default preserves legacy N.
    parameter ARRAY_M    = N,
    parameter ARRAY_N    = N,
    parameter SQUARE_ENABLE = 0,
    parameter SQUARE_V1 = 0
)(
    input  wire                    clk,
    input  wire                    rst,
    input  wire                    en,
    input  wire [1:0]              square_mode,
    input  wire signed [5:0]       square_unit_exp,

    input  wire signed [D_W-1:0]          a_in [0:ARRAY_M-1],
    input  wire signed [D_W-1:0]          b_in [0:ARRAY_N-1],

    output wire signed [ACC_WIDTH-1:0]    c_out [0:ARRAY_M-1][0:ARRAY_N-1],
    output wire signed [ACC_WIDTH-1:0]    acc_out [0:ARRAY_M*ARRAY_N-1],

    output reg                     done,

    output wire [ARRAY_M-1:0]      active_row,
    output wire [ARRAY_N-1:0]      active_column,

    // per-row drain trigger (one pulse per M row when its rightmost MAC finishes)
    output wire [ARRAY_M-1:0]      row_done_pulse,
    // per-row streaming output engine (each row streams ARRAY_N cols)
    output reg  signed [ACC_WIDTH-1:0]    acc_stream_out [0:ARRAY_M-1]
);

    // ------------------------------------------------------------
    // Internal systolic signals — ARRAY_M rows × ARRAY_N cols
    // ------------------------------------------------------------
    wire signed [D_W-1:0]  a_sig     [0:ARRAY_M][0:ARRAY_N-1];
    wire signed [D_W-1:0]  a_sig_out [0:ARRAY_M][0:ARRAY_N-1];
    wire signed [D_W-1:0]  b_sig     [0:ARRAY_M-1][0:ARRAY_N];
    wire signed [D_W-1:0]  b_sig_out [0:ARRAY_M-1][0:ARRAY_N];
    wire           en_sig    [0:ARRAY_M][0:ARRAY_N-1];
    wire           en_sig_out[0:ARRAY_M][0:ARRAY_N-1];

    genvar r, c;
    generate
        for (r = 0; r < ARRAY_M; r = r + 1) begin : row
            for (c = 0; c < ARRAY_N; c = c + 1) begin : col
                mac #(
                    .D_W(D_W),
                    .ACC_WIDTH(ACC_WIDTH),
                    .SQUARE_ENABLE(SQUARE_ENABLE),
                    .SQUARE_V1(SQUARE_V1)
                ) pe (
                    .clk(clk),
                    .rst(rst),
                    .en(en_sig[r][c]),
                    .square_mode(square_mode),
                    .square_unit_exp(square_unit_exp),
                    .a(a_sig[r][c]),
                    .b(b_sig[r][c]),
                    .acc(c_out[r][c]),
                    .a_out(a_sig_out[r][c]),
                    .b_out(b_sig_out[r][c]),
                    .en_out(en_sig_out[r][c])
                );

                assign en_sig[r][c] =
                    (r==0 && c==0) ? en :
                    (c==0) ? en_sig_out[r-1][c] :
                             en_sig_out[r][c-1];

                assign a_sig[r][c] = (c == 0) ? a_in[r] : a_sig_out[r][c-1];
                assign b_sig[r][c] = (r == 0) ? b_in[c] : b_sig_out[r-1][c];
            end
        end
    endgenerate

    // ------------------------------------------------------------
    // done — asserted one cycle after the (ARRAY_M-1, ARRAY_N-1)
    // corner MAC deasserts its enable
    // ------------------------------------------------------------
    wire done_tmp;
    assign done_tmp = en_sig_out[ARRAY_M-1][ARRAY_N-1] & ~en_sig[ARRAY_M-1][ARRAY_N-1];

    always @(posedge clk or negedge rst)
        if (!rst) done <= 1'b0;
        else      done <= done_tmp;

    // ------------------------------------------------------------
    // activity signals
    // ------------------------------------------------------------
    genvar rr, cc;
    generate
        for (rr = 0; rr < ARRAY_M; rr = rr + 1)
            assign active_row[rr] = en_sig[rr][0];

        for (cc = 0; cc < ARRAY_N; cc = cc + 1)
            assign active_column[cc] = en_sig[0][cc];
    endgenerate

    // ------------------------------------------------------------
    // flatten outputs row-major
    // ------------------------------------------------------------
    genvar fr, fc;
    generate
        for (fr = 0; fr < ARRAY_M; fr = fr + 1)
            for (fc = 0; fc < ARRAY_N; fc = fc + 1)
                assign acc_out[fr*ARRAY_N + fc] = c_out[fr][fc];
    endgenerate

    // ============================================================
    // row_done_pulse — one pulse per row when its rightmost
    // (ARRAY_N-1)th MAC deasserts enable
    // ============================================================
    wire [ARRAY_M-1:0] row_last_en;
    reg  [ARRAY_M-1:0] row_last_en_d;

    generate
        for (rr = 0; rr < ARRAY_M; rr = rr + 1)
            assign row_last_en[rr] = en_sig_out[rr][ARRAY_N-1];
    endgenerate

    always @(posedge clk or negedge rst)
        if (!rst) row_last_en_d <= '0;
        else      row_last_en_d <= row_last_en;

    generate
        for (rr = 0; rr < ARRAY_M; rr = rr + 1)
            assign row_done_pulse[rr] = row_last_en_d[rr] & ~row_last_en[rr];
    endgenerate

    // ============================================================
    // per-row stream-out engine — each row streams ARRAY_N cols
    // ============================================================
    reg [ARRAY_M-1:0]              streaming;
    reg [$clog2(ARRAY_N)-1:0]      col_ptr [0:ARRAY_M-1];

    integer r_int;
    always @(posedge clk or negedge rst) begin
        if (!rst) begin
            streaming <= '0;
            for (r_int = 0; r_int < ARRAY_M; r_int = r_int + 1) begin
                col_ptr[r_int] <= '0;
                acc_stream_out[r_int] <= '0;
            end
        end else begin
            for (r_int = 0; r_int < ARRAY_M; r_int = r_int + 1) begin
                if (row_done_pulse[r_int]) begin
                    streaming[r_int] <= 1'b1;
                    col_ptr[r_int]   <= '0;
                end

                if (streaming[r_int]) begin
                    acc_stream_out[r_int] <= c_out[r_int][col_ptr[r_int]];

                    if (col_ptr[r_int] == ARRAY_N-1) begin
                        streaming[r_int] <= 1'b0;
                        col_ptr[r_int]   <= '0;
                    end else begin
                        col_ptr[r_int] <= col_ptr[r_int] + 1'b1;
                    end
                end else begin
                    acc_stream_out[r_int] <= '0;
                end
            end
        end
    end

endmodule
