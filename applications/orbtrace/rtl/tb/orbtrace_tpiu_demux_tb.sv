`default_nettype none
// Regression test for the 2026-08-19 sync-word byte-order fix in
// orbtrace_tpiu_demux.sv (see M3_TRACE_VERIFICATION_PLAN.md). A genuine
// CoreSight Full Sync Packet is, chronologically, three 0xFF bytes followed
// by a terminating 0x7F -- confirmed against sigrok's independent arm_tpiu
// reference decoder. The old match constant (32'hffffff7f) searched for the
// reverse order (0x7F first, then three 0xFF), so it could never correctly
// frame-align on a genuine sync event even if one occurred. This testbench
// feeds a byte stream containing the CORRECT chronological sync sequence
// immediately followed by a synthetic, well-formed 16-byte CoreSight frame
// (all real content on TRACEBUSID/channel 1) and checks that the demux
// locks and decodes it byte-for-byte -- something the pre-fix constant
// would have missed at this exact alignment.
module orbtrace_tpiu_demux_tb;
    logic clk = 0;
    always #5 clk = ~clk;

    logic reset_n = 0;
    logic reset_sync = 0;
    logic [7:0] input_data = 0;
    logic input_valid = 0;
    logic input_ready;
    logic [6:0] output_channel;
    logic [7:0] output_data;
    logic output_valid;
    logic output_ready = 1;
    logic [63:0] sync_loss_count;
    logic m3_source = 1;

    orbtrace_tpiu_demux dut(.clk,.reset_n,.reset_sync,.input_data,.input_valid,.input_ready,
        .output_channel,.output_data,.output_valid,.output_ready,.sync_loss_count,.m3_source);

    // Once emitting starts, the demux self-paces on output_ready (tied high
    // here) and drains an entire 16-byte frame across the following ~15
    // cycles WITHOUT waiting for new input -- input_ready drops for that
    // whole span. A stimulus task that blocks on input_ready (send_byte,
    // below) can therefore return only *after* a frame has already been
    // fully emitted and drained, having observed none of it. Decouple
    // observation from stimulus: record every (channel,data) the demux
    // emits into a queue continuously, for the whole simulation, and let
    // each test phase inspect the queue's tail once its stimulus settles.
    typedef struct packed { logic [6:0] channel; logic [7:0] data; } emitted_t;
    emitted_t emitted_q[$];
    always @(posedge clk) begin
        if (output_valid) emitted_q.push_back('{output_channel, output_data});
    end

    // Each test phase below starts from a known-clean state via this pulse
    // (reset_sync is the DUT's own soft-resync input) rather than relying on
    // whatever synced/fill/emitting state the previous phase happened to
    // leave behind -- e.g. a continuous idle stream can leave a transient
    // false lock (synced==1) dangling at its end, which would otherwise eat
    // the next phase's first stimulus byte as if it were ordinary frame
    // content instead of a sync search.
    // Only used to isolate each test phase's state (see below); the
    // sync_loss_count bump this itself can cause (if the DUT was mid-lock)
    // is accounted for by capturing loss_before AFTER calling this, not
    // before.
    task automatic resync_dut;
        begin
            @(posedge clk);
            reset_sync = 1;
            @(posedge clk);
            reset_sync = 0;
            @(posedge clk);
        end
    endtask

    task automatic send_byte(input logic [7:0] b);
        begin
            @(posedge clk);
            input_data = b;
            input_valid = 1;
            @(negedge clk);
            while (!input_ready) begin
                @(posedge clk); @(negedge clk);
            end
            @(posedge clk);
            input_valid = 0;
            @(negedge clk);
        end
    endtask

    // One well-formed 16-byte CoreSight formatter frame, all content on
    // channel 1 (M3's fixed TRACEBUSID), aux byte (frame[15]) all zero so
    // every even-index byte's reconstructed LSB is 0 (no delayed-channel
    // path exercised -- kept simple, that path is orthogonal to the
    // byte-order fix under test).
    // frame[0] = ID byte selecting channel 1: unmangled[7:1]=7'd1, is_id bit
    // (raw LSB) = 1 -> 8'b0000001_1 = 8'h03.
    // frame[2],[4],...,[14] = arbitrary real data, LSB=0 so they are never
    // mistaken for ID bytes.
    // frame[1],[3],...,[13] = arbitrary real data (odd positions are never
    // ID bytes regardless of their LSB).
    // frame[1..14] is what should actually be observed at the output (frame[0]
    // is consumed as the ID byte, frame[15] is consumed as the aux byte --
    // neither is ever emitted directly).
    logic [7:0] real_frame [0:15];
    initial begin
        real_frame[0]  = 8'h03; // ID byte -> channel 1
        real_frame[1]  = 8'hA1;
        real_frame[2]  = 8'hA2; // LSB=0, plain data
        real_frame[3]  = 8'hA3;
        real_frame[4]  = 8'hA4;
        real_frame[5]  = 8'hA5;
        real_frame[6]  = 8'hA6;
        real_frame[7]  = 8'hA7;
        real_frame[8]  = 8'hA8;
        real_frame[9]  = 8'hA9;
        real_frame[10] = 8'hAA;
        real_frame[11] = 8'hAB;
        real_frame[12] = 8'hAC;
        real_frame[13] = 8'hAD;
        real_frame[14] = 8'hAE; // LSB=0
        real_frame[15] = 8'h00; // aux byte, all bits 0
    end

    int errors = 0;

    initial begin
        reset_n = 0; input_valid = 0; m3_source = 1;
        repeat (4) @(posedge clk);
        reset_n = 1;
        @(posedge clk);

        // --- Test 1: idle alias never produces a stable channel==1 lock ---
        // The empirically-observed real-hardware idle sub-pattern
        // (FF 7F FF FF repeating) is byte-order-symmetric: it contains the
        // 4-byte substring in BOTH chronological orders somewhere in its
        // period, so it can still cause the demux to (falsely) enter the
        // synced state. What it must never do is produce a stable,
        // sustained channel==1 output, because a real ID byte drawn from
        // this alias content won't legitimately decode to channel 1 -- the
        // channel-plausibility gate (m3_source) must keep kicking it back to
        // the search state.
        begin
            logic [7:0] idle_period [0:3];
            int loss_before;
            idle_period[0] = 8'hFF; idle_period[1] = 8'h7F;
            idle_period[2] = 8'hFF; idle_period[3] = 8'hFF;
            loss_before = sync_loss_count;
            emitted_q.delete();
            for (int i = 0; i < 400; i++) send_byte(idle_period[i % 4]);
            foreach (emitted_q[i]) begin
                if (emitted_q[i].channel == 7'd1) begin
                    $display("FAIL: idle alias produced a channel==1 output (data=%02x)", emitted_q[i].data);
                    errors++;
                end
            end
            if (sync_loss_count == loss_before) begin
                $display("FAIL: idle alias never triggered a single sync_loss (plausibility gate not exercising)");
                errors++;
            end
            $display("idle-alias sync_loss_count after 400 bytes: %0d (delta %0d)", sync_loss_count, sync_loss_count - loss_before);
        end

        // --- Test 2: genuine chronological sync (FF,FF,FF,7F) + real
        // channel-1 frame decodes correctly end-to-end ---
        begin
            int loss_before;
            resync_dut(); // don't inherit whatever lock state test 1 left behind
            loss_before = sync_loss_count;
            emitted_q.delete();

            send_byte(8'hFF);
            send_byte(8'hFF);
            send_byte(8'hFF);
            send_byte(8'h7F); // completes the genuine sync -- synced<=1, fill<=0

            // Feed the 16-byte frame; the monitor above records everything
            // the demux emits as it drains, even though this loop's last
            // send_byte call blocks (on input_ready) until the whole frame
            // has been emitted internally.
            for (int i = 0; i < 16; i++) send_byte(real_frame[i]);
            repeat (4) @(posedge clk); // let anything trailing settle

            if (emitted_q.size() != 14) begin
                $display("FAIL: expected 14 emitted bytes (frame[1..14]), got %0d", emitted_q.size());
                errors++;
            end else begin
                bit ok = 1;
                foreach (emitted_q[i]) begin
                    logic [7:0] expected = real_frame[i + 1];
                    if (emitted_q[i].channel !== 7'd1 || emitted_q[i].data !== expected) begin
                        $display("FAIL: emitted[%0d] = channel=%0d data=%02x, expected channel=1 data=%02x",
                            i, emitted_q[i].channel, emitted_q[i].data, expected);
                        errors++;
                        ok = 0;
                    end
                end
                if (ok) $display("genuine sync + real channel-1 frame decoded correctly, byte-for-byte (byte-order fix verified)");
            end
            if (sync_loss_count !== loss_before) begin
                $display("FAIL: genuine, channel-1-consistent frame incorrectly triggered a sync_loss");
                errors++;
            end
        end

        // --- Test 3: plausibility gate still rejects a wrong-channel ID
        // byte mid-stream (unchanged behavior, regression check) ---
        begin
            int loss_before;
            resync_dut(); // don't inherit test 2's post-frame lock state
            loss_before = sync_loss_count;
            send_byte(8'hFF); send_byte(8'hFF); send_byte(8'hFF); send_byte(8'h7F);
            // ID byte selecting channel 2 instead of 1: unmangled[7:1]=7'd2,
            // is_id bit=1 -> 8'b0000010_1 = 8'h05.
            send_byte(8'h05);
            repeat (16) send_byte(8'h00);
            if (sync_loss_count === loss_before) begin
                $display("FAIL: wrong-channel ID byte under m3_source did not trigger sync_loss");
                errors++;
            end else begin
                $display("wrong-channel ID byte correctly rejected (sync_loss_count delta %0d)", sync_loss_count - loss_before);
            end
        end

        if (errors == 0) $display("orbtrace TPIU demux RTL tests passed");
        else $display("orbtrace TPIU demux RTL tests FAILED (%0d errors)", errors);
        $finish;
    end
endmodule
`default_nettype wire
