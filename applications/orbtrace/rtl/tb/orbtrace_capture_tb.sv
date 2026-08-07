`timescale 1ns/1ps
`default_nettype none
module orbtrace_capture_tb;
    logic clk=0, reset_n=0, capture_enable=0;
    logic [1:0] width_select=0;
    logic [3:0] trace_data=0;
    logic [7:0] capture_data, nrz_data, manchester_data;
    logic capture_valid, nrz_valid, manchester_valid;
    logic swo=1;
    logic [63:0] nrz_malformed, manchester_malformed;
    always #5 clk = ~clk;

    orbtrace_ddr_capture capture(.trace_clk(clk),.reset_n,.enable(capture_enable),
        .width_select,.trace_data,.byte_data(capture_data),.byte_valid(capture_valid));
    orbtrace_swo_nrz nrz(.clk,.reset_n,.swo,.sample_tick(1'b1),.output_data(nrz_data),
        .output_valid(nrz_valid),.output_ready(1'b1),.malformed_count(nrz_malformed));
    orbtrace_swo_manchester manchester(.clk,.reset_n,.swo,.sample_tick(1'b1),
        .output_data(manchester_data),.output_valid(manchester_valid),.output_ready(1'b1),
        .malformed_count(manchester_malformed));

    task automatic reset_all;
        begin
            reset_n=0; capture_enable=0; swo=1;
            repeat(3) @(posedge clk);
            @(negedge clk); reset_n=1;
            repeat(3) @(posedge clk);
            @(negedge clk); #1;
        end
    endtask

    task automatic ddr_cycle(input logic [3:0] rising, input logic [3:0] falling);
        begin
            // Present each half-cycle before the edge on which the DUT samples
            // it.  The task returns at the falling edge so the next call can
            // immediately present the following rising-edge value.
            trace_data=rising;
            @(posedge clk); #1 trace_data=falling;
            @(negedge clk); #1;
        end
    endtask

    task automatic hold_swo(input logic level, input integer quarters);
        begin
            @(negedge clk); swo=level;
            repeat(quarters) @(posedge clk);
        end
    endtask

    task automatic send_nrz(input logic [7:0] value);
        integer bit_index;
        begin
            hold_swo(0,4);
            for (bit_index=0;bit_index<8;bit_index=bit_index+1) hold_swo(value[bit_index],4);
            hold_swo(1,4);
        end
    endtask

    task automatic send_manchester(input logic [7:0] value);
        integer bit_index;
        logic bit_value;
        begin
            for (bit_index=0;bit_index<10;bit_index=bit_index+1) begin
                if (bit_index==0) bit_value=0;
                else if (bit_index==9) bit_value=1;
                else bit_value=value[bit_index-1];
                hold_swo(bit_value,2);
                hold_swo(!bit_value,2);
            end
        end
    endtask

    initial begin
        reset_all(); width_select=2; capture_enable=1;
        ddr_cycle(4'hb,4'ha); ddr_cycle(4'h5,4'hc);
        if (!capture_valid || capture_data!==8'hab) $fatal(1,"4-bit DDR mismatch: %02x",capture_data);

        reset_all(); width_select=1; capture_enable=1;
        ddr_cycle(3,2); ddr_cycle(2,2); ddr_cycle(0,0);
        if (!capture_valid || capture_data!==8'hab) $fatal(1,"2-bit DDR mismatch: %02x",capture_data);

        reset_all(); width_select=0; capture_enable=1;
        ddr_cycle(1,1); ddr_cycle(0,1); ddr_cycle(0,1); ddr_cycle(0,1); ddr_cycle(0,0);
        if (!capture_valid || capture_data!==8'hab) $fatal(1,"1-bit DDR mismatch: %02x",capture_data);

        reset_all(); send_nrz(8'ha6); wait(nrz_valid);
        if (nrz_data!==8'ha6 || nrz_malformed!==0) $fatal(1,"NRZ mismatch: %02x",nrz_data);

        reset_all(); send_manchester(8'ha6); wait(manchester_valid);
        if (manchester_data!==8'ha6 || manchester_malformed!==0)
            $fatal(1,"Manchester mismatch: %02x errors=%0d",manchester_data,manchester_malformed);
        $display("orbtrace capture RTL tests passed");
        $finish;
    end
endmodule
`default_nettype wire
