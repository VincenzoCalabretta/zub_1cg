`default_nettype none
// Vendor timing primitive is isolated here. One TPIU byte is reconstructed per
// trace clock from the rising and falling four-bit EMIO values.
module orbtrace_ddr4_capture (
    input wire logic trace_clk, input wire logic reset_n, input wire logic [3:0] trace_data,
    output logic [7:0] byte_data, output logic byte_valid
);
    logic [3:0] rise, fall;
    // CoreSight EMIO is an internal fabric connection, so I/O-block IDDRE1
    // primitives cannot legally be placed. Explicit opposite-edge registers
    // preserve the DDR behavior and receive a half-cycle timing requirement.
    always_ff @(negedge trace_clk) begin
        if (!reset_n) fall <= 0;
        else fall <= trace_data;
    end
    always_ff @(posedge trace_clk) begin
        if (!reset_n) begin byte_data <= 0; byte_valid <= 0; end
        else begin rise <= trace_data; byte_data <= {fall,rise}; byte_valid <= 1; end
    end
endmodule
`default_nettype wire
