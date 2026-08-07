`timescale 1ns/1ps
`default_nettype none
module orbtrace_pipeline_tb;
    logic clk = 0, reset_n = 0;
    logic [6:0] input_channel = 7;
    logic [7:0] input_data = 0;
    logic input_valid = 0, input_ready;
    logic [6:0] packet_channel;
    logic [7:0] packet_data;
    logic packet_valid, packet_last, packet_ready;
    logic [7:0] output_data;
    logic output_valid, output_last, output_ready = 0;
    logic overrun;
    logic [63:0] received_bytes, dropped_bytes;
    logic [15:0] lfsr = 16'h1ace;
    logic stalled;
    logic [7:0] stalled_data;
    logic stalled_last;
    integer output_count = 0;
    integer cycles = 0;
    logic [7:0] expected [0:7];

    always #5 clk = ~clk;

    orbtrace_channel_packetizer #(.MAX_PACKET(16), .IDLE_CYCLES(3)) packetizer (
        .clk, .reset_n, .input_channel, .input_data, .input_valid, .input_ready,
        .output_channel(packet_channel), .output_data(packet_data),
        .output_valid(packet_valid), .output_last(packet_last), .output_ready(packet_ready));
    orbtrace_orbflow_encoder #(.MAX_PAYLOAD(16)) encoder (
        .clk, .reset_n, .input_channel(packet_channel), .input_data(packet_data),
        .input_valid(packet_valid), .input_last(packet_last), .input_ready(packet_ready),
        .output_data, .output_valid, .output_last, .output_ready, .overrun,
        .received_bytes, .dropped_bytes);

    task automatic reset_all;
        begin
            reset_n = 0;
            input_valid = 0;
            repeat (3) @(posedge clk);
            @(negedge clk); reset_n = 1;
        end
    endtask

    task automatic send_byte(input logic [7:0] value);
        begin
            @(negedge clk); input_data = value; input_valid = 1;
            do @(posedge clk); while (!input_ready);
            @(negedge clk); input_valid = 0;
        end
    endtask

    always_ff @(posedge clk) begin
        if (!reset_n) begin
            lfsr <= 16'h1ace;
            output_ready <= 0;
            stalled <= 0;
            output_count <= 0;
            cycles <= 0;
        end else begin
            lfsr <= {lfsr[14:0], lfsr[15] ^ lfsr[13] ^ lfsr[12] ^ lfsr[10]};
            output_ready <= lfsr[0] | lfsr[3];
            cycles <= cycles + 1;
            if (cycles > 300) $fatal(1, "pipeline timed out before packet delimiter");
            if (stalled && (output_data !== stalled_data || output_last !== stalled_last))
                $fatal(1, "output changed while stalled");
            stalled <= output_valid && !output_ready;
            stalled_data <= output_data;
            stalled_last <= output_last;
            if (output_valid && output_ready) begin
                if (output_data !== expected[output_count])
                    $fatal(1, "encoded byte %0d mismatch: %02x", output_count, output_data);
                if (output_last !== (output_count == 7))
                    $fatal(1, "last flag mismatch at encoded byte %0d", output_count);
                output_count <= output_count + 1;
                if (output_count == 7) begin
                    if (received_bytes !== 4 || dropped_bytes !== 0 || overrun)
                        $fatal(1, "counter mismatch received=%0d dropped=%0d", received_bytes, dropped_bytes);
                    $display("orbtrace randomized pipeline RTL tests passed");
                    $finish;
                end
            end
        end
    end

    initial begin
        expected[0]=8'h02; expected[1]=8'h07; expected[2]=8'h05; expected[3]=8'hff;
        expected[4]=8'h01; expected[5]=8'h02; expected[6]=8'hf7; expected[7]=8'h00;
        reset_all();
        send_byte(8'h99);
        @(negedge clk); reset_n = 0;
        repeat (2) @(posedge clk);
        @(negedge clk); reset_n = 1;
        send_byte(8'h00); send_byte(8'hff); send_byte(8'h01); send_byte(8'h02);
    end
endmodule
`default_nettype wire
