# mio_sweep.tcl — find which MIO pin is connected to UART1 TX on /dev/ttyUSB1.
#
# For each MIO pin 0-77:
#   1. Set L3_SEL=6 (UART function) on that pin
#   2. Send byte = pin_number from UART1
#   3. Restore pin to GPIO (L3_SEL=0)
#
# Capture /dev/ttyUSB1 on the host: whatever byte appears tells you the MIO pin.
# Also sweeps UART0 in case the console is UART0 not UART1.

init
after 500
targets uscale.axi

if { [catch { read_memory 0xFF9A0000 32 1 } err] } {
    echo "ERROR: AXI inaccessible."
    shutdown
    return
}

echo "=== Setup: clocks + peripheral reset ==="
# UART0 ref clock: SRCSEL=IOPLL, DIV1=1, DIV0=24 -> 69.44 MHz
mww 0xFF5E0070 0x01011800
# UART1 ref clock: same
mww 0xFF5E0074 0x01011800
# Release ALL RST_LPD_IOU2
mww 0xFF5E0238 0x00000000
after 5

# Init UART1 (0xFF010000): 69.44 MHz / (43*14) = 115313 baud
mww 0xFF01000C 0xFFFFFFFF
mww 0xFF010000 0x00000003
after 2
mww 0xFF010004 0x00000020
mww 0xFF010018 43
mww 0xFF010034 13
mww 0xFF010000 0x00000014

# Init UART0 (0xFF000000): same baud settings
mww 0xFF00000C 0xFFFFFFFF
mww 0xFF000000 0x00000003
after 2
mww 0xFF000004 0x00000020
mww 0xFF000018 43
mww 0xFF000034 13
mww 0xFF000000 0x00000014
after 2

echo "=== MIO sweep: UART1 TX (byte = pin number) ==="
echo "    (set L3_SEL=6 on each pin, send pin# as byte, restore to GPIO)"

# IOU_SLCR_MIO_PIN base = 0xFF180000
# MIO_PIN_N is at 0xFF180000 + N*4
# L3_SEL[5:3]=6 -> bits = 0b110_000 = 0x30
# Also set PULLUP[25]=1 and IO_TYPE[10:9]=01 (LVCMOS33) for good measure: 0x02000030
# But keep it simple: just 0x00000030

for {set pin 0} {$pin < 78} {incr pin} {
    set mio_addr [expr {0xFF180000 + $pin * 4}]
    # Set UART function
    mww $mio_addr 0x00000030
    # Send the pin number as a byte from UART1
    mww 0xFF010030 $pin
    after 2
    # Restore to GPIO
    mww $mio_addr 0x00000000
}

after 10
echo "UART1 sweep done."
echo ""
echo "=== MIO sweep: UART0 TX (byte = 0x80 | pin number) ==="

for {set pin 0} {$pin < 78} {incr pin} {
    set mio_addr [expr {0xFF180000 + $pin * 4}]
    mww $mio_addr 0x00000030
    # Send 0x80|pin from UART0 (so we can distinguish UART0 vs UART1 results)
    mww 0xFF000030 [expr {0x80 | $pin}]
    after 2
    mww $mio_addr 0x00000000
}

after 10
echo "UART0 sweep done."
echo {If bytes appeared on ttyUSB1:}
echo {  byte < 0x80  -> UART1 TX on MIO[byte]}
echo {  byte >= 0x80 -> UART0 TX on MIO[byte & 0x7F]}
echo ""
shutdown
