# read_mio_clk.tcl — read MIO pin defaults, clock state, and peripheral resets.
# Tells us:
#   1. IOPLL frequency (from FBDIV + PS_REF_CLK=33.333 MHz)
#   2. UART1_REF_CTRL: whether clock is active and what the divisors are
#   3. RST_LPD_IOU2: whether peripherals are in hardware reset
#   4. MIO_PIN_0..17 raw values: reveals actual bit layout (L3_SEL position)
#   5. MIO_PIN_18..29: covers likely UART0/UART1 pin candidates
#   6. MIO_PIN_38..43: another UART range for some Avnet boards
#   7. MIO_PIN_74..77: high MIO range used in some designs
#   8. UART1 CR: 0x114 = untouched; TX_EN set = R5 ran uart_init()

init
after 500

targets uscale.axi

if { [catch { read_memory 0xFF9A0000 32 1 } err] } {
    echo "ERROR: AXI inaccessible (STICKYERR). Power-cycle and re-run."
    shutdown
    return
}

echo ""
echo "=== PLL / Clock State ==="
set iopll  [lindex [read_memory 0xFF5E0020 32 1] 0]
set rpll   [lindex [read_memory 0xFF5E0030 32 1] 0]
set uart1c [lindex [read_memory 0xFF5E0074 32 1] 0]
echo [format {IOPLL_CTRL         @ 0xFF5E0020 = 0x%08x  FBDIV=%d} $iopll [expr {($iopll>>8)&0x7f}]]
echo [format {RPLL_CTRL          @ 0xFF5E0030 = 0x%08x  FBDIV=%d} $rpll  [expr {($rpll >>8)&0x7f}]]
set div1  [expr {($uart1c>>16)&0x3f}]
set div0  [expr {($uart1c>> 8)&0x3f}]
set srcsel [expr {$uart1c & 0x7}]
set clkact [expr {($uart1c>>24)&0x1}]
echo [format {UART1_REF_CTRL     @ 0xFF5E0074 = 0x%08x  CLKACT=%d SRCSEL=%d DIV1=%d DIV0=%d} \
      $uart1c $clkact $srcsel $div1 $div0]

set iopll_fbdiv [expr {($iopll>>8)&0x7f}]
if {$iopll_fbdiv > 0} {
    set iopll_mhz [expr {33333333 * $iopll_fbdiv}]
    if {$div0 > 0 && $div1 > 0} {
        set uart_ref [expr {$iopll_mhz / ($div0 * ($div1 > 0 ? $div1 : 1))}]
        echo [format {  => UART1 ref_clk (if IOPLL src): %d Hz = %.2f MHz} \
              $uart_ref [expr {$uart_ref / 1000000.0}]]
    }
}

echo ""
echo "=== Reset State ==="
set rst_iou2 [lindex [read_memory 0xFF5E0238 32 1] 0]
set rst_lpd  [lindex [read_memory 0xFF5E023C 32 1] 0]
echo [format {RST_LPD_IOU2       @ 0xFF5E0238 = 0x%08x  (0=released; bit0=UART0, bit1=UART1)} $rst_iou2]
echo [format {RST_LPD_TOP        @ 0xFF5E023C = 0x%08x  (bit0=R5-0; 0=running)} $rst_lpd]

echo ""
echo "=== UART1 Registers ==="
set uart_cr  [lindex [read_memory 0xFF010000 32 1] 0]
set uart_mr  [lindex [read_memory 0xFF010004 32 1] 0]
set uart_bg  [lindex [read_memory 0xFF010018 32 1] 0]
set uart_bd  [lindex [read_memory 0xFF010034 32 1] 0]
set uart_sr  [lindex [read_memory 0xFF01002C 32 1] 0]
echo [format {UART_CR   @ 0xFF010000 = 0x%08x} $uart_cr]
echo [format {UART_MR   @ 0xFF010004 = 0x%08x  (0x20 = 8N1)} $uart_mr]
echo [format {UART_BG   @ 0xFF010018 = 0x%08x  (CD)} $uart_bg]
echo [format {UART_BD   @ 0xFF010034 = 0x%08x  (BDIV)} $uart_bd]
echo [format {UART_SR   @ 0xFF01002C = 0x%08x  (bit3=TX_EMPTY)} $uart_sr]
if { $uart_cr == 0x00000114 } {
    echo "  → default reset state: R5 has NOT touched UART1"
} elseif { $uart_cr == 0 } {
    echo "  → all zeros: UART1 peripheral may be in HW reset (RST_LPD_IOU2)"
} elseif { [expr {$uart_cr & 0x10}] } {
    echo "  → TX_EN set: uart_init() ran on R5"
}

echo ""
echo "=== MIO_PIN raw values (revealing bit layout) ==="
echo "Pins 18-29 (typical UART0/UART1 range for ZU+):"
for {set i 18} {$i <= 29} {incr i} {
    set addr [expr {0xFF180000 + $i * 4}]
    set v [lindex [read_memory $addr 32 1] 0]
    echo [format {  MIO_PIN_%02d @ 0x%08x = 0x%08x} $i $addr $v]
}

echo "Pins 38-43:"
for {set i 38} {$i <= 43} {incr i} {
    set addr [expr {0xFF180000 + $i * 4}]
    set v [lindex [read_memory $addr 32 1] 0]
    echo [format {  MIO_PIN_%02d @ 0x%08x = 0x%08x} $i $addr $v]
}

echo "Pins 74-77:"
for {set i 74} {$i <= 77} {incr i} {
    set addr [expr {0xFF180000 + $i * 4}]
    set v [lindex [read_memory $addr 32 1] 0]
    echo [format {  MIO_PIN_%02d @ 0x%08x = 0x%08x} $i $addr $v]
}

echo ""
echo "=== OCM contents at 0xFFFF0000 ==="
set w0 [lindex [read_memory 0xFFFF0000 32 1] 0]
set w1 [lindex [read_memory 0xFFFF0004 32 1] 0]
echo [format {0xFFFF0000 = 0x%08x  (LDR pc,=reset_handler if ELF loaded)} $w0]
echo [format {0xFFFF0004 = 0x%08x} $w1]

echo ""
shutdown
