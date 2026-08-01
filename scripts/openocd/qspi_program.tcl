# qspi_program.tcl — Program BOOT.BIN to QSPI flash on AES-ZUB
# Uses ZynqMP GQSPI controller registers at 0xFF0F0100+ (correct offsets)

source /tmp/flash_data.tcl

# ── GQSPI base (LPD_QSPI) ───────────────────────────────────────────────
set B 0xFF0F0000

# GQSPI Register offsets (from Linux driver spi-zynqmp-gqspi.c)
set REG_CONFIG  [expr {$B + 0x100}]   ;# GQSPI_CONFIG
set REG_ISR     [expr {$B + 0x104}]   ;# GQSPI_ISR
set REG_IER     [expr {$B + 0x108}]   ;# GQSPI_IER
set REG_IDR     [expr {$B + 0x10C}]   ;# GQSPI_IDR
set REG_EN      [expr {$B + 0x114}]   ;# GQSPI_EN
set REG_TXD     [expr {$B + 0x11C}]   ;# GQSPI_TXD
set REG_RXD     [expr {$B + 0x120}]   ;# GQSPI_RXD (DON'T READ IF EMPTY)
set REG_STATUS  [expr {$B + 0x124}]   ;# GQSPI_STATUS (GQSPI_SR?)
set REG_TX_THR  [expr {$B + 0x128}]   ;# GQSPI_TX_THRESHOLD
set REG_RX_THR  [expr {$B + 0x12C}]   ;# GQSPI_RX_THRESHOLD
set REG_GF      [expr {$B + 0x140}]   ;# GQSPI_GEN_FIFO
set REG_SEL     [expr {$B + 0x144}]   ;# GQSPI_SEL
set REG_FIFO_CTRL [expr {$B + 0x14C}];# GQSPI_FIFO_CTRL
set REG_GF_THR  [expr {$B + 0x150}]   ;# GQSPI_GF_THRESHOLD

# Bit masks
set M_SEL       [expr {1 << 0}]       ;# SEL=1 for GQSPI
set M_EN        [expr {1 << 0}]       ;# EN=1 for enable
set M_MANUAL    [expr {1 << 29}]      ;# GEN_FIFO_START_MODE
set M_RESET_FIFO 0x7                  ;# Reset all FIFOs
set M_TX_THR    32
set M_RX_THR    1
set M_GF_THR    16

# GenFIFO commands
set G_DATA_XFER  0x00000100
set G_MODE_SPI   0x00000400
set G_CS_LOWER   0x00001000
set G_CS_DEASSERT 0x00004000
set G_TX         0x00010000
set G_RX         0x00020000

proc rd32 {a} { return [lindex [read_memory $a 32 1] 0] }
proc wr32 {a v} { mww $a $v }

proc poll_genfifo_not_full {} {
    for {set i 0} {$i < 5000} {incr i} {
        set ir [rd32 $::REG_ISR]
        if {$ir & 0x200} { return }    ;# GENFIFONOT_FULL
        after 2
    }
    error "GENFIFO full timeout"
}

proc poll_tx_not_full {} {
    for {set i 0} {$i < 5000} {incr i} {
        set ir [rd32 $::REG_ISR]
        if {$ir & 0x004} { return }    ;# TXNOT_FULL
        after 2
    }
    error "TX full timeout"
}

proc poll_tx_empty {} {
    for {set i 0} {$i < 50000} {incr i} {
        set ir [rd32 $::REG_ISR]
        if {$ir & 0x100} { return }    ;# TXEMPTY
        after 2
    }
    error "TX empty timeout"
}

proc poll_genfifo_empty {} {
    for {set i 0} {$i < 50000} {incr i} {
        set ir [rd32 $::REG_ISR]
        if {$ir & 0x080} { return }    ;# GENFIFOEMPTY
        after 2
    }
    error "GF empty timeout"
}

# Execute a SPI command via GenFIFO
proc spi_cmd {cmd_addr_words nbytes {flags 0}} {
    set extra [expr {$flags | $::G_DATA_XFER | $::G_MODE_SPI | $::G_CS_LOWER | $::G_TX}]
    # Command in TXD
    poll_tx_not_full
    wr32 $::REG_TXD $cmd_addr_words
    # Push to GenFIFO
    poll_genfifo_not_full
    wr32 $::REG_GF [expr {$nbytes | $extra}]
}

proc flash_wren {} {
    spi_cmd 0x06 1
    after 2
}

proc sector_erase {addr} {
    flash_wren
    set cmd [expr {0xD8000000 | ($addr & 0xFFFFFF)}]
    spi_cmd $cmd 4
    after 3000
}

proc page_program {addr data_words} {
    flash_wren
    # Wait for previous transfer to complete
    poll_genfifo_empty
    poll_tx_empty
    # Clear ISR bits
    wr32 $::REG_ISR 0xFFFFFFFF
    after 2
    
    set header [expr {0x02000000 | ($addr & 0xFFFFFF)}]
    set total_bytes [expr {4 + [llength $data_words] * 4}]
    
    # Write header + data words to TX FIFO (9 words, within 64-word depth)
    wr32 $::REG_TXD $header
    foreach w $data_words {
        wr32 $::REG_TXD $w
    }
    
    # Push gen FIFO entry
    poll_genfifo_not_full
    wr32 $::REG_GF [expr {$total_bytes | $::G_DATA_XFER | $::G_MODE_SPI | $::G_CS_LOWER | $::G_TX}]
    
    # Wait for GenFIFO to drain
    poll_genfifo_empty
    after 10
}

# ── Main ────────────────────────────────────────────────────────────────
init
after 500
targets uscale.axi
irscan uscale.tap 0x8; drscan uscale.tap 35 0xF8; runtest 100
irscan uscale.tap 0xA; drscan uscale.tap 35 0x280000102; drscan uscale.tap 35 0x07; runtest 200
uscale.axi arp_examine

puts "=== Step 1: Init system ==="
wr32 0xFF5E0238 0x00000000
after 10
wr32 0xFF5E00B0 0x01011800
after 10

puts "=== Step 2: QSPI MIO config ==="
for {set p 1} {$p <= 6} {incr p} {
    wr32 [expr {0xFF180000 + $p * 4}] 0x0000E121
}
puts {  MIO 1-6 = QSPI}

puts "=== Step 3: Init GQSPI controller ==="
wr32 $REG_FIFO_CTRL $M_RESET_FIFO
after 10
wr32 $REG_SEL $M_SEL
after 10
wr32 $REG_TX_THR $M_TX_THR
wr32 $REG_RX_THR $M_RX_THR
wr32 $REG_GF_THR $M_GF_THR

# Configure: manual start, disable poll timeout, clear baud
set cfg [rd32 $REG_CONFIG]
set cfg [expr {$cfg | $M_MANUAL}]
set cfg [expr {$cfg & ~(0x38)}]        ;# clear baud rate div
wr32 $REG_CONFIG $cfg
after 10
wr32 $REG_EN $M_EN
after 10
puts [format {  CONFIG=0x%08x EN=%d} [rd32 $REG_CONFIG] [rd32 $REG_EN]]

puts "=== Step 4: Erase sectors 0-1 ==="
sector_erase 0
puts {  Sector 0 done}
sector_erase 0x10000
puts {  Sector 1 done}

puts "=== Step 5: Program BOOT.BIN ==="
set chunk_words 8
set total [expr {($flash_nwords + $chunk_words - 1) / $chunk_words}]
for {set c 0} {$c < $total} {incr c} {
    set start [expr {$c * $chunk_words}]
    set chunk [lrange $flash_words $start [expr {$start + $chunk_words - 1}]]
    set byte_addr [expr {$start * 4}]
    puts -nonewline [format "\r  %d/%d @ 0x%05x..." [expr {$c+1}] $total $byte_addr]
    flush stdout
    page_program $byte_addr $chunk
}
puts {""}

puts "=== Done ==="
puts "Set boot mode to QSPI and power-cycle."
puts "Connect UART: tio -b 115200 /dev/ttyUSB1"
shutdown