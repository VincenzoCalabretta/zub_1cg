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
    // Left at their `.*`-bound defaults (use_real_target=0) so this test
    // exercises the synthetic DAP_JTAG_Sequence/DAP_SWJ_Pins path — an
    // unconnected `use_real_target` input would otherwise float to 'z' and
    // make the real-vs-synthetic branch in orbtrace_dap_engine.sv undefined.
    logic use_real_target=0, jtag_tck, jtag_tms, jtag_tdi, jtag_tdo=0, jtag_ntrst, jtag_nreset;
    logic [7:0] captured [0:31];
    integer captured_length;
    integer tck_pulses;
    always #5 clk=~clk;
    orbtrace_dap_engine #(.MAX_PACKET(64),.MAX_TRANSFERS(8)) dut(.*);
    always @(posedge jtag_tck) tck_pulses = tck_pulses + 1;

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
        clear_response();

        // use_real_target: DAP_JTAG_Sequence/DAP_SWJ_Pins drive real pins
        // instead of synthesizing a response.
        use_real_target=1;

        jtag_tdo=1; tck_pulses=0;
        send_byte(8'h14,0); send_byte(8'h01,0); send_byte(8'h81,0); send_byte(8'h01,1); // tms=1,tdi=1
        await_response(3);
        if (tck_pulses!==1) $fatal(1,"real JTAG: expected 1 TCK pulse, got %0d",tck_pulses);
        if (jtag_tms!==1 || jtag_tdi!==1) $fatal(1,"real JTAG: TMS/TDI mismatch tms=%b tdi=%b",jtag_tms,jtag_tdi);
        if (captured[2][0]!==1) $fatal(1,"real JTAG: TDO=1 not sampled, got %02x",captured[2]);
        clear_response();

        jtag_tdo=0; tck_pulses=0;
        send_byte(8'h14,0); send_byte(8'h01,0); send_byte(8'h01,0); send_byte(8'h00,1); // tms=0,tdi=0
        await_response(3);
        if (tck_pulses!==1) $fatal(1,"real JTAG: expected 1 TCK pulse, got %0d",tck_pulses);
        if (jtag_tms!==0 || jtag_tdi!==0) $fatal(1,"real JTAG: TMS/TDI mismatch tms=%b tdi=%b",jtag_tms,jtag_tdi);
        if (captured[2][0]!==0) $fatal(1,"real JTAG: TDO=0 not sampled, got %02x",captured[2]);
        clear_response();

        send_byte(8'h10,0); send_byte(8'ha0,1); await_response(2); // nTRST/nRESET both deasserted
        if (jtag_ntrst!==1 || jtag_nreset!==1) $fatal(1,"real SWJ_Pins: expected both deasserted");
        clear_response();

        send_byte(8'h10,0); send_byte(8'h00,1); await_response(2); // both asserted
        if (jtag_ntrst!==0 || jtag_nreset!==0) $fatal(1,"real SWJ_Pins: expected both asserted");
        clear_response();
        use_real_target=0;

        $display("orbtrace CMSIS-DAP RTL tests passed");
        $finish;
    end
endmodule
`default_nettype wire
