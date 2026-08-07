# fix_uart.tcl — release UART1 from HW reset and test it while R5 is running.
#
# Without FSBL, RST_LPD_IOU2 default = 0x0017FFFF keeps all LPD IOU peripherals
# in reset.  UART registers read 0x00 and writes are ignored.
#
# Clock: IOPLL(FBDIV=50)*PS_REF_CLK(33.333MHz) = 1666.67 MHz
#        UART1_REF_CTRL: SRCSEL=IOPLL, DIV1=0(bypass), DIV0=24 => 69.44 MHz
#        For 115200: CD=43, BDIV=13 => 69444444/(43*14) = 115313 baud (0.1%)

init
after 500
targets uscale.axi

if { [catch { read_memory 0xFF9A0000 32 1 } probe_err] } {
    echo "ERROR: AXI inaccessible. Power-cycle and retry."
    shutdown
    return
}

echo ""
echo "=== Reset register (pre-fix) ==="
set rst_iou2 [lindex [read_memory 0xFF5E0238 32 1] 0]
echo [format {RST_LPD_IOU2  @ 0xFF5E0238 = 0x%08x} $rst_iou2]

echo ""
echo "=== Step 1: Enable UART1 ref clock ==="
# CLKACT=1, DIVISOR1=1, DIVISOR0=24, SRCSEL=IOPLL => 1666.67/1/24 = 69.44 MHz
mww 0xFF5E0074 0x01011800
set v [lindex [read_memory 0xFF5E0074 32 1] 0]
echo [format {UART1_REF_CTRL = 0x%08x  (expect 0x01011800)} $v]

echo ""
echo "=== Step 2: Release all RST_LPD_IOU2 resets (UART0+UART1 bits 0-1) ==="
mww 0xFF5E0238 0x00000000
after 5
set v [lindex [read_memory 0xFF5E0238 32 1] 0]
echo [format {RST_LPD_IOU2  = 0x%08x  (expect 0x00000000)} $v]

echo ""
echo "=== Step 3: Read UART1 CR (should be non-zero now) ==="
set uart_cr [lindex [read_memory 0xFF010000 32 1] 0]
echo [format {UART_CR pre-init = 0x%08x} $uart_cr]

echo ""
echo "=== Step 4: Init UART1 registers ==="
mww 0xFF01000C 0xFFFFFFFF    ;# IDR: disable all interrupts
mww 0xFF010000 0x00000003    ;# CR: software reset TX+RX
after 5
mww 0xFF010004 0x00000020    ;# MR: 8N1, no parity
mww 0xFF010018 43            ;# BAUDGEN: CD=43
mww 0xFF010034 13            ;# BAUDDIV: BDIV=13 (69.44MHz/43/14=115313)
mww 0xFF010000 0x00000014    ;# CR: TX_EN | RX_EN
after 2

set uart_cr [lindex [read_memory 0xFF010000 32 1] 0]
set uart_mr [lindex [read_memory 0xFF010004 32 1] 0]
set uart_bg [lindex [read_memory 0xFF010018 32 1] 0]
set uart_bd [lindex [read_memory 0xFF010034 32 1] 0]
set uart_sr [lindex [read_memory 0xFF01002C 32 1] 0]
echo [format {UART_CR      = 0x%08x  (expect 0x14)} $uart_cr]
echo [format {UART_MR      = 0x%08x  (expect 0x20)} $uart_mr]
echo [format {UART_BAUDGEN = 0x%08x  (expect 43)} $uart_bg]
echo [format {UART_BAUDDIV = 0x%08x  (expect 13)} $uart_bd]
echo [format {UART_SR      = 0x%08x  (bit3=TX_EMPTY)} $uart_sr]

echo ""
echo "=== Step 5: Write test string to FIFO ==="
# "OpenOCD\r\n"
mww 0xFF010030 0x4F
mww 0xFF010030 0x70
mww 0xFF010030 0x65
mww 0xFF010030 0x6E
mww 0xFF010030 0x4F
mww 0xFF010030 0x43
mww 0xFF010030 0x44
mww 0xFF010030 0x0D
mww 0xFF010030 0x0A
after 20

set uart_sr [lindex [read_memory 0xFF01002C 32 1] 0]
echo [format {UART_SR after  = 0x%08x  (bit3=1 means TX FIFO empty=sent)} $uart_sr]

echo ""
echo "Check picocom — if 'OpenOCD' appeared, UART hardware is working."
echo ""
shutdown
