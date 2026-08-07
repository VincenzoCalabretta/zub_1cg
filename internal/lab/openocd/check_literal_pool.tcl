# check_literal_pool.tcl — verify reset_handler literal pool and detect R5 crash vector.
init
after 300
targets uscale.axi

if { [catch { read_memory 0xFF9A0000 32 1 } err] } {
    echo "ERROR: AXI blocked."
    shutdown
    return
}

echo ""
echo "=== reset_handler literal pool (0xFFFF023C..0xFFFF025F) ==="
echo "(Expected: _stack_top=0xFFFFFFFC  sizes=0x400,0x200,0x200,0x200,0x400,0x1000)"
echo "(Expected: _bss_start=0xFFFF2BCC  _bss_end=0xFFFF42C8)"
set names {_stack_top _irq_stk _fiq_stk _abt_stk _und_stk _sys_stk _svc_stk _bss_start _bss_end}
for {set i 0} {$i < 9} {incr i} {
    set a [expr {0xFFFF023C + $i*4}]
    set v [lindex [read_memory $a 32 1] 0]
    set n [lindex $names $i]
    echo [format {0x%08x = 0x%08x  (%s)} $a $v $n]
}

echo ""
echo "=== __tx_reserved_handler at 0xFFFF0238 (expect B 0xFFFF0238 = 0xeafffffe) ==="
set v [lindex [read_memory 0xFFFF0238 32 1] 0]
echo [format {0xFFFF0238 = 0x%08x} $v]

echo ""
echo "=== __tx_abort_handler (spin loop candidate at 0xFFFF0234) ==="
set v [lindex [read_memory 0xFFFF0234 32 1] 0]
echo [format {0xFFFF0234 = 0x%08x  (__tx_abort_handler, expect 0xeafffffe)} $v]

echo ""
echo "=== Checking if R5 is stuck in abort handler ==="
echo "=== (Read abort count register via JTAG not possible without R5 debug target) ==="
echo "=== Indirect test: modify OCM marker word and see if abort handler overwrites it ==="
set marker_addr 0xFFFF2BB0
set marker_val_before [lindex [read_memory $marker_addr 32 1] 0]
echo [format {marker @ 0x%08x = 0x%08x  (before)} $marker_addr $marker_val_before]
mww $marker_addr 0xCAFEBABE
after 200
set marker_val_after [lindex [read_memory $marker_addr 32 1] 0]
echo [format {marker @ 0x%08x = 0x%08x  (after 200ms)} $marker_addr $marker_val_after]
if { $marker_val_after == 0xCAFEBABE } {
    echo "  → Marker unchanged: R5 is NOT writing to that address (stuck in abort loop or not running)."
} else {
    echo "  → Marker CHANGED: R5 IS executing and wrote to that address!"
}

echo ""
shutdown
