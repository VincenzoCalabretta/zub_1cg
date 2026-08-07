# check_stickyerr.tcl — verify whether writes to CRL_APB/RPU trigger STICKYERR.
#
# If writes to RST_LPD_TOP or RPU_0_CFG generate AXI SLVERR (XPPU/protection),
# the DAP sets STICKYERR.  OpenOCD does NOT raise a Tcl error for SLVERR; it
# silently returns 0 for reads and drops writes.  Reading CTRL/STAT via raw
# DPACC exposes the STICKYERR bit (bit 5 of the 32-bit CTRL/STAT value).

proc read_ctrlstat {} {
    # DP CTRL/STAT: A[3:2]=01, RnW=1 → 35-bit DPACC request = 0x3
    # First scan posts the read request; second scan retrieves the result.
    irscan uscale.tap 0xA
    drscan uscale.tap 35 0x3   ;# post read request (previous result in output)
    set raw [drscan uscale.tap 35 0x3]   ;# retrieve CTRL/STAT
    # raw is a 35-bit hex integer; CTRL/STAT data = raw >> 3
    set raw_int [expr {[lindex $raw 0]}]
    return [expr {($raw_int >> 3) & 0xFFFFFFFF}]
}

proc clear_stickyerr {} {
    irscan uscale.tap 0x8
    drscan uscale.tap 35 0xF8    ;# ABORT: all sticky error bits
    runtest 50
    irscan uscale.tap 0xA
    drscan uscale.tap 35 0x280000102  ;# CTRL/STAT: CDBGPWRUPREQ|CSYSPWRUPREQ|STKERRCLR
    drscan uscale.tap 35 0x07
    runtest 100
}

init
after 500
targets uscale.axi

clear_stickyerr
uscale.axi arp_examine
targets uscale.axi

echo ""
echo "=== CTRL/STAT baseline (expect STICKYERR=0) ==="
set cs [read_ctrlstat]
echo [format {CTRL/STAT = 0x%08x  (bit5=STICKYERR, bit28=CDBGPWRUPACK, bit30=CSYSPWRUPACK)} $cs]
if { [expr {$cs & 0x20}] } {
    echo "STICKYERR is SET at baseline — JTAG channel has stale errors."
} else {
    echo "STICKYERR clear at baseline. OK."
}

echo ""
echo "=== Writing to OCM (should NOT set STICKYERR) ==="
mww 0xFFFF3000 0xBEEF0001
set cs_ocm [read_ctrlstat]
echo [format {CTRL/STAT after OCM write = 0x%08x  (STICKYERR=[expr {($cs_ocm>>5)&1}])} $cs_ocm]
if { [expr {$cs_ocm & 0x20}] } {
    echo "STICKYERR SET after OCM write — unexpected, possible AXI issue."
    clear_stickyerr
    uscale.axi arp_examine
    targets uscale.axi
} else {
    echo "STICKYERR clear after OCM write — OCM is accessible."
}

echo ""
echo "=== Writing to RST_LPD_TOP 0xFF5E023C ==="
mww 0xFF5E023C 0x00000001
set cs_rst [read_ctrlstat]
echo [format {CTRL/STAT after RST write  = 0x%08x} $cs_rst]
if { [expr {$cs_rst & 0x20}] } {
    echo "STICKYERR SET — RST_LPD_TOP write generated SLVERR (XPPU-protected)."
} else {
    echo "STICKYERR clear — RST_LPD_TOP write was accepted (or silently ignored)."
    echo [format {RST_LPD_TOP readback = 0x%08x} [lindex [read_memory 0xFF5E023C 32 1] 0]]
}

clear_stickyerr
uscale.axi arp_examine
targets uscale.axi

echo ""
echo "=== Writing to RPU_0_CFG 0xFF9A0100 ==="
mww 0xFF9A0100 0x00000001
set cs_rpu [read_ctrlstat]
echo [format {CTRL/STAT after RPU write  = 0x%08x} $cs_rpu]
if { [expr {$cs_rpu & 0x20}] } {
    echo "STICKYERR SET — RPU_0_CFG write generated SLVERR (XPPU-protected)."
} else {
    echo "STICKYERR clear — RPU_0_CFG write was accepted."
    echo [format {RPU_0_CFG readback = 0x%08x} [lindex [read_memory 0xFF9A0100 32 1] 0]]
}

echo ""
shutdown
