# diag_canary.tcl — patch exception handlers to detect which one R5 is stuck in.
# Writes a small test program at 0xFFFFEF00 that stores 0xCAFEBABE to a canary
# address (0xFFFFFF00).  Each of the four main exception handlers (undef/swi/pabt/dabt)
# is redirected to a unique sub-program that writes a unique sentinel so we can
# tell WHICH exception fired.
#
# Canary map:
#   0xFFFFFF00 = 0xCAFE0001 → undef (bad instruction)
#   0xFFFFFF00 = 0xCAFE0002 → swi   (spurious syscall)
#   0xFFFFFF00 = 0xCAFE0003 → prefetch abort
#   0xFFFFFF00 = 0xCAFE0004 → data abort  ← most likely (BSS STR fault)
#   0xFFFFFF00 = 0xDEADDEAD → unchanged  (R5 not in any handler)

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
targets uscale.axi
clear_sticky
uscale.axi arp_examine
targets uscale.axi

# ── RPU control register snapshot ──────────────────────────────────────────────
echo ""
echo "=== RPU control registers ==="
clear_sticky
echo [format "  RST_LPD_TOP   @ 0xFF5E023C = 0x%08x  (bit1=RPU_R50: 0=released)" \
    [lindex [read_memory 0xFF5E023C 32 1] 0]]
clear_sticky
echo [format "  RPU_GLBL_CNTL @ 0xFF9A0000 = 0x%08x  (0x8 = SLSPLIT mode)" \
    [lindex [read_memory 0xFF9A0000 32 1] 0]]
clear_sticky
echo [format "  RPU_0_CFG     @ 0xFF9A0100 = 0x%08x  (0x1 = VINITHI high vectors)" \
    [lindex [read_memory 0xFF9A0100 32 1] 0]]
clear_sticky
echo [format "  CPU_R5_CTRL   @ 0xFF5E0090 = 0x%08x  (bit24=CLKACT)" \
    [lindex [read_memory 0xFF5E0090 32 1] 0]]

# ── Pre-patch snapshot ─────────────────────────────────────────────────────────
echo ""
echo "=== Pre-patch snapshot ==="
clear_sticky
echo [format "  BSS\[0\]   @ 0xFFFF2BCC = 0x%08x  (0=zeroed, DEADBEEF=loop not run)" \
    [lindex [read_memory 0xFFFF2BCC 32 1] 0]]
clear_sticky
echo [format "  UART1_CR  @ 0xFF010000 = 0x%08x  (0x14 = uart_init done)" \
    [lindex [read_memory 0xFF010000 32 1] 0]]
clear_sticky
echo [format "  tx_state  @ 0xFFFF2BC8 = 0x%08x  (0xF0F0F0F0=not started)" \
    [lindex [read_memory 0xFFFF2BC8 32 1] 0]]

echo ""
echo "  Exception handler instructions (expect 0xEAFFFFFE = B .):"
foreach {name addr} {
    undef  0xFFFF0228
    swi    0xFFFF022C
    pabt   0xFFFF0230
    dabt   0xFFFF0234
} {
    clear_sticky
    set v [lindex [read_memory $addr 32 1] 0]
    set tag [expr {$v == 0xEAFFFFFE ? "B. confirmed" : "UNEXPECTED"}]
    echo [format "    %-5s @ 0x%08x = 0x%08x  (%s)" $name $addr $v $tag]
}

# ── Write 4 small test programs at 0xFFFFEF00..0xFFFFEF7F ─────────────────────
# Each is 6 words (24 bytes):
#   +0: E59F0008  LDR r0, [pc, #8]   ; r0 = canary value
#   +4: E59F1008  LDR r1, [pc, #8]   ; r1 = canary address (0xFFFFFF00)
#   +8: E5810000  STR r0, [r1]        ; mem[0xFFFFFF00] = canary value
#   +C: EAFFFFFE  B .                 ; spin
#  +10: <canary>  .word canary_value
#  +14: FFFFFF00  .word 0xFFFFFF00

# Branch encodings: B <dst> from <src>
#   imm24 = (dst - (src + 8)) / 4
#   instruction = 0xEA000000 | (imm24 & 0xFFFFFF)
#
# undef  0xFFFF0228 -> 0xFFFFEF00: (0xEF00-0x0230)/4 = 0xECD0/4 = 0x3B34 -> 0xEA003B34
# swi    0xFFFF022C -> 0xFFFFEF20: (0xEF20-0x0234)/4 = 0xECEC/4 = 0x3B3B -> 0xEA003B3B
# pabt   0xFFFF0230 -> 0xFFFFEF40: (0xEF40-0x0238)/4 = 0xED08/4 = 0x3B42 -> 0xEA003B42
# dabt   0xFFFF0234 -> 0xFFFFEF60: (0xEF60-0x023C)/4 = 0xED24/4 = 0x3B49 -> 0xEA003B49

echo ""
echo "=== Writing test programs at 0xFFFFEF00-0xFFFFEF7F ==="
clear_sticky

# undef sentinel -> 0xFFFFEF00
mww 0xFFFFEF00 0xE59F0008
mww 0xFFFFEF04 0xE59F1008
mww 0xFFFFEF08 0xE5810000
mww 0xFFFFEF0C 0xEAFFFFFE
mww 0xFFFFEF10 0xCAFE0001
mww 0xFFFFEF14 0xFFFFFF00

# swi sentinel -> 0xFFFFEF20
mww 0xFFFFEF20 0xE59F0008
mww 0xFFFFEF24 0xE59F1008
mww 0xFFFFEF28 0xE5810000
mww 0xFFFFEF2C 0xEAFFFFFE
mww 0xFFFFEF30 0xCAFE0002
mww 0xFFFFEF34 0xFFFFFF00

# pabt sentinel -> 0xFFFFEF40
mww 0xFFFFEF40 0xE59F0008
mww 0xFFFFEF44 0xE59F1008
mww 0xFFFFEF48 0xE5810000
mww 0xFFFFEF4C 0xEAFFFFFE
mww 0xFFFFEF50 0xCAFE0003
mww 0xFFFFEF54 0xFFFFFF00

# dabt sentinel -> 0xFFFFEF60
mww 0xFFFFEF60 0xE59F0008
mww 0xFFFFEF64 0xE59F1008
mww 0xFFFFEF68 0xE5810000
mww 0xFFFFEF6C 0xEAFFFFFE
mww 0xFFFFEF70 0xCAFE0004
mww 0xFFFFEF74 0xFFFFFF00

# Pre-fill canary with sentinel
mww 0xFFFFFF00 0xDEADDEAD

clear_sticky
echo [format "  undef prog @ 0xFFFFEF10 = 0x%08x  (expect 0xCAFE0001)" \
    [lindex [read_memory 0xFFFFEF10 32 1] 0]]
clear_sticky
echo [format "  dabt  prog @ 0xFFFFEF70 = 0x%08x  (expect 0xCAFE0004)" \
    [lindex [read_memory 0xFFFFEF70 32 1] 0]]
clear_sticky
echo [format "  canary     @ 0xFFFFFF00 = 0x%08x  (expect 0xDEADDEAD)" \
    [lindex [read_memory 0xFFFFFF00 32 1] 0]]

# ── Patch exception handlers ───────────────────────────────────────────────────
echo ""
echo "=== Patching exception handlers ==="
clear_sticky
mww 0xFFFF0228 0xEA003B34   ;# undef -> 0xFFFFEF00
mww 0xFFFF022C 0xEA003B3B   ;# swi   -> 0xFFFFEF20
mww 0xFFFF0230 0xEA003B42   ;# pabt  -> 0xFFFFEF40
mww 0xFFFF0234 0xEA003B49   ;# dabt  -> 0xFFFFEF60
echo "  Handlers patched."

# ── Wait and read canary ───────────────────────────────────────────────────────
echo ""
echo "=== Waiting 500ms for R5 to react ==="
after 500

echo ""
echo "=== Post-patch canary ==="
clear_sticky
set canary [lindex [read_memory 0xFFFFFF00 32 1] 0]
echo [format "  canary @ 0xFFFFFF00 = 0x%08x" $canary]
clear_sticky
echo [format "  BSS\[0\] @ 0xFFFF2BCC = 0x%08x" [lindex [read_memory 0xFFFF2BCC 32 1] 0]]
clear_sticky
echo [format "  UART1_CR @ 0xFF010000 = 0x%08x" [lindex [read_memory 0xFF010000 32 1] 0]]

echo ""
switch $canary {
    0xCAFE0001 {
        echo "RESULT: UNDEFINED INSTRUCTION exception"
        echo "  -> R5 executed an invalid opcode during startup"
    }
    0xCAFE0002 {
        echo "RESULT: SWI/SVC exception"
        echo "  -> Unexpected syscall during startup"
    }
    0xCAFE0003 {
        echo "RESULT: PREFETCH ABORT exception"
        echo "  -> Instruction fetch from bad address"
    }
    0xCAFE0004 {
        echo "RESULT: DATA ABORT exception  <-- BSS STR fault"
        echo "  -> STR to OCM (0xFFFF2BCC) causes data abort"
        echo "  -> Next: check MPU/XMPU config or OCM accessibility from R5"
    }
    0xDEADDEAD {
        echo "RESULT: CANARY UNCHANGED"
        echo "  -> R5 is NOT in undef/swi/pabt/dabt handler"
        echo "  -> Possible: R5 not running (clock/reset issue)"
        echo "     or stuck before mode-switch code"
        echo "     or BSS loop is running but OCM stores don't work via R5"
    }
    default {
        echo [format "RESULT: UNEXPECTED canary value 0x%08x" $canary]
    }
}

echo ""
shutdown
