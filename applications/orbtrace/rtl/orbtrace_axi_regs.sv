`default_nettype none
module orbtrace_axi_regs (
    input  wire logic        aclk, input wire logic aresetn,
    input  wire logic [15:0] s_axi_awaddr, input wire logic s_axi_awvalid, output logic s_axi_awready,
    input  wire logic [31:0] s_axi_wdata, input wire logic [3:0] s_axi_wstrb,
    input  wire logic s_axi_wvalid, output logic s_axi_wready,
    output logic [1:0] s_axi_bresp, output logic s_axi_bvalid, input wire logic s_axi_bready,
    input  wire logic [15:0] s_axi_araddr, input wire logic s_axi_arvalid, output logic s_axi_arready,
    output logic [31:0] s_axi_rdata, output logic [1:0] s_axi_rresp,
    output logic s_axi_rvalid, input wire logic s_axi_rready,
    output logic start_pulse, output logic stop_pulse, output logic reset_pulse,
    output logic [1:0] source_select, output logic [2:0] trace_format, output logic [31:0] swo_baud,
    output logic [63:0] dma_base, output logic [31:0] dma_ring_size,
    input wire logic irq_dma_complete, input wire logic irq_overrun, input wire logic irq_debug_complete,
    output logic irq,
    input wire logic [63:0] received_bytes, input wire logic [63:0] dropped_bytes,
    input wire logic [63:0] sync_loss, input wire logic [31:0] fifo_high_water,
    input wire logic [63:0] dma_faults,
    output logic [7:0] dap_command_data, output logic dap_command_valid,
    output logic dap_command_last, input wire logic dap_command_ready,
    input wire logic [7:0] dap_response_data, input wire logic dap_response_valid,
    input wire logic dap_response_last, output logic dap_response_ready,
    output logic dap_force_wait, output logic dap_force_fault, output logic dap_force_parity_error,
    input wire logic [63:0] dap_transfer_count, input wire logic [31:0] dap_abort_count
);
    `include "orbtrace_regs.svh"
    logic [2:0] irq_status, irq_enable;
    logic [15:0] awaddr_hold;
    logic [31:0] wdata_hold;
    logic [3:0] wstrb_hold;
    logic aw_pending, w_pending;
    logic [7:0] dap_response_hold;
    logic dap_response_hold_valid, dap_response_hold_last;
    wire aw_take = s_axi_awvalid && s_axi_awready;
    wire w_take = s_axi_wvalid && s_axi_wready;
    wire write_fire = (aw_pending || aw_take) && (w_pending || w_take) && !s_axi_bvalid;
    wire [15:0] write_address = aw_pending ? awaddr_hold : s_axi_awaddr;
    wire [31:0] write_data = w_pending ? wdata_hold : s_axi_wdata;
    wire read_fire = s_axi_arvalid && s_axi_arready;
    assign s_axi_awready = !aw_pending && !s_axi_bvalid;
    assign s_axi_wready = !w_pending && !s_axi_bvalid;
    assign s_axi_bresp = 2'b00;
    assign s_axi_arready = !s_axi_rvalid;
    assign s_axi_rresp = 2'b00;
    assign irq = |(irq_status & irq_enable);
    assign dap_response_ready = !dap_response_hold_valid;

    always_ff @(posedge aclk) begin
        if (!aresetn) begin
            s_axi_bvalid <= 0; s_axi_rvalid <= 0; s_axi_rdata <= 0;
            aw_pending <= 0; w_pending <= 0; awaddr_hold <= 0; wdata_hold <= 0; wstrb_hold <= 0;
            start_pulse <= 0; stop_pulse <= 0; reset_pulse <= 0;
            source_select <= 0; trace_format <= 0; swo_baud <= 2_000_000;
            dma_base <= 0; dma_ring_size <= 0; irq_status <= 0; irq_enable <= 0;
            dap_command_data <= 0; dap_command_valid <= 0; dap_command_last <= 0;
            dap_response_hold <= 0; dap_response_hold_valid <= 0; dap_response_hold_last <= 0;
            dap_force_wait <= 0; dap_force_fault <= 0; dap_force_parity_error <= 0;
        end else begin
            start_pulse <= 0; stop_pulse <= 0; reset_pulse <= 0;
            if (irq_dma_complete) irq_status[0] <= 1;
            if (irq_overrun) irq_status[1] <= 1;
            if (irq_debug_complete) irq_status[2] <= 1;
            if (s_axi_bvalid && s_axi_bready) s_axi_bvalid <= 0;
            if (dap_command_valid && dap_command_ready) dap_command_valid <= 0;
            if (dap_response_valid && dap_response_ready) begin
                dap_response_hold <= dap_response_data;
                dap_response_hold_last <= dap_response_last;
                dap_response_hold_valid <= 1;
            end
            if (aw_take) begin awaddr_hold <= s_axi_awaddr; aw_pending <= 1; end
            if (w_take) begin wdata_hold <= s_axi_wdata; wstrb_hold <= s_axi_wstrb; w_pending <= 1; end
            if (write_fire) begin
                s_axi_bvalid <= 1; aw_pending <= 0; w_pending <= 0;
                case (write_address)
                    ORBTRACE_REG_CONTROL: begin start_pulse <= write_data[0]; stop_pulse <= write_data[1]; reset_pulse <= write_data[2]; end
                    ORBTRACE_REG_SOURCE_FORMAT: begin source_select <= write_data[1:0]; trace_format <= write_data[10:8]; end
                    ORBTRACE_REG_SWO_BAUD: swo_baud <= write_data;
                    ORBTRACE_REG_DMA_BASE_LO: dma_base[31:0] <= write_data;
                    ORBTRACE_REG_DMA_BASE_HI: dma_base[63:32] <= write_data;
                    ORBTRACE_REG_DMA_RING_SIZE: dma_ring_size <= write_data;
                    ORBTRACE_REG_IRQ_STATUS: irq_status <= irq_status & ~write_data[2:0];
                    ORBTRACE_REG_IRQ_ENABLE: irq_enable <= write_data[2:0];
                    ORBTRACE_REG_DAP_COMMAND: if (!dap_command_valid) begin
                        dap_command_data <= write_data[7:0];
                        dap_command_last <= write_data[8];
                        dap_command_valid <= 1;
                    end
                    ORBTRACE_REG_DAP_CONTROL: begin
                        dap_force_wait <= write_data[0];
                        dap_force_fault <= write_data[1];
                        dap_force_parity_error <= write_data[2];
                    end
                    default: ;
                endcase
            end
            if (s_axi_rvalid && s_axi_rready) s_axi_rvalid <= 0;
            if (read_fire) begin
                s_axi_rvalid <= 1;
                case (s_axi_araddr)
                    ORBTRACE_REG_ID: s_axi_rdata <= 32'h4f524254;
                    ORBTRACE_REG_VERSION: s_axi_rdata <= 32'h00010000;
                    ORBTRACE_REG_SOURCE_FORMAT: s_axi_rdata <= {21'b0,trace_format,6'b0,source_select};
                    ORBTRACE_REG_SWO_BAUD: s_axi_rdata <= swo_baud;
                    ORBTRACE_REG_DMA_BASE_LO: s_axi_rdata <= dma_base[31:0];
                    ORBTRACE_REG_DMA_BASE_HI: s_axi_rdata <= dma_base[63:32];
                    ORBTRACE_REG_DMA_RING_SIZE: s_axi_rdata <= dma_ring_size;
                    ORBTRACE_REG_IRQ_STATUS: s_axi_rdata <= {29'b0,irq_status};
                    ORBTRACE_REG_IRQ_ENABLE: s_axi_rdata <= {29'b0,irq_enable};
                    ORBTRACE_REG_RX_BYTES_LO: s_axi_rdata <= received_bytes[31:0];
                    ORBTRACE_REG_RX_BYTES_HI: s_axi_rdata <= received_bytes[63:32];
                    ORBTRACE_REG_DROP_BYTES_LO: s_axi_rdata <= dropped_bytes[31:0];
                    ORBTRACE_REG_DROP_BYTES_HI: s_axi_rdata <= dropped_bytes[63:32];
                    ORBTRACE_REG_SYNC_LOSS_LO: s_axi_rdata <= sync_loss[31:0];
                    ORBTRACE_REG_SYNC_LOSS_HI: s_axi_rdata <= sync_loss[63:32];
                    ORBTRACE_REG_FIFO_HIGH_WATER: s_axi_rdata <= fifo_high_water;
                    ORBTRACE_REG_DMA_FAULTS_LO: s_axi_rdata <= dma_faults[31:0];
                    ORBTRACE_REG_DMA_FAULTS_HI: s_axi_rdata <= dma_faults[63:32];
                    ORBTRACE_REG_DAP_RESPONSE: begin
                        s_axi_rdata <= {22'b0,dap_response_hold_last,dap_response_hold_valid,dap_response_hold};
                        dap_response_hold_valid <= 0;
                    end
                    ORBTRACE_REG_DAP_STATUS: s_axi_rdata <= {30'b0,dap_response_hold_valid,!dap_command_valid};
                    ORBTRACE_REG_DAP_CONTROL: s_axi_rdata <= {29'b0,dap_force_parity_error,dap_force_fault,dap_force_wait};
                    ORBTRACE_REG_DAP_TRANSFERS_LO: s_axi_rdata <= dap_transfer_count[31:0];
                    ORBTRACE_REG_DAP_TRANSFERS_HI: s_axi_rdata <= dap_transfer_count[63:32];
                    ORBTRACE_REG_DAP_ABORTS: s_axi_rdata <= dap_abort_count;
                    default: s_axi_rdata <= 0;
                endcase
            end
        end
    end
endmodule
`default_nettype wire
