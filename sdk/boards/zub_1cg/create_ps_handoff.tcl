if {$argc != 4} {
    error "usage: create_ps_handoff.tcl WORKSPACE OUTPUT_DIR BDF_ROOT PRESET_HELPER"
}
set workspace [file normalize [lindex $argv 0]]
set output_dir [file normalize [lindex $argv 1]]
set bdf_root [file normalize [lindex $argv 2]]
set preset_helper [file normalize [lindex $argv 3]]
file mkdir $output_dir

source $preset_helper
set_param board.repoPaths [list $bdf_root]
create_project -force zub1cg_ps_handoff $output_dir -part xczu1cg-sbva484-1-e
zub1cg_select_board $bdf_root
create_bd_design zub1cg_ps
set ps [create_bd_cell -type ip -vlnv xilinx.com:ip:zynq_ultra_ps_e:* ps]
zub1cg_apply_ps_preset $ps
set_property -dict [list \
    CONFIG.PSU__USE__M_AXI_GP0 {1} \
    CONFIG.PSU__USE__M_AXI_GP1 {1}] $ps
connect_bd_net [get_bd_pins $ps/pl_clk0] \
    [get_bd_pins $ps/maxihpm0_fpd_aclk] \
    [get_bd_pins $ps/maxihpm1_fpd_aclk]
validate_bd_design
save_bd_design
generate_target all [get_files zub1cg_ps.bd]
zub1cg_write_ps_fingerprint $ps [file join $output_dir ps_configuration.json]
write_hw_platform -fixed -force -file [file join $output_dir zub1cg_ps.xsa]
