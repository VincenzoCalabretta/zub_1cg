if {![info exists output_dir]} {
    if {$argc != 1} { error "usage: vivado -source write_manifest.tcl -tclargs OUTPUT_DIR" }
    set output_dir [file normalize [lindex $argv 0]]
}

set manifest [open [file join $output_dir artifacts.json] w]
puts $manifest "{\n  \"schema_version\": 2,"
puts $manifest "  \"vivado_version\": \"[version -short]\","
puts $manifest "  \"part\": \"xczu1cg-sbva484-1-e\","
puts $manifest "  \"artifacts\": \["
set names {zub_orbtrace.bit zub_orbtrace.xsa psu_init.tcl}
for {set i 0} {$i < [llength $names]} {incr i} {
    set name [lindex $names $i]
    set path [file join $output_dir $name]
    if {![file exists $path]} { error "missing artifact $path" }
    set hash [lindex [split [exec sha256sum $path]] 0]
    set comma [expr {$i + 1 == [llength $names] ? "" : ","}]
    puts $manifest "    {\"path\": \"$name\", \"size_bytes\": [file size $path], \"sha256\": \"$hash\"}$comma"
}
puts $manifest "  \]\n}"
close $manifest
