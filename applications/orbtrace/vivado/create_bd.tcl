create_bd_design zub_orbtrace
set ps [create_bd_cell -type ip -vlnv xilinx.com:ip:zynq_ultra_ps_e:* ps]
zub1cg_apply_ps_preset $ps
# PSU__MAXIGP0__DATA_WIDTH {32}: M_AXI_HPM0_FPD defaults to 128-bit, but
# every control_ic slave (trace_pl, trace_dma, m3_mem_ctrl) is 32-bit --
# root cause of the M3 BRAM load bug (see
# documentation/M3_BRAM_LOAD_BUG_HANDOFF_2026-08-16.md). ILA capture on
# m3_mem_ctrl/S_AXI showed control_ic's 128->32 down-conversion split a
# single 32-bit AWLEN=0 write into two downstream W beats (a bogus
# WSTRB=0/WLAST=0 dummy beat, then the real WSTRB=0xF/WLAST=1 data beat)
# while leaving AWLEN=0 on the narrow side. axi_bram_ctrl is strict about
# beat counting: it commits after the first (no-op) beat and silently
# drops the real data, but still returns BRESP=OKAY, matching the observed
# symptom exactly (write reports success, readback never changes).
# trace_pl's own hand-rolled register-write logic tolerates this same
# pattern (any nonzero-WSTRB beat is accepted regardless of WLAST/AWLEN),
# which is why m3_control/identity-register writes never showed the bug.
# Matching M_AXI_HPM0_FPD's width to its downstream slaves removes the
# down-converter -- and the spurious extra beat -- entirely.
set_property -dict [list \
    CONFIG.PSU__USE__M_AXI_GP0 {1} CONFIG.PSU__USE__M_AXI_GP1 {0} CONFIG.PSU__USE__M_AXI_GP2 {0} \
    CONFIG.PSU__USE__S_AXI_GP2 {1} CONFIG.PSU__USE__IRQ {1} CONFIG.PSU__USE__IRQ0 {1} \
    CONFIG.PSU__FPGA_PL0_ENABLE {1} CONFIG.PSU__CRL_APB__PL0_REF_CTRL__FREQMHZ {100} \
    CONFIG.PSU__CRF_APB__DBG_TRACE_CTRL__FREQMHZ {100} \
    CONFIG.PSU__TRACE__PERIPHERAL__ENABLE {1} CONFIG.PSU__TRACE__PERIPHERAL__IO {EMIO} \
    CONFIG.PSU__TRACE__WIDTH {4Bit} CONFIG.PSU__TRACE__INTERNAL_WIDTH {32} \
    CONFIG.PSU__MAXIGP0__DATA_WIDTH {32}] $ps

set dma [create_bd_cell -type ip -vlnv xilinx.com:ip:axi_dma:* trace_dma]
set_property -dict [list CONFIG.c_include_sg {1} CONFIG.c_include_mm2s {0} \
    CONFIG.c_include_s2mm {1} CONFIG.c_sg_include_stscntrl_strm {0} \
    CONFIG.c_sg_length_width {26} CONFIG.c_s2mm_burst_size {256} \
    CONFIG.c_s_axis_s2mm_tdata_width {64} CONFIG.c_m_axi_s2mm_data_width {64}] $dma
set control_ic [create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:* control_ic]
set_property CONFIG.NUM_MI {3} $control_ic
set data_ic [create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:* data_ic]
set_property CONFIG.NUM_SI {2} $data_ic
# m3_mem_ctrl (the A53 preload path into the M3's BRAM) gets a dedicated
# axi_interconnect stage between it and control_ic's M02_AXI leg, instead
# of connecting to that leg directly. Root cause, from real ILA capture on
# control_ic/M02_AXI <-> m3_mem_ctrl/S_AXI (see
# M3_TRACE_VERIFICATION_PLAN.md's 2026-08-17 Phase D regression writeup):
# back-to-back single-beat writes through that leg only carry real
# WDATA/WSTRB on the first of every 4 transactions -- the following 3 each
# report BRESP=OKAY but silently repeat the PREVIOUS transaction's stale
# WDATA with WSTRB=0, so 3 of every 4 words never actually land in BRAM.
# Every hop's data width was independently confirmed 32-bit end to end
# (ruling out a down-conversion beat-count mismatch), so this looks like a
# fault specific to control_ic (smartconnect)'s own internal write-channel
# demux/pipeline on that leg -- trace_pl and trace_dma, the other two
# legs, show no such symptom. Giving m3_mem_ctrl a fully separate PS
# master port (M_AXI_HPM1_FPD) was tried first and rejected: real Vivado
# address-decode rejects 0xA0xxxxxx (the address firmware/host tooling
# already hardcode) as unreachable through that port at all ("must fit an
# available aperture ... {<0xB000_0000 [256M]>, <0x5_0000_0000 [4G]>,
# <0x48_0000_0000 [224G]>}"), so this stays on control_ic's existing M02
# leg/address path and instead interposes a plain axi_interconnect
# (matching m3_core_ic's own proven role converting/re-timing AXI
# transactions elsewhere in this design) right before m3_mem_ctrl, to see
# whether normalizing/re-registering the transaction there is enough to
# fix (or conclusively rule out) whatever control_ic itself is doing wrong.
set m3_mem_ctrl_ic [create_bd_cell -type ip -vlnv xilinx.com:ip:axi_interconnect:* m3_mem_ctrl_ic]
set_property -dict [list CONFIG.NUM_SI {1} CONFIG.NUM_MI {1}] $m3_mem_ctrl_ic
set rst [create_bd_cell -type ip -vlnv xilinx.com:ip:proc_sys_reset:* pl_reset]
set trace [create_bd_cell -type module -reference orbtrace_pl trace_pl]
set irq_concat [create_bd_cell -type ip -vlnv xilinx.com:ip:xlconcat:* irq_concat]
set_property CONFIG.NUM_PORTS {1} $irq_concat
set zero [create_bd_cell -type ip -vlnv xilinx.com:ip:xlconstant:* zero]
set_property -dict [list CONFIG.CONST_WIDTH {1} CONFIG.CONST_VAL {0}] $zero
set zero2 [create_bd_cell -type ip -vlnv xilinx.com:ip:xlconstant:* zero2]
set_property -dict [list CONFIG.CONST_WIDTH {2} CONFIG.CONST_VAL {0}] $zero2

# On-chip Cortex-M3 (Arm DesignStart FPGA edition, Xilinx package AT426) —
# Orbtrace's real CoreSight ITM/TPIU trace source and JTAG-DP debug target,
# replacing the synthetic vex/test placeholder previously wired to
# source_select==0. JTAG (not SWD) because the existing host-side debug
# bridge (applications/orbtrace/model/src/main.rs's remote_bitbang())
# already speaks JTAG bit-by-bit over DAP_JTAG_Sequence/DAP_SWJ_Pins — see
# orbtrace_dap_engine.sv's use_real_target path. The IP itself is licensed
# by Arm and delivered as an IP-XACT package for Vivado's IP catalog, not
# committed to this repo — see ../vivado/README.md and the
# `ARM_DESIGNSTART_IP_ROOT`-driven `ip_repo_paths`/`update_ip_catalog`
# calls in build.tcl (point it at the package's `vivado/Arm_ipi_repository`
# subdirectory, which contains the `CM3DbgAXI` IP-XACT component).
#
# VLNV and every pin/interface name below are confirmed against the real
# delivered IP-XACT and a real Vivado `create_bd_cell`/`get_bd_pins` probe
# (nix develop -c vivado -mode batch, part xczu1cg-sbva484-1-e) — see the
# `orbtrace-m3-integration` memory note for the full verification. They are
# no longer best-effort guesses.
set m3_core [create_bd_cell -type ip -vlnv Arm.com:CortexM:CORTEXM3_AXI:1.1 m3_core]
set_property -dict [list \
    CONFIG.JTAG_PRESENT {true} CONFIG.WIC_PRESENT {false} \
    CONFIG.ITCM_SIZE {8kB} CONFIG.DTCM_SIZE {2kB} CONFIG.ITCM_INIT_RAM {false}] $m3_core

# M3 program/data memory: a true dual-port BRAM, one port per address view.
# The A53 preload path (via control_ic, below) and the M3 core's own
# instruction fetch (CM3_CODE_AXI3) reach the same physical BRAM but at two
# different, non-contiguous addresses (0xA0020000 for the A53/PS view,
# 0x0 for the M3's own view — see the address assignment below and
# sdk/bsp/m3/memory.lds's RAM origin). A single axi_bram_ctrl can't serve
# both: its MEM_DEPTH is hard-marked propagate_only in its own packaged
# Tcl and always auto-derives BRAM geometry from every reachable
# address-space view, which fails hard once the M3 side has a real address
# space (reproduced against real Vivado: "Could not compute memory range
# ... Found more than 1 disjointed assignments (Not Contiguous)" — this
# only started failing once m3_core replaced the earlier placeholder stub,
# which had no address space of its own to expose the conflict). Fix: two
# independent axi_bram_ctrl instances, one per BRAM port, each with
# exactly one reachable root address.
#
# m3_core's CM3_CODE_AXI3 is AXI3, not AXI4 — `xilinx.com:ip:smartconnect`
# cannot elaborate against it (reproduced against real Vivado: it fails
# hard trying to read the interface's NUM_READ_OUTSTANDING/
# NUM_WRITE_OUTSTANDING parameters), so m3_core_ic must be the legacy
# `axi_interconnect`, which auto-inserts an AXI3-to-AXI4 protocol converter
# — matching what Arm's own Arty A7 reference design does for this same
# IP. m3_core also exposes a separate CM3_SYS_AXI3 (data/peripheral)
# master, deliberately left unconnected: sdk/bsp/m3/memory.lds maps the
# whole firmware image (.text/.data/.bss/stack) into this one 64K
# Code-region BRAM, so nothing the firmware does touches the
# System-region address space CM3_SYS_AXI3 would reach — an unconnected
# AXI master interface is legal in Vivado (a benign "incomplete address
# path" warning, not an error).
set m3_core_ic [create_bd_cell -type ip -vlnv xilinx.com:ip:axi_interconnect:* m3_core_ic]
set_property -dict [list CONFIG.NUM_SI {1} CONFIG.NUM_MI {1}] $m3_core_ic
set m3_mem_ctrl [create_bd_cell -type ip -vlnv xilinx.com:ip:axi_bram_ctrl:* m3_mem_ctrl]
set m3_mem_ctrl_core [create_bd_cell -type ip -vlnv xilinx.com:ip:axi_bram_ctrl:* m3_mem_ctrl_core]
# SINGLE_PORT_BRAM {1}: axi_bram_ctrl defaults to dual-port mode
# (SINGLE_PORT_BRAM=0), which -- root cause of the M3 BRAM load bug, see
# documentation/M3_BRAM_LOAD_BUG_HANDOFF_2026-08-16.md -- services AXI
# writes via BRAM port A but AXI reads via port B. Each of these two
# controllers only ever gets ONE BRAM port wired below (m3_mem_ctrl to
# m3_mem's BRAM_PORTA, m3_mem_ctrl_core to BRAM_PORTB), so each
# controller's own unconnected second port gets auto-tied to a constant by
# the generated wrapper (confirmed in the generated Verilog:
# m3_mem_ctrl's bram_rddata_b tied to 32'h00000008, exactly the fixed
# value every read returned) -- writes silently landed in the real BRAM via
# port A while reads always came back from the disconnected, tied-off
# port B. SINGLE_PORT_BRAM {1} makes each controller use its single
# connected port for both reads and writes, matching how it's actually
# wired here.
#
# DATA_WIDTH {32} / MEM_DEPTH {16384}: explicit, to match m3_mem's real
# Write_Width_A/B {32} x Write_Depth_A/B {16384} below -- harmless and
# arguably correct practice, but NOT a fix for anything: a 2026-08-17
# investigation initially concluded (via `get_pins -hier -filter {NAME =~
# "*ctrl/U0/*s_axi_wdata*"}`) that these controllers were silently running
# 128-bit/4096-deep instead, and added these properties to force 32-bit --
# two full Vivado rebuilds later, that finding turned out to be a
# measurement error (the loose wildcard matched an unrelated internal
# signal, not the real port; a precise `get_pins -of_objects [get_cells
# .../U0] -filter {...}` check, and the real generated VHDL's `GENERIC MAP`,
# both confirm both controllers were genuinely 32-bit/16384-deep all along,
# even before this fix was added). The actual M3 BRAM load bug this was
# chasing -- a reproducible 4:1 word-drop, only every 4th 32-bit word
# surviving a write, on both this controller and m3_mem_ctrl_core -- is
# NOT explained by this and remains open. See M3_TRACE_VERIFICATION_PLAN.md's
# 2026-08-17 Phase D regression writeup (including the correction) for the
# full history; real ILA hardware capture on m3_mem_ctrl/S_AXI is the next
# step, not further CONFIG.* changes here.
set_property CONFIG.SINGLE_PORT_BRAM {1} $m3_mem_ctrl
set_property -dict [list CONFIG.DATA_WIDTH {32} CONFIG.MEM_DEPTH {16384}] $m3_mem_ctrl
set_property CONFIG.SINGLE_PORT_BRAM {1} $m3_mem_ctrl_core
set_property -dict [list CONFIG.DATA_WIDTH {32} CONFIG.MEM_DEPTH {16384}] $m3_mem_ctrl_core
set m3_mem [create_bd_cell -type ip -vlnv xilinx.com:ip:blk_mem_gen:* m3_mem]
# 64K (16384 x 32-bit) per port — must match RAM's LENGTH in
# sdk/bsp/m3/memory.lds. True dual-port so the A53 preload path (port A,
# via m3_mem_ctrl) and the M3's own fetch (port B, via m3_mem_ctrl_core)
# each get an independent axi_bram_ctrl / address view.
set_property -dict [list CONFIG.Memory_Type {True_Dual_Port_RAM} \
    CONFIG.Write_Width_A {32} CONFIG.Write_Depth_A {16384} CONFIG.Read_Width_A {32} \
    CONFIG.Write_Width_B {32} CONFIG.Read_Width_B {32} \
    CONFIG.Enable_B {Use_ENB_Pin}] $m3_mem
connect_bd_intf_net [get_bd_intf_pins m3_mem_ctrl/BRAM_PORTA] [get_bd_intf_pins m3_mem/BRAM_PORTA]
connect_bd_intf_net [get_bd_intf_pins m3_mem_ctrl_core/BRAM_PORTA] [get_bd_intf_pins m3_mem/BRAM_PORTB]
connect_bd_intf_net [get_bd_intf_pins m3_core_ic/M00_AXI] [get_bd_intf_pins m3_mem_ctrl_core/S_AXI]
connect_bd_intf_net [get_bd_intf_pins m3_core/CM3_CODE_AXI3] [get_bd_intf_pins m3_core_ic/S00_AXI]

connect_bd_intf_net [get_bd_intf_pins ps/M_AXI_HPM0_FPD] [get_bd_intf_pins control_ic/S00_AXI]
connect_bd_intf_net [get_bd_intf_pins control_ic/M00_AXI] [get_bd_intf_pins trace_pl/s_axi]
connect_bd_intf_net [get_bd_intf_pins control_ic/M01_AXI] [get_bd_intf_pins trace_dma/S_AXI_LITE]
connect_bd_intf_net [get_bd_intf_pins control_ic/M02_AXI] [get_bd_intf_pins m3_mem_ctrl_ic/S00_AXI]
connect_bd_intf_net [get_bd_intf_pins m3_mem_ctrl_ic/M00_AXI] [get_bd_intf_pins m3_mem_ctrl/S_AXI]
connect_bd_intf_net [get_bd_intf_pins trace_pl/m_axis] [get_bd_intf_pins trace_dma/S_AXIS_S2MM]
connect_bd_intf_net [get_bd_intf_pins trace_dma/M_AXI_S2MM] [get_bd_intf_pins data_ic/S00_AXI]
connect_bd_intf_net [get_bd_intf_pins trace_dma/M_AXI_SG] [get_bd_intf_pins data_ic/S01_AXI]
connect_bd_intf_net [get_bd_intf_pins data_ic/M00_AXI] [get_bd_intf_pins ps/S_AXI_HP0_FPD]

connect_bd_net [get_bd_pins ps/pl_clk0] [get_bd_pins pl_reset/slowest_sync_clk] \
    [get_bd_pins control_ic/aclk] [get_bd_pins data_ic/aclk] [get_bd_pins trace_dma/s_axi_lite_aclk] \
    [get_bd_pins trace_dma/m_axi_s2mm_aclk] [get_bd_pins trace_dma/m_axi_sg_aclk] [get_bd_pins trace_pl/aclk] \
    [get_bd_pins ps/pl_ps_trace_clk] [get_bd_pins ps/maxihpm0_fpd_aclk] [get_bd_pins ps/saxihp0_fpd_aclk] \
    [get_bd_pins m3_mem_ctrl/s_axi_aclk] [get_bd_pins m3_mem_ctrl_core/s_axi_aclk] \
    [get_bd_pins m3_core_ic/ACLK] [get_bd_pins m3_core_ic/S00_ACLK] [get_bd_pins m3_core_ic/M00_ACLK] \
    [get_bd_pins m3_mem_ctrl_ic/ACLK] [get_bd_pins m3_mem_ctrl_ic/S00_ACLK] [get_bd_pins m3_mem_ctrl_ic/M00_ACLK]
connect_bd_net [get_bd_pins ps/trace_clk_out] [get_bd_pins trace_pl/trace_clk]
connect_bd_net [get_bd_pins ps/ps_pl_tracedata] [get_bd_pins trace_pl/trace_data]
connect_bd_net [get_bd_pins ps/pl_resetn0] [get_bd_pins pl_reset/ext_reset_in]
connect_bd_net [get_bd_pins pl_reset/peripheral_aresetn] [get_bd_pins trace_pl/aresetn] \
    [get_bd_pins trace_dma/axi_resetn] [get_bd_pins m3_mem_ctrl/s_axi_aresetn] \
    [get_bd_pins m3_mem_ctrl_core/s_axi_aresetn] [get_bd_pins m3_core_ic/ARESETN] \
    [get_bd_pins m3_core_ic/S00_ARESETN] [get_bd_pins m3_core_ic/M00_ARESETN] \
    [get_bd_pins m3_mem_ctrl_ic/ARESETN] [get_bd_pins m3_mem_ctrl_ic/S00_ARESETN] \
    [get_bd_pins m3_mem_ctrl_ic/M00_ARESETN]
connect_bd_net [get_bd_pins trace_dma/s2mm_introut] [get_bd_pins trace_pl/dma_complete]
connect_bd_net [get_bd_pins zero/dout] [get_bd_pins trace_pl/dma_fault] [get_bd_pins trace_pl/debug_complete]
connect_bd_net [get_bd_pins trace_pl/irq] [get_bd_pins irq_concat/In0]
connect_bd_net [get_bd_pins irq_concat/dout] [get_bd_pins ps/pl_ps_irq0]

# m3_core clock/reset/trace pin names below are confirmed against the real
# delivered IP-XACT — there is no HRESETn/PORESETn on this IP; instead a
# single SYSRESETn plus a separate DBGRESETn for the debug-logic domain,
# both driven the same way trace_pl's m3_release-gated reset already was.
connect_bd_net [get_bd_pins ps/pl_clk0] [get_bd_pins m3_core/HCLK]
connect_bd_net [get_bd_pins trace_pl/m3_reset_n] [get_bd_pins m3_core/SYSRESETn] \
    [get_bd_pins m3_core/DBGRESETn]
# TPIU trace clock is the IP's own output (mirrors ps/trace_clk_out above),
# not tied to pl_clk0 directly, even though HCLK feeds both.
connect_bd_net [get_bd_pins m3_core/TRACECLK] [get_bd_pins trace_pl/trace_clk_m3]
connect_bd_net [get_bd_pins m3_core/TRACEDATA] [get_bd_pins trace_pl/trace_data_m3]

# Debug/config inputs this IP requires but this design doesn't use: tie the
# 1-bit ones to the same constant-0 net already driving trace_pl's unused
# inputs, and CFGITCMEN (2 bits: one per TCM) to its own 2-bit constant.
# CFGITCMEN=0 keeps the internal ITCM/DTCM (an unavoidable minimum-size
# BRAM on this IP even when unused — see CONFIG.ITCM_SIZE/DTCM_SIZE above)
# out of the Code-region address decode, so address 0x0 always resolves
# externally to m3_mem via CM3_CODE_AXI3, matching sdk/bsp/m3/memory.lds's
# RAM origin. m3_core/IRQ (16-bit peripheral interrupt input) is left
# unconnected — this design wires no external interrupt source to the M3,
# matching the firmware's poll-driven Workload model (Vivado tie-offs it
# to 0 automatically, with only a benign DRC warning).
connect_bd_net [get_bd_pins zero2/dout] [get_bd_pins m3_core/CFGITCMEN]
connect_bd_net [get_bd_pins zero/dout] [get_bd_pins m3_core/NMI] \
    [get_bd_pins m3_core/EDBGRQ] [get_bd_pins m3_core/DBGRESTART] \
    [get_bd_pins m3_core/STCLK] [get_bd_pins m3_core/WICENREQ]

# JTAG-DP debug port — bit-banged directly by trace_pl's DAP engine when
# ORBTRACE_REG_M3_CONTROL bit 1 is set (host tooling: TCP 3240 CMSIS-DAP,
# bridged from OpenOCD's remote-bitbang by
# applications/orbtrace/model/src/main.rs's remote_bitbang()). Idle
# (TCK=0, TMS=1, TDI=0, nTRST/nRESET deasserted) otherwise — see
# orbtrace_dap_engine.sv's reset defaults. m3_core's DAP is a combined
# SWJ-DP where JTAG and SWD share pins (TCK->SWCLKTCK, TMS->SWDITMS); it
# autodetects JTAG vs SWD from the standard switch sequence (JTAGNSW/
# JTAGTOP are DAP status outputs, not mode-select inputs), so no extra
# mode-select wiring is needed. TDI/TDO/nTRST are dedicated JTAG-only pins.
connect_bd_net [get_bd_pins trace_pl/jtag_tck] [get_bd_pins m3_core/SWCLKTCK]
connect_bd_net [get_bd_pins trace_pl/jtag_tms] [get_bd_pins m3_core/SWDITMS]
connect_bd_net [get_bd_pins trace_pl/jtag_tdi] [get_bd_pins m3_core/TDI]
connect_bd_net [get_bd_pins m3_core/TDO] [get_bd_pins trace_pl/jtag_tdo]
connect_bd_net [get_bd_pins trace_pl/jtag_ntrst] [get_bd_pins m3_core/nTRST]

# trace_dma must stay at the fixed 0xA0010000 firmware/a53 already hardcodes
# (AXI_DMA_BASE in firmware/a53/src/lib.rs, TRACE_DMA_BASE in
# firmware/a53_app/src/main.c) — this predates the M3 work and isn't a free
# address to renegotiate. But assign_bd_address's own auto-assignment (con-
# firmed against real Vivado) places the new m3_mem_ctrl segment at exactly
# that address, and trace_dma's segment ends up at 0xA0020000 instead — so
# moving trace_dma straight to 0xA0010000 collides with m3_mem_ctrl still
# sitting there. Route m3_mem_ctrl through an unused scratch address first
# to break the cycle, matching what real Vivado actually requires (tested:
# setting trace_dma's offset before relocating m3_mem_ctrl out of the way
# fails with a hard address-overlap error).
assign_bd_address
set_property offset 0xA0030000 [get_bd_addr_segs ps/Data/SEG_m3_mem_ctrl_Mem0]
set_property offset 0xA0000000 [get_bd_addr_segs ps/Data/SEG_trace_pl_reg0]
set_property range 64K [get_bd_addr_segs ps/Data/SEG_trace_pl_reg0]
set_property offset 0xA0010000 [get_bd_addr_segs ps/Data/SEG_trace_dma_Reg]
set_property range 64K [get_bd_addr_segs ps/Data/SEG_trace_dma_Reg]
set_property offset 0xA0020000 [get_bd_addr_segs ps/Data/SEG_m3_mem_ctrl_Mem0]
set_property range 64K [get_bd_addr_segs ps/Data/SEG_m3_mem_ctrl_Mem0]
# Cortex-M always fetches its reset SP/PC from address 0x0, so m3_core's own
# view of m3_mem_ctrl (as opposed to the PS/A53 preload view pinned above)
# must be mapped at 0 to match sdk/bsp/m3/memory.lds's RAM origin. Found by
# pattern rather than a hardcoded segment name since the exact segment name
# is generated from m3_core's CM3_CODE_AXI3 interface name by Vivado.
foreach seg [get_bd_addr_segs -of_objects [get_bd_cells m3_core]] {
    if {[string match "*m3_mem_ctrl*" $seg]} {
        set_property offset 0x00000000 $seg
        set_property range 64K $seg
    }
}
