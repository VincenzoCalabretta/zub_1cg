# check_xmpu.tcl — check OCM XMPU (memory protection) configuration.
#
# If XMPU blocks RPU (R5) from accessing OCM, the R5 would get a prefetch
# abort on its very first instruction fetch from 0xFFFF0000, leaving it
# stuck in an abort loop without zeroing BSS.
#
# XMPU_OCM base: 0xFF980000
# XMPU_CFG registers: ISR, IMR, STATUS, ERR_ADDR, ERR_STATUS at base
# XMPU_R0..R15 regions at 0xFF980100..0xFF98017C (16 regions * 4 regs * 4 bytes)
#
# Also check FPD/LPD safety configuration.

init
after 300
targets uscale.axi

if { [catch { read_memory 0xFF9A0000 32 1 } err] } {
    echo "ERROR: AXI blocked."
    shutdown
    return
}

proc rd {a} { return [lindex [read_memory $a 32 1] 0] }

echo ""
echo "=== OCM XMPU (0xFF980000) ==="
echo [format {XMPU_OCM ISR       @ 0xFF980000 = 0x%08x} [rd 0xFF980000]]
echo [format {XMPU_OCM IMR       @ 0xFF980004 = 0x%08x} [rd 0xFF980004]]
echo [format {XMPU_OCM STATUS    @ 0xFF980008 = 0x%08x  (bit0=violation)} [rd 0xFF980008]]
echo [format {XMPU_OCM ERR_ADDR  @ 0xFF98000C = 0x%08x  (addr that violated)} [rd 0xFF98000C]]
echo [format {XMPU_OCM ERR_STAT  @ 0xFF980010 = 0x%08x} [rd 0xFF980010]]
echo [format {XMPU_OCM CTRL      @ 0xFF980014 = 0x%08x  (bit0=defr_perm)} [rd 0xFF980014]]

echo ""
echo "=== XMPU_OCM regions 0..3 (each region: addr, end, master, cfg) ==="
for {set r 0} {$r < 4} {incr r} {
    set base [expr {0xFF980100 + $r * 16}]
    set a0 [rd $base]
    set a1 [rd [expr {$base + 4}]]
    set ms [rd [expr {$base + 8}]]
    set cf [rd [expr {$base + 12}]]
    echo [format {  Region%d: START=0x%08x END=0x%08x MASTER=0x%08x CFG=0x%08x} \
        $r $a0 $a1 $ms $cf]
}

echo ""
echo "=== OCM access test from OpenOCD AXI (should succeed) ==="
echo [format {OCM[0xFFFF0000] = 0x%08x  (expect 0xe59ff018)} [rd 0xFFFF0000]]

echo ""
echo "=== R5 debug: try to access memory via R5 instruction trace (not possible) ==="
echo "(R5 debug target not available — inferring from OCM XMPU error registers)"

echo ""
echo "=== Check if R5 is stuck in prefetch abort loop ==="
echo "=== Read XMPU ERR_ADDR after a short delay to see if R5 triggered a violation ==="
# Clear the ISR first
mww 0xFF980000 0xFFFFFFFF
after 50

set isr [rd 0xFF980000]
set err [rd 0xFF98000C]
echo [format {XMPU ISR after 50ms = 0x%08x  (non-zero = violation occurred)} $isr]
echo [format {XMPU ERR_ADDR       = 0x%08x  (address that triggered)} $err]
if {$isr != 0} {
    echo "XMPU VIOLATION DETECTED! R5 is being blocked by XMPU."
    echo [format "  Blocked address: 0x%08x" $err]
} else {
    echo "No XMPU violation recorded in last 50ms."
}

echo ""
echo "=== FPD XMPU (0xFD5D0000) and LPD XMPU ==="
if { [catch { set fpd [rd 0xFD5D0000] } e] } {
    echo "FPD XMPU not accessible (expected — FPD might need power-up)"
} else {
    echo [format {FPD XMPU ISR @ 0xFD5D0000 = 0x%08x} $fpd]
}

echo ""
shutdown
