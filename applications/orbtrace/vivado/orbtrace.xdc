# The 100 MHz PL0 clock drives the EMIO TPIU clock input (the limit is 125 MHz).
# The PS IP emits a derived source-synchronous trace_clk_out; the asynchronous
# FIFO is the sole path from that domain back to PL0.
create_generated_clock -name trace_clk_emio \
    -source [get_pins {zub_orbtrace_i/ps/inst/PS8_i/PLCLK[0]}] \
    -divide_by 2 [get_pins zub_orbtrace_i/ps/trace_clk_out]
set_property ASYNC_REG TRUE [get_cells -quiet -hier -regexp {.*(gray_[wr][12]|swo_[ms]).*}]

# The M3 IP's own CoreSight TPIU re-drives its core clock as TRACECLK (no
# separate trace PLL/divider in this DesignStart config, so trace_clk_m3 is
# HCLK republished through an output buffer) -- same pattern as trace_clk_emio
# above. The async FIFO in orbtrace_pl.v (m3_trace_fifo) is the sole path back
# to aclk; its Gray-code synchronizers are already covered by the ASYNC_REG
# regexp above.
create_generated_clock -name trace_clk_m3 \
    -source [get_pins zub_orbtrace_i/m3_core/inst/inst/HCLK] \
    -divide_by 1 [get_pins zub_orbtrace_i/m3_core/inst/inst/TRACECLK]

# SWCLKTCK is bit-banged from FPGA logic by orbtrace_dap_engine.sv
# (JTAG_HALF_PERIOD), not a free-running clock -- JTAG/SWD transactions are
# far slower than any setup/hold margin here. Give it a nominal slow clock so
# report_methodology stops flagging every DAP register as unclocked, and
# exclude it from real STA rather than pretending it's synchronous to
# anything else in the design.
#
# Deliberately created on m3_core's SWCLKTCK *input* pin, not on the driving
# trace_pl/jtag_tck register: jtag_tck is an ordinary clk_pl_0-domain FDRE
# output (just toggled slowly), so declaring a primary clock there conflicts
# with its already-propagated clk_pl_0 lineage (TIMING-4, "invalid clock
# redefinition on a clock tree") in addition to the pin-type complaint. The
# M3 IP's EDIF-linked boundary blocks that lineage tracing at SWCLKTCK, so
# only the pin-type complaint (TIMING-2, "invalid primary clock source pin")
# fires there -- expected and waived in build.tcl, same as for the CDC
# crossings this clock definition also unblocks.
create_clock -name swclktck -period 1000 \
    [get_pins zub_orbtrace_i/m3_core/inst/inst/SWCLKTCK]
set_clock_groups -asynchronous -group [get_clocks swclktck]
