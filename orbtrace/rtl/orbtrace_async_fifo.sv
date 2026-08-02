`default_nettype none
module orbtrace_async_fifo #(
    parameter int WIDTH = 8,
    parameter int DEPTH_LOG2 = 5
) (
    input  wire logic                 write_clk,
    input  wire logic                 write_reset_n,
    input  wire logic [WIDTH-1:0]     write_data,
    input  wire logic                 write_valid,
    output      logic                 write_ready,
    output      logic [DEPTH_LOG2:0]  write_level,
    input  wire logic                 read_clk,
    input  wire logic                 read_reset_n,
    output      logic [WIDTH-1:0]     read_data,
    output      logic                 read_valid,
    input  wire logic                 read_ready
);
    localparam int PTR_W = DEPTH_LOG2 + 1;
    logic [WIDTH-1:0] mem [0:(1<<DEPTH_LOG2)-1];
    logic [PTR_W-1:0] wr_bin, rd_bin, wr_gray, rd_gray;
    (* ASYNC_REG = "TRUE" *) logic [PTR_W-1:0] rd_gray_w1, rd_gray_w2;
    (* ASYNC_REG = "TRUE" *) logic [PTR_W-1:0] wr_gray_r1, wr_gray_r2;
    logic [PTR_W-1:0] wr_next, wr_gray_next, wr_plus_one_gray;

    assign wr_next = wr_bin + (write_valid && write_ready);
    assign wr_gray_next = (wr_next >> 1) ^ wr_next;
    assign wr_plus_one_gray = ((wr_bin + 1'b1) >> 1) ^ (wr_bin + 1'b1);
    assign write_ready = wr_plus_one_gray != {~rd_gray_w2[PTR_W-1:PTR_W-2], rd_gray_w2[PTR_W-3:0]};
    assign read_valid = rd_gray != wr_gray_r2;
    assign read_data = mem[rd_bin[DEPTH_LOG2-1:0]];
    assign write_level = wr_bin - ((rd_gray_w2 >> 1) ^ rd_gray_w2);

    always_ff @(posedge write_clk) begin
        if (!write_reset_n) begin wr_bin <= '0; wr_gray <= '0; rd_gray_w1 <= '0; rd_gray_w2 <= '0; end
        else begin
            rd_gray_w1 <= rd_gray; rd_gray_w2 <= rd_gray_w1;
            if (write_valid && write_ready) begin
                mem[wr_bin[DEPTH_LOG2-1:0]] <= write_data;
                wr_bin <= wr_next; wr_gray <= wr_gray_next;
            end
        end
    end
    always_ff @(posedge read_clk) begin
        if (!read_reset_n) begin rd_bin <= '0; rd_gray <= '0; wr_gray_r1 <= '0; wr_gray_r2 <= '0; end
        else begin
            wr_gray_r1 <= wr_gray; wr_gray_r2 <= wr_gray_r1;
            if (read_valid && read_ready) begin
                rd_bin <= rd_bin + 1'b1;
                rd_gray <= ((rd_bin + 1'b1) >> 1) ^ (rd_bin + 1'b1);
            end
        end
    end
endmodule
`default_nettype wire
