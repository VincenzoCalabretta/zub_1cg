# check_ocm.tcl — verify ELF is properly loaded in OCM and R5 state.
init
after 300
targets uscale.axi

if { [catch { read_memory 0xFF9A0000 32 1 } err] } {
    echo "ERROR: AXI blocked. Power-cycle and re-run load_r5.tcl first."
    shutdown
    return
}

echo ""
echo "=== Vector table + literal pool (0xFFFF0000..0xFFFF003F) ==="
for {set i 0} {$i < 16} {incr i} {
    set a [expr {0xFFFF0000 + $i*4}]
    set v [lindex [read_memory $a 32 1] 0]
    echo [format {0x%08x = 0x%08x} $a $v]
}

echo ""
echo "=== .text start (0xFFFF0040..0xFFFF006F) ==="
for {set i 0} {$i < 12} {incr i} {
    set a [expr {0xFFFF0040 + $i*4}]
    set v [lindex [read_memory $a 32 1] 0]
    echo [format {0x%08x = 0x%08x} $a $v]
}

echo ""
echo "=== BSS region start (0xFFFF2BCC..0xFFFF2BDF) ==="
for {set i 0} {$i < 5} {incr i} {
    set a [expr {0xFFFF2BCC + $i*4}]
    set v [lindex [read_memory $a 32 1] 0]
    echo [format {0x%08x = 0x%08x} $a $v]
}

echo ""
echo "=== RPU and RST state ==="
echo [format {RPU_GLBL_CNTL  @ 0xFF9A0000 = 0x%08x} [lindex [read_memory 0xFF9A0000 32 1] 0]]
echo [format {RPU_0_CFG      @ 0xFF9A0100 = 0x%08x  (bit0=VINITHI)} [lindex [read_memory 0xFF9A0100 32 1] 0]]
echo [format {RST_LPD_TOP    @ 0xFF5E023C = 0x%08x  (bit0=0 means R5 running)} [lindex [read_memory 0xFF5E023C 32 1] 0]]

echo ""
shutdown
