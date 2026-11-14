`timescale 1ns/1ps
module tb_square_array;
    reg clk=0;
    always #5 clk=~clk;
    reg rst_n=0,en=0;
    reg [1:0] mode=2'b01;
    reg signed [5:0] unit_exp=0;
    wire signed [31:0] c [0:1][0:1];
    wire signed [31:0] acc_flat [0:3];
    wire signed [31:0] stream [0:1];
    wire [1:0] rows,cols,done_rows;
    wire done;
    wire signed [7:0] a [0:1];
    wire signed [7:0] b [0:1];
    assign a[0] = 8'h03; assign a[1] = 8'h03;
    assign b[0] = 8'h04; assign b[1] = 8'h04;
    systolic_array #(
       .D_W(8),.ACC_WIDTH(32),.N(2),
       .ARRAY_M(2),.ARRAY_N(2),.SQUARE_ENABLE(1)
    ) dut (
       .clk(clk),.rst(rst_n),.en(en),
       .square_mode(mode),.square_unit_exp(unit_exp),
       .a_in(a),.b_in(b),
       .c_out(c),.acc_out(acc_flat),.done(done),
       .active_row(rows),.active_column(cols),
       .row_done_pulse(done_rows),.acc_stream_out(stream)
    );
    integer i;
    initial begin
      repeat (3) @(negedge clk); rst_n=1;
      @(negedge clk); en=1;
      @(negedge clk); en=0;
      repeat (12) @(negedge clk);
      for (i=0;i<4;i=i+1) begin
         if (acc_flat[i] !== 32'sd3) begin
            $display("ERROR PE %0d acc=%0d",i,acc_flat[i]);
            $fatal(1);
         end
      end
      $display("PASS square 2x2 array integration");
      $finish;
    end
endmodule
