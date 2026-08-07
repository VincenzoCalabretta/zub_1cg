`default_nettype none
module orbtrace_channel_packetizer #(
    parameter int MAX_PACKET = 1024,
    parameter int IDLE_CYCLES = 7500000
) (
    input wire logic clk, input wire logic reset_n,
    input wire logic [6:0] input_channel, input wire logic [7:0] input_data,
    input wire logic input_valid, output logic input_ready,
    output logic [6:0] output_channel, output logic [7:0] output_data,
    output logic output_valid, output logic output_last, input wire logic output_ready
);
    logic [6:0] channel;
    logic [7:0] held_data;
    logic [$clog2(MAX_PACKET+1)-1:0] count;
    logic [$clog2(IDLE_CYCLES+1)-1:0] idle;
    logic held, end_pending;
    wire channel_boundary = held && input_valid && input_channel != channel;
    wire size_boundary = held && count == MAX_PACKET;
    assign output_channel = channel;
    assign output_data = held_data;
    // Retain one byte until the following byte or an idle/size boundary is
    // known.  Otherwise the final byte could be consumed with last deasserted
    // and there would be nothing on which to signal the packet boundary.
    assign output_valid = held && (input_valid || end_pending || size_boundary);
    assign output_last = end_pending || channel_boundary || size_boundary;
    assign input_ready = !held || (output_ready && output_valid);
    always_ff @(posedge clk) begin
        if (!reset_n) begin held<=0; end_pending<=0; count<=0; idle<=0; channel<=0; held_data<=0; end
        else begin
            if (held && idle != 0) idle <= idle-1'b1;
            if (held && idle == 1) end_pending <= 1;
            if (output_valid && output_ready) begin
                if (input_valid) begin
                    held <= 1;
                    held_data <= input_data;
                    channel <= input_channel;
                    count <= output_last ? 1 : count + 1'b1;
                    idle <= IDLE_CYCLES;
                    end_pending <= 0;
                end else begin
                    held <= 0;
                    end_pending <= 0;
                    count <= 0;
                    idle <= 0;
                end
            end else if (!held && input_valid) begin
                held <= 1;
                held_data <= input_data;
                channel <= input_channel;
                count <= 1;
                idle <= IDLE_CYCLES;
            end
        end
    end
endmodule
`default_nettype wire
