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
    localparam int MAX_RAW = MAX_PAYLOAD + 2; // channel + payload + checksum
    localparam int MAX_GROUPS = MAX_RAW + 1;
    localparam int RW = $clog2(MAX_RAW + 1);
    localparam int GW = $clog2(MAX_GROUPS + 1);
    localparam int PW = $clog2(MAX_PAYLOAD + 2);

    /* Each ping-pong bank has two single-write-port distributed memories:
     * raw packet bytes and the COBS length of each group.  This retains the
     * one-byte-per-clock encoder while mapping to RAM primitives; writing a
     * fully encoded image at both its tail and an earlier code position in
     * the same clock instead creates an unimplementable multiport register
     * array on ZU1CG. */
    (* ram_style = "distributed" *) logic [7:0] packet0 [0:MAX_RAW-1];
    (* ram_style = "distributed" *) logic [7:0] packet1 [0:MAX_RAW-1];
    (* ram_style = "distributed" *) logic [7:0] groups0 [0:MAX_GROUPS-1];
    (* ram_style = "distributed" *) logic [7:0] groups1 [0:MAX_GROUPS-1];
    logic [GW-1:0] bank_group_count [0:1];
    logic [1:0] bank_full;
    logic load_bank, encode_bank, next_encode_bank;

    typedef enum logic [1:0] {L_ACCEPT, L_FIRST_DATA, L_CHECKSUM, L_FINALIZE} load_state_t;
    typedef enum logic [1:0] {E_IDLE, E_CODE, E_DATA, E_DELIMITER} emit_state_t;
    load_state_t load_state;
    emit_state_t emit_state;

    logic [PW-1:0] load_payload_count;
    logic [RW-1:0] load_raw_length;
    logic [GW-1:0] load_group_index;
    logic [7:0] load_group_length;
    logic [7:0] checksum, pending_data;
    logic pending_last, dropping;
    logic append_valid;
    logic [7:0] append_value;

    logic [RW-1:0] emit_raw_index;
    logic [GW-1:0] emit_group_index;
    logic [7:0] emit_group_length, emit_group_remaining, emit_raw_data;

    assign input_ready = load_state == L_ACCEPT && !bank_full[load_bank];
    assign output_valid = emit_state != E_IDLE;
    assign output_last = emit_state == E_DELIMITER;

    always_comb begin
        emit_group_length = encode_bank ? groups1[emit_group_index] : groups0[emit_group_index];
        emit_raw_data = encode_bank ? packet1[emit_raw_index] : packet0[emit_raw_index];
        case (emit_state)
            E_CODE: output_data = emit_group_length == 8'hfe
                                  ? 8'hff : emit_group_length + 1'b1;
            E_DATA: output_data = emit_raw_data;
            default: output_data = 8'h00;
        endcase
    end

    /* At most one raw byte is incorporated per clock.  The first payload
     * byte is held while the channel byte is inserted, and the checksum is
     * appended after input_last.  The other bank hides these fixed packet
     * setup/finalization clocks during sustained traffic. */
    always_comb begin
        append_valid = 1'b0;
        append_value = 8'h00;
        case (load_state)
            L_ACCEPT: begin
                if (input_valid && input_ready) begin
                    if (load_payload_count == 0) begin
                        append_valid = 1'b1;
                        append_value = {1'b0, input_channel};
                    end else if (!dropping && load_payload_count < MAX_PAYLOAD) begin
                        append_valid = 1'b1;
                        append_value = input_data;
                    end
                end
            end
            L_FIRST_DATA: begin
                append_valid = 1'b1;
                append_value = pending_data;
            end
            L_CHECKSUM: begin
                append_valid = 1'b1;
                append_value = checksum;
            end
            default: begin end
        endcase
    end

    always_ff @(posedge clk) begin
        if (!reset_n) begin
            load_state <= L_ACCEPT;
            emit_state <= E_IDLE;
            load_bank <= 1'b0;
            encode_bank <= 1'b0;
            next_encode_bank <= 1'b0;
            bank_full <= 2'b00;
            bank_group_count[0] <= 0;
            bank_group_count[1] <= 0;
            load_payload_count <= 0;
            load_raw_length <= 0;
            load_group_index <= 0;
            load_group_length <= 0;
            checksum <= 0;
            pending_data <= 0;
            pending_last <= 0;
            dropping <= 0;
            emit_raw_index <= 0;
            emit_group_index <= 0;
            emit_group_remaining <= 0;
            overrun <= 0;
            received_bytes <= 0;
            dropped_bytes <= 0;
        end else begin
            /* Store the raw stream sequentially and finish a COBS group at
             * each zero or after 254 nonzero bytes.  Raw and group memories
             * are independent, so both writes remain single-port patterns. */
            if (append_valid) begin
                if (load_bank) packet1[load_raw_length] <= append_value;
                else packet0[load_raw_length] <= append_value;
                load_raw_length <= load_raw_length + 1'b1;

                if (append_value == 0) begin
                    if (load_bank) groups1[load_group_index] <= load_group_length;
                    else groups0[load_group_index] <= load_group_length;
                    load_group_index <= load_group_index + 1'b1;
                    load_group_length <= 0;
                end else if (load_group_length == 8'hfd) begin
                    if (load_bank) groups1[load_group_index] <= 8'hfe;
                    else groups0[load_group_index] <= 8'hfe;
                    load_group_index <= load_group_index + 1'b1;
                    load_group_length <= 0;
                end else load_group_length <= load_group_length + 1'b1;
            end

            case (load_state)
                L_ACCEPT: begin
                    if (input_valid && input_ready) begin
                        received_bytes <= received_bytes + 1'b1;
                        if (load_payload_count == 0) begin
                            pending_data <= input_data;
                            pending_last <= input_last;
                            checksum <= 0 - {1'b0, input_channel} - input_data;
                            load_payload_count <= 1;
                            load_state <= L_FIRST_DATA;
                        end else if (dropping) begin
                            dropped_bytes <= dropped_bytes + 1'b1;
                            if (input_last) begin
                                dropping <= 0;
                                load_payload_count <= 0;
                                load_raw_length <= 0;
                                load_group_index <= 0;
                                load_group_length <= 0;
                                checksum <= 0;
                            end
                        end else if (load_payload_count < MAX_PAYLOAD) begin
                            checksum <= checksum - input_data;
                            load_payload_count <= load_payload_count + 1'b1;
                            if (input_last) load_state <= L_CHECKSUM;
                        end else begin
                            overrun <= 1;
                            dropped_bytes <= dropped_bytes + load_payload_count + 1'b1;
                            if (input_last) begin
                                dropping <= 0;
                                load_payload_count <= 0;
                                load_raw_length <= 0;
                                load_group_index <= 0;
                                load_group_length <= 0;
                                checksum <= 0;
                            end else dropping <= 1;
                        end
                    end
                end
                L_FIRST_DATA: begin
                    if (pending_last) load_state <= L_CHECKSUM;
                    else load_state <= L_ACCEPT;
                end
                L_CHECKSUM: load_state <= L_FINALIZE;
                L_FINALIZE: begin
                    if (load_bank) groups1[load_group_index] <= load_group_length;
                    else groups0[load_group_index] <= load_group_length;
                    bank_group_count[load_bank] <= load_group_index + 1'b1;
                    bank_full[load_bank] <= 1'b1;
                    load_bank <= ~load_bank;
                    load_payload_count <= 0;
                    load_raw_length <= 0;
                    load_group_index <= 0;
                    load_group_length <= 0;
                    checksum <= 0;
                    load_state <= L_ACCEPT;
                end
                default: load_state <= L_ACCEPT;
            endcase

            case (emit_state)
                E_IDLE: begin
                    if (bank_full[next_encode_bank]) begin
                        encode_bank <= next_encode_bank;
                        emit_raw_index <= 0;
                        emit_group_index <= 0;
                        emit_state <= E_CODE;
                    end
                end
                E_CODE: begin
                    if (output_ready) begin
                        if (emit_group_length == 0) begin
                            if (emit_group_index + 1'b1 == bank_group_count[encode_bank])
                                emit_state <= E_DELIMITER;
                            else begin
                                emit_raw_index <= emit_raw_index + 1'b1; // skip zero
                                emit_group_index <= emit_group_index + 1'b1;
                            end
                        end else begin
                            emit_group_remaining <= emit_group_length;
                            emit_state <= E_DATA;
                        end
                    end
                end
                E_DATA: begin
                    if (output_ready) begin
                        if (emit_group_remaining == 1) begin
                            if (emit_group_index + 1'b1 == bank_group_count[encode_bank])
                                emit_state <= E_DELIMITER;
                            else begin
                                emit_raw_index <= emit_raw_index +
                                                  (emit_group_length == 8'hfe ? 1 : 2);
                                emit_group_index <= emit_group_index + 1'b1;
                                emit_state <= E_CODE;
                            end
                        end else begin
                            emit_raw_index <= emit_raw_index + 1'b1;
                            emit_group_remaining <= emit_group_remaining - 1'b1;
                        end
                    end
                end
                E_DELIMITER: begin
                    if (output_ready) begin
                        bank_full[encode_bank] <= 1'b0;
                        next_encode_bank <= ~encode_bank;
                        emit_state <= E_IDLE;
                    end
                end
                default: emit_state <= E_IDLE;
            endcase
        end
    end
endmodule
`default_nettype wire
