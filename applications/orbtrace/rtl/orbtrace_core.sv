`default_nettype none
module orbtrace_core #(
    parameter int PACKET_TIMEOUT = 7500000
) (
    input wire logic clk, input wire logic reset_n,
    input wire logic [1:0] source_select, input wire logic [2:0] trace_format, input wire logic reset_sync,
    input wire logic [7:0] m3_data, input wire logic m3_valid, output logic m3_ready,
    input wire logic [7:0] coresight_data, input wire logic coresight_valid, output logic coresight_ready,
    input wire logic [7:0] test_data, input wire logic test_valid, output logic test_ready,
    output logic [7:0] m_axis_tdata, output logic m_axis_tvalid,
    output logic m_axis_tlast, input wire logic m_axis_tready,
    output logic overrun, output logic [63:0] received_bytes,
    output logic [63:0] dropped_bytes, output logic [63:0] sync_loss
);
    // The board link uses a 9000-byte MTU. An 8192-byte payload produces at
    // most 8228 encoded bytes (channel + checksum + 33 COBS code bytes and
    // delimiter), below the resulting 8960-byte TCP MSS while halving the
    // DMA, NetX, GEM, and ACK event rate relative to 4096-byte frames.
    localparam int ORBFLOW_PAYLOAD = 8192;
    logic [7:0] selected_data; logic selected_valid, selected_ready;
    wire bypass_tpiu = trace_format >= 3;
    logic [6:0] demux_channel; logic [7:0] demux_data; logic demux_valid, demux_ready, tpiu_input_ready;
    logic [6:0] packet_channel; logic [7:0] packet_data; logic packet_valid, packet_last, packet_ready;
    orbtrace_source_mux mux(.*,.select(source_select),.output_data(selected_data),.output_valid(selected_valid),.output_ready(selected_ready));
    orbtrace_tpiu_demux demux(.clk,.reset_n,.reset_sync,.input_data(selected_data),
        .input_valid(selected_valid && !bypass_tpiu),.input_ready(tpiu_input_ready),.output_channel(demux_channel),
        .output_data(demux_data),.output_valid(demux_valid),.output_ready(demux_ready),.sync_loss_count(sync_loss));
    assign selected_ready = bypass_tpiu ? demux_ready : tpiu_input_ready;
    orbtrace_channel_packetizer #(
        .MAX_PACKET(ORBFLOW_PAYLOAD),
        .IDLE_CYCLES(PACKET_TIMEOUT)
    ) packetizer(
        .clk,.reset_n,.input_channel(bypass_tpiu ? 7'd1 : demux_channel),
        .input_data(bypass_tpiu ? selected_data : demux_data),
        .input_valid(bypass_tpiu ? selected_valid : demux_valid),.input_ready(demux_ready),
        .output_channel(packet_channel),.output_data(packet_data),.output_valid(packet_valid),
        .output_last(packet_last),.output_ready(packet_ready));
    orbtrace_orbflow_encoder #(.MAX_PAYLOAD(ORBFLOW_PAYLOAD)) encoder(
        .clk,.reset_n,.input_channel(packet_channel),
        .input_data(packet_data),.input_valid(packet_valid),.input_last(packet_last),.input_ready(packet_ready),
        .output_data(m_axis_tdata),.output_valid(m_axis_tvalid),.output_last(m_axis_tlast),
        .output_ready(m_axis_tready),.overrun,.received_bytes,.dropped_bytes);
endmodule
`default_nettype wire
