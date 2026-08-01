# diag_write_regs.tcl — empirically check which register writes stick.
# Goal: determine whether RST_LPD_TOP and RPU are XPPU-protected or just
# self-clearing (pulse reset registers that always read back 0).

init
after 500
targets uscale.axi

# Unconditional STICKYERR clear
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
proc wr {a v} { mww $a $v }

echo ""
echo "=== Canary: OCM write (should always work) ==="
wr 0xFFFF3004 0x11223344
echo [format {OCM[0xFFFF3004] = 0x%08x  (expect 0x11223344)} [rd 0xFFFF3004]]

echo ""
echo "=== RST_LPD_IOU2 (0xFF5E0238) — was writable in previous session ==="
echo [format {IOU2 before = 0x%08x} [rd 0xFF5E0238]]
wr 0xFF5E0238 0x00000001
after 5
echo [format {IOU2 after  = 0x%08x  (wrote 0x1)} [rd 0xFF5E0238]]
wr 0xFF5E0238 0x00000000  ;# restore to 0
echo [format {IOU2 restore= 0x%08x  (restored to 0)} [rd 0xFF5E0238]]

echo ""
echo "=== RST_LPD_TOP (0xFF5E023C) — CPU reset register ==="
echo [format {TOP before = 0x%08x} [rd 0xFF5E023C]]
wr 0xFF5E023C 0x00000003   ;# try to hold both R5s in reset
after 50
echo [format {TOP after  = 0x%08x  (wrote 0x3)} [rd 0xFF5E023C]]

echo ""
echo "=== RPU_GLBL_CNTL (0xFF9A0000) ==="
echo [format {RPU_CTRL before = 0x%08x} [rd 0xFF9A0000]]
wr 0xFF9A0000 0x00000008
after 5
echo [format {RPU_CTRL after  = 0x%08x  (wrote 0x8)} [rd 0xFF9A0000]]

echo ""
echo "=== RPU_0_CFG (0xFF9A0100) ==="
echo [format {RPU_CFG before = 0x%08x} [rd 0xFF9A0100]]
wr 0xFF9A0100 0x00000001
after 5
echo [format {RPU_CFG after  = 0x%08x  (wrote 0x1)} [rd 0xFF9A0100]]

echo ""
echo "=== PMU_GLOBAL soft reset (0xFFD80608) — try alternative CPU reset ==="
# PMU_GLOBAL base 0xFFD80000, RPU_RESET at offset 0x608
echo [format {PMU_GLOBAL.RPU_RESET before = 0x%08x} [rd 0xFFD80608]]
wr 0xFFD80608 0x00000002   ;# bit1 = RPU_R5_0 reset request
after 50
echo [format {PMU_GLOBAL.RPU_RESET after  = 0x%08x  (wrote 0x2)} [rd 0xFFD80608]]

echo ""
echo "=== Summary of which writes stuck ==="
echo [format {OCM writable:          %s} [expr { [rd 0xFFFF3004] == 0x11223344 ? "YES" : "NO" }]]
echo [format {RST_LPD_IOU2 writable: %s} [expr { [rd 0xFF5E0238] == 0 ? "(zero, inconclusive)" : "YES if 1 read above" }]]
echo [format {RST_LPD_TOP writable:  %s} [expr { [rd 0xFF5E023C] == 3 ? "YES (level-hold)" : ([rd 0xFF5E023C] == 0 ? "NO or pulse-clear" : "PARTIAL") }]]
echo [format {RPU_GLBL_CNTL writable:%s} [expr { [rd 0xFF9A0000] != 0 ? "YES" : "NO" }]]
echo [format {RPU_0_CFG writable:    %s} [expr { [rd 0xFF9A0100] != 0 ? "YES" : "NO" }]]

echo ""
shutdown
