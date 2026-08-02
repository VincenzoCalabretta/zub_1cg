`default_nettype none
// Complete-packet buffering makes overflow explicit: a too-large packet is
// consumed and discarded as a unit, never emitted as a corrupt partial frame.
module orbtrace_orbflow_encoder #(
    parameter int MAX_PAYLOAD = 1024
) (
    input  wire logic       clk,
    input  wire logic       reset_n,
    input  wire logic [6:0] input_channel,
    input  wire logic [7:0] input_data,
    input  wire logic       input_valid,
    input  wire logic       input_last,
    output      logic       input_ready,
    output      logic [7:0] output_data,
    output      logic       output_valid,
    output      logic       output_last,
    input  wire logic       output_ready,
    output      logic       overrun,
    output      logic [63:0] received_bytes,
    output      logic [63:0] dropped_bytes
);
    localparam int AW = $clog2(MAX_PAYLOAD + 3);
    typedef enum logic [2:0] {LOAD, FIND, CODE, DATA, DELIMITER} state_t;
    state_t state;
    logic [7:0] packet [0:MAX_PAYLOAD+1];
    logic [AW-1:0] length, scan, group_start, group_len, emit_index;
    logic [7:0] checksum;
    logic dropping;

    assign input_ready = state == LOAD;
    assign output_valid = state == CODE || state == DATA || state == DELIMITER;
    assign output_last = state == DELIMITER;
    always_comb begin
        output_data = 0;
        if (state == CODE) output_data = group_len == 254 ? 8'hff : group_len + 1'b1;
        else if (state == DATA) output_data = packet[emit_index];
    end

    always_ff @(posedge clk) begin
        if (!reset_n) begin
            state <= LOAD; length <= 0; checksum <= 0; dropping <= 0; overrun <= 0;
            received_bytes <= 0; dropped_bytes <= 0; scan <= 0; group_len <= 0;
        end else begin
            if (input_valid && input_ready) begin
                received_bytes <= received_bytes + 1'b1;
                if (length == 0 && !dropping) begin
                    packet[0] <= {1'b0,input_channel}; packet[1] <= input_data;
                    checksum <= 0 - {1'b0,input_channel} - input_data; length <= 2;
                    if (input_last) begin
                        packet[2] <= 0 - {1'b0,input_channel} - input_data;
                        length <= 3; scan <= 0; group_start <= 0; group_len <= 0; state <= FIND;
                    end
                end else if (!dropping) begin
                    if (length <= MAX_PAYLOAD) begin
                        packet[length] <= input_data; checksum <= checksum - input_data; length <= length + 1'b1;
                    end else begin dropping <= 1; overrun <= 1; dropped_bytes <= dropped_bytes + length + 1'b1; end
                end else dropped_bytes <= dropped_bytes + 1'b1;
                if (input_last && length != 0) begin
                    if (dropping || length > MAX_PAYLOAD) begin length <= 0; checksum <= 0; dropping <= 0; end
                    else begin
                        packet[length + 1'b1] <= checksum - input_data;
                        length <= length + 2'd2; scan <= 0; group_start <= 0; group_len <= 0; state <= FIND;
                    end
                end
            end
            if (state == FIND) begin
                if (scan == length || packet[scan] == 0 || group_len == 254) begin
                    emit_index <= group_start; state <= CODE;
                end else begin scan <= scan + 1'b1; group_len <= group_len + 1'b1; end
            end
            if (state == CODE && output_ready) begin
                if (group_len == 0) begin
                    if (scan >= length) state <= DELIMITER;
                    else begin scan <= scan + 1'b1; group_start <= scan + 1'b1; group_len <= 0; state <= FIND; end
                end else state <= DATA;
            end
            if (state == DATA && output_ready) begin
                if (emit_index + 1'b1 == group_start + group_len) begin
                    if (scan >= length) state <= DELIMITER;
                    else if (group_len == 254) begin group_start <= scan; group_len <= 0; state <= FIND; end
                    else begin scan <= scan + 1'b1; group_start <= scan + 1'b1; group_len <= 0; state <= FIND; end
                end else emit_index <= emit_index + 1'b1;
            end
            if (state == DELIMITER && output_ready) begin state <= LOAD; length <= 0; checksum <= 0; end
        end
    end
endmodule
`default_nettype wire
