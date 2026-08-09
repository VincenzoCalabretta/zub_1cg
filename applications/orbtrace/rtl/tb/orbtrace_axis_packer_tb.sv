`timescale 1ns/1ps
`default_nettype none
module orbtrace_axis_packer_tb;
    logic clk = 0, reset_n = 0;
    logic [7:0] input_data = 0;
    logic input_valid = 0, input_last = 0, input_ready;
    logic [63:0] output_data;
    logic [7:0] output_keep;
    logic output_valid, output_last, output_ready = 0;
    integer beat = 0;

    always #5 clk = ~clk;

    orbtrace_axis_packer #(.FRAMES_PER_TRANSFER(2)) dut(.*);

    task automatic send_byte(input logic [7:0] value, input logic last);
        begin
            @(negedge clk);
            input_data = value;
            input_last = last;
            input_valid = 1;
            do @(posedge clk); while (!input_ready);
            @(negedge clk);
            input_valid = 0;
            input_last = 0;
        end
    endtask

    always_ff @(posedge clk) begin
        if (reset_n && output_valid && output_ready) begin
            if (beat == 0) begin
                if (output_data !== 64'h0706050403020100 ||
                    output_keep !== 8'hff || output_last)
                    $fatal(1, "first packed beat mismatch");
            end else if (beat == 1) begin
                if (output_data !== 64'h00000000000a0908 ||
                    output_keep !== 8'h07 || !output_last)
                    $fatal(1, "final packed beat mismatch");
                $display("orbtrace AXI stream packer RTL tests passed");
                $finish;
            end else $fatal(1, "unexpected packed beat");
            beat <= beat + 1;
        end
    end

    initial begin
        repeat (3) @(posedge clk);
        @(negedge clk); reset_n = 1;
        // The first frame boundary must remain only a byte-stream delimiter;
        // the second frame boundary ends the coalesced AXI transfer.
        for (integer i = 0; i < 11; i++) send_byte(i[7:0], i == 4 || i == 10);
        repeat (30) @(posedge clk);
        $fatal(1, "AXI stream packer test timed out");
    end

    initial begin
        repeat (20) @(posedge clk);
        @(negedge clk); output_ready = 1;
    end
endmodule
`default_nettype wire
