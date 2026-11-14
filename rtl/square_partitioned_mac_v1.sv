// V1: optimized restricted-E4M3-compatible product core.
//
// Exactly ONE signed INT32 accumulator in the companion accumulator module.
// FP2 SIMD uses two 2x2-bit multiplication cells, combined into ONE increment.
// Common scalar/FP2 lane 0 multiplier reuses the same hardware resources.
//
// Optimization vs V0:
// - 4-bit exponent decode, 5-bit product exponent, 7-bit scale delta
// - common sign/magnitude (5-bit magnitude) before product alignment
// - narrow right-shift (5 bits) rather than 32-bit right-shift
// - single 32-bit left-shift cone; no signed wide shifter
// - single sign restore after alignment
//
// Policy remains exact V0: truncate toward zero PER ISSUE, modulo-2^32
// on left shifting / INT32 accumulator; NaR contributes zero.
// acc_unit_exp is signed, stable for entire dot product.
// This core is not an IEEE FP8 arithmetic implementation.
module square_partitioned_product_v1 (
    input  wire [7:0] a, b,
    input  wire [1:0] mode,
    input  wire signed [5:0] acc_unit_exp,
    output wire signed [31:0] increment,
    output wire nar
);
    wire simd = mode[1];
    wire scalar_fp4 = (mode == 2'b01);
    wire fp2_e0m1 = mode[0];

    function automatic signed [3:0] pexp(input [3:0] x);
        begin
            case (x[2:0])
                3'b000: pexp = 4'sd0; // zero, NaR also masked
                3'b001: pexp = -4'sd4;
                3'b010: pexp = -4'sd2;
                3'b011: pexp = -4'sd1;
                3'b100: pexp = 4'sd0;
                3'b101: pexp = 4'sd1;
                3'b110: pexp = 4'sd2;
                3'b111: pexp = 4'sd4;
            endcase
        end
    endfunction
    // Negative posit codes mirror positive-code exponent magnitudes:
    // e(9)=+4, e(A)=+2, e(B)=+1, e(C)=0, e(D)=-1, e(E)=-2, e(F)=-4.
    function automatic signed [3:0] posit_exp(input [3:0] x);
        begin
            case (x)
                4'h1,4'hf: posit_exp=-4'sd4;
                4'h2,4'he: posit_exp=-4'sd2;
                4'h3,4'hd: posit_exp=-4'sd1;
                4'h5,4'hb: posit_exp=4'sd1;
                4'h6,4'ha: posit_exp=4'sd2;
                4'h7,4'h9: posit_exp=4'sd4;
                default:   posit_exp=4'sd0;
            endcase
        end
    endfunction
    function automatic signed [2:0] fp4_exp(input [3:0] x);
        begin
            case (x[2:1])
                2'b00,2'b01: fp4_exp=-3'sd1;
                2'b10: fp4_exp=3'sd0;
                default: fp4_exp=3'sd1;
            endcase
        end
    endfunction
    function automatic [1:0] fp2mag(input [1:0] x,input z,input e0m1);
        begin
            if (z) fp2mag=2'd0;
            else if (e0m1) fp2mag=x[0] ? 2'd3 : 2'd2;
            else fp2mag=x[0] ? 2'd1 : 2'd2;
        end
    endfunction

    wire [1:0] xa0 = simd ? fp2mag(a[1:0],a[4],fp2_e0m1)
                      : scalar_fp4 ? {a[2:1]!=0,a[0]}
                      : ((a[3:0]==4'h0)||(a[3:0]==4'h8)) ? 2'd0 : 2'd1;
    wire [1:0] xb0 = simd ? fp2mag(b[1:0],b[4],fp2_e0m1)
                      : scalar_fp4 ? {b[2:1]!=0,b[0]}
                      : ((b[3:0]==4'h0)||(b[3:0]==4'h8)) ? 2'd0 : 2'd1;
    wire [1:0] xa1 = simd ? fp2mag(a[3:2],a[5],fp2_e0m1) : 2'd0;
    wire [1:0] xb1 = simd ? fp2mag(b[3:2],b[5],fp2_e0m1) : 2'd0;
    wire [3:0] p0 = xa0*xb0;
    wire [3:0] p1 = xa1*xb1;

    wire sign0 = simd ? a[1]^b[1] : a[3]^b[3];
    wire sign1 = a[3]^b[3];
    wire signed [5:0] t0 = sign0 ? -$signed({2'b00,p0}) : $signed({2'b00,p0});
    wire signed [5:0] t1 = sign1 ? -$signed({2'b00,p1}) : $signed({2'b00,p1});
    wire signed [5:0] sum = t0 + t1; // p1=0 in scalar
    wire negative = sum[5];
    // 6-bit abs supports maximum lane sum 18.
    wire [5:0] abs_sum = negative ? -sum : sum;

    wire signed [4:0] e_posit =
        $signed(posit_exp(a[3:0])) + $signed(posit_exp(b[3:0]));
    wire signed [4:0] e_fp4 =
        $signed(fp4_exp(a[3:0])) + $signed(fp4_exp(b[3:0]));
    wire signed [4:0] exp_prod = simd ? -5'sd2 :
                                          scalar_fp4 ? e_fp4 : e_posit;
    // exp_prod [-8,8] minus acc_unit_exp [-32,31] => [-39,40].
    wire signed [6:0] delta = $signed(exp_prod) - $signed(acc_unit_exp);

    assign nar = (mode==2'b00) &&
                 ((a[3:0]==4'h8)||(b[3:0]==4'h8));

    wire [5:0] left_n = delta[5:0];
    wire [5:0] right_n = -delta;
    // Narrow right shift: only six bits of magnitude can be nonzero.
    wire [5:0] right_mag = (right_n>=6'd6) ? 6'd0 :
                           abs_sum >> right_n[2:0];
    // The left-shift input has only 6 information bits even though
    // the output must remain 32 bits to support all integer units.
    wire [31:0] left_mag = (delta>=7'sd32) ? 32'd0 :
                           ({{26{1'b0}},abs_sum} << left_n[4:0]);
    wire [31:0] unsigned_inc = delta[6] ? {26'd0,right_mag} : left_mag;
    assign increment = nar ? 32'sd0 :
                       negative ? -$signed(unsigned_inc) : $signed(unsigned_inc);
endmodule

module square_partitioned_accumulator_v1 (
    input wire clk, rst_n, en,
    input wire [7:0] a,b,
    input wire [1:0] mode,
    input wire signed [5:0] acc_unit_exp,
    output reg signed [31:0] acc
);
    wire signed [31:0] increment;
    wire nar;
    reg en_d;
    square_partitioned_product_v1 u_product(
        .a(a),.b(b),.mode(mode),.acc_unit_exp(acc_unit_exp),
        .increment(increment),.nar(nar)
    );
    always @(posedge clk) begin
        if (!rst_n) begin acc<=32'sd0; en_d<=1'b0; end
        else begin
            en_d<=en;
            if (en && !en_d) acc<=increment;
            else if (en) acc<=acc+increment;
        end
    end
endmodule
