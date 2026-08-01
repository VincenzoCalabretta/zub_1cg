# halt_r5_inspect.tcl — load ELF, start R5, halt it via debug target, dump PC+regs.
#
# This is the first run with SLSPLIT=1 (split mode) in RPU_GLBL_CNTL.
# Previously bit[3] was never set, leaving R5 in lock-step mode with R5-1 in
# module reset — which may have prevented R5-0 from executing any instructions.

set ELF "bazel-bin/apps/rpu/hello_world/hello_world"

if {![file exists $ELF]} {
    puts "ERROR: $ELF not found."
    shutdown
    return
}

init
after 500
targets uscale.axi

if { [catch { read_memory 0xFF9A0000 32 1 } probe_err] } {
    echo "AXI inaccessible — power-cycle and retry."
    shutdown
    return
}

proc rd {a} { return [lindex [read_memory $a 32 1] 0] }

echo ""
echo "=== RPU state before config ==="
echo [format {RPU_GLBL_CNTL (0xFF9A0000) = 0x%08x  (bit3=SLSPLIT: 0=lock-step 1=split)} [rd 0xFF9A0000]]
echo [format {RPU_0_CFG     (0xFF9A0100) = 0x%08x  (bit0=VINITHI)} [rd 0xFF9A0100]]
echo [format {RST_LPD_TOP   (0xFF5E023C) = 0x%08x  (bit0=R5-0 reset)} [rd 0xFF5E023C]]

echo ""
echo "=== Step 1: RPU config — split mode + VINITHI=1 ==="
set rpu [rd 0xFF9A0000]
mww 0xFF9A0000 [expr {($rpu & ~0x1) | 0x8}]
echo [format {RPU_GLBL_CNTL written = 0x%08x  (SLSPLIT=1)} [rd 0xFF9A0000]]
mww 0xFF9A0100 0x00000001
echo [format {RPU_0_CFG written     = 0x%08x  (VINITHI=1)} [rd 0xFF9A0100]]

echo ""
echo "=== Step 2: UART1 peripheral init ==="
mww 0xFF5E0238 0x00000000
after 5
mww 0xFF5E0074 0x01011800
mww 0xFF01000C 0xFFFFFFFF
mww 0xFF010000 0x00000003
after 5
mww 0xFF010004 0x00000020
mww 0xFF010018 43
mww 0xFF010034 13
mww 0xFF010000 0x00000014
after 2

echo ""
echo "=== Step 3: Load ELF into OCM ==="
set rst [rd 0xFF5E023C]
echo [format {RST_LPD_TOP = 0x%08x} $rst]

load_image $ELF 0 elf

set w0 [rd 0xFFFF0000]
echo [format {OCM[0xFFFF0000] = 0x%08x  (expect 0xe59ff018)} $w0]
if { $w0 == 0 || $w0 == 0xFFFFFFFF } {
    echo "ERROR: OCM load failed."
    shutdown
    return
}

echo ""
echo "=== Step 4: Write BSS sentinel 0xAABBCCDD ==="
mww 0xFFFF2BCC 0xAABBCCDD
echo [format {BSS sentinel: 0x%08x} [rd 0xFFFF2BCC]]

echo ""
echo "=== Step 5: Examine R5 debug target ==="
if { [catch { uscale.r5_0 arp_examine } err] } {
    echo "R5 debug target examine failed: $err"
    echo "(Continuing with release and AXI polling)"
    set r5_debug 0
} else {
    echo "R5 debug target examined OK."
    set r5_debug 1
}

echo ""
echo "=== Step 6: Release R5-0 from module reset ==="
set rst_run [expr {($rst & ~0x1) | 0x2}]
mww 0xFF5E023C $rst_run
echo [format {RST_LPD_TOP = 0x%08x  (bit0=0 = R5 running)} $rst_run]
after 20

echo ""
echo "=== Step 7: Poll BSS every 10ms for 200ms ==="
set found 0
for {set i 1} {$i <= 20} {incr i} {
    after 10
    set b [rd 0xFFFF2BCC]
    if { $b != 0xAABBCCDD } {
        echo [format {  +%dms: BSS changed to 0x%08x — R5 IS RUNNING!} [expr {$i*10}] $b]
        set found 1
        break
    }
}

echo ""
echo "=== Step 8: Halt R5 via debug target and read registers ==="
if { $r5_debug } {
    if { [catch { uscale.r5_0 halt 500 } herr] } {
        echo "Halt failed: $herr"
    } else {
        echo "R5 halted."
        targets uscale.r5_0
        if { [catch { reg } reg_err] } {
            echo "reg dump failed: $reg_err"
        } else {
            reg
        }
    }
}

echo ""
echo "=== Step 9: AXI post-run snapshot ==="
targets uscale.axi
echo [format {BSS[0xFFFF2BCC] = 0x%08x  (0=bss_loop done, AABBCCDD=stuck)} [rd 0xFFFF2BCC]]
echo [format {UART1_CR        = 0x%08x  (0x114=uart_init ran)} [rd 0xFF010000]]
echo [format {UART1_SR        = 0x%08x  (bit3=TX_EMPTY)} [rd 0xFF01002C]]
echo [format {RPU_GLBL_CNTL   = 0x%08x} [rd 0xFF9A0000]]
echo [format {RST_LPD_TOP     = 0x%08x} [rd 0xFF5E023C]]

if { !$found } {
    echo ""
    echo "CONCLUSION: BSS unchanged — R5 still stuck before bss_loop."
    echo "R5 debug halt above should show where PC is."
} else {
    echo ""
    echo "CONCLUSION: R5 is RUNNING (BSS changed). Check UART for output."
}

echo ""
shutdown
