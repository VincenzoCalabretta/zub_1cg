# probe_uart.tcl — after psu_init has run, dump UART pinmux + clock
# registers and blast a marker byte out both UART0 and UART1 to see which
# one is wired to /dev/ttyUSB1 on this board.

puts "==== post-psu_init: UART pinmux + clocks ===="
foreach {name addr} {
    MIO_PIN_10     0xFF180028
    MIO_PIN_11     0xFF18002C
    MIO_PIN_22     0xFF180058
    MIO_PIN_23     0xFF18005C
    UART0_REF_CTRL 0xFF5E0074
    UART1_REF_CTRL 0xFF5E0078
    UART0_RST_REG  0xFF5E0230
    MIO_MST_TRI0   0xFF180204
    IOU_SLCR_BANK0_CTRL0 0xFF180138
    IOU_SLCR_BANK0_CTRL1 0xFF18013C
} {
    set v [lindex [read_memory $addr 32 1] 0]
    puts [format "  %-25s (0x%08X) = 0x%08X" $name $addr $v]
}

puts "==== deassert UART0 + UART1 reset (RST_LPD_IOU0 bits 0,1) ===="
proc rmw {addr mask val} {
    set cur [lindex [read_memory $addr 32 1] 0]
    set new [expr {($cur & ~$mask) | ($val & $mask)}]
    mww $addr $new
    set rb [lindex [read_memory $addr 32 1] 0]
    puts [format "  0x%08X: 0x%08X → 0x%08X" $addr $cur $rb]
}
rmw 0xFF5E0230 0x00000003 0x00000000
after 10
puts "  UART0_SR=[format 0x%08X [lindex [read_memory 0xFF00002C 32 1] 0]]"

puts "==== send 'BOOT0\\r\\n' via UART0 (0xFF000000) ===="
mww 0xFF00000C 0xFFFFFFFF
mww 0xFF000000 0x00000003
after 5
mww 0xFF000004 0x00000020
mww 0xFF000018 7
mww 0xFF000034 124
mww 0xFF000000 0x00000014
after 10
foreach b {0x42 0x4F 0x4F 0x54 0x30 0x0D 0x0A} { mww 0xFF000030 $b; after 5 }
after 100
puts "  UART0_SR=[format 0x%08X [lindex [read_memory 0xFF00002C 32 1] 0]]"

puts "==== send 'BOOT1\\r\\n' via UART1 (0xFF010000) ===="
mww 0xFF01000C 0xFFFFFFFF
mww 0xFF010000 0x00000003
after 5
mww 0xFF010004 0x00000020
mww 0xFF010018 43
mww 0xFF010034 13
mww 0xFF010000 0x00000014
after 10
foreach b {0x42 0x4F 0x4F 0x54 0x31 0x0D 0x0A} { mww 0xFF010030 $b; after 5 }
after 100
shutdown
