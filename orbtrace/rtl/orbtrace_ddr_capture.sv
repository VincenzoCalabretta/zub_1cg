`timescale 1ns/1ps
`default_nettype none
// Reassembles a byte from 1, 2, or 4 DDR trace lanes. Samples are packed in
// wire order: the rising-edge sample occupies the least-significant lane bits,
// followed by the falling-edge sample. Width changes discard a partial byte.
module orbtrace_ddr_capture (
    input wire logic trace_clk,
    input wire logic reset_n,
    input wire logic enable,
    input wire logic [1:0] width_select, // 0=1 bit, 1=2 bits, 2=4 bits
    input wire logic [3:0] trace_data,
    output logic [7:0] byte_data,
    output logic byte_valid
);
    logic [3:0] rise_sample, fall_sample;
    logic [7:0] partial;
    logic [1:0] phase, active_width;
    logic primed;

    always_ff @(negedge trace_clk or negedge reset_n) begin
        if (!reset_n) fall_sample <= 0;
        else if (enable) fall_sample <= trace_data;
    end

    always_ff @(posedge trace_clk or negedge reset_n) begin
        if (!reset_n) begin
            rise_sample <= 0;
            partial <= 0;
            phase <= 0;
            active_width <= 0;
            primed <= 0;
            byte_data <= 0;
            byte_valid <= 0;
        end else begin
            byte_valid <= 0;
            rise_sample <= trace_data;
            if (!enable || width_select > 2) begin
                primed <= 0;
                phase <= 0;
            end else if (!primed || active_width != width_select) begin
                primed <= 1;
                active_width <= width_select;
                phase <= 0;
                partial <= 0;
            end else begin
                case (active_width)
                    2'd2: begin
                        byte_data <= {fall_sample, rise_sample};
                        byte_valid <= 1;
                    end
                    2'd1: begin
                        if (phase == 0) begin
                            partial[3:0] <= {fall_sample[1:0], rise_sample[1:0]};
                            phase <= 1;
                        end else begin
                            byte_data <= {fall_sample[1:0], rise_sample[1:0], partial[3:0]};
                            byte_valid <= 1;
                            phase <= 0;
                        end
                    end
                    default: begin
                        case (phase)
                            0: partial[1:0] <= {fall_sample[0], rise_sample[0]};
                            1: partial[3:2] <= {fall_sample[0], rise_sample[0]};
                            2: partial[5:4] <= {fall_sample[0], rise_sample[0]};
                            default: begin
                                byte_data <= {fall_sample[0], rise_sample[0], partial[5:0]};
                                byte_valid <= 1;
                            end
                        endcase
                        phase <= phase == 3 ? 0 : phase + 1'b1;
                    end
                endcase
            end
        end
    end
endmodule
`default_nettype wire
