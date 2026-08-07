# load_and_uart.tcl — combined: load ELF + release UART + configure MIO + start R5.
#
# Correct MIO value: L3_SEL[14:12]=6 + drive bits[4:3]=1 = 0x00006018
#   vs. our earlier wrong 0x00000030 (L3_SEL at [5:3], wrong position)
#
# UART ref clock: IOPLL(33.333*50=1666.67MHz) / DIV0=24 = 69.44 MHz
# At 115200: CD=43, BDIV=13 -> 69444444/(43*14)=115313 baud (0.1% err)

set ELF "bazel-bin/apps/rpu/hello_world/hello_world"

if {![file exists $ELF]} {
    echo "ERROR: ELF not found."
    shutdown
    return
}

init
after 500

targets uscale.axi
if { [catch { read_memory 0xFF9A0000 32 1 } err] } {
    echo "AXI blocked — clearing STICKYERR..."
    irscan uscale.tap 0x8
    drscan uscale.tap 35 0xF8
    runtest 100
    irscan uscale.tap 0xA
    drscan uscale.tap 35 0x280000102
    drscan uscale.tap 35 0x07
    runtest 200
    uscale.axi arp_examine
    targets uscale.axi
    if { [catch { read_memory 0xFF9A0000 32 1 } err2] } {
        echo "ERROR: AXI still blocked — power-cycle."
        shutdown
        return
    }
}

# ── Step 1: Configure RPU ────────────────────────────────────────────────────
mww 0xFF9A0000 0x00000050   ;# GLBL_CNTL: split mode, SLCLAMP on (default OK)
mww 0xFF9A0100 0x00000001   ;# RPU_0_CFG: VINITHI=1 → reset vector at 0xFFFF0000
echo "RPU: VINITHI=1 → OCM high vectors."

# ── Step 2: Release peripheral resets ───────────────────────────────────────
mww 0xFF5E0238 0x00000000   ;# RST_LPD_IOU2: release all IOU2 peripherals
after 5

# ── Step 3: UART clock setup ─────────────────────────────────────────────────
# IOPLL = 33.333*50 = 1666.67 MHz; UART ref = /1/24 = 69.44 MHz
mww 0xFF5E0070 0x01011800   ;# CRL_APB_UART0_REF_CTRL: SRCSEL=IOPLL, DIV1=1, DIV0=24
mww 0xFF5E0074 0x01011800   ;# CRL_APB_UART1_REF_CTRL: same

# ── Step 4: Init UART1 (69.44 MHz / (43*14) = 115313 baud) ─────────────────
mww 0xFF01000C 0xFFFFFFFF   ;# UART1 IDR: disable all interrupts
mww 0xFF010000 0x00000003   ;# UART1 CR: SW reset TX+RX
after 2
mww 0xFF010004 0x00000020   ;# UART1 MR: 8N1, no parity, normal mode
mww 0xFF010018 43           ;# UART1 BAUDGEN: CD=43
mww 0xFF010034 13           ;# UART1 BAUDDIV: BDIV=13
mww 0xFF010000 0x00000014   ;# UART1 CR: TX_EN | RX_EN
after 2

echo [format {UART1 CR = 0x%08x  SR = 0x%08x} \
    [lindex [read_memory 0xFF010000 32 1] 0] \
    [lindex [read_memory 0xFF01002C 32 1] 0]]

# ── Step 5: Configure ALL 78 MIO pins to L3_SEL=6 + drive (0x6018) ──────────
# L3_SEL[14:12]=110=6 (UART), bit[4]=8mA, bit[3]=Schmitt
echo "Setting all MIO pins to UART function (0x6018)..."
for {set i 0} {$i < 78} {incr i} {
    mww [expr {0xFF180000 + $i * 4}] 0x00006018
}

# ── Step 6: Send test bytes from UART1 (before releasing R5) ────────────────
echo "Sending test bytes from UART1..."
# "OpenOCD\r\n" + 5 extra U bytes (0x55) to be sure
foreach b {0x4F 0x70 0x65 0x6E 0x4F 0x43 0x44 0x0D 0x0A 0x55 0x55 0x55 0x55 0x55} {
    mww 0xFF010030 $b
    after 1
}
after 30

echo [format {UART1 SR after send = 0x%08x} [lindex [read_memory 0xFF01002C 32 1] 0]]

# ── Step 7: Load ELF into OCM ───────────────────────────────────────────────
set rst [lindex [read_memory 0xFF5E023C 32 1] 0]
echo [format {RST_LPD_TOP = 0x%08x  (R5-0 in reset)} $rst]
echo "Loading ELF..."
load_image $ELF 0 elf

set w0 [lindex [read_memory 0xFFFF0000 32 1] 0]
echo [format {OCM[0xFFFF0000] = 0x%08x  (expect 0xe59ff018)} $w0]
if { $w0 == 0x00000000 || $w0 == 0xFFFFFFFF } {
    echo "ERROR: OCM spot-check failed."
    shutdown
    return
}
echo "ELF loaded OK."

# ── Step 8: Release R5-0 ────────────────────────────────────────────────────
set rst_run [expr {($rst & ~0x1) | 0x2}]
mww 0xFF5E023C $rst_run
echo [format {RST_LPD_TOP = 0x%08x  (R5-0 running)} $rst_run]
echo "R5-0 executing from OCM. R5 uart_init() will overwrite UART divisors."
echo "Waiting 1s for R5 to run, then re-applying correct divisors..."

after 1000

# ── Step 9: Re-apply correct UART1 divisors (R5 uart_init used wrong clock) ─
# R5 uart_init() assumed 100 MHz; it overwrote BAUDGEN/BAUDDIV with wrong vals.
# Re-correct here.
mww 0xFF01000C 0xFFFFFFFF
mww 0xFF010000 0x00000003
after 2
mww 0xFF010004 0x00000020
mww 0xFF010018 43
mww 0xFF010034 13
mww 0xFF010000 0x00000014
after 2
echo "UART1 re-initialized with correct divisors (CD=43, BDIV=13)."

echo ""
echo "=== ThreadX state check ==="
set tx_sys_state [lindex [read_memory 0xFFFF2BC8 32 1] 0]
echo [format {_tx_thread_system_state @ 0xFFFF2BC8 = 0x%08x} $tx_sys_state]
set tx_timer_ts [lindex [read_memory 0xFFFF42C4 32 1] 0]
echo [format {_tx_timer_time_slice   @ 0xFFFF42C4 = 0x%08x} $tx_timer_ts]

echo ""
echo "Waiting 3s (R5 prints Hello World now)..."
after 3000

set tx_sys_state2 [lindex [read_memory 0xFFFF2BC8 32 1] 0]
set tx_timer_ts2  [lindex [read_memory 0xFFFF42C4 32 1] 0]
echo [format {_tx_thread_system_state = 0x%08x  (was 0x%08x)} $tx_sys_state2 $tx_sys_state]
echo [format {_tx_timer_time_slice    = 0x%08x  (was 0x%08x)} $tx_timer_ts2 $tx_timer_ts]
if { $tx_timer_ts2 != $tx_timer_ts } {
    echo "Timer tick is advancing — ThreadX scheduler is running!"
} else {
    echo "WARNING: timer tick unchanged — timer ISR may not be firing."
}

echo ""
echo "=== UART1 state ==="
echo [format {UART1 CR  = 0x%08x} [lindex [read_memory 0xFF010000 32 1] 0]]
echo [format {UART1 SR  = 0x%08x} [lindex [read_memory 0xFF01002C 32 1] 0]]
echo [format {UART1 BAUDGEN = 0x%08x} [lindex [read_memory 0xFF010018 32 1] 0]]
echo [format {UART1 BAUDDIV = 0x%08x} [lindex [read_memory 0xFF010034 32 1] 0]]

echo ""
echo {R5-0 executing. UART on ttyUSB1 at 115200.}
echo ""
shutdown
