# check_reset_handler.tcl — verify reset_handler at 0xFFFF0118 and R5 execution state.
init
after 300
targets uscale.axi

if { [catch { read_memory 0xFF9A0000 32 1 } err] } {
    echo "ERROR: AXI blocked."
    shutdown
    return
}

echo ""
echo "=== reset_handler (at 0xFFFF0118 from literal pool) ==="
for {set i 0} {$i < 16} {incr i} {
    set a [expr {0xFFFF0118 + $i*4}]
    set v [lindex [read_memory $a 32 1] 0]
    echo [format {0x%08x = 0x%08x} $a $v]
}

echo ""
echo "=== BSS zeroing progress check ==="
echo "(If reset_handler is running, BSS at 0xFFFF2BCC should fill with 0 over time)"
echo "First read:"
set b0 [lindex [read_memory 0xFFFF2BCC 32 1] 0]
set b1 [lindex [read_memory 0xFFFF2BD0 32 1] 0]
echo [format {  0xFFFF2BCC = 0x%08x} $b0]
echo [format {  0xFFFF2BD0 = 0x%08x} $b1]
after 100
echo "Second read (100ms later):"
set b0b [lindex [read_memory 0xFFFF2BCC 32 1] 0]
set b1b [lindex [read_memory 0xFFFF2BD0 32 1] 0]
echo [format {  0xFFFF2BCC = 0x%08x} $b0b]
echo [format {  0xFFFF2BD0 = 0x%08x} $b1b]
if { $b0 != $b0b } {
    echo "  → BSS changed! R5 is executing (bss_loop in progress)."
} elseif { $b0 == 0 } {
    echo "  → BSS is zero (reset_handler completed startup)."
} else {
    echo "  → BSS unchanged and non-zero (R5 stuck before bss_loop)."
}

echo ""
echo "=== _tx_thread_system_state (ThreadX) ==="
set tx_state [lindex [read_memory 0xFFFF2BC8 32 1] 0]
echo [format {_tx_thread_system_state @ 0xFFFF2BC8 = 0x%08x} $tx_state]
echo "(1=initializing, 2=ready, 9=in_interrupt — 0=not_started)"

echo ""
echo "=== UART1 CR (shows if uart_init ran) ==="
echo [format {UART1_CR @ 0xFF010000 = 0x%08x  (0x114 = TX_EN|RX_EN|STOPBRK)} \
    [lindex [read_memory 0xFF010000 32 1] 0]]
echo [format {UART1_BGEN @ 0xFF010018 = 0x%08x  (43 = correct for 69.44MHz)} \
    [lindex [read_memory 0xFF010018 32 1] 0]]

echo ""
shutdown
