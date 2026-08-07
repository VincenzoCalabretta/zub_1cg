# diag_live_bss.tcl — check if R5 is executing by watching BSS and canary.
# Writes a fresh test pattern to BSS[0], waits 2s, checks if R5 zeroed it.
# Also reads the canary at 0xFFFFFF00 (written by diag_canary.tcl patches).

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
echo "=== Canary poll (may have been written after previous script exited) ==="
clear_sticky
set canary [lindex [read_memory 0xFFFFFF00 32 1] 0]
echo [format "  canary @ 0xFFFFFF00 = 0x%08x" $canary]
switch $canary {
    0xCAFE0001 { echo "  -> UNDEF exception fired!" }
    0xCAFE0002 { echo "  -> SWI exception fired!" }
    0xCAFE0003 { echo "  -> PREFETCH ABORT exception fired!" }
    0xCAFE0004 { echo "  -> DATA ABORT exception fired!" }
    0xDEADDEAD { echo "  -> Still no exception (sentinel unchanged)" }
    default     { echo [format "  -> Unexpected value 0x%08x" $canary] }
}

echo ""
echo "=== Confirm exception handler patches still in place ==="
clear_sticky
echo [format "  0xFFFF0228 = 0x%08x  (expect 0xEA003B34 = B 0xFFFFEF00)" \
    [lindex [read_memory 0xFFFF0228 32 1] 0]]
clear_sticky
echo [format "  0xFFFF0234 = 0x%08x  (expect 0xEA003B49 = B 0xFFFFEF60)" \
    [lindex [read_memory 0xFFFF0234 32 1] 0]]

echo ""
echo "=== Writing fresh pattern 0xBAADF00D to BSS[0..3] ==="
clear_sticky
mww 0xFFFF2BCC 0xBAADF00D
mww 0xFFFF2BD0 0xBAADF00D
mww 0xFFFF2BD4 0xBAADF00D
mww 0xFFFF2BD8 0xBAADF00D
clear_sticky
echo [format "  BSS[0] @ 0xFFFF2BCC = 0x%08x  (wrote BAADF00D)" \
    [lindex [read_memory 0xFFFF2BCC 32 1] 0]]

echo ""
echo "=== Waiting 3 seconds for R5 to zero BSS if it's running ==="
after 3000

echo ""
echo "=== Post-wait check ==="
clear_sticky
set bss0 [lindex [read_memory 0xFFFF2BCC 32 1] 0]
clear_sticky
set bss1 [lindex [read_memory 0xFFFF2BD0 32 1] 0]
clear_sticky
set can2 [lindex [read_memory 0xFFFFFF00 32 1] 0]

echo [format "  BSS[0] @ 0xFFFF2BCC = 0x%08x" $bss0]
echo [format "  BSS[1] @ 0xFFFF2BD0 = 0x%08x" $bss1]
echo [format "  canary @ 0xFFFFFF00 = 0x%08x" $can2]

echo ""
if { $bss0 == 0x00000000 } {
    echo "RESULT: BSS[0] = 0x00000000"
    echo "  -> R5 IS executing the BSS zero loop"
    echo "  -> Exception handlers were NOT triggered"
    echo "  -> R5 completed startup and BSS was zeroed"
} elseif { $bss0 == 0xBAADF00D } {
    echo "RESULT: BSS[0] still 0xBAADF00D (not zeroed)"
    echo "  -> R5 has NOT reached the BSS loop in 3 seconds"
    echo "  -> R5 is stuck between reset_handler start and BSS loop"
    echo "  -> OR R5 is not running at all"
} else {
    echo [format "RESULT: BSS[0] = 0x%08x (unexpected)" $bss0]
}

if { $can2 == 0xCAFE0004 } {
    echo "  canary=CAFE0004 -> data abort exception fired (R5 crashed at BSS STR)"
}

echo ""
echo "=== Vector table first word (confirms ELF in OCM) ==="
clear_sticky
echo [format "  0xFFFF0000 = 0x%08x  (reset vector LDR pc insn)" \
    [lindex [read_memory 0xFFFF0000 32 1] 0]]
clear_sticky
echo [format "  0xFFFF0020 = 0x%08x  (reset handler ptr, expect 0xFFFF0118)" \
    [lindex [read_memory 0xFFFF0020 32 1] 0]]

echo ""
shutdown
