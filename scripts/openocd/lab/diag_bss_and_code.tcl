# diag_bss_and_code.tcl — check BSS zeroing state and disassemble BSS loop area.

init
after 300
targets uscale.axi

irscan uscale.tap 0x8
drscan uscale.tap 35 0xF8
runtest 100
irscan uscale.tap 0xA
drscan uscale.tap 35 0x280000102
drscan uscale.tap 35 0x07
runtest 200
uscale.axi arp_examine
targets uscale.axi

proc rd {a} { return [lindex [read_memory $a 32 1] 0] }

echo ""
echo "=== BSS sample — 8 words from _bss_start to _bss_start+28 ==="
for {set i 0} {$i < 8} {incr i} {
    set a [expr {0xFFFF2BCC + $i * 4}]
    echo [format {  0x%08x = 0x%08x} $a [rd $a]]
}

echo ""
echo "=== BSS end sample — last 4 words before _bss_end ==="
for {set i -4} {$i < 0} {incr i} {
    set a [expr {0xFFFF42C8 + $i * 4}]
    echo [format {  0x%08x = 0x%08x} $a [rd $a]]
}

echo ""
echo "=== Are ANY BSS words zero? (scan first 32 words) ==="
set any_zero 0
for {set i 0} {$i < 32} {incr i} {
    set a [expr {0xFFFF2BCC + $i * 4}]
    if { [rd $a] == 0 } {
        echo [format {  ZERO at 0x%08x (offset +%d)} $a [expr {$i*4}]]
        set any_zero 1
    }
}
if { !$any_zero } { echo "  No zeros in first 32 words — BSS loop never ran." }

echo ""
echo "=== Disassemble BSS loop region (approx. 0xFFFF01E0–0xFFFF0220) ==="
for {set i 0} {$i < 16} {incr i} {
    set a [expr {0xFFFF01E0 + $i * 4}]
    echo [format {  0x%08x = 0x%08x} $a [rd $a]]
}

echo ""
echo "=== Exception handler addresses (data abort = 0xFFFF0010) ==="
echo [format {vector[0] (reset)  @ 0xFFFF0000 = 0x%08x} [rd 0xFFFF0000]]
echo [format {vector[3] (prefetch)@ 0xFFFF000C = 0x%08x} [rd 0xFFFF000C]]
echo [format {vector[4] (data ab) @ 0xFFFF0010 = 0x%08x} [rd 0xFFFF0010]]
echo [format {vector[6] (IRQ)     @ 0xFFFF0018 = 0x%08x} [rd 0xFFFF0018]]
echo ""
echo "=== Data abort handler target (follow LDR pc from vector) ==="
set dabh_lit [rd 0xFFFF0030]   ;# literal pool entry for data abort (3rd LDR = vector[4]+lit)
echo [format {data_abort literal pool entry @ 0xFFFF0030 = 0x%08x} $dabh_lit]
if { $dabh_lit != 0 } {
    set instr [rd $dabh_lit]
    echo [format {__tx_abort_handler @ 0x%08x = 0x%08x  (0xEAFFFFFE = B .)} $dabh_lit $instr]
}

echo ""
echo "=== UART1 state ==="
echo [format {UART1_CR   = 0x%08x  (0x14=TX+RX_EN, 0x114=TX+RX+STPBRK)} [rd 0xFF010000]]
echo [format {UART1_BGEN = 0x%08x  (43=correct)} [rd 0xFF010018]]

echo ""
echo "=== _tx_thread_system_state ==="
echo [format {_tx_thread_system_state @ 0xFFFF2BC8 = 0x%08x  (0=not started)} [rd 0xFFFF2BC8]]

echo ""
shutdown
