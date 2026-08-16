# Synthesize the Arm-encrypted Cortex-M3 IP in Vivado 2019.1.x.  The ZU1CG
# device was introduced after that Vivado release, so this deliberately uses
# a supported Zynq UltraScale+ part and emits an EDIF boundary for the 2023.2
# ZU1 integration flow.
set script_dir [file dirname [file normalize [info script]]]
set repo_dir [file normalize [file join $script_dir ../../..]]
if {[info exists ::env(M3_OOC_OUTPUT_DIR)]} {
    set output_dir [file normalize $::env(M3_OOC_OUTPUT_DIR)]
} else {
    set output_dir [file join $repo_dir bazel-out m3-ooc-2019]
}
if {![info exists ::env(ARM_DESIGNSTART_IP_ROOT)]} {
    error "set ARM_DESIGNSTART_IP_ROOT to Arm_ipi_repository"
}

file mkdir $output_dir
create_project -force m3_ooc_2019 $output_dir -part xczu3eg-sbva484-1-e
set_property target_language Verilog [current_project]
set_property ip_repo_paths [list [file normalize $::env(ARM_DESIGNSTART_IP_ROOT)]] [current_project]
update_ip_catalog

set m3 [create_ip -vlnv Arm.com:CortexM:CORTEXM3_AXI:1.1 -module_name m3_core]
set_property -dict [list \
    CONFIG.JTAG_PRESENT {true} CONFIG.WIC_PRESENT {false} \
    CONFIG.ITCM_SIZE {8kB} CONFIG.DTCM_SIZE {2kB} CONFIG.ITCM_INIT_RAM {false}] [get_ips m3_core]
generate_target all [get_ips m3_core]
synth_ip [get_ips m3_core]
set ip_dcp [file join [get_property IP_DIR [get_ips m3_core]] m3_core.dcp]
if {![file exists $ip_dcp]} {
    error "M3 OOC synthesis did not generate $ip_dcp"
}
open_checkpoint $ip_dcp
write_checkpoint -force [file join $output_dir m3_core_2019.dcp]
write_edif -force [file join $output_dir m3_core.edf]
report_utilization -file [file join $output_dir m3_core_2019_utilization.rpt]
puts "M3 OOC EDIF: [file join $output_dir m3_core.edf]"
