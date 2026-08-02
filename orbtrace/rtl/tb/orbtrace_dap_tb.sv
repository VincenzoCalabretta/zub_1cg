`timescale 1ns/1ps
`default_nettype none
module orbtrace_dap_tb;
    logic clk=0, reset_n=0;
    logic [7:0] command_data;
    logic command_valid=0, command_last=0, command_ready;
    logic [7:0] response_data;
    logic response_valid, response_last, response_ready=1;
    logic force_wait=0, force_fault=0, force_parity_error=0, debug_complete;
    logic [63:0] transfer_count;
    logic [31:0] abort_count;
    logic [7:0] captured [0:31];
    integer captured_length;
    always #5 clk=~clk;
    orbtrace_dap_engine #(.MAX_PACKET(64),.MAX_TRANSFERS(8)) dut(.*);

    always @(posedge clk) if (response_valid && response_ready) begin
        captured[captured_length] <= response_data;
        captured_length <= captured_length + 1;
    end

    task automatic send_byte(input logic [7:0] value, input logic last);
        begin
            @(negedge clk); command_data=value; command_valid=1; command_last=last;
            do @(posedge clk); while(!command_ready);
            @(negedge clk); command_valid=0; command_last=0;
        end
    endtask
    task automatic await_response(input integer length);
        begin
            wait(debug_complete); #1;
            if (captured_length != length) $fatal(1,"response length %0d expected %0d",captured_length,length);
        end
    endtask
    task automatic clear_response;
        begin @(negedge clk); captured_length=0; end
    endtask

    initial begin
        captured_length=0;
        repeat(3) @(posedge clk); @(negedge clk); reset_n=1;
        send_byte(8'h00,0); send_byte(8'h01,1); await_response(9);
        if (captured[1]!==7 || captured[2]!==8'h4f || captured[8]!==8'h65) $fatal(1,"DAP_Info mismatch");
        clear_response();

        force_wait=1;
        send_byte(5,0); send_byte(0,0); send_byte(1,0); send_byte(2,1); await_response(3);
        if (captured[0]!==5 || captured[1]!==0 || captured[2]!==2) $fatal(1,"WAIT mismatch");
        clear_response(); force_wait=0;

        send_byte(5,0); send_byte(0,0); send_byte(1,0); send_byte(2,1); await_response(7);
        if (captured[0]!==5 || captured[1]!==1 || captured[2]!==1 ||
            captured[3]!==2 || captured[5]!==8'ha5 || captured[6]!==8'ha5) $fatal(1,"read mismatch");
        clear_response();

        force_fault=1;
        send_byte(5,0); send_byte(0,0); send_byte(1,0); send_byte(2,1); await_response(3);
        if (captured[2]!==4) $fatal(1,"FAULT mismatch");
        clear_response(); force_fault=0;

        send_byte(8,1); await_response(2);
        if (abort_count!==1) $fatal(1,"abort counter mismatch");
        $display("orbtrace CMSIS-DAP RTL tests passed");
        $finish;
    end
endmodule
`default_nettype wire
