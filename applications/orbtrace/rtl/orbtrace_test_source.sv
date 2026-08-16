`default_nettype none
// Deterministic byte source for the permanent synthetic regression path
// (source_select==3) used for pipeline validation independent of any real
// trace-generating core. TPIU modes emit sync plus a formatter frame; serial
// modes emit the decoded workload bytes directly.
module orbtrace_test_source #(
    parameter logic [7:0] RAW_BASE = 8'h40
) (
    input wire logic clk,
    input wire logic reset_n,
    input wire logic enable,
    input wire logic tpiu_mode,
    output logic [7:0] output_data,
    output logic output_valid,
    input wire logic output_ready
);
    logic [4:0] index;
    always_comb begin
        if (!tpiu_mode) output_data = RAW_BASE + index[3:0];
        else case (index)
            0: output_data=8'h7f; 1,2,3: output_data=8'hff;
            4: output_data=8'h05; // immediate formatter ID 2
            19: output_data=8'h00; // formatter auxiliary bits
            default: output_data=RAW_BASE + index - 5'd5;
        endcase
        output_valid = enable;
    end
    always_ff @(posedge clk) begin
        if (!reset_n || !enable) index <= 0;
        else if (output_valid && output_ready) begin
            if (tpiu_mode) index <= index == 19 ? 0 : index + 1'b1;
            else index <= index == 15 ? 0 : index + 1'b1;
        end
    end
endmodule
`default_nettype wire
