`timescale 1ns/1ps
module tb_square_mac_stream;
 reg clk=0,rst=0,en=0; reg [7:0] a=0,b=0; reg [1:0] mode=1; reg signed [5:0] unit_exp=0;
 wire signed [31:0] acc0,acc1,increment; wire nar;
 wire signed [7:0] ao0,bo0,ao1,bo1; wire eo0,eo1;
 reg previous_en=0,previous_core_en=0; reg [31:0] expected=0;
 always #5 clk=~clk;
 mac #(.D_W(8),.ACC_WIDTH(32),.SQUARE_ENABLE(1),.SQUARE_V1(0)) u0
   (.clk(clk),.rst(rst),.en(en),.square_mode(mode),.square_unit_exp(unit_exp),.a(a),.b(b),.acc(acc0),.a_out(ao0),.b_out(bo0),.en_out(eo0));
 mac #(.D_W(8),.ACC_WIDTH(32),.SQUARE_ENABLE(1),.SQUARE_V1(1)) u1
   (.clk(clk),.rst(rst),.en(en),.square_mode(mode),.square_unit_exp(unit_exp),.a(a),.b(b),.acc(acc1),.a_out(ao1),.b_out(bo1),.en_out(eo1));
 square_partitioned_product product(.a(a),.b(b),.mode(mode),.acc_unit_exp(unit_exp),.increment(increment),.nar(nar));
 always @(posedge clk) begin
   if (!rst) begin previous_en=0; previous_core_en=0; expected=0; end
   else begin
     if (previous_en && !previous_core_en) expected=increment;
     else if (previous_en) expected=expected+increment;
     previous_core_en=previous_en;
     previous_en=en;
   end
   #1;
   if (acc0!==expected || acc1!==expected) $fatal(1,"stream mismatch exp=%h v0=%h v1=%h",expected,acc0,acc1);
 end
 task drive(input bit e,input [7:0] x,input [7:0] y);
   begin @(negedge clk); en=e; a=x; b=y; end
 endtask
 initial begin
   repeat(2) @(negedge clk); rst=1;
   drive(1,8'h02,8'h02); drive(1,8'h03,8'h04);
   drive(1,8'h0a,8'h04); drive(0,8'h04,8'h04);
   drive(0,8'h00,8'h00); drive(1,8'h02,8'h04);
   drive(1,8'h04,8'h04); drive(0,8'h03,8'h04);
   repeat(4) drive(0,0,0);
   // Posit 1*1 in units 2^-30; four terms wrap the INT32 accumulator.
   @(negedge clk); mode=0; unit_exp=-30;
   drive(1,4,4); drive(1,4,4); drive(1,4,4); drive(1,4,4);
   drive(0,4,4); repeat(4) drive(0,0,0);
   $display("PASS V0/V1 MAC stream and wrap"); $finish;
 end
endmodule
