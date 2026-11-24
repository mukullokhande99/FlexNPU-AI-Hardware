`timescale 1ns/1ps
module tb_fifo_boundary;
 reg clk=0, rst=0, wr=0, rd=0; reg [7:0] din=0;
 wire [7:0] dout; wire full, empty;
 always #5 clk=~clk;
 fifo #(.WIDTH(8),.DEPTH(3)) dut(.clk(clk),.rst(rst),.wr_en(wr),.rd_en(rd),.din(din),.dout(dout),.full(full),.empty(empty));
 task step(input bit w, input bit r, input [7:0] x);
   begin @(negedge clk); wr=w; rd=r; din=x; @(posedge clk); #1; end
 endtask
 task flags(input bit f,input bit e);
   if (full!==f || empty!==e) $fatal(1,"flags full=%b empty=%b",full,empty);
 endtask
 initial begin
   repeat (2) @(negedge clk); rst=1;
   step(1,1,8'h11); flags(0,0); // empty: write only
   step(1,0,8'h22); step(1,0,8'h33); flags(1,0);
   step(1,1,8'h44); if(dout!==8'h11) $fatal; flags(0,0); // full: read only
   step(1,1,8'h44); if(dout!==8'h22) $fatal;
   step(0,1,0); if(dout!==8'h33) $fatal;
   step(0,1,0); if(dout!==8'h44) $fatal; flags(0,1);
   step(0,1,0); flags(0,1);
   $display("PASS FIFO boundary and non-power-of-two wrap"); $finish;
 end
endmodule
