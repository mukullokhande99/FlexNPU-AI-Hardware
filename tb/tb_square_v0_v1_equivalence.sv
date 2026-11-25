`timescale 1ns/1ps
// Exhaustive encoded-input equivalence for V0 vs V1 products,
// and every signed six-bit integer-unit exponent; no external dependencies.
module tb_square_v0_v1_equivalence;
    reg [7:0] a,b;
    reg [1:0] mode;
    reg signed [5:0] acc_unit_exp;
    wire signed [31:0] inc0,inc1;
    wire nar0,nar1;
    integer m,ai,bi,sc,cases;
    square_partitioned_product u_v0 (
        .a(a),.b(b),.mode(mode),.acc_unit_exp(acc_unit_exp),
        .increment(inc0),.nar(nar0)
    );
    square_partitioned_product_v1 u_v1 (
        .a(a),.b(b),.mode(mode),.acc_unit_exp(acc_unit_exp),
        .increment(inc1),.nar(nar1)
    );
    initial begin
        cases=0;
        for (m=0;m<4;m=m+1) begin
          mode=m;
          for (ai=0;ai<((m<2)?16:64);ai=ai+1) begin
            for (bi=0;bi<((m<2)?16:64);bi=bi+1) begin
              a=ai; b=bi;
              for (sc=-32;sc<32;sc=sc+1) begin
                acc_unit_exp=sc;
                #1;
                cases=cases+1;
                if (inc0!==inc1 || nar0!==nar1) begin
                    $display("FAIL mode=%0d a=%h b=%h exp=%0d V0=%0d V1=%0d nar=%b/%b",
                        m,a,b,acc_unit_exp,inc0,inc1,nar0,nar1);
                    $fatal(1);
                end
              end
            end
          end
        end
        $display("PASS V0==V1: %0d exhaustive/all-scale comparisons",cases);
        $finish;
    end
endmodule

