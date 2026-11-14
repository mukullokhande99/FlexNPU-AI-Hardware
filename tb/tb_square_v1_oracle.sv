`timescale 1ns/1ps
module tb_square_v1_oracle;
    reg [7:0] a,b;
    reg [1:0] mode;
    reg signed [5:0] acc_unit_exp;
    reg signed [31:0] expected;
    integer expected_nar;
    wire signed [31:0] actual;
    wire nar;
    integer fd, scanned, m, av, bv, unit, count;
    reg [31:0] result_hex;
    square_partitioned_product_v1 dut (
        .a(a),.b(b),.mode(mode),.acc_unit_exp(acc_unit_exp),
        .increment(actual),.nar(nar)
    );
    initial begin
        fd=$fopen("build/square_ppa/oracle_vectors.txt","r");
        if (fd==0) $fatal(1,"generate oracle vectors first");
        count=0;
        while (!$feof(fd)) begin
            scanned=$fscanf(fd,"%d %h %h %d %h %d\n",
                            m,av,bv,unit,result_hex,expected_nar);
            if (scanned==6) begin
                mode=m; a=av; b=bv; acc_unit_exp=unit; expected=result_hex;
                #1;
                count=count+1;
                if (actual!==expected || nar!==expected_nar[0]) begin
                    $display("FAIL case %0d mode=%0d a=%h b=%h exp=%0d actual=%h expected=%h nar=%b/%b",
                        count,mode,a,b,acc_unit_exp,actual,expected,nar,expected_nar);
                    $fatal(1);
                end
            end
            else if (!$feof(fd)) $fatal(1,"Malformed oracle vector file");
        end
        $fclose(fd);
        if (count!=87040) $fatal(1,"Wrong vector count %0d",count);
        $display("PASS V1 vs independent exact-rational oracle: %0d vectors",count);
        $finish;
    end
endmodule
