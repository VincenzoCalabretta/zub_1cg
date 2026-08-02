# diag_r5_debug.tcl — find R5-0 debug registers via APB-AP mem_ap.
# APB-AP is AP[1] (IDR=0x44770002 confirmed by scan_aps.tcl).
# We create a second mem_ap target on AP[1] to read APB addresses.

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

# Bring up AXI mem_ap first
targets uscale.axi
clear_sticky
uscale.axi arp_examine
targets uscale.axi

# Bring up APB mem_ap (AP[1])
clear_sticky
uscale.apb arp_examine
targets uscale.apb

echo ""
echo "=== APB-AP probe: candidate R5-0 debug registers ==="
echo "  ARM Cortex-R5 DBGDIDR: expect 0x55014440 (version=1, debug=5) or similar"

proc rd_apb {a} { return [lindex [read_memory $a 32 1] 0] }

# ZU+ LPD APB debug addresses for R5-0:
# Try 0xFE800000, 0xFE810000, 0xFEB00000 — TRM varies by version
foreach base {0xFE800000 0xFE810000 0xFEB00000 0xFE000000} {
    clear_sticky
    set val [rd_apb $base]
    set desc ""
    if { ($val & 0xFFF0FFFF) == 0x55000000 } { set desc " ← Cortex-R5 DBGDIDR!" }
    if { ($val & 0xFFF0FFFF) == 0x15000000 } { set desc " ← Cortex-R5 DBGDIDR!" }
    echo [format {  0x%08x = 0x%08x%s} $base $val $desc]
}

echo ""
echo "=== BSS and UART sanity check via AXI ==="
targets uscale.axi
clear_sticky

proc rd_axi {a} { return [lindex [read_memory $a 32 1] 0] }
echo [format {  BSS[0] @ 0xFFFF2BCC = 0x%08x  (0=zeroed, 0xdeadbeef=not zeroed)} [rd_axi 0xFFFF2BCC]]
echo [format {  UART1_CR  @ 0xFF010000 = 0x%08x} [rd_axi 0xFF010000]]

echo ""
shutdown
