`default_nettype none
// Fractional-N 4x baud tick. The accumulator avoids a run-time divider and
// bounds sampling drift to one input clock.
module orbtrace_baud_tick #(
    parameter longint unsigned CLOCK_HZ = 100_000_000
) (
    input wire logic clk,
    input wire logic reset_n,
    input wire logic [31:0] baud,
    output logic tick
);
    logic [63:0] accumulator;
    logic [63:0] next_accumulator;
    always_comb next_accumulator = accumulator + {30'b0, baud, 2'b0};
    always_ff @(posedge clk) begin
        if (!reset_n) begin
            accumulator <= 0;
            tick <= 0;
        end else if (baud == 0) begin
            accumulator <= 0;
            tick <= 0;
        end else if (next_accumulator >= CLOCK_HZ) begin
            accumulator <= next_accumulator - CLOCK_HZ;
            tick <= 1;
        end else begin
            accumulator <= next_accumulator;
            tick <= 0;
        end
    end
endmodule
`default_nettype wire
