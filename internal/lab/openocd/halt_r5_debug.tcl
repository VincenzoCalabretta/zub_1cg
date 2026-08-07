# halt_r5_debug.tcl — try to halt R5 via its CoreSight debug registers over AXI.
#
# R5-0 Cortex-R5 debug base in ZU+ CoreSight topology: 0xFE810000.
# If accessible through AXI mem_ap, we can:
#  1. Enable debug (DBGOSLAR=0 / DBGPRCR.COREPURQ=1 / DBGEDSRCR)
#  2. Halt R5 (DBGDRCR bit0=HaltReq)
#  3. Write to CP15 SCTLR via debug interface to change VINITHI
#  4. Resume R5 → R5 restarts from 0xFFFF0000 (our ELF)
#
# Cortex-R5 external debug register offsets (ARM DDI0460D):
#   0x000: DBGDIDR     (ID register, read-only, should be 0x35141000 or similar)
#   0x030: DBGSCR      (Secure Debug Control/Status)
#   0x080: DBGDTRTX    (Host-to-target data)
#   0x084: DBGDTRRX    (Target-to-host data)
#   0x088: DBGDSCR     (Debug Status and Control)
#   0x090: DBGDTRTX    (same alias?)
#   0x094: DBGPCSR     (Program Counter Sample, read only)
#   0x0A0: DBGDRCR     (Debug Run Control: bit0=HaltReq, bit1=RestartReq)
#   0x300: DBGEDSCRR   (External Debug Status)
#   0xFB0: DBGLAR      (Lock Access: write 0xC5ACCE55 to unlock)
#   0xFB4: DBGLSR      (Lock Status)

init
after 500
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

set R5DBG 0xFE810000

echo ""
echo "=== Try to read R5-0 debug registers at 0xFE810000 ==="
if { [catch { set didr [rd $R5DBG] } err] } {
    echo "R5 debug base 0xFE810000 NOT accessible: $err"
    echo "Trying alternate base 0xFF9B0000..."
    set R5DBG 0xFF9B0000
    if { [catch { set didr [rd $R5DBG] } err2] } {
        echo "0xFF9B0000 also not accessible: $err2"
        echo "Cannot access R5 debug registers via AXI."
        shutdown
        return
    }
}
echo [format {DBGDIDR @ 0x%08x = 0x%08x  (expect 0x?5?41000 for Cortex-R5)} $R5DBG $didr]
echo [format {DBGDSCR  = 0x%08x} [rd [expr {$R5DBG + 0x088}]]]
echo [format {DBGDRCR  = 0x%08x} [rd [expr {$R5DBG + 0x0A0}]]]
echo [format {DBGLSR   = 0x%08x  (bit1=1=locked)} [rd [expr {$R5DBG + 0xFB4}]]]

echo ""
echo "=== Unlock debug access (DBGLAR) ==="
mww [expr {$R5DBG + 0xFB0}] 0xC5ACCE55
after 5
echo [format {DBGLSR after unlock = 0x%08x  (expect bit1=0)} [rd [expr {$R5DBG + 0xFB4}]]]

echo ""
echo "=== Read DBGPCSR (PC sample, shows where R5 is executing) ==="
set pc0 [rd [expr {$R5DBG + 0x084}]]
after 10
set pc1 [rd [expr {$R5DBG + 0x084}]]
after 10
set pc2 [rd [expr {$R5DBG + 0x084}]]
echo [format {DBGPCSR sample 1 = 0x%08x} $pc0]
echo [format {DBGPCSR sample 2 = 0x%08x} $pc1]
echo [format {DBGPCSR sample 3 = 0x%08x} $pc2]

echo ""
echo "=== Halt R5 via DBGDRCR (HaltReq) ==="
mww [expr {$R5DBG + 0x0A0}] 0x00000001   ;# DBGDRCR: bit0=HaltReq
after 100
set dscr [rd [expr {$R5DBG + 0x088}]]
echo [format {DBGDSCR after halt = 0x%08x  (bit0=Halted, bit1=Restarted)} $dscr]
if { [expr {$dscr & 0x1}] } {
    echo "R5 IS HALTED."
    set pc [rd [expr {$R5DBG + 0x084}]]
    echo [format {DBGPCSR (halted PC) = 0x%08x} $pc]

    echo ""
    echo "=== Try to issue instruction via ITR (DBGITR) to read CPSR ==="
    echo "(ITR access requires DBGDSCR.ITRen=1)"
} else {
    echo "R5 did NOT halt (DSCR bit0=0). Debug may not be enabled or wrong base address."
}

echo ""
echo "=== DBGDSCR detail ==="
echo [format {DBGDSCR = 0x%08x} [rd [expr {$R5DBG + 0x088}]]]

echo ""
shutdown
