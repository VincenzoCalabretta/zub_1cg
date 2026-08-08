# Vivado 2023.2 batch entry point. Run from any directory:
# vivado -mode batch -source applications/orbtrace/vivado/build.tcl
set script_dir [file dirname [file normalize [info script]]]
set repo_dir [file normalize [file join $script_dir ../../..]]
set output_dir [file join $repo_dir bazel-out orbtrace-vivado]
file mkdir $output_dir

create_project -force zub_orbtrace $output_dir -part xczu1cg-sbva484-1-e
set_property target_language Verilog [current_project]
set_property simulator_language Mixed [current_project]

foreach source [glob [file join $repo_dir applications orbtrace rtl *.sv]] { read_verilog -sv $source }
foreach source [glob [file join $repo_dir applications orbtrace rtl *.v]] { read_verilog $source }
add_files -norecurse [file join $repo_dir applications orbtrace rtl orbtrace_regs.svh]
set_property file_type {Verilog Header} [get_files orbtrace_regs.svh]
set_property include_dirs [list [file join $repo_dir applications orbtrace rtl]] [get_filesets sources_1]
read_xdc [file join $script_dir orbtrace.xdc]
source [file join $script_dir create_bd.tcl]

validate_bd_design
save_bd_design
generate_target all [get_files zub_orbtrace.bd]
make_wrapper -files [get_files zub_orbtrace.bd] -top
add_files -norecurse [file join $output_dir zub_orbtrace.gen sources_1 bd zub_orbtrace hdl zub_orbtrace_wrapper.v]
set_property top zub_orbtrace_wrapper [current_fileset]

launch_runs synth_1 -jobs 8
wait_on_run synth_1
if {[get_property STATUS [get_runs synth_1]] ne "synth_design Complete!"} { error "synthesis failed" }
launch_runs impl_1 -to_step write_bitstream -jobs 8
wait_on_run impl_1
if {[get_property STATUS [get_runs impl_1]] ne "write_bitstream Complete!"} { error "implementation failed" }
open_run impl_1
report_timing_summary -file [file join $output_dir timing_summary.rpt]
set cdc_text [report_cdc -details -return_string]
set cdc_file [open [file join $output_dir cdc.rpt] w]
puts $cdc_file $cdc_text
close $cdc_file
if {[regexp {CDC-[0-9]+[[:space:]]+Critical} $cdc_text]} { error "critical CDC violation" }
set methodology_text [report_methodology -return_string]
set methodology_file [open [file join $output_dir methodology.rpt] w]
puts $methodology_file $methodology_text
close $methodology_file
if {[regexp {\|[[:space:]]+[^|]+\|[[:space:]]+Critical Warning[[:space:]]+\|} $methodology_text]} {
    error "critical methodology violation"
}
report_utilization -file [file join $output_dir utilization.rpt]
if {[get_property SLACK [get_timing_paths -delay_type max -max_paths 1]] < 0} { error "negative timing slack" }
if {[get_property SLACK [get_timing_paths -delay_type min -max_paths 1]] < 0} { error "negative hold slack" }

file copy -force [file join $output_dir zub_orbtrace.runs impl_1 zub_orbtrace_wrapper.bit] [file join $output_dir zub_orbtrace.bit]
write_hw_platform -fixed -include_bit -force -file [file join $output_dir zub_orbtrace.xsa]
set xsct [expr {[info exists ::env(XSCT)] ? $::env(XSCT) : [auto_execok xsct]}]
if {$xsct eq ""} { error "xsct not found; set XSCT to the Vitis 2023.2 executable" }
exec $xsct [file join $script_dir export_psu_init.tcl] [file join $output_dir zub_orbtrace.xsa] $output_dir
source [file join $script_dir write_manifest.tcl]
