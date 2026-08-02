# diag_ap1_base.tcl — read APB-AP (AP[1]) register set and probe R5-0 debug address.

proc clear_sticky {} {
    irscan uscale.tap 0x8
    drscan uscale.tap 35 0xF8
    runtest 100
    irscan uscale.tap 0xA
    drscan uscale.tap 35 0x280000102
    drscan uscale.tap 35 0x07
    runtest 200
}

init
after 300
targets uscale.axi
clear_sticky
uscale.axi arp_examine
targets uscale.axi

echo ""
echo "=== AP[1] APB-AP registers ==="
foreach {name offset} {IDR 0xfc BASE 0xf8 CFG 0xf4 CSW 0x00 TAR 0x04} {
    clear_sticky
    set v 0
    if { [catch { set v [uscale.dap apreg 1 $offset] } e] } {
        echo [format "  %-6s (0x%02x) = ERROR: %s" $name $offset $e]
    } else {
        echo [format "  %-6s (0x%02x) = 0x%08x" $name $offset $v]
    }
}

echo ""
echo "=== AXI reads of candidate R5-0 debug base addresses ==="
echo "  (R5 debug registers are on APB, NOT AXI — expect 0 or garbage here)"
proc rd {a} { return [lindex [read_memory $a 32 1] 0] }

clear_sticky
foreach addr {0xFE800000 0xFE810000 0xFE820000 0xFE900000} {
    clear_sticky
    echo [format {  0x%08x = 0x%08x} $addr [rd $addr]]
}

echo ""
echo "=== UART1_CR current state ==="
clear_sticky
echo [format {  UART1_CR @ 0xFF010000 = 0x%08x} [rd 0xFF010000]]

echo ""
echo "=== BSS start — still 0xdeadbeef? ==="
clear_sticky
for {set i 0} {$i < 4} {incr i} {
    set a [expr {0xFFFF2BCC + $i * 4}]
    echo [format {  0x%08x = 0x%08x} $a [rd $a]]
}

echo ""
shutdown
