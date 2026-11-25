// Experimental SQuARE/FlexNPU shared low-precision product fabric.
// No Q8.8 output. Exactly one signed INT32 accumulating register per PE.
// mode: 00 Posit(4,1), 01 FP4 E2M1, 10 dual FP2 E1M0, 11 dual FP2 E0M1.
// FP2 packing (per 8-bit operand): [1:0] lane0, [3:2] lane1,
// [4] zero lane0, [5] zero lane1. Not the compressed shared-sign FP2 format.
// acc_unit_exp describes the integer unit: stored increment is
// trunc_towards_zero((a0*b0 + a1*b1) / 2**acc_unit_exp).
// acc_unit_exp=0 => true integer units. No hidden fractional accumulator.
// The two FP2 products are added BEFORE truncation.
// No subcycle partial-sum persistence is implemented.
module square_partitioned_product (
    input  wire [7:0] a, b,
    input  wire [1:0] mode,
    input  wire signed [5:0] acc_unit_exp,
    output reg signed [31:0] increment,
    output wire nar
);
    wire simd = mode[1];
    wire fp2_e0m1 = mode[0];

    function automatic signed [5:0] posit_exp(input [3:0] code);
        begin
            case (code)
                4'h1, 4'hf: posit_exp = -6'sd4;
                4'h2, 4'he: posit_exp = -6'sd2;
                4'h3, 4'hd: posit_exp = -6'sd1;
                4'h4, 4'hc: posit_exp =  6'sd0;
                4'h5, 4'hb: posit_exp =  6'sd1;
                4'h6, 4'ha: posit_exp =  6'sd2;
                4'h7, 4'h9: posit_exp =  6'sd4;
                default:    posit_exp =  6'sd0;
            endcase
        end
    endfunction

    function automatic [1:0] fp4_sig(input [3:0] x);
        begin
            fp4_sig = (x[2:1] == 2'b00) ? {1'b0,x[0]} : {1'b1,x[0]};
        end
    endfunction

    function automatic signed [5:0] fp4_exp(input [3:0] x);
        begin
            case (x[2:1])
                2'b00,2'b01: fp4_exp = -6'sd1;
                2'b10:       fp4_exp =  6'sd0;
                default:     fp4_exp =  6'sd1;
            endcase
        end
    endfunction

    function automatic [1:0] fp2_mag(input [1:0] x, input zero, input e0m1);
        begin
            if (zero) fp2_mag = 2'd0;
            else if (e0m1) fp2_mag = x[0] ? 2'd3 : 2'd2;
            else fp2_mag = x[0] ? 2'd1 : 2'd2;
        end
    endfunction

    // Significand multiplier cells are shared by precision mode.
    // P0: scalar FP4/Posit or SIMD lane0. P1: SIMD lane1 ONLY.
    wire [1:0] ma0 = simd ? fp2_mag(a[1:0],a[4],fp2_e0m1) :
                     (mode == 2'b01) ? fp4_sig(a[3:0]) :
                     ((a[3:0] == 4'h0 || a[3:0] == 4'h8) ? 2'd0 : 2'd1);
    wire [1:0] mb0 = simd ? fp2_mag(b[1:0],b[4],fp2_e0m1) :
                     (mode == 2'b01) ? fp4_sig(b[3:0]) :
                     ((b[3:0] == 4'h0 || b[3:0] == 4'h8) ? 2'd0 : 2'd1);
    wire [1:0] ma1 = simd ? fp2_mag(a[3:2],a[5],fp2_e0m1) : 2'd0;
    wire [1:0] mb1 = simd ? fp2_mag(b[3:2],b[5],fp2_e0m1) : 2'd0;
    wire [3:0] p0 = ma0 * mb0;
    wire [3:0] p1 = ma1 * mb1;

    wire s0 = simd ? (a[1] ^ b[1]) : (a[3] ^ b[3]);
    wire s1 = a[3] ^ b[3];
    wire signed [5:0] c0 = s0 ? -$signed({2'b00,p0}) : $signed({2'b00,p0});
    wire signed [5:0] c1 = s1 ? -$signed({2'b00,p1}) : $signed({2'b00,p1});
    wire signed [5:0] coefficient = simd ? (c0+c1) : c0;

    wire signed [6:0] exponent = simd ? -7'sd2 :
        (mode == 2'b01) ?
            ($signed(fp4_exp(a[3:0])) + $signed(fp4_exp(b[3:0]))) :
            ($signed(posit_exp(a[3:0])) + $signed(posit_exp(b[3:0])));
    wire signed [7:0] shift = exponent - $signed(acc_unit_exp);
    assign nar = (mode == 2'b00) &&
                 ((a[3:0] == 4'h8) || (b[3:0] == 4'h8));

    // Signed shift with truncation toward zero, including negative terms.
    // Left shift wraps modulo 2^32 (the INT32 accumulator also wraps).
    // Guard shift magnitudes so simulator and synthesis agree.
    reg signed [31:0] base;
    reg [31:0] magnitude;
    reg [7:0] right_shift;
    always @* begin
        base = {{26{coefficient[5]}},coefficient};
        magnitude = coefficient[5] ? $unsigned(-base) : $unsigned(base);
        right_shift = $unsigned(-shift);
        if (nar) increment = 32'sd0;
        else if (shift >= 0)
            increment = (shift >= 8'sd32) ? 32'sd0 : (base <<< shift);
        else if (right_shift >= 8'd32)
            increment = 32'sd0;
        else
            increment = coefficient[5] ?
                -$signed(magnitude >> right_shift) :
                 $signed(magnitude >> right_shift);
    end
endmodule

// Drop-in *computation* cell for the original FlexNPU mac wrapper.
// Keeps the original rising-enable "first product overwrites accumulator"
// convention. Exactly one accumulator flop bank for every precision mode.
module square_partitioned_accumulator (
    input wire clk,
    input wire rst_n,
    input wire en,
    input wire [7:0] a,b,
    input wire [1:0] mode,
    input wire signed [5:0] acc_unit_exp,
    output reg signed [31:0] acc
);
    wire signed [31:0] increment;
    wire nar;
    reg en_d;
    square_partitioned_product u_product(
       .a(a),.b(b),.mode(mode),.acc_unit_exp(acc_unit_exp),
       .increment(increment),.nar(nar)
    );
    always @(posedge clk) begin
        if (!rst_n) begin
            acc <= 32'sd0;
            en_d <= 1'b0;
        end else begin
            en_d <= en;
            if (en && !en_d)
                acc <= increment;
            else if (en)
                acc <= acc + increment;
        end
    end
endmodule
