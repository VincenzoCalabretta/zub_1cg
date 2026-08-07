# write_test.tcl — verify that mww writes actually stick for OCM and CRL_APB.
#
# If CRL_APB writes fail silently (XPPU protection or STICKYERR), we'll see
# readback = 0 for RST_LPD_TOP and RPU registers.

init
after 500
targets uscale.axi

# Always clear STICKYERR first
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
echo "=== Baseline reads ==="
echo [format {OCM[0xFFFF3000] = 0x%08x  (baseline, uninitialized)} [rd 0xFFFF3000]]
echo [format {RPU_GLBL_CNTL   = 0x%08x  (expect 0x00000000 at power-on)} [rd 0xFF9A0000]]
echo [format {RPU_0_CFG        = 0x%08x  (expect 0x00000000 at power-on)} [rd 0xFF9A0100]]
echo [format {RST_LPD_TOP      = 0x%08x  (default: varies by boot mode)} [rd 0xFF5E023C]]
echo [format {RST_LPD_IOU2     = 0x%08x  (default: 0x0017FFFF)} [rd 0xFF5E0238]]
echo [format {CRL_APB_RESERVE  = 0x%08x  (0xFF5E0004 — read-only or unused)} [rd 0xFF5E0004]]

echo ""
echo "=== OCM write test: write 0xDEAD1234 to 0xFFFF3000 ==="
mww 0xFFFF3000 0xDEAD1234
after 5
echo [format {OCM[0xFFFF3000] readback = 0x%08x  (expect 0xDEAD1234)} [rd 0xFFFF3000]]

echo ""
echo "=== CRL_APB write test: try to write RST_LPD_TOP ==="
echo "(Put R5 in reset by setting bit0=1)"
mww 0xFF5E023C 0x00000001
after 20
echo [format {RST_LPD_TOP readback = 0x%08x  (expect 0x00000001 if write works)} [rd 0xFF5E023C]]

echo ""
echo "=== RPU write test: write RPU_0_CFG ==="
mww 0xFF9A0100 0x00000001
after 5
echo [format {RPU_0_CFG readback = 0x%08x  (expect 0x00000001 if write works)} [rd 0xFF9A0100]]

echo ""
echo "=== RPU write test: write RPU_GLBL_CNTL ==="
mww 0xFF9A0000 0x00000008
after 5
echo [format {RPU_GLBL_CNTL readback = 0x%08x  (expect 0x00000008 if write works)} [rd 0xFF9A0000]]

echo ""
echo "=== STICKYERR check after writes ==="
echo "Clearing STICKYERR again and doing clean reads..."
irscan uscale.tap 0x8
drscan uscale.tap 35 0xF8
runtest 50
irscan uscale.tap 0xA
drscan uscale.tap 35 0x280000102
drscan uscale.tap 35 0x07
runtest 100
uscale.axi arp_examine
targets uscale.axi
echo [format {RST_LPD_TOP (post-clear) = 0x%08x} [rd 0xFF5E023C]]
echo [format {RPU_0_CFG   (post-clear) = 0x%08x} [rd 0xFF9A0100]]
echo [format {RPU_GLBL_CNTL (post-cl)  = 0x%08x} [rd 0xFF9A0000]]
echo [format {OCM[0xFFFF3000] (confirm) = 0x%08x  (should still be 0xDEAD1234)} [rd 0xFFFF3000]]

echo ""
shutdown
