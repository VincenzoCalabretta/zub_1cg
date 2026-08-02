`timescale 1ns/1ps
`default_nettype none
// Packet-level CMSIS-DAP command engine. Ethernet framing stays in firmware;
// this block sees the unchanged USB payload. The synthetic target provides
// deterministic Arm DP/AP ACK and read-data behavior for regression tests.
module orbtrace_dap_engine #(
    parameter integer MAX_PACKET = 1024,
    parameter integer MAX_TRANSFERS = 32
) (
    input wire logic clk,
    input wire logic reset_n,
    input wire logic [7:0] command_data,
    input wire logic command_valid,
    input wire logic command_last,
    output logic command_ready,
    output logic [7:0] response_data,
    output logic response_valid,
    output logic response_last,
    input wire logic response_ready,
    input wire logic force_wait,
    input wire logic force_fault,
    input wire logic force_parity_error,
    output logic debug_complete,
    output logic [63:0] transfer_count,
    output logic [31:0] abort_count
);
    localparam integer MAX_RESPONSE = 3 + MAX_TRANSFERS * 4;
    typedef enum logic [2:0] {CAPTURE, PREPARE, FETCH_TRANSFER, BUILD_TRANSFER, RESPOND} state_t;
    state_t state;
    logic [7:0] request_mem [0:MAX_PACKET-1];
    logic [7:0] response_mem [0:MAX_RESPONSE-1];
    integer request_pos, request_length, response_pos, response_length;
    integer transfer_index, transfer_request_pos, output_index, completed_transfers;
    integer request_read_address;
    logic [7:0] request_read_data, header_command, header_argument, header_count, header_data;

    assign command_ready = state == CAPTURE && request_pos < MAX_PACKET;
    assign response_valid = state == RESPOND;
    assign response_data = response_mem[response_pos];
    assign response_last = state == RESPOND && response_pos + 1 == response_length;

    // Dedicated synchronous read port lets Vivado infer block RAM for the
    // 1024-byte command packet instead of a very large asynchronous LUT mux.
    always_ff @(posedge clk) request_read_data <= request_mem[request_read_address];

    always_ff @(posedge clk) begin
        if (!reset_n) begin
            state <= CAPTURE;
            request_pos <= 0; request_length <= 0;
            response_pos <= 0; response_length <= 0;
            request_read_address <= 0; header_command <= 0; header_argument <= 0; header_count <= 0; header_data <= 0;
            debug_complete <= 0; transfer_count <= 0; abort_count <= 0;
        end else begin
            debug_complete <= 0;
            case (state)
                CAPTURE: if (command_valid && command_ready) begin
                    request_mem[request_pos] <= command_data;
                    if (request_pos == 0) header_command <= command_data;
                    if (request_pos == 1) header_argument <= command_data;
                    if (request_pos == 2) header_count <= command_data;
                    if (request_pos == 3) header_data <= command_data;
                    if (command_last) begin
                        request_length <= request_pos + 1;
                        request_pos <= 0;
                        state <= PREPARE;
                    end else request_pos <= request_pos + 1;
                end
                PREPARE: begin
                    response_mem[0] <= header_command;
                    response_pos <= 0;
                    case (header_command)
                        8'h00: begin // DAP_Info
                            if (request_length > 1 && header_argument == 8'h01) begin
                                response_mem[1]<=7;
                                response_mem[2]<=8'h4f; response_mem[3]<=8'h72; response_mem[4]<=8'h62;
                                response_mem[5]<=8'h63; response_mem[6]<=8'h6f; response_mem[7]<=8'h64;
                                response_mem[8]<=8'h65; response_length<=9;
                            end else begin response_mem[1]<=0; response_length<=2; end
                        end
                        8'h02: begin // DAP_Connect
                            response_mem[1] <= request_length > 1 ? header_argument & 8'h03 : 0;
                            response_length <= 2;
                        end
                        8'h05: begin // DAP_Transfer
                            response_mem[1] <= 0;
                            if (request_length < 3 || header_count > MAX_TRANSFERS) begin
                                response_mem[2] <= 8'h04;
                                response_length <= 3;
                            end else if (force_wait) begin
                                response_mem[2] <= 8'h02;
                                response_length <= 3;
                            end else if (force_fault) begin
                                response_mem[2] <= 8'h04;
                                response_length <= 3;
                            end else begin
                                response_mem[1] <= 0;
                                response_mem[2] <= force_parity_error ? 8'h09 : 8'h01;
                                output_index = 3;
                                transfer_index <= 0;
                                transfer_request_pos <= 3;
                                request_read_address <= 3;
                                completed_transfers <= 0;
                                state <= FETCH_TRANSFER;
                            end
                        end
                        8'h07,8'h08: begin // TransferAbort / WriteABORT
                            abort_count <= abort_count + 1'b1;
                            response_mem[1] <= 0;
                            response_length <= 2;
                        end
                        8'h10: begin // DAP_SWJ_Pins
                            response_mem[1] <= request_length > 1 ? header_argument : 0;
                            response_length <= 2;
                        end
                        8'h14: begin // DAP_JTAG_Sequence synthetic TDO
                            response_mem[1] <= 0;
                            response_mem[2] <= request_length > 3 ? header_data ^ 8'h01 : 0;
                            response_length <= 3;
                        end
                        default: begin response_mem[1]<=8'hff; response_length<=2; end
                    endcase
                    if (header_command != 8'h05 || request_length < 3 ||
                        header_count > MAX_TRANSFERS || force_wait || force_fault)
                        state <= RESPOND;
                end
                FETCH_TRANSFER: state <= BUILD_TRANSFER;
                BUILD_TRANSFER: begin
                    if (transfer_index >= header_count) begin
                        response_mem[1] <= completed_transfers;
                        response_length <= output_index;
                        state <= RESPOND;
                    end else if (transfer_request_pos >= request_length) begin
                        response_mem[1] <= completed_transfers;
                        response_mem[2] <= 8'h04;
                        response_length <= output_index;
                        state <= RESPOND;
                    end else begin
                        transfer_count <= transfer_count + 1'b1;
                        completed_transfers <= completed_transfers + 1;
                        transfer_index <= transfer_index + 1;
                        if (request_read_data[1]) begin
                            response_mem[output_index] <= request_read_data;
                            response_mem[output_index+1] <= 8'h00;
                            response_mem[output_index+2] <= 8'ha5;
                            response_mem[output_index+3] <= 8'ha5;
                            output_index <= output_index + 4;
                            transfer_request_pos <= transfer_request_pos + 1;
                            request_read_address <= transfer_request_pos + 1;
                            if (transfer_index + 1 < header_count) state <= FETCH_TRANSFER;
                        end else if (transfer_request_pos + 4 < request_length) begin
                            transfer_request_pos <= transfer_request_pos + 5;
                            request_read_address <= transfer_request_pos + 5;
                            if (transfer_index + 1 < header_count) state <= FETCH_TRANSFER;
                        end else begin
                            response_mem[1] <= completed_transfers;
                            response_mem[2] <= 8'h04;
                            response_length <= output_index;
                            state <= RESPOND;
                        end
                    end
                end
                RESPOND: if (response_valid && response_ready) begin
                    if (response_last) begin
                        state <= CAPTURE;
                        response_pos <= 0;
                        debug_complete <= 1;
                    end else response_pos <= response_pos + 1;
                end
            endcase
        end
    end
endmodule
`default_nettype wire
