# diag_romtable.tcl — read APB-AP ROM table to find correct R5-0 debug base.
# APB-AP BASE = 0x80000003 → ROM table at 0x80000000 in AP address space.
#
# Usage:
#   openocd -f scripts/openocd/aes_zub.cfg -f scripts/openocd/diag_romtable.tcl

proc clear_stickyerr {} {
    irscan uscale.tap 0x8
    drscan uscale.tap 35 0xF8
    runtest 100
    irscan uscale.tap 0xA
    drscan uscale.tap 35 0x280000102
    drscan uscale.tap 35 0x07
    runtest 200
}

proc apb_read {addr} {
    uscale.dap apreg 1 0x0 0x00000002
    uscale.dap apreg 1 0x4 $addr
    return [uscale.dap apreg 1 0xC]
}

init
poll off
after 500
clear_stickyerr

echo ""
echo "=== APB-AP info ==="
catch { echo [format "  IDR  = 0x%08x" [uscale.dap apreg 1 0xfc]] }
catch { echo [format "  BASE = 0x%08x" [uscale.dap apreg 1 0xf8]] }
clear_stickyerr

echo ""
echo "=== ROM table at APB 0x80000000 ==="
for {set i 0} {$i < 16} {incr i} {
    set raddr [expr {0x80000000 + $i * 4}]
    clear_stickyerr
    if { [catch {
        set val [apb_read $raddr]
        set present [expr {$val & 0x1}]
        if { $present } {
            set raw_off [expr {$val & 0xFFFFF000}]
            # sign-extend 32-bit offset (already signed in Tcl)
            set comp [expr {(0x80000000 + $raw_off) & 0xFFFFFFFF}]
            echo [format "  \[%2d\] 0x%08x → component at 0x%08x  (raw=0x%08x)" \
                $i $raddr $comp $val]
        } else {
            echo [format "  \[%2d\] 0x%08x = 0x%08x  (not present / end)" $i $raddr $val]
            if { $val == 0 } { break }
        }
    } err] } {
        echo [format "  \[%2d\] 0x%08x  ERROR: %s" $i $raddr $err]
        clear_stickyerr
    }
}

echo ""
echo "=== Probe candidate debug bases ==="
foreach base {0x80010000 0x80110000 0x80210000 0x80310000 0x80410000} {
    clear_stickyerr
    if { [catch {
        set didr [apb_read $base]
        echo [format "  APB 0x%08x DIDR = 0x%08x" $base $didr]
    } err] } {
        echo [format "  APB 0x%08x  FAILED" $base]
        clear_stickyerr
    }
}

echo ""
echo "=== Compare: probe 0xFE810000 ==="
clear_stickyerr
if { [catch { echo [format "  APB 0xFE810000 = 0x%08x" [apb_read 0xFE810000]] } err] } {
    echo "  APB 0xFE810000 FAILED (expected if wrong address)"
}
clear_stickyerr

echo ""
shutdown
