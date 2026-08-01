# restart_r5.tcl — cycle R5 reset and watch BSS[0] for zeroing (decisve test).
#
# BSS at 0xFFFF2BCC starts as 0xdeadbeef (OCM uninitialized).
# If reset_handler runs bss_loop -> value becomes 0x00000000.
# We restart R5 and poll 50x at 10ms intervals.

init
after 300
targets uscale.axi

if { [catch { read_memory 0xFF9A0000 32 1 } err] } {
    echo "ERROR: AXI blocked — power-cycle and restart."
    shutdown
    return
}

proc rd {a} { return [lindex [read_memory $a 32 1] 0] }

echo ""
echo "=== Current state before restart ==="
echo [format "RST_LPD_TOP = 0x%08x  (bit0=0 = R5 running)" [rd 0xFF5E023C]]
echo [format "BSS at 0xFFFF2BCC = 0x%08x" [rd 0xFFFF2BCC]]
echo [format "UART1_CR    = 0x%08x" [rd 0xFF010000]]
echo [format "UART1_BGEN  = 0x%08x  (43 = correct)" [rd 0xFF010018]]

echo ""
echo "=== Cycling R5 reset (bit0: 0->1->0) ==="
set rst [rd 0xFF5E023C]
mww 0xFF5E023C [expr {$rst | 0x1}]   ;# hold R5 in reset
after 100
echo [format "RST held    = 0x%08x" [rd 0xFF5E023C]]

# Re-verify VINITHI still set
echo [format "RPU_0_CFG   = 0x%08x  (bit0=1=VINITHI required)" [rd 0xFF9A0100]]

# Re-apply VINITHI=1 in case reset affected it
mww 0xFF9A0100 0x00000001

# Release from module reset
mww 0xFF5E023C [expr {($rst & ~0x1) | 0x2}]
echo [format "RST released= 0x%08x  (R5 now running from 0xFFFF0000)" [rd 0xFF5E023C]]

echo ""
echo "=== Polling BSS at 0xFFFF2BCC (10ms intervals, 50 samples) ==="
set found 0
for {set i 1} {$i <= 50} {incr i} {
    after 10
    set b [rd 0xFFFF2BCC]
    if {$b != 0xdeadbeef} {
        echo [format "+%dms: BSS changed to 0x%08x — bss_loop RAN!" [expr {$i*10}] $b]
        set found 1
        break
    }
}
if {!$found} {
    echo "500ms elapsed: BSS still 0xdeadbeef — R5 stuck BEFORE bss_loop"
}

echo ""
echo "=== Final snapshot ==="
echo [format "BSS at 0xFFFF2BCC = 0x%08x" [rd 0xFFFF2BCC]]
echo [format "RST_LPD_TOP       = 0x%08x" [rd 0xFF5E023C]]
echo [format "UART1_CR          = 0x%08x" [rd 0xFF010000]]

echo ""
shutdown
