# restart_and_watch.tcl — put R5 back in reset, release, watch BSS in real-time.
#
# If BSS[0] becomes 0 within 50ms, reset_handler ran the BSS zero loop.
# If it stays deadbeef, R5 is stuck BEFORE the BSS loop.
# Also watches OCM[0xFFFF0238] (__tx_reserved_handler) to detect if R5 is
# spinning in that instruction (which would indicate the literal pool loads
# are clobbering __tx_reserved_handler's address range).

init
after 300
targets uscale.axi

if { [catch { read_memory 0xFF9A0000 32 1 } err] } {
    echo "ERROR: AXI blocked."
    shutdown
    return
}

echo ""
echo "=== Pre-restart state ==="
echo [format {RST_LPD_TOP (pre)     = 0x%08x} [lindex [read_memory 0xFF5E023C 32 1] 0]]
echo [format {BSS[0xFFFF2BCC] (pre) = 0x%08x} [lindex [read_memory 0xFFFF2BCC 32 1] 0]]
echo [format {UART1_CR (pre)        = 0x%08x} [lindex [read_memory 0xFF010000 32 1] 0]]

echo ""
echo "=== Step 1: Put R5 back in module reset ==="
set rst [lindex [read_memory 0xFF5E023C 32 1] 0]
set rst_hold [expr {$rst | 0x1}]
mww 0xFF5E023C $rst_hold
after 50
echo [format {RST_LPD_TOP after hold = 0x%08x  (bit0=1 = R5 in reset)} \
    [lindex [read_memory 0xFF5E023C 32 1] 0]]

echo ""
echo "=== Step 2: Write sentinel 0xAABBCCDD to BSS[0] ==="
mww 0xFFFF2BCC 0xAABBCCDD
echo [format {BSS[0] written         = 0x%08x} [lindex [read_memory 0xFFFF2BCC 32 1] 0]]

echo ""
echo "=== Step 3: Re-release R5 ==="
set rst_run [expr {($rst_hold & ~0x1) | 0x2}]
mww 0xFF5E023C $rst_run
echo [format {RST_LPD_TOP released   = 0x%08x  (bit0=0 = R5 running)} $rst_run]

echo ""
echo "=== Step 4: Poll BSS[0] and UART1_CR every 10ms for 500ms ==="
for {set tick 0} {$tick < 50} {incr tick} {
    after 10
    set bss [lindex [read_memory 0xFFFF2BCC 32 1] 0]
    set cr  [lindex [read_memory 0xFF010000 32 1] 0]
    if { $bss != 0xAABBCCDD } {
        echo [format {  +%dms: BSS changed to 0x%08x  UART_CR=0x%08x ← RUNNING!} \
            [expr {($tick+1)*10}] $bss $cr]
        if { $bss == 0 } {
            echo "  → BSS is ZERO: reset_handler bss_loop completed."
        } else {
            echo "  → BSS changed to non-zero (unexpected)."
        }
        break
    }
    if { $tick == 9 } { echo "  100ms: still AABBCCDD — bss_loop not yet running" }
    if { $tick == 24 } { echo "  250ms: still AABBCCDD — bss_loop not running" }
}

set bss_final [lindex [read_memory 0xFFFF2BCC 32 1] 0]
set cr_final  [lindex [read_memory 0xFF010000 32 1] 0]
echo ""
echo [format {=== Final state: BSS[0]=0x%08x  UART_CR=0x%08x  RST=0x%08x ===} \
    $bss_final $cr_final [lindex [read_memory 0xFF5E023C 32 1] 0]]

if { $bss_final == 0xAABBCCDD } {
    echo "CONCLUSION: R5 is stuck BEFORE bss_loop (reset_handler or mode setup fault)"
} elseif { $bss_final == 0 } {
    echo "CONCLUSION: R5 completed bss_loop (main() was called)"
} else {
    echo "CONCLUSION: BSS changed to unexpected value"
}

echo ""
shutdown
