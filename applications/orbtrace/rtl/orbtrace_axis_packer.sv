`timescale 1ns/1ps
`default_nettype none
// Pack a byte stream into 64-bit AXI beats. TKEEP preserves the exact byte
// count of the final partial beat, and no byte is accepted while an output
// beat is stalled.
module orbtrace_axis_packer #(
    parameter integer FRAMES_PER_TRANSFER = 1
) (
    input  wire logic        clk,
    input  wire logic        reset_n,
    input  wire logic [7:0]  input_data,
    input  wire logic        input_valid,
    input  wire logic        input_last,
    output      logic        input_ready,
    output      logic [63:0] output_data,
    output      logic [7:0]  output_keep,
    output      logic        output_valid,
    output      logic        output_last,
    input  wire logic        output_ready
);
    logic [63:0] buffer;
    logic [7:0] keep;
    logic [2:0] count;
    localparam integer FRAME_COUNT_WIDTH = FRAMES_PER_TRANSFER <= 1
                                               ? 1 : $clog2(FRAMES_PER_TRANSFER);
    logic [FRAME_COUNT_WIDTH-1:0] frame_count;
    logic [63:0] assembled_data;
    logic [7:0] assembled_keep;
    logic transfer_last;

    assign input_ready = !output_valid || output_ready;

    always_comb begin
        assembled_data = buffer;
        assembled_keep = keep;
        assembled_data[count * 8 +: 8] = input_data;
        assembled_keep[count] = 1'b1;
        transfer_last = input_last &&
                        (FRAMES_PER_TRANSFER == 1 ||
                         frame_count == FRAMES_PER_TRANSFER - 1);
    end

    always_ff @(posedge clk) begin
        if (!reset_n) begin
            buffer <= 0;
            keep <= 0;
            count <= 0;
            frame_count <= 0;
            output_data <= 0;
            output_keep <= 0;
            output_valid <= 0;
            output_last <= 0;
        end else begin
            if (output_valid && output_ready) output_valid <= 1'b0;

            if (input_valid && input_ready) begin
                if (transfer_last || count == 3'd7) begin
                    output_data <= assembled_data;
                    output_keep <= assembled_keep;
                    output_last <= transfer_last;
                    output_valid <= 1'b1;
                    buffer <= 0;
                    keep <= 0;
                    count <= 0;
                end else begin
                    buffer <= assembled_data;
                    keep <= assembled_keep;
                    count <= count + 1'b1;
                end

                if (input_last) begin
                    if (transfer_last) frame_count <= 0;
                    else frame_count <= frame_count + 1'b1;
                end
            end
        end
    end
endmodule
`default_nettype wire
