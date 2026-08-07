`timescale 1ns/1ps
`default_nettype none
// SWO Manchester decoder driven by a 4x-bit-rate tick. A zero is low/high and
// a one is high/low. Start and stop use the same 8-N-1 logical framing.
module orbtrace_swo_manchester (
    input wire logic clk,
    input wire logic reset_n,
    input wire logic swo,
    input wire logic sample_tick,
    output logic [7:0] output_data,
    output logic output_valid,
    input wire logic output_ready,
    output logic [63:0] malformed_count
);
    (* ASYNC_REG="TRUE" *) logic swo_meta, swo_sync;
    logic active, first_half;
    logic [3:0] quarter, bit_index;
    logic [7:0] shift;

    always_ff @(posedge clk) begin
        swo_meta <= swo;
        swo_sync <= swo_meta;
        if (!reset_n) begin
            active <= 0; first_half <= 0; quarter <= 0; bit_index <= 0;
            shift <= 0; output_data <= 0; output_valid <= 0; malformed_count <= 0;
        end else begin
            if (output_valid && output_ready) output_valid <= 0;
            if (!active && !output_valid && !swo_sync) begin
                active <= 1;
                quarter <= 0;
                bit_index <= 0;
            end else if (active && sample_tick) begin
                if (quarter == 0) first_half <= swo_sync;
                if (quarter == 2) begin
                    if (first_half == swo_sync) begin
                        active <= 0;
                        malformed_count <= malformed_count + 1'b1;
                    end else if (bit_index == 0 && first_half) begin
                        active <= 0;
                        malformed_count <= malformed_count + 1'b1;
                    end else if (bit_index >= 1 && bit_index <= 8) begin
                        shift[bit_index-1'b1] <= first_half;
                    end else if (bit_index == 9) begin
                        active <= 0;
                        if (first_half) begin
                            output_data <= shift;
                            output_valid <= 1;
                        end else malformed_count <= malformed_count + 1'b1;
                    end
                end
                if (active) begin
                    if (quarter == 3) begin
                        quarter <= 0;
                        bit_index <= bit_index + 1'b1;
                    end else quarter <= quarter + 1'b1;
                end
            end
        end
    end
endmodule
`default_nettype wire
