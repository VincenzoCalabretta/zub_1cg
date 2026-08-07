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
    output      logic [63:0] sync_loss_count
);
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
                    if ({input_data, sync_window[31:8]} == 32'hffffff7f) begin synced <= 1; fill <= 0; end
                end else begin
                    frame[fill] <= input_data;
                    if (fill == 15) begin fill <= 0; emit <= 0; emitting <= 1; end
                    else fill <= fill + 1'b1;
                end
            end
            if (emitting && (is_id || channel == 0 || output_ready)) begin
                if (is_id) begin
                    if (unmangled[0]) begin delayed_channel <= unmangled[7:1]; delayed_valid <= 1; end
                    else channel <= unmangled[7:1];
                end else if (delayed_valid) begin channel <= delayed_channel; delayed_valid <= 0; end
                if (emit == 14) emitting <= 0; else emit <= emit + 1'b1;
            end
        end
    end
endmodule
`default_nettype wire

