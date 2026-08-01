# Diagnostic: configure UART0 (per Vitis psu_init) from OpenOCD, then send
# a test byte and readback UART0_SR to check TX status.
init
poll off

targets uscale.axi

proc rmw {addr mask val} {
    set cur [lindex [read_memory $addr 32 1] 0]
    set new [expr {($cur & ~$mask) | ($val & $mask)}]
    mww $addr $new
    set rb [lindex [read_memory $addr 32 1] 0]
    echo [format "  rmw 0x%08X mask=0x%08X val=0x%08X  before=0x%08X  after=0x%08X" \
          $addr $mask $val $cur $rb]
}

echo "==== initial state ===="
foreach {name addr} {
    MIO_PIN_10      0xFF180028
    MIO_PIN_11      0xFF18002C
    UART0_REF_CTRL  0xFF5E0074
    UART0_RST_REG   0xFF5E0230
    MIO_MST_TRI0    0xFF180204
    UART0_CR        0xFF000000
    UART0_MR        0xFF000004
    UART0_SR        0xFF00002C
} {
    set v [lindex [read_memory $addr 32 1] 0]
    echo [format "  %-15s (0x%08X) = 0x%08X" $name $addr $v]
}

echo "==== deassert UART0 reset ===="
rmw 0xFF5E0230 0x00000002 0x00000000

echo "==== MIO pinmux for UART0 (MIO 10=RX, 11=TX per psu_init) ===="
rmw 0xFF180028 0x000000FE 0x000000C0
rmw 0xFF18002C 0x000000FE 0x000000C0

echo "==== clear tristate on MIO 10, 11 ===="
rmw 0xFF180204 0x00000C00 0x00000000

echo "==== UART0 clock: SRCSEL=IOPLL DIV1=1 DIV0=15 → 100 MHz ===="
rmw 0xFF5E0074 0x013F3F07 0x01010F00
after 5

echo "==== UART0 controller init ===="
mww 0xFF00000C 0xFFFFFFFF
mww 0xFF000000 0x00000003
after 5
mww 0xFF000004 0x00000020
mww 0xFF000018 7
mww 0xFF000034 124
mww 0xFF000000 0x00000014
after 10

echo "==== send 'X' 0x58, then check SR ===="
mww 0xFF000030 0x58
after 30
set sr [lindex [read_memory 0xFF00002C 32 1] 0]
echo [format "  UART0_SR = 0x%08X   (TXEMPTY=bit3, TXFULL=bit4)" $sr]

echo "==== send 'PING\\n' ===="
foreach b {0x50 0x49 0x4E 0x47 0x0D 0x0A} {
    mww 0xFF000030 $b
    after 5
}
after 100
shutdown
