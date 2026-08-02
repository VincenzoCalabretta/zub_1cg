`default_nettype none
module orbtrace_source_mux (
    input wire logic [1:0] select,
    input wire logic [7:0] vex_data, input wire logic vex_valid, output logic vex_ready,
    input wire logic [7:0] coresight_data, input wire logic coresight_valid, output logic coresight_ready,
    input wire logic [7:0] test_data, input wire logic test_valid, output logic test_ready,
    output logic [7:0] output_data, output logic output_valid, input wire logic output_ready
);
    always_comb begin
        output_data = 0; output_valid = 0; vex_ready = 0; coresight_ready = 0; test_ready = 0;
        case (select)
            2'd0: begin output_data=vex_data; output_valid=vex_valid; vex_ready=output_ready; end
            2'd1,2'd2: begin output_data=coresight_data; output_valid=coresight_valid; coresight_ready=output_ready; end
            default: begin output_data=test_data; output_valid=test_valid; test_ready=output_ready; end
        endcase
    end
endmodule
`default_nettype wire

