# load_r5.tcl — load ELF into OCM and run on R5-0, without FSBL or A53 debug.
#
# Handles two board states automatically:
#
#   A) Fresh power-cycle (R5 in module reset):
#      RST_LPD_TOP bit[0]=1.  Write VINITHI=1 (before SLSPLIT change) →
#      RPU_GLBL_CNTL split mode → UART init → ELF load → release R5-0 →
#      CoreSight examine/halt/redirect (non-fatal) → resume.
#
#   B) R5 already running (no power-cycle):
#      RST_LPD_TOP bit[0]=0.  First try software reset (write bit[0]=1 to
#      RST_LPD_TOP to put R5 back in reset, then run full Path A sequence).
#      If RST_LPD_TOP write is blocked by XPPU, fall back to CoreSight halt.
#
# Key: uscale.axi has no -defer-examine (reverted — defer caused STICKYERR in
#      jtag arp_init). uscale.r5_0 has -defer-examine; we examine only after
#      confirming R5 is out of module reset and debug domain is live.
#
# Usage:
#   openocd -f scripts/openocd/aes_zub.cfg -f scripts/openocd/load_r5.tcl

# ELF can be pre-set via: openocd --command "set ELF /abs/path" -f load_r5.tcl
if {![info exists ELF]} {
    set ELF "bazel-bin/apps/rpu/hello_world/hello_world"
}

if {![file exists $ELF]} {
    puts "ERROR: $ELF not found. Run: bazel build --config=rpu //apps/rpu/hello_world"
    shutdown
    return
}

proc clear_stickyerr {} {
    irscan uscale.tap 0x8
    drscan uscale.tap 35 0xF8
    runtest 100
    irscan uscale.tap 0xA
    drscan uscale.tap 35 0x280000102
    drscan uscale.tap 35 0x07
    runtest 200
}

proc setup_uart {} {
    # UART0 (0xFF000000) is the console on ZUBoard 1CG: MIO pins 10/11
    # (L3_SEL=6) route UART0 RX/TX to ttyUSB1 via the FT2232H.
    # UART0_REF_CTRL (0xFF5E0074): SRCSEL=IOPLL, DIV1=1, DIV0=24 → 69.44 MHz
    # at power-on (IOPLL FBDIV=50 × 33.333 MHz / 24 = 69.44 MHz).
    targets uscale.axi

    # Enable UART0 reference clock: SRCSEL=IOPLL(0), DIV1=1, DIV0=15, CLKACT=1
    # → 1500 MHz / 1 / 15 = 100 MHz (after psu_init sets IOPLL FBDIV=45).
    mww 0xFF5E0074 0x01010F00

    # MIO_PIN_10 = UART0_RX, MIO_PIN_11 = UART0_TX (L3_SEL=6, LVCMOS1.8V)
    mww 0xFF180028 0x000000C0
    mww 0xFF18002C 0x000000C0
    after 5

    # UART0 (0xFF000000): reset, configure 8N1 at 115200 baud, enable TX
    mww 0xFF00000C 0xFFFFFFFF
    mww 0xFF000000 0x00000003
    after 5
    mww 0xFF000004 0x00000020
    mww 0xFF000018 62
    mww 0xFF000034 13
    mww 0xFF000000 0x00000014
    after 2
    foreach b {0x42 0x4F 0x4F 0x54 0x0D 0x0A} { mww 0xFF000030 $b }
    after 20
    set sr [lindex [read_memory 0xFF00002C 32 1] 0]
    if { [expr {$sr & 0x08}] } {
        echo "Boot marker 'BOOT\\r\\n' sent on UART0 (/dev/ttyUSB1)."
    } else {
        echo [format "WARNING: UART0_SR=0x%08x TX_EMPTY not set" $sr]
    }
}

proc load_elf {elf} {
    targets uscale.axi
    echo "Loading $elf into OCM at 0xFFFF0000..."
    load_image $elf 0 elf
    set word0 [lindex [read_memory 0xFFFF0000 32 1] 0]
    echo [format {OCM[0xFFFF0000] = 0x%08x  (expect 0xe59ff018)} $word0]
    if { $word0 == 0x00000000 || $word0 == 0xFFFFFFFF } {
        echo "ERROR: OCM readback bad."
        return 0
    }
    return 1
}

proc examine_and_run {} {
    # Examine R5-0 CoreSight (APB-AP 1) and redirect PC.
    # Call only when debug domain is live (R5 out of module reset).
    clear_stickyerr
    echo "Examining R5-0 CoreSight (APB-AP 1, 0xFE810000)..."
    targets uscale.r5_0
    if { [catch { uscale.r5_0 arp_examine } e] } {
        echo "ERROR: CoreSight examine failed: $e"
        clear_stickyerr
        return 0
    }
    echo "R5-0 examine OK — halting..."
    halt
    if { [catch { wait_halt 3000 } e] } {
        echo "ERROR: wait_halt: $e"
        clear_stickyerr
        return 0
    }
    echo "R5-0 halted."
    reg pc 0xffff0000
    reg cpsr 0xd3
    echo "PC → 0xFFFF0000, CPSR → 0xD3."
    resume
    after 1000   ;# wait for R5 to start UART output before shutdown
    echo "R5-0 executing from OCM."
    return 1
}

# Full init sequence used by both Path A and software-reset Path B.
proc full_init_from_reset {} {
    global ELF

    # Reset the complete RPU group before changing its static mode straps.
    # SLSPLIT is sampled as the processor group leaves reset; resetting only
    # the individual cores is insufficient after a prior lock-step boot.
    # Keep both core resets asserted while releasing the power-island and
    # AMBA resets, then program the straps below.
    set rst [lindex [read_memory 0xFF5E023C 32 1] 0]
    mww 0xFF5E023C [expr {$rst | 0x17}]
    after 50
    mww 0xFF5E023C [expr {($rst | 0x3) & ~0x14}]
    after 50
    echo [format "RST_LPD_TOP = 0x%08x  (both R5s in reset)" \
        [lindex [read_memory 0xFF5E023C 32 1] 0]]

    # RPU_0_CFG: NCPUHALT=1 (bit 0) and VINITHI=1 (bit 2).  VINITHI selects
    # the OCM high-vector reset address, 0xFFFF0000, where this ELF is loaded.
    # Write it before SLSPLIT: mode straps are sampled as the RPU leaves reset.
    mww 0xFF9A0100 0x00000005
    echo [format "RPU_0_CFG     = 0x%08x  (NCPUHALT + VINITHI high vectors)" \
        [lindex [read_memory 0xFF9A0100 32 1] 0]]

    # RPU_GLBL_CNTL=0x8: SLSPLIT=1, SLCLAMP=0
    mww 0xFF9A0000 0x00000008
    set glbl [lindex [read_memory 0xFF9A0000 32 1] 0]
    echo [format "RPU_GLBL_CNTL = 0x%08x  (expect 0x00000008)" $glbl]
    if { $glbl != 0x8 } {
        echo "ERROR: RPU_GLBL_CNTL write dropped — XPPU locked."
        echo "Power-cycle the board and re-run."
        return 0
    }

    # Enable and source the R5 CPU clock.  Normally this is done by the
    # Vitis-generated psu_init script, but hw_server can start without finding
    # the FTDI target; in that case psu_init is a no-op and a released R5 has
    # no clock.  The mask/value pair below is from board/zub_1cg/psu_init.tcl:
    # CLKACT=1, DIVISOR0=3, SRCSEL=IOPLL.
    set r5_clk [lindex [read_memory 0xFF5E0090 32 1] 0]
    set r5_clk [expr {($r5_clk & ~0x01003F07) | 0x01000302}]
    mww 0xFF5E0090 $r5_clk
    set r5_clk [lindex [read_memory 0xFF5E0090 32 1] 0]
    echo [format {CPU_R5_CTRL   = 0x%08x  (CLKACT bit24 must be set)} $r5_clk]
    if { ($r5_clk & 0x01000000) == 0 } {
        echo "ERROR: R5 CPU clock did not enable. Power-cycle the board and re-run."
        return 0
    }

    setup_uart
    if { ![load_elf $ELF] } { return 0 }

    # Write a sentinel to a binary-independent OCM location before R5 release.
    # startup.S (compiled with -DR5_STARTUP_TRACE=1) overwrites this with
    # 0x52535431 ("RST1") as its very first instruction — before any stack
    # setup or BSS zeroing.  If the sentinel survives 2 s of R5 running,
    # reset_handler never fetched from 0xFFFF0000 (VINITHI not effective, or
    # R5 stalled before the first instruction).
    mww 0xFFFFFF00 0xDEADDEAD
    echo "DIAG: sentinel 0xDEADDEAD written to 0xFFFFFF00 before R5 release"

    # Release R5-0; R5-1 stays in reset (split mode, no partner fault)
    set rst_now [lindex [read_memory 0xFF5E023C 32 1] 0]
    set rst_run [expr {($rst_now & ~0x1) | 0x2}]
    mww 0xFF5E023C $rst_run
    echo [format "RST_LPD_TOP = 0x%08x  (R5-0 released, R5-1 in reset)" $rst_run]
    echo "R5-0 released — VINITHI=1 → should fetch from 0xFFFF0000 (OCM)."

    after 500
    clear_stickyerr
    if { ![examine_and_run] } {
        echo "CoreSight halt failed — UART output is ground truth."
    }

    # Diagnostic: read UART0 registers via AXI ~2 s after R5 runs.
    # UART0_CR should reflect R5's uart_init result.
    # BAUDGEN/BAUDDIV change from 62/13 (setup_uart) to 124/6 (_compute_baud) if
    # R5 reached uart_init.  Inject "PING\r\n" — visible on serial iff UART0 TX works.
    targets uscale.axi
    clear_stickyerr
    after 2000
    if { [catch {
        set cr   [lindex [read_memory 0xFF000000 32 1] 0]
        set mr   [lindex [read_memory 0xFF000004 32 1] 0]
        set bg   [lindex [read_memory 0xFF000018 32 1] 0]
        set bd   [lindex [read_memory 0xFF000034 32 1] 0]
        set sr   [lindex [read_memory 0xFF00002C 32 1] 0]
        set reset_marker [lindex [read_memory 0xFFFFFF00 32 1] 0]
        echo [format "DIAG: UART0_CR=0x%08x MR=0x%08x BAUDGEN=%d BAUDDIV=%d SR=0x%08x" \
              $cr $mr $bg $bd $sr]
        # Braces are required here: Tcl evaluates square brackets in double-
        # quoted strings as commands, which used to hide this decisive check.
        echo [format {DIAG: reset marker[0xFFFFFF00]=0x%08x  (52535431 = startup traced; deaddead = reset_handler never ran)} \
              $reset_marker]
        # Inject "PING\r\n" — newline-terminated so serial watch prints it
        if { ($sr & 0x10) == 0 } {
            foreach b {0x50 0x49 0x4E 0x47 0x0D 0x0A} { mww 0xFF000030 $b }
            after 10
            echo "DIAG: Injected PING\\r\\n via AXI FIFO"
        } else {
            echo "DIAG: TX FIFO full — skipping PING injection"
        }
    } e] } {
        echo "DIAG WARNING: UART0 read failed: $e"
    }
    return 1
}

# ── Step 1: Connect and examine AXI ──────────────────────────────────────────
# psu_init_run.tcl initializes the DAP before it configures clocks and MIO.
# Avoid a second init when the scripts are chained by zub_ctl.
if {![info exists zub_psu_initialized]} {
    init
}
poll off        ;# stop background polling — prevents STICKYERR retriggering
after 500
clear_stickyerr ;# clear any residual STICKYERR before first AXI access

targets uscale.axi

# ── Step 2: Probe board state via RST_LPD_TOP ─────────────────────────────────
if { [catch { set rst [lindex [read_memory 0xFF5E023C 32 1] 0] } e] } {
    echo "ERROR: AXI read failed: $e — power-cycle and re-run."
    shutdown
    return
}
echo [format "RST_LPD_TOP = 0x%08x" $rst]
set r5_in_reset [expr { ($rst & 0x1) != 0 }]

# ── Path B: R5 already running ───────────────────────────────────────────────
if { !$r5_in_reset } {
    echo "=== Path B: R5 running ==="
    echo "Attempting software reset via RST_LPD_TOP..."

    # Try to put R5-0 back in module reset.  This only works if XPPU has not
    # locked CRL_APB (i.e. this is the first run after power-on, or XPPU is
    # configured to allow JTAG writes to RST_LPD_TOP).
    set rst_hold [expr {$rst | 0x3}]
    mww 0xFF5E023C $rst_hold
    after 50
    clear_stickyerr
    set rst_check [lindex [read_memory 0xFF5E023C 32 1] 0]
    echo [format "RST_LPD_TOP after reset-write = 0x%08x  (wrote 0x%08x)" \
        $rst_check $rst_hold]

    if { ($rst_check & 0x1) != 0 } {
        echo "Software reset OK — running full init sequence..."
        clear_stickyerr
        full_init_from_reset
    } else {
        echo "RST_LPD_TOP write blocked (XPPU). Falling back to CoreSight halt..."
        if { ![load_elf $ELF] } { shutdown; return }
        setup_uart
        if { ![examine_and_run] } {
            echo "CoreSight halt failed. Power-cycle the board and re-run."
            shutdown
            return
        }
    }

    echo ""
    echo "picocom -b 115200 /dev/ttyUSB1"
    echo ""
    shutdown
    return
}

# ── Path A: R5 in module reset (clean power-cycle) ───────────────────────────
echo "=== Path A: fresh reset — full boot sequence ==="
echo [format "RPU_GLBL_CNTL = 0x%08x" [lindex [read_memory 0xFF9A0000 32 1] 0]]
full_init_from_reset

echo ""
echo "picocom -b 115200 /dev/ttyUSB1"
echo ""
shutdown
