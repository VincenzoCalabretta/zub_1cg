# uart_probe.tcl — determine which UART (0 or 1) is connected to ttyUSB1.
#
# Sends "UART0\r\n" from UART0 and "UART1\r\n" from UART1.
# Whichever string appears on /dev/ttyUSB1 tells us the correct UART base.
# Also reads BSS area to verify R5 reset_handler ran.
#
# Usage (while R5 is running from prior load_r5.tcl session):
#   openocd -f scripts/openocd/aes_zub.cfg -f scripts/openocd/uart_probe.tcl

init
after 300

targets uscale.axi
if { [catch { read_memory 0xFF9A0000 32 1 } err] } {
    echo "ERROR: AXI blocked. Power-cycle and re-run load_r5.tcl first."
    shutdown
    return
}

echo ""
echo "=== R5 startup check (BSS at 0xFFFF2BCC) ==="
set bss0 [lindex [read_memory 0xFFFF2BCC 32 1] 0]
set bss1 [lindex [read_memory 0xFFFF2BD0 32 1] 0]
set bss2 [lindex [read_memory 0xFFFF2BD4 32 1] 0]
echo [format {BSS[0] @ 0xFFFF2BCC = 0x%08x  (0=BSS zeroed by reset_handler)} $bss0]
echo [format {BSS[1] @ 0xFFFF2BD0 = 0x%08x} $bss1]
echo [format {BSS[2] @ 0xFFFF2BD4 = 0x%08x} $bss2]
if { $bss0 == 0 && $bss1 == 0 } {
    echo "  BSS is zeroed → reset_handler ran, main() was called."
} else {
    echo "  BSS NOT zeroed → R5 may be stuck before/during bss_loop."
}

echo ""
echo "=== UART1 state (0xFF010000) ==="
set cr  [lindex [read_memory 0xFF010000 32 1] 0]
set mr  [lindex [read_memory 0xFF010004 32 1] 0]
set bg  [lindex [read_memory 0xFF010018 32 1] 0]
set bd  [lindex [read_memory 0xFF010034 32 1] 0]
set sr  [lindex [read_memory 0xFF01002C 32 1] 0]
echo [format {UART1_CR   = 0x%08x} $cr]
echo [format {UART1_MR   = 0x%08x  (0x20=8N1)} $mr]
echo [format {UART1_BGEN = 0x%08x  (43=correct)} $bg]
echo [format {UART1_BDIV = 0x%08x  (13=correct)} $bd]
echo [format {UART1_SR   = 0x%08x  (bit3=TX_EMPTY)} $sr]

echo ""
echo "=== UART0 state (0xFF000000) ==="
set cr0 [lindex [read_memory 0xFF000000 32 1] 0]
echo [format {UART0_CR   = 0x%08x  (0x114=default, 0=in_HW_reset)} $cr0]

echo ""
echo "=== Ensuring UART clock covers UART0 too ==="
mww 0xFF5E0070 0x01011800   ;# UART0_REF_CTRL: same as UART1 (IOPLL/24=69.44MHz)

echo ""
echo "=== Init UART0 with same divisors ==="
mww 0xFF00000C 0xFFFFFFFF
mww 0xFF000000 0x00000003
after 5
mww 0xFF000004 0x00000020
mww 0xFF000018 43
mww 0xFF000034 13
mww 0xFF000000 0x00000014
after 2
echo "UART0 initialized."

echo ""
echo "=== Send 'UART0\\r\\n' from UART0 ==="
foreach b {0x55 0x41 0x52 0x54 0x30 0x0D 0x0A 0x55 0x55 0x55} {
    mww 0xFF000030 $b
}
after 30
set sr0 [lindex [read_memory 0xFF00002C 32 1] 0]
echo [format {UART0_SR after = 0x%08x  (bit3=TX_EMPTY)} $sr0]

echo ""
echo "=== Re-init UART1 with correct divisors ==="
mww 0xFF01000C 0xFFFFFFFF
mww 0xFF010000 0x00000003
after 5
mww 0xFF010004 0x00000020
mww 0xFF010018 43
mww 0xFF010034 13
mww 0xFF010000 0x00000014
after 2

echo "=== Send 'UART1\\r\\n' from UART1 ==="
foreach b {0x55 0x41 0x52 0x54 0x31 0x0D 0x0A 0x55 0x55 0x55} {
    mww 0xFF010030 $b
}
after 30
set sr1 [lindex [read_memory 0xFF01002C 32 1] 0]
echo [format {UART1_SR after = 0x%08x  (bit3=TX_EMPTY)} $sr1]

echo ""
echo "Both strings sent. Watch /dev/ttyUSB1:"
echo "  'UART0' appears → board uses UART0 (base 0xFF000000)"
echo "  'UART1' appears → board uses UART1 (base 0xFF010000)"
echo ""
shutdown
