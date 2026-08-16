`timescale 1ns/1ps
`default_nettype none
// Exercises orbtrace_source_mux's routing between the PL-hosted Cortex-M3
// capture path (source_select==0, replacing the old synthetic vex/test
// placeholder), the PS CoreSight trace path (1/2), and the permanent
// synthetic regression source (3) — including that only the selected
// source's ready line is ever asserted, so an inactive source can't be
// starved or corrupted by a mux it isn't feeding.
module orbtrace_m3_source_tb;
    logic [1:0] select = 0;
    logic [7:0] m3_data = 0, coresight_data = 0, test_data = 0;
    logic m3_valid = 0, coresight_valid = 0, test_valid = 0;
    logic m3_ready, coresight_ready, test_ready;
    logic [7:0] output_data;
    logic output_valid, output_ready = 0;

    orbtrace_source_mux mux(.select,.m3_data,.m3_valid,.m3_ready,
        .coresight_data,.coresight_valid,.coresight_ready,
        .test_data,.test_valid,.test_ready,
        .output_data,.output_valid,.output_ready);

    task automatic check_route(
        input logic [1:0] sel,
        input string label
    );
        begin
            select = sel;
            m3_data = 8'h11; coresight_data = 8'h22; test_data = 8'h33;
            m3_valid = 1; coresight_valid = 1; test_valid = 1;
            output_ready = 1;
            #1;
            case (sel)
                2'd0: if (output_data !== 8'h11 || !m3_ready || coresight_ready || test_ready)
                    $fatal(1, "%s: expected m3 route, got data=%02x ready(m3=%b,cs=%b,test=%b)",
                        label, output_data, m3_ready, coresight_ready, test_ready);
                2'd1, 2'd2: if (output_data !== 8'h22 || m3_ready || !coresight_ready || test_ready)
                    $fatal(1, "%s: expected coresight route, got data=%02x ready(m3=%b,cs=%b,test=%b)",
                        label, output_data, m3_ready, coresight_ready, test_ready);
                default: if (output_data !== 8'h33 || m3_ready || coresight_ready || !test_ready)
                    $fatal(1, "%s: expected test route, got data=%02x ready(m3=%b,cs=%b,test=%b)",
                        label, output_data, m3_ready, coresight_ready, test_ready);
            endcase
            if (!output_valid) $fatal(1, "%s: output_valid deasserted while source valid", label);
        end
    endtask

    initial begin
        check_route(2'd0, "select=0 (M3)");
        check_route(2'd1, "select=1 (PS R5)");
        check_route(2'd2, "select=2 (PS A53)");
        check_route(2'd3, "select=3 (synthetic test)");

        // Backpressure: deasserting output_ready must deassert the *selected*
        // source's ready only.
        select = 2'd0; m3_valid = 1; coresight_valid = 1; test_valid = 1;
        output_ready = 0;
        #1;
        if (m3_ready || coresight_ready || test_ready)
            $fatal(1, "backpressure: a source's ready stayed high with output_ready low");

        // An unselected source's valid must not leak into output_valid.
        select = 2'd1; m3_valid = 1; coresight_valid = 0; test_valid = 1;
        output_ready = 1;
        #1;
        if (output_valid) $fatal(1, "leak: output_valid high while selected (coresight) source invalid");

        $display("orbtrace M3 source mux RTL tests passed");
        $finish;
    end
endmodule
`default_nettype wire
