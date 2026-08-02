`timescale 1ns/1ps
`default_nettype none
module orbtrace_swo_nrz (
    input wire logic clk, input wire logic reset_n, input wire logic swo,
    input wire logic sample_tick,
    output logic [7:0] output_data, output logic output_valid, input wire logic output_ready,
    output logic [63:0] malformed_count
);
    (* ASYNC_REG="TRUE" *) logic swo_m, swo_s;
    logic [1:0] quarter;
    logic [3:0] bit_index;
    logic [7:0] shift;
    logic active;
    always_ff @(posedge clk) begin
        swo_m <= swo; swo_s <= swo_m;
        if (!reset_n) begin quarter<=0; bit_index<=0; shift<=0; active<=0; output_valid<=0; malformed_count<=0; end
        else begin
            if (output_valid && output_ready) output_valid <= 0;
            if (!active && !swo_s && !output_valid) begin active<=1; bit_index<=0; quarter<=0; end
            else if (active && sample_tick) begin
                if (quarter == 2) begin
                    if (bit_index == 0 && swo_s) begin
                        active<=0;
                        malformed_count<=malformed_count+1'b1;
                    end else if (bit_index >= 1 && bit_index <= 8) begin
                        shift[bit_index-1'b1]<=swo_s;
                    end else if (bit_index == 9) begin
                        active<=0;
                        if (swo_s) begin output_data<=shift; output_valid<=1; end
                        else malformed_count<=malformed_count+1'b1;
                    end
                end
                if (active) begin
                    if (quarter == 3) begin quarter<=0; bit_index<=bit_index+1'b1; end
                    else quarter<=quarter+1'b1;
                end
            end
        end
    end
endmodule
`default_nettype wire
