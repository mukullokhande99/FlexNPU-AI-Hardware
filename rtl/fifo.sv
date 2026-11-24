module fifo #(
    parameter WIDTH = 1,
    parameter DEPTH = 16,
    parameter PRECISION=4
)(
    input  wire clk,
    input  wire rst,
    input  wire wr_en,
    input  wire rd_en,
    // input [2:0] precision,
    input  wire [WIDTH-1:0] din,
    output reg  [WIDTH-1:0] dout,
    // input [3:0] precision,
    output wire full,
    output wire empty
    // output wire active
);

    reg [WIDTH-1:0] mem [0:DEPTH-1] ;
    localparam PTR_WIDTH = (DEPTH <= 1) ? 1 : $clog2(DEPTH);
    reg [PTR_WIDTH-1:0] wr_ptr;
    reg [PTR_WIDTH-1:0] rd_ptr;
    reg [$clog2(DEPTH):0]  count;
    //reg [3:0] precision_count;

    assign full  = (count == DEPTH);
    assign empty = (count == 0);

    // No bypass: at empty only a write is accepted, at full only a read.
    wire do_write = wr_en && !full;
    wire do_read  = rd_en && !empty;
    integer i;

    always @(posedge clk or negedge rst) begin
        if (!rst) begin
            wr_ptr <= 0;
            rd_ptr <= 0;
            dout   <= 0;
            for (i = 0; i < DEPTH; i = i + 1)
                mem[i] <= 0;
        end else begin
            if (do_write) begin // write
                mem[wr_ptr] <= din;
                wr_ptr <= (wr_ptr == DEPTH-1) ? '0 : wr_ptr + 1'b1;
            end
            if (do_read) begin // read 
                dout <= mem[rd_ptr];
                rd_ptr <= (rd_ptr == DEPTH-1) ? '0 : rd_ptr + 1'b1;
            end
        end
    end

    // count
    always @(posedge clk or negedge rst) begin
        if (!rst) begin
            count  <= 0;
        end
        else begin
            case ({do_write, do_read})
                2'b10: count <= count + 1'b1;
                2'b01: count <= count - 1'b1;
                default: count <= count;
            endcase
        end
    end

endmodule

