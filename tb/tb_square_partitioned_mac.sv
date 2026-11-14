// Standalone directed test: iverilog -g2012 -s tb_square_partitioned_mac
// -o /tmp/square.vvp tb/tb_square_partitioned_mac.sv rtl/square_partitioned_mac.sv
// && vvp /tmp/square.vvp
`timescale 1ns/1ps
module tb_square_partitioned_mac;
    reg clk=0;
    always #5 clk=~clk;
    reg rst_n=0, en=0;
    reg [7:0] a=0,b=0;
    reg [1:0] mode=0;
    reg signed [5:0] unit_exp=0;
    wire signed [31:0] acc,term;
    wire nar;
    square_partitioned_product dut_product(
       .a(a),.b(b),.mode(mode),.acc_unit_exp(unit_exp),
       .increment(term),.nar(nar)
    );
    square_partitioned_accumulator dut_acc(
       .clk(clk),.rst_n(rst_n),.en(en),
       .a(a),.b(b),.mode(mode),.acc_unit_exp(unit_exp),.acc(acc)
    );
    task check_term(input [1:0] m,input [7:0] x,y,
                    input signed [5:0] u,input signed [31:0] expected);
        begin
            mode=m; a=x; b=y; unit_exp=u; #1;
            if (term !== expected) begin
                $display("ERROR mode=%b a=%h b=%h unit=%0d term=%0d expected=%0d",
                  m,x,y,u,term,expected);
                $fatal(1);
            end
        end
    endtask
    initial begin
        check_term(2'b01,8'h03,8'h04,0,3); // FP4 1.5 * 2
        check_term(2'b00,8'h05,8'h06,0,8); // posit 2 * 4
        check_term(2'b00,8'h01,8'h01,0,0); // 1/256 truncated in integer units
        check_term(2'b10,8'h08,8'h00,0,0); // +1 + (-1)
        check_term(2'b11,8'h05,8'h00,0,3); // 1.5 + 1.5
        check_term(2'b10,8'h00,8'h00,-6'sd2,8); // 2 * 1 at unit=1/4
        check_term(2'b00,8'h08,8'h04,0,0);
        if (!nar) $fatal(1,"NaR not flagged");
        @(negedge clk); rst_n=1;
        check_term(2'b01,8'h03,8'h04,0,3);
        en=1; @(posedge clk); #1;
        if (acc !== 3) $fatal(1,"first accumulation: %0d",acc);
        @(posedge clk); #1;
        if (acc !== 6) $fatal(1,"second accumulation: %0d",acc);
        @(negedge clk); en=0; @(posedge clk); #1;
        @(negedge clk); en=1;
        check_term(2'b11,8'h05,8'h00,0,3);
        @(posedge clk); #1;
        if (acc !== 3) $fatal(1,"new dot product: %0d",acc);
        $display("PASS square_partitioned_mac directed tests");
        $finish;
    end
endmodule
