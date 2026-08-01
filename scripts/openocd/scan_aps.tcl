# scan_aps.tcl — read IDR register from each DAP AP (0–7) and report.
# Clears STICKYERR before each AP access to avoid corruption from a bad AP probe.

proc clear_stickyerr {} {
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
clear_stickyerr
uscale.axi arp_examine
targets uscale.axi

echo ""
echo "=== DAP AP scan — IDR at offset 0xFC ==="
for {set ap 0} {$ap < 8} {incr ap} {
    clear_stickyerr
    set err ""
    set idr 0
    if { [catch { set idr [uscale.dap apreg $ap 0xfc] } err] } {
        echo [format "  AP[%d] ERROR: %s" $ap $err]
    } else {
        # IDR class bits [16:13], type bits [3:0]
        set class [expr {($idr >> 13) & 0xF}]
        set type  [expr {$idr & 0xF}]
        set desc ""
        if { $class == 8 && $type == 4 } { set desc " (AXI-AP)" }
        if { $class == 8 && $type == 2 } { set desc " (APB-AP)" }
        if { $class == 8 && $type == 1 } { set desc " (AHB-AP)" }
        if { $idr == 0 }                 { set desc " (invalid/empty)" }
        echo [format "  AP\[%d\] IDR = 0x%08x%s" $ap $idr $desc]
    }
    clear_stickyerr
}

echo ""
echo "=== AP[1] probe — read base register at 0xF8 (APB-AP CFG/BASE) ==="
clear_stickyerr
catch {
    echo [format "  AP\[1\] BASE = 0x%08x" [uscale.dap apreg 1 0xf8]]
}
clear_stickyerr
catch {
    echo [format "  AP\[0\] BASE = 0x%08x" [uscale.dap apreg 0 0xf8]]
}

echo ""
shutdown
