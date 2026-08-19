`default_nettype none
module orbtrace_tpiu_demux (
    input  wire logic       clk,
    input  wire logic       reset_n,
    input  wire logic       reset_sync,
    input  wire logic [7:0] input_data,
    input  wire logic       input_valid,
    output      logic       input_ready,
    output      logic [6:0] output_channel,
    output      logic [7:0] output_data,
    output      logic       output_valid,
    input  wire logic       output_ready,
    output      logic [63:0] sync_loss_count,
    // M3-source-only sync-lock plausibility gate -- see the sequential
    // block below. Leaves the PS/ETM path (m3_source==0) bit-for-bit
    // unchanged.
    input  wire logic       m3_source
);
    // TRACEBUSID the M3 always configures (sdk/bsp/m3/itm.h's
    // m3_itm_init(), M3_ITM_TCR_TRACEBUSID_SHIFT) -- never varies in this
    // codebase, so any is_id byte decoding to a different channel while
    // m3_source is selected is conclusive evidence of a false sync lock,
    // not a legitimate second trace source.
    localparam logic [6:0] M3_EXPECTED_CHANNEL = 7'd1;
    logic synced;
    logic [31:0] sync_window;
    logic [7:0] frame [0:15];
    logic [4:0] fill;
    logic [4:0] emit;
    logic emitting;
    logic [6:0] channel, delayed_channel;
    logic delayed_valid;
    logic [7:0] unmangled;
    logic is_id;

    always_comb begin
        unmangled = frame[emit];
        is_id = 1'b0;
        if (!emit[0]) begin
            unmangled = {frame[emit][7:1], frame[15][emit >> 1]};
            is_id = frame[emit][0];
        end
        input_ready = !emitting;
        output_valid = emitting && !is_id && channel != 0;
        output_channel = channel;
        output_data = unmangled;
    end

    always_ff @(posedge clk) begin
        if (!reset_n) begin
            synced <= 0; sync_window <= 0; fill <= 0; emit <= 0; emitting <= 0;
            channel <= 0; delayed_channel <= 0; delayed_valid <= 0; sync_loss_count <= 0;
        end else begin
            if (reset_sync) begin
                if (synced) sync_loss_count <= sync_loss_count + 1'b1;
                synced <= 0; fill <= 0; emitting <= 0;
            end
            if (input_valid && input_ready) begin
                sync_window <= {input_data, sync_window[31:8]};
                if (!synced) begin
                    // Full Sync Packet is, chronologically, three 0xFF bytes
                    // followed by a terminating 0x7F (confirmed against an
                    // independent reference decoder -- sigrok's arm_tpiu
                    // -- since the local TRM defers the exact wire format to
                    // the CoreSight Architecture Specification, not present
                    // in this project's docs). sync_window's shift-register
                    // convention places the newest byte at [31:24] and the
                    // oldest of the tracked 4 at [7:0], so the chronologically
                    // -last byte (0x7F, which completes the match) belongs at
                    // the top: 8'h7f (newest) ++ 8'hff ++ 8'hff ++ 8'hff
                    // (oldest). The previous constant, 32'hffffff7f, put 0x7F
                    // at the OLDEST position instead -- backwards -- so any
                    // genuine sync event was searched for in the wrong byte
                    // order and could never align frame decode correctly
                    // even when real content followed it. See the
                    // 2026-08-19 M3_TRACE_VERIFICATION_PLAN.md entry for the
                    // real ILA capture that found this.
                    if ({input_data, sync_window[31:8]} == 32'h7fffffff) begin synced <= 1; fill <= 0; end
                end else begin
                    frame[fill] <= input_data;
                    if (fill == 15) begin fill <= 0; emit <= 0; emitting <= 1; end
                    else fill <= fill + 1'b1;
                end
            end
            if (emitting && (is_id || channel == 0 || output_ready)) begin
                // Plausibility gate (2026-08-18, M3 only): the bare
                // FFFFFF7F substring search above can and does false-lock
                // onto structure that occurs naturally within the M3's own
                // idle output -- see M3_TRACE_VERIFICATION_PLAN.md's
                // 2026-08-18 "cycle-accurate capture" finding (a stable
                // `FF 7F FF FF` idle sub-pattern trivially contains
                // FFFFFF7F at every period). A genuine lock's every ID byte
                // must decode to M3_EXPECTED_CHANNEL; anything else is
                // conclusive evidence of a false lock -- drop back to
                // searching immediately rather than continuing to emit
                // garbage at the wrong frame phase forever.
                if (m3_source && is_id && unmangled[7:1] != M3_EXPECTED_CHANNEL) begin
                    synced <= 0;
                    fill <= 0;
                    emitting <= 0;
                    sync_loss_count <= sync_loss_count + 1'b1;
                end else begin
                    if (is_id) begin
                        if (unmangled[0]) begin delayed_channel <= unmangled[7:1]; delayed_valid <= 1; end
                        else channel <= unmangled[7:1];
                    end else if (delayed_valid) begin channel <= delayed_channel; delayed_valid <= 0; end
                    if (emit == 14) emitting <= 0; else emit <= emit + 1'b1;
                end
            end
        end
    end
endmodule
`default_nettype wire

