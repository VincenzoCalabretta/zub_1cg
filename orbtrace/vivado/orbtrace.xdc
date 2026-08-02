# The 100 MHz PL0 clock drives the EMIO TPIU clock input (the limit is 125 MHz).
# The PS IP emits a derived source-synchronous trace_clk_out; the asynchronous
# FIFO is the sole path from that domain back to PL0.
create_generated_clock -name trace_clk_emio \
    -source [get_pins {zub_orbtrace_i/ps/inst/PS8_i/PLCLK[0]}] \
    -divide_by 2 [get_pins zub_orbtrace_i/ps/trace_clk_out]
set_property ASYNC_REG TRUE [get_cells -quiet -hier -regexp {.*(gray_[wr][12]|swo_[ms]).*}]
