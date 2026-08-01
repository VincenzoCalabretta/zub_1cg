# diag_literal_pool.tcl — read vector literal pool, BSS loop literal pool,
# and the full reset handler prologue (0xFFFF0040–0xFFFF0120).

init
after 300
targets uscale.axi

irscan uscale.tap 0x8
drscan uscale.tap 35 0xF8
runtest 100
irscan uscale.tap 0xA
drscan uscale.tap 35 0x280000102
drscan uscale.tap 35 0x07
runtest 200
uscale.axi arp_examine
targets uscale.axi

proc rd {a} { return [lindex [read_memory $a 32 1] 0] }

echo ""
echo "=== Vector literal pool (0xFFFF0020–0xFFFF003F) ==="
for {set i 0} {$i < 8} {incr i} {
    set a [expr {0xFFFF0020 + $i * 4}]
    echo [format {  0x%08x = 0x%08x} $a [rd $a]]
}

echo ""
echo "=== Code 0xFFFF0040–0xFFFF0120 (reset handler prologue) ==="
for {set i 0} {$i < 56} {incr i} {
    set a [expr {0xFFFF0040 + $i * 4}]
    echo [format {  0x%08x = 0x%08x} $a [rd $a]]
}

echo ""
echo "=== BSS loop literal pool — 0xFFFF0250–0xFFFF0270 ==="
for {set i 0} {$i < 9} {incr i} {
    set a [expr {0xFFFF0250 + $i * 4}]
    echo [format {  0x%08x = 0x%08x} $a [rd $a]]
}

echo ""
echo "=== Stack-size literal pool scan — 0xFFFF0120–0xFFFF01E0 (every 4th) ==="
for {set i 0} {$i < 48} {incr i} {
    set a [expr {0xFFFF0120 + $i * 4}]
    echo [format {  0x%08x = 0x%08x} $a [rd $a]]
}

echo ""
shutdown
