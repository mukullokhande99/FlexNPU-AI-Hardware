module mac_comp #(
    parameter int D_W       = 8,
    parameter int ACC_WIDTH = 16
)(
    input  wire                         clk,
    input  wire                         rst,   // active-low, synchronous
    input  wire                         en,
    input  wire signed [D_W-1:0]        a,
    input  wire signed [D_W-1:0]        b,
    // (* use_dsp = "yes" *): synthesis hint to place the i8*i8+i32 MACC
    // in a DSP block when the FPGA vendor toolchain supports it.
    // iverilog and verilator ignore the attribute, so behavior is unchanged.
    (* use_dsp = "yes" *)
    output reg  signed [ACC_WIDTH-1:0]  acc
);

    wire signed [(2*D_W)-1:0] prod = a * b;

    // robust sign-extend / truncate to ACC_WIDTH (no negative concat)
    wire signed [ACC_WIDTH-1:0] prod_ext = $signed(prod);

    reg en_d;
    wire en_rise = en & ~en_d;

    always @(posedge clk) begin
        if (!rst) begin
            acc  <= '0;
            en_d <= 1'b0;
        end else begin
            en_d <= en;

            if (en_rise) begin
                acc <= prod_ext;          // first cycle of pass
            end else if (en) begin
                acc <= acc + prod_ext;    // accumulate
            end
        end
    end
endmodule


// Wrapper PE with systolic forwarding and configurable delay
module mac #
(
    parameter D_W            = 8,
    parameter ACC_WIDTH      = 16,
    parameter SQUARE_ENABLE  = 0,
    parameter SQUARE_V1      = 0
    // parameter PROP_DELAY     = 1   // cycles to hold before propagating
)
(
    input  wire                  clk,
    input  wire                  rst,
    input  wire                  en,
    input  wire [1:0]            square_mode,
    input  wire signed [5:0]     square_unit_exp,
    input  wire signed [D_W-1:0]        a,
    input  wire signed [D_W-1:0]        b,
    output wire signed [ACC_WIDTH-1:0]  acc,

    // systolic pass-through (delayed by PROP_DELAY cycles)
    output wire signed [D_W-1:0]        a_out,
    output wire signed [D_W-1:0]        b_out,
    output wire                  en_out
);

    // --- Shift registers for delayed propagation ---
    reg signed [D_W-1:0] a_reg,  b_reg;
    reg           en_reg;
    // --- Core MAC computation ---
    generate
      if (SQUARE_ENABLE && SQUARE_V1) begin : g_square_v1
        square_partitioned_accumulator_v1 u_square_v1 (
            .clk(clk), .rst_n(rst), .en(en_reg),
            .a(a), .b(b), .mode(square_mode),
            .acc_unit_exp(square_unit_exp), .acc(acc)
        );
      end else if (SQUARE_ENABLE) begin : g_square
        // Explicit experimental mode. Requires D_W=8 and ACC_WIDTH=32.
        square_partitioned_accumulator u_square (
            .clk(clk), .rst_n(rst), .en(en_reg),
            .a(a), .b(b), .mode(square_mode),
            .acc_unit_exp(square_unit_exp), .acc(acc)
        );
      end else begin : g_exact
        mac_comp #(.D_W(D_W), .ACC_WIDTH(ACC_WIDTH)) u_mac4 (
            .clk(clk), .rst(rst), .en(en_reg),
            .a(a), .b(b), .acc(acc)
        );
      end
    endgenerate

`ifndef SYNTHESIS
    initial if (SQUARE_ENABLE && (D_W != 8 || ACC_WIDTH != 32))
        $fatal(1, "SQuARE requires D_W=8 and ACC_WIDTH=32");
`endif

    integer i;
    always @(posedge clk) begin
        if (!rst) begin
            a_reg <= 0;
            b_reg <= 0;
            en_reg <= 0;
        end else begin
            a_reg  <= a;
            b_reg <= b;
            en_reg <= en;
        end
    end

    // Outputs after PROP_DELAY cycles
    assign a_out  = a_reg;
    assign b_out  = b_reg;
    assign en_out = en_reg;

endmodule

