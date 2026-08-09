create_bd_design zub_orbtrace
set ps [create_bd_cell -type ip -vlnv xilinx.com:ip:zynq_ultra_ps_e:* ps]
zub1cg_apply_ps_preset $ps
set_property -dict [list \
    CONFIG.PSU__USE__M_AXI_GP0 {1} CONFIG.PSU__USE__M_AXI_GP1 {0} CONFIG.PSU__USE__M_AXI_GP2 {0} \
    CONFIG.PSU__USE__S_AXI_GP2 {1} CONFIG.PSU__USE__IRQ {1} CONFIG.PSU__USE__IRQ0 {1} \
    CONFIG.PSU__FPGA_PL0_ENABLE {1} CONFIG.PSU__CRL_APB__PL0_REF_CTRL__FREQMHZ {100} \
    CONFIG.PSU__CRF_APB__DBG_TRACE_CTRL__FREQMHZ {100} \
    CONFIG.PSU__TRACE__PERIPHERAL__ENABLE {1} CONFIG.PSU__TRACE__PERIPHERAL__IO {EMIO} \
    CONFIG.PSU__TRACE__WIDTH {4Bit} CONFIG.PSU__TRACE__INTERNAL_WIDTH {32}] $ps

set dma [create_bd_cell -type ip -vlnv xilinx.com:ip:axi_dma:* trace_dma]
set_property -dict [list CONFIG.c_include_sg {1} CONFIG.c_include_mm2s {0} \
    CONFIG.c_include_s2mm {1} CONFIG.c_sg_include_stscntrl_strm {0} \
    CONFIG.c_sg_length_width {26} CONFIG.c_s2mm_burst_size {256}] $dma
set control_ic [create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:* control_ic]
set_property CONFIG.NUM_MI {2} $control_ic
set data_ic [create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:* data_ic]
set_property CONFIG.NUM_SI {2} $data_ic
set rst [create_bd_cell -type ip -vlnv xilinx.com:ip:proc_sys_reset:* pl_reset]
set trace [create_bd_cell -type module -reference orbtrace_pl trace_pl]
set irq_concat [create_bd_cell -type ip -vlnv xilinx.com:ip:xlconcat:* irq_concat]
set_property CONFIG.NUM_PORTS {1} $irq_concat
set zero [create_bd_cell -type ip -vlnv xilinx.com:ip:xlconstant:* zero]
set_property -dict [list CONFIG.CONST_WIDTH {1} CONFIG.CONST_VAL {0}] $zero

connect_bd_intf_net [get_bd_intf_pins ps/M_AXI_HPM0_FPD] [get_bd_intf_pins control_ic/S00_AXI]
connect_bd_intf_net [get_bd_intf_pins control_ic/M00_AXI] [get_bd_intf_pins trace_pl/s_axi]
connect_bd_intf_net [get_bd_intf_pins control_ic/M01_AXI] [get_bd_intf_pins trace_dma/S_AXI_LITE]
connect_bd_intf_net [get_bd_intf_pins trace_pl/m_axis] [get_bd_intf_pins trace_dma/S_AXIS_S2MM]
connect_bd_intf_net [get_bd_intf_pins trace_dma/M_AXI_S2MM] [get_bd_intf_pins data_ic/S00_AXI]
connect_bd_intf_net [get_bd_intf_pins trace_dma/M_AXI_SG] [get_bd_intf_pins data_ic/S01_AXI]
connect_bd_intf_net [get_bd_intf_pins data_ic/M00_AXI] [get_bd_intf_pins ps/S_AXI_HP0_FPD]

connect_bd_net [get_bd_pins ps/pl_clk0] [get_bd_pins pl_reset/slowest_sync_clk] \
    [get_bd_pins control_ic/aclk] [get_bd_pins data_ic/aclk] [get_bd_pins trace_dma/s_axi_lite_aclk] \
    [get_bd_pins trace_dma/m_axi_s2mm_aclk] [get_bd_pins trace_dma/m_axi_sg_aclk] [get_bd_pins trace_pl/aclk] \
    [get_bd_pins ps/pl_ps_trace_clk] [get_bd_pins ps/maxihpm0_fpd_aclk] [get_bd_pins ps/saxihp0_fpd_aclk]
connect_bd_net [get_bd_pins ps/trace_clk_out] [get_bd_pins trace_pl/trace_clk]
connect_bd_net [get_bd_pins ps/ps_pl_tracedata] [get_bd_pins trace_pl/trace_data]
connect_bd_net [get_bd_pins ps/pl_resetn0] [get_bd_pins pl_reset/ext_reset_in]
connect_bd_net [get_bd_pins pl_reset/peripheral_aresetn] [get_bd_pins trace_pl/aresetn] [get_bd_pins trace_dma/axi_resetn]
connect_bd_net [get_bd_pins trace_dma/s2mm_introut] [get_bd_pins trace_pl/dma_complete]
connect_bd_net [get_bd_pins zero/dout] [get_bd_pins trace_pl/dma_fault] [get_bd_pins trace_pl/debug_complete]
connect_bd_net [get_bd_pins trace_pl/irq] [get_bd_pins irq_concat/In0]
connect_bd_net [get_bd_pins irq_concat/dout] [get_bd_pins ps/pl_ps_irq0]
assign_bd_address
set_property offset 0xA0000000 [get_bd_addr_segs ps/Data/SEG_trace_pl_reg0]
set_property range 64K [get_bd_addr_segs ps/Data/SEG_trace_pl_reg0]
set_property offset 0xA0010000 [get_bd_addr_segs ps/Data/SEG_trace_dma_Reg]
set_property range 64K [get_bd_addr_segs ps/Data/SEG_trace_dma_Reg]
