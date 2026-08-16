`timescale 1ns/1ps
`default_nettype none
// Packet-level CMSIS-DAP command engine. Ethernet framing stays in firmware;
// this block sees the unchanged USB payload. DAP_Transfer always uses a
// synthetic target providing deterministic Arm DP/AP ACK and read-data
// behavior for regression tests. DAP_JTAG_Sequence and DAP_SWJ_Pins are
// synthetic by default (same reason) but drive a real JTAG-DP — e.g. the
// PL-hosted Cortex-M3's — when use_real_target is set.
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
    output logic [31:0] abort_count,
    // When 0 (default — required for the deterministic protocol tests in
    // tb/orbtrace_dap_tb.sv), DAP_JTAG_Sequence and DAP_SWJ_Pins keep their
    // synthetic behavior below and these pins stay idle. When 1
    // (ORBTRACE_REG_M3_CONTROL bit 1), those two commands instead drive a
    // real JTAG-DP — the only two commands the host-side remote-bitbang
    // bridge (applications/orbtrace/model/src/main.rs's remote_bitbang())
    // actually sends. DAP_Transfer stays synthetic either way: no ADIv5
    // DPACC/APACC (JTAG-DP register access) implementation exists here, and
    // nothing in this repo's host tooling exercises it over a real target.
    input wire logic use_real_target,
    output logic jtag_tck,
    output logic jtag_tms,
    output logic jtag_tdi,
    input wire logic jtag_tdo,
    output logic jtag_ntrst,
    output logic jtag_nreset
);
    localparam integer MAX_RESPONSE = 3 + MAX_TRANSFERS * 4;
    typedef enum logic [2:0] {CAPTURE, PREPARE, FETCH_TRANSFER, BUILD_TRANSFER, RESPOND, JTAG_SETUP, JTAG_HOLD} state_t;
    state_t state;
    // JTAG_SETUP/JTAG_HOLD half-period counter. 8 aclk cycles/half (100 MHz
    // aclk -> ~6.25 MHz TCK) is a conservative default for bit-banged JTAG;
    // adjust once real M3 JTAG-DP timing is characterized on hardware.
    localparam integer JTAG_HALF_PERIOD = 8;
    integer jtag_wait;
    logic jtag_tdo_sample;
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
            // Idle-safe: nTRST/nRESET default deasserted (1) so a reset
            // pulse never leaves the M3 held in reset via a stale pin
            // value — see orbtrace_pl.v's m3_reset_n = m3_release & jtag_nreset.
            jtag_tck <= 0; jtag_tms <= 1; jtag_tdi <= 0;
            jtag_ntrst <= 1; jtag_nreset <= 1; jtag_wait <= 0; jtag_tdo_sample <= 0;
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
                        8'h10: begin // DAP_SWJ_Pins: bit5=nTRST, bit7=nRESET (CMSIS-DAP layout)
                            if (use_real_target && request_length > 1) begin
                                jtag_ntrst <= header_argument[5];
                                jtag_nreset <= header_argument[7];
                            end
                            response_mem[1] <= request_length > 1 ? header_argument : 0;
                            response_length <= 2;
                        end
                        8'h14: begin // DAP_JTAG_Sequence
                            response_mem[1] <= 0;
                            if (use_real_target) begin
                                // Single-clock-per-packet encoding only —
                                // matches remote_bitbang()'s actual usage
                                // (applications/orbtrace/model/src/main.rs),
                                // not the full multi-sequence CMSIS-DAP wire
                                // format. header_count[7]=TMS, header_data[0]=TDI.
                                jtag_tms <= header_count[7];
                                jtag_tdi <= header_data[0];
                                jtag_tck <= 0;
                                jtag_wait <= 0;
                            end else begin
                                response_mem[2] <= request_length > 3 ? header_data ^ 8'h01 : 0;
                                response_length <= 3;
                            end
                        end
                        default: begin response_mem[1]<=8'hff; response_length<=2; end
                    endcase
                    if (header_command == 8'h14 && use_real_target)
                        state <= JTAG_SETUP;
                    else if (header_command != 8'h05 || request_length < 3 ||
                        header_count > MAX_TRANSFERS || force_wait || force_fault)
                        state <= RESPOND;
                end
                JTAG_SETUP: begin // TCK held low so TMS/TDI are stable before the rising edge
                    jtag_wait <= jtag_wait + 1;
                    if (jtag_wait >= JTAG_HALF_PERIOD) begin
                        jtag_tck <= 1;
                        jtag_wait <= 0;
                        state <= JTAG_HOLD;
                    end
                end
                JTAG_HOLD: begin // TCK held high; sample TDO before the falling edge
                    jtag_wait <= jtag_wait + 1;
                    if (jtag_wait == JTAG_HALF_PERIOD - 1) jtag_tdo_sample <= jtag_tdo;
                    if (jtag_wait >= JTAG_HALF_PERIOD) begin
                        jtag_tck <= 0;
                        response_mem[2] <= {7'b0, jtag_tdo_sample};
                        response_length <= 3;
                        state <= RESPOND;
                    end
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
