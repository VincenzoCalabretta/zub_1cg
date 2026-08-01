# diag_r5.tcl — probe R5 state without resetting it.
# Run after load_r5.tcl to diagnose why CoreSight fails.
#
# Usage:
#   openocd -f scripts/openocd/aes_zub.cfg -f scripts/openocd/diag_r5.tcl

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
poll off
after 500
clear_stickyerr

targets uscale.axi

echo ""
echo "=== RPU registers ==="
set rst   [lindex [read_memory 0xFF5E023C 32 1] 0]
set glbl  [lindex [read_memory 0xFF9A0000 32 1] 0]
set cfg0  [lindex [read_memory 0xFF9A0100 32 1] 0]
echo [format "RST_LPD_TOP   = 0x%08x  (bit0=R5-0-reset, bit1=R5-1-reset)" $rst]
echo [format "RPU_GLBL_CNTL = 0x%08x  (bit3=SLSPLIT)" $glbl]
echo [format "RPU_0_CFG     = 0x%08x  (bit0=VINITHI)" $cfg0]

echo ""
echo "=== UART1 registers ==="
set uart_cr [lindex [read_memory 0xFF010000 32 1] 0]
set uart_mr [lindex [read_memory 0xFF010004 32 1] 0]
set uart_sr [lindex [read_memory 0xFF01002C 32 1] 0]
set uart_bg [lindex [read_memory 0xFF010018 32 1] 0]
set uart_bd [lindex [read_memory 0xFF010034 32 1] 0]
echo [format "UART1_CR   = 0x%08x  (expect 0x14: TX+RX enabled)" $uart_cr]
echo [format "UART1_MR   = 0x%08x  (expect 0x20: 8N1)" $uart_mr]
echo [format "UART1_SR   = 0x%08x  (bit3=TX_EMPTY)" $uart_sr]
echo [format "UART1_BAUD = CD=0x%x BDIV=0x%x" $uart_bg $uart_bd]

echo ""
echo "=== OCM vector table ==="
for {set i 0} {$i < 16} {incr i} {
    set addr [expr {0xFFFF0000 + $i * 4}]
    set val  [lindex [read_memory $addr 32 1] 0]
    echo [format "  0x%08x = 0x%08x" $addr $val]
}

echo ""
echo "=== APB-AP (AP 1) registers ==="
clear_stickyerr
catch {
    set idr [uscale.dap apreg 1 0xfc]
    echo [format "  APB-AP IDR (0xFC) = 0x%08x  (expect 0x44770002)" $idr]
} err
if { $err ne "" } { echo "  IDR read failed: $err"; clear_stickyerr }

catch {
    set csw [uscale.dap apreg 1 0x0]
    echo [format "  APB-AP CSW (0x00) = 0x%08x" $csw]
} err
if { $err ne "" } { echo "  CSW read failed: $err"; clear_stickyerr }

catch {
    set base [uscale.dap apreg 1 0xf8]
    echo [format "  APB-AP BASE(0xF8) = 0x%08x" $base]
} err
if { $err ne "" } { echo "  BASE read failed: $err"; clear_stickyerr }

echo ""
echo "=== APB bus access: R5-0 debug at 0xFE810000 ==="
# Write CSW: 32-bit access, AddrInc=0
clear_stickyerr
catch { uscale.dap apreg 1 0x0 0x00000002 } err
if { $err ne "" } { echo "  CSW write failed: $err"; clear_stickyerr }

# Write TAR = 0xFE810000
catch { uscale.dap apreg 1 0x4 0xFE810000 } err
if { $err ne "" } { echo "  TAR write failed: $err"; clear_stickyerr }

# Read DRW — triggers APB access at 0xFE810000 (DIDR of R5-0 debug)
catch {
    set drw [uscale.dap apreg 1 0xC]
    echo [format "  R5-0 DIDR via APB = 0x%08x  (Cortex-R5 = 0x35159145 or similar)" $drw]
} err
if { $err ne "" } {
    echo "  APB read at 0xFE810000 FAILED: $err"
    echo "  → R5 debug domain not accessible (lockup or power gate)"
}
clear_stickyerr

echo ""
echo "=== AXI read at 0xFE810000 ==="
clear_stickyerr
targets uscale.axi
catch {
    set axi_val [lindex [read_memory 0xFE810000 32 1] 0]
    echo [format "  AXI 0xFE810000 = 0x%08x" $axi_val]
} err
if { $err ne "" } {
    echo "  AXI read at 0xFE810000 FAILED: $err"
}
clear_stickyerr

echo ""
shutdown
