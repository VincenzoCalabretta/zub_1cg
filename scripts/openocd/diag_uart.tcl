# diag_uart.tcl — read UART1 and clock registers while R5 is free-running.
# Run AFTER load_r5.tcl has started the R5.
# Usage:
#   openocd -f scripts/openocd/aes_zub.cfg -f scripts/openocd/diag_uart.tcl

init
after 500

targets uscale.axi

if { [catch { read_memory 0xFF9A0000 32 1 } probe_err] } {
    echo "ERROR: AXI inaccessible (STICKYERR?). Power-cycle and retry."
    shutdown
    return
}

echo ""
echo "=== RPU ==="
set v [lindex [read_memory 0xFF9A0000 32 1] 0]
echo [format {RPU_GLBL_CNTL     @ 0xFF9A0000 = 0x%08x} $v]
set v [lindex [read_memory 0xFF9A0100 32 1] 0]
echo [format {RPU_0_CFG          @ 0xFF9A0100 = 0x%08x  (bit0=VINITHI)} $v]
set v [lindex [read_memory 0xFF5E023C 32 1] 0]
echo [format {RST_LPD_TOP        @ 0xFF5E023C = 0x%08x  (bit0=R5-0 rst)} $v]

echo ""
echo "=== UART1 ref clock ==="
# CRL_APB_UART1_REF_CTRL @ 0xFF5E0074
# [24]=CLKACT, [21:16]=DIVISOR1, [13:8]=DIVISOR0, [2:0]=SRCSEL
# SRCSEL: 0=IOPLL, 2=RPLL, 3=DPLL
set v [lindex [read_memory 0xFF5E0074 32 1] 0]
echo [format {UART1_REF_CTRL     @ 0xFF5E0074 = 0x%08x} $v]
set clkact  [expr {($v >> 24) & 0x1}]
set div1    [expr {($v >> 16) & 0x3f}]
set div0    [expr {($v >>  8) & 0x3f}]
set srcsel  [expr {($v >>  0) & 0x7}]
echo [format {  CLKACT=%d DIVISOR1=%d DIVISOR0=%d SRCSEL=%d} $clkact $div1 $div0 $srcsel]

# IOPLL control (to estimate output freq)
# CRL_APB_IOPLL_CTRL @ 0xFF5E0020
set v [lindex [read_memory 0xFF5E0020 32 1] 0]
echo [format {IOPLL_CTRL         @ 0xFF5E0020 = 0x%08x} $v]
set fbdiv [expr {($v >> 8) & 0x7f}]
echo [format {  FBDIV=%d  (freq = 33.333 MHz * FBDIV / (DIV0*DIV1))} $fbdiv]

# RPLL control
set v [lindex [read_memory 0xFF5E0030 32 1] 0]
echo [format {RPLL_CTRL          @ 0xFF5E0030 = 0x%08x} $v]
set fbdiv_r [expr {($v >> 8) & 0x7f}]
echo [format {  FBDIV=%d} $fbdiv_r]

echo ""
echo "=== UART1 registers (0xFF010000) ==="
set uart_cr   [lindex [read_memory 0xFF010000 32 1] 0]
set uart_mr   [lindex [read_memory 0xFF010004 32 1] 0]
set uart_sr   [lindex [read_memory 0xFF01002C 32 1] 0]
set uart_bg   [lindex [read_memory 0xFF010018 32 1] 0]
set uart_bd   [lindex [read_memory 0xFF010034 32 1] 0]
echo [format {UART_CR     @ +0x00 = 0x%08x  (bit4=TX_EN bit1=TXRST)} $uart_cr]
echo [format {UART_MR     @ +0x04 = 0x%08x  (expect 0x20 for 8N1)} $uart_mr]
echo [format {UART_BAUDGEN@ +0x18 = 0x%08x  (CD divisor)} $uart_bg]
echo [format {UART_SR     @ +0x2C = 0x%08x  (bit3=TX_EMPTY)} $uart_sr]
echo [format {UART_BAUDDIV@ +0x34 = 0x%08x  (BDIV)} $uart_bd]
if { $uart_cr == 0x00000114 } {
    echo "UART_CR = default (R5 has NOT written to UART yet)"
} elseif { [expr {$uart_cr & 0x10}] } {
    echo "UART_CR has TX_EN set → uart_init() ran"
} elseif { [expr {$uart_cr & 0x2}] } {
    echo "UART_CR has TXRST set → uart_init() is STUCK in reset wait loop"
}

echo ""
echo "=== OCM sanity (first 4 vector words) ==="
set w0 [lindex [read_memory 0xFFFF0000 32 1] 0]
set w1 [lindex [read_memory 0xFFFF0004 32 1] 0]
set w2 [lindex [read_memory 0xFFFF0008 32 1] 0]
set w3 [lindex [read_memory 0xFFFF000C 32 1] 0]
echo [format {0xFFFF0000: 0x%08x  (reset: LDR pc)} $w0]
echo [format {0xFFFF0004: 0x%08x  (undef)} $w1]
echo [format {0xFFFF0008: 0x%08x  (SWI)} $w2]
echo [format {0xFFFF000C: 0x%08x  (prefetch)} $w3]

echo ""
shutdown
