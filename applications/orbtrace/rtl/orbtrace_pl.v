`default_nettype none
// Verilog-2001 block-design boundary. Vivado module references do not accept a
// SystemVerilog source as their top file; behavior remains in the SV units.
module orbtrace_pl (
    input wire aclk, input wire aresetn,
    input wire trace_clk, input wire [3:0] trace_data,
    input wire [15:0] s_axi_awaddr, input wire s_axi_awvalid, output wire s_axi_awready,
    input wire [31:0] s_axi_wdata, input wire [3:0] s_axi_wstrb, input wire s_axi_wvalid, output wire s_axi_wready,
    output wire [1:0] s_axi_bresp, output wire s_axi_bvalid, input wire s_axi_bready,
    input wire [15:0] s_axi_araddr, input wire s_axi_arvalid, output wire s_axi_arready,
    output wire [31:0] s_axi_rdata, output wire [1:0] s_axi_rresp, output wire s_axi_rvalid, input wire s_axi_rready,
    output wire [7:0] m_axis_tdata, output wire m_axis_tvalid, output wire m_axis_tlast, input wire m_axis_tready,
    input wire dma_complete, input wire dma_fault, input wire debug_complete, output wire irq
);
    wire [7:0] trace_byte, cdc_data, nrz_data, manchester_data;
    wire trace_valid, cdc_valid, cdc_ready, cdc_write_ready;
    wire [5:0] cdc_level;
    reg [31:0] high_water;
    reg cdc_overrun;
    reg [63:0] cdc_drops, cdc_drops_gray;
    (* ASYNC_REG="TRUE" *) reg overrun_s1, overrun_s2;
    (* ASYNC_REG="TRUE" *) reg [63:0] drops_gray_s1, drops_gray_s2;
    wire [1:0] source_select; wire [2:0] trace_format; wire [31:0] swo_baud;
    wire [63:0] dma_base; wire [31:0] dma_ring_size;
    wire start_pulse, stop_pulse, reset_pulse, core_overrun;
    wire [63:0] core_received, core_dropped, tpiu_sync_loss;
    wire [63:0] cdc_drops_sync;
    wire baud_tick, nrz_valid, nrz_ready, manchester_valid, manchester_ready;
    wire [63:0] nrz_malformed, manchester_malformed;
    wire [7:0] selected_capture_data, vex_data, test_data;
    wire selected_capture_valid, selected_capture_ready, vex_valid, vex_ready, test_valid, test_ready;
    reg running;
    (* ASYNC_REG="TRUE" *) reg [2:0] format_trace_m, format_trace_s;
    (* ASYNC_REG="TRUE" *) reg running_trace_m, running_trace_s;
    (* ASYNC_REG="TRUE" *) reg [1:0] trace_reset_sync;
    wire trace_reset_n=trace_reset_sync[1];
    wire [7:0] dap_command_data, dap_response_data;
    wire dap_command_valid, dap_command_last, dap_command_ready;
    wire dap_response_valid, dap_response_last, dap_response_ready;
    wire dap_force_wait, dap_force_fault, dap_force_parity_error, dap_complete;
    wire [63:0] dap_transfer_count;
    wire [31:0] dap_abort_count;
    function [63:0] gray_to_binary;
        input [63:0] gray;
        integer index;
        begin
            gray_to_binary[63]=gray[63];
            for (index=62;index>=0;index=index-1) gray_to_binary[index]=gray_to_binary[index+1]^gray[index];
        end
    endfunction
    assign cdc_drops_sync=gray_to_binary(drops_gray_s2);

    always @(posedge aclk) begin
        if (!aresetn) running<=0;
        else begin
            if (start_pulse) running<=1;
            if (stop_pulse || reset_pulse) running<=0;
        end
    end
    always @(posedge trace_clk) begin
        if (!trace_reset_n) begin format_trace_m<=0; format_trace_s<=0; running_trace_m<=0; running_trace_s<=0; end
        else begin
            format_trace_m<=trace_format; format_trace_s<=format_trace_m;
            running_trace_m<=running; running_trace_s<=running_trace_m;
        end
    end
    always @(posedge trace_clk or negedge aresetn) begin
        if (!aresetn) trace_reset_sync<=0;
        else trace_reset_sync<={trace_reset_sync[0],1'b1};
    end
    orbtrace_ddr_capture capture(.trace_clk(trace_clk),.reset_n(trace_reset_n),
        .enable(running_trace_s && format_trace_s<=2),.width_select(format_trace_s[1:0]),
        .trace_data(trace_data),.byte_data(trace_byte),.byte_valid(trace_valid));
    orbtrace_async_fifo trace_fifo(.write_clk(trace_clk),.write_reset_n(trace_reset_n),.write_data(trace_byte),
        .write_valid(trace_valid),.write_ready(cdc_write_ready),.write_level(cdc_level),
        .read_clk(aclk),.read_reset_n(aresetn),.read_data(cdc_data),.read_valid(cdc_valid),.read_ready(cdc_ready));
    always @(posedge trace_clk) begin
        if (!trace_reset_n) begin cdc_overrun<=0; cdc_drops<=0; cdc_drops_gray<=0; end
        else if (trace_valid && !cdc_write_ready) begin
            cdc_overrun<=1; cdc_drops<=cdc_drops+1'b1;
            cdc_drops_gray<=((cdc_drops+1'b1)>>1)^(cdc_drops+1'b1);
        end
    end
    always @(posedge aclk) begin
        if (!aresetn) begin overrun_s1<=0; overrun_s2<=0; drops_gray_s1<=0; drops_gray_s2<=0; high_water<=0; end
        else begin
            overrun_s1<=cdc_overrun; overrun_s2<=overrun_s1; drops_gray_s1<=cdc_drops_gray; drops_gray_s2<=drops_gray_s1;
            if (cdc_level > high_water) high_water<=cdc_level;
        end
    end

    orbtrace_baud_tick baud_generator(.clk(aclk),.reset_n(aresetn),.baud(swo_baud),.tick(baud_tick));
    orbtrace_swo_nrz nrz(.clk(aclk),.reset_n(aresetn),.swo(trace_data[0]),.sample_tick(baud_tick),
        .output_data(nrz_data),.output_valid(nrz_valid),.output_ready(nrz_ready),.malformed_count(nrz_malformed));
    orbtrace_swo_manchester manchester(.clk(aclk),.reset_n(aresetn),.swo(trace_data[0]),.sample_tick(baud_tick),
        .output_data(manchester_data),.output_valid(manchester_valid),.output_ready(manchester_ready),
        .malformed_count(manchester_malformed));
    assign selected_capture_data = trace_format==3 ? nrz_data : trace_format==4 ? manchester_data : cdc_data;
    assign selected_capture_valid = running && (trace_format==3 ? nrz_valid : trace_format==4 ? manchester_valid : cdc_valid);
    assign cdc_ready = trace_format<3 ? selected_capture_ready : 1'b1;
    assign nrz_ready = trace_format==3 ? selected_capture_ready : 1'b0;
    assign manchester_ready = trace_format==4 ? selected_capture_ready : 1'b0;
    orbtrace_test_source #(.RAW_BASE(8'h20)) vex_stimulus(.clk(aclk),.reset_n(aresetn),
        .enable(running && source_select==0),.tpiu_mode(trace_format<3),.output_data(vex_data),
        .output_valid(vex_valid),.output_ready(vex_ready));
    orbtrace_test_source #(.RAW_BASE(8'h80)) deterministic_stimulus(.clk(aclk),.reset_n(aresetn),
        .enable(running && source_select==3),.tpiu_mode(trace_format<3),.output_data(test_data),
        .output_valid(test_valid),.output_ready(test_ready));
    orbtrace_core core(.clk(aclk),.reset_n(aresetn),.source_select(source_select),.trace_format(trace_format),.reset_sync(reset_pulse),
        .vex_data(vex_data),.vex_valid(vex_valid),.vex_ready(vex_ready),
        .coresight_data(selected_capture_data),.coresight_valid(selected_capture_valid),.coresight_ready(selected_capture_ready),
        .test_data(test_data),.test_valid(test_valid),.test_ready(test_ready),.m_axis_tdata(m_axis_tdata),.m_axis_tvalid(m_axis_tvalid),
        .m_axis_tlast(m_axis_tlast),.m_axis_tready(m_axis_tready),.overrun(core_overrun),
        .received_bytes(core_received),.dropped_bytes(core_dropped),.sync_loss(tpiu_sync_loss));
    orbtrace_dap_engine dap_engine(.clk(aclk),.reset_n(aresetn),
        .command_data(dap_command_data),.command_valid(dap_command_valid),.command_last(dap_command_last),
        .command_ready(dap_command_ready),.response_data(dap_response_data),.response_valid(dap_response_valid),
        .response_last(dap_response_last),.response_ready(dap_response_ready),.force_wait(dap_force_wait),
        .force_fault(dap_force_fault),.force_parity_error(dap_force_parity_error),.debug_complete(dap_complete),
        .transfer_count(dap_transfer_count),.abort_count(dap_abort_count));
    orbtrace_axi_regs regs(.aclk(aclk),.aresetn(aresetn),.s_axi_awaddr(s_axi_awaddr),.s_axi_awvalid(s_axi_awvalid),.s_axi_awready(s_axi_awready),
        .s_axi_wdata(s_axi_wdata),.s_axi_wstrb(s_axi_wstrb),.s_axi_wvalid(s_axi_wvalid),.s_axi_wready(s_axi_wready),
        .s_axi_bresp(s_axi_bresp),.s_axi_bvalid(s_axi_bvalid),.s_axi_bready(s_axi_bready),.s_axi_araddr(s_axi_araddr),
        .s_axi_arvalid(s_axi_arvalid),.s_axi_arready(s_axi_arready),.s_axi_rdata(s_axi_rdata),.s_axi_rresp(s_axi_rresp),
        .s_axi_rvalid(s_axi_rvalid),.s_axi_rready(s_axi_rready),.start_pulse(start_pulse),.stop_pulse(stop_pulse),
        .reset_pulse(reset_pulse),.source_select(source_select),.trace_format(trace_format),.swo_baud(swo_baud),
        .dma_base(dma_base),.dma_ring_size(dma_ring_size),.irq_dma_complete(dma_complete),
        .irq_overrun(core_overrun|overrun_s2),.irq_debug_complete(debug_complete|dap_complete),.irq(irq),
        .received_bytes(core_received),.dropped_bytes(core_dropped+cdc_drops_sync),
        .sync_loss(tpiu_sync_loss+nrz_malformed+manchester_malformed),
        .fifo_high_water(high_water),.dma_faults({63'b0,dma_fault}),
        .dap_command_data(dap_command_data),.dap_command_valid(dap_command_valid),
        .dap_command_last(dap_command_last),.dap_command_ready(dap_command_ready),
        .dap_response_data(dap_response_data),.dap_response_valid(dap_response_valid),
        .dap_response_last(dap_response_last),.dap_response_ready(dap_response_ready),
        .dap_force_wait(dap_force_wait),.dap_force_fault(dap_force_fault),
        .dap_force_parity_error(dap_force_parity_error),.dap_transfer_count(dap_transfer_count),
        .dap_abort_count(dap_abort_count));
endmodule
`default_nettype wire
