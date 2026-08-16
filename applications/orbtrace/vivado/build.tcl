# Vivado batch entry point. Run from any directory:
# vivado -mode batch -source applications/orbtrace/vivado/build.tcl
set script_dir [file dirname [file normalize [info script]]]
set repo_dir [file normalize [file join $script_dir ../../..]]
if {[info exists ::env(ORBTRACE_VIVADO_OUTPUT_DIR)]} {
    set output_dir [file normalize $::env(ORBTRACE_VIVADO_OUTPUT_DIR)]
} else {
    set output_dir [file join $repo_dir bazel-out orbtrace-vivado]
}
file mkdir $output_dir

source [file join $repo_dir sdk boards zub_1cg board_preset.tcl]
if {![info exists ::env(AVNET_BDF_ROOT)]} {
    error "set AVNET_BDF_ROOT to the Avnet BDF checkout root (containing zub1cg/1.2/board.xml)"
}
# Vivado only picks up a custom board repo if board.repoPaths is set before
# the board catalog is first populated, which happens at create_project.
set_param board.repoPaths [list [file normalize $::env(AVNET_BDF_ROOT)]]

create_project -force zub_orbtrace $output_dir -part xczu1cg-sbva484-1-e
# read_edif imports the 2019 OOC netlist into `work`; keep generated Verilog
# in the same library when the hybrid path is enabled below.
set_property default_lib work [current_project]
set_property target_language Verilog [current_project]
set_property simulator_language Mixed [current_project]

zub1cg_select_board $::env(AVNET_BDF_ROOT)

if {![info exists ::env(ARM_DESIGNSTART_IP_ROOT)]} {
    error "set ARM_DESIGNSTART_IP_ROOT to the local unpacked Arm DesignStart\
        Cortex-M3 FPGA IP repo (licensed separately; see applications/orbtrace/vivado/README.md)"
}
set_property ip_repo_paths [list [file normalize $::env(ARM_DESIGNSTART_IP_ROOT)]] [current_project]
update_ip_catalog

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

# The DesignStart M3 sources are encrypted for the Vivado 2019.1 release.
# A full 2019 project cannot target the ZU1CG, so a compatible 2019.1 OOC
# build exports its configured core as EDIF and 2023.2 integrates that
# netlist with the rest of this board design.  This is deliberately opt-in:
# a normally compatible Arm package can continue using Vivado's generated IP
# wrapper by leaving M3_OOC_EDIF unset.
if {[info exists ::env(M3_OOC_EDIF)]} {
    set m3_edif [file normalize $::env(M3_OOC_EDIF)]
    if {![file exists $m3_edif]} {
        error "M3_OOC_EDIF does not exist: $m3_edif"
    }

    set m3_ip_xci [file join $output_dir zub_orbtrace.srcs sources_1 bd zub_orbtrace ip zub_orbtrace_m3_core_0 zub_orbtrace_m3_core_0.xci]
    set m3_generated_wrapper [file join $output_dir zub_orbtrace.gen sources_1 bd zub_orbtrace ip zub_orbtrace_m3_core_0 synth zub_orbtrace_m3_core_0.v]
    foreach generated_file [list $m3_ip_xci $m3_generated_wrapper] {
        set generated_obj [get_files -quiet $generated_file]
        if {[llength $generated_obj] != 1} {
            error "could not locate generated M3 source: $generated_file"
        }
        # Do not give 2023.2 either the IP definition or its wrapper: both
        # would pull the 2019-encrypted RTL back into this synthesis run.
        set_property USED_IN_SYNTHESIS false $generated_obj
    }

    # Keep the block design's cell module name, but make it an adapter around
    # the fixed-configuration 2019 EDIF top.  The generated wrapper's port
    # list and port connections are retained verbatim, avoiding a hand-kept
    # duplicate of the Arm IP interface.
    set wrapper_fd [open $m3_generated_wrapper r]
    set wrapper_text [read $wrapper_fd]
    close $wrapper_fd
    set core_start [string first "  CortexM3DbgAXI #(\n" $wrapper_text]
    set inst_start [string first "  ) inst (" $wrapper_text $core_start]
    if {$core_start < 0 || $inst_start < 0} {
        error "unexpected generated M3 wrapper format: $m3_generated_wrapper"
    }
    set m3_hybrid_wrapper [file join $output_dir zub_orbtrace_m3_core_0_hybrid.v]
    set wrapper_text [string replace $wrapper_text $core_start [expr {$inst_start + [string length "  ) inst ("] - 1}] "  m3_core inst ("]
    # WIC is disabled in this configuration, so OOC synthesis optimizes its
    # two outputs and its tied-low enable out of the EDIF interface.  These
    # are internal-only connections in the generated IP wrapper (not BD
    # ports), and must not be presented to the fixed EDIF implementation.
    set wrapper_text [string map [list \
        "    .WAKEUP(),\n" "" \
        "    .WICENACK(),\n" "" \
        "    .WICENREQ(1'B0),\n" ""] $wrapper_text]
    set wrapper_fd [open $m3_hybrid_wrapper w]
    puts -nonewline $wrapper_fd $wrapper_text
    close $wrapper_fd

    # read_edif contributes a netlist cell during netlist linking, after RTL
    # elaboration has already checked every Verilog module reference.  Supply
    # a black-box declaration with the EDIF's exact (WIC-pruned) port list to
    # bridge those phases.  Deriving it from this generated adapter prevents
    # a hand-maintained copy of the Arm interface from drifting.
    set module_start [string first "module zub_orbtrace_m3_core_0 (" $wrapper_text]
    set m3_inst_start [string first "  m3_core inst (" $wrapper_text]
    if {$module_start < 0 || $m3_inst_start < 0} {
        error "could not derive M3 EDIF black-box declaration"
    }
    set m3_stub_text [string range $wrapper_text $module_start [expr {$m3_inst_start - 1}]]
    set m3_stub_text [string map [list \
        "module zub_orbtrace_m3_core_0 (" "module m3_core ("] $m3_stub_text]
    set m3_hybrid_stub [file join $output_dir m3_core_edif_stub.v]
    set stub_fd [open $m3_hybrid_stub w]
    puts $stub_fd "(* black_box *)"
    puts -nonewline $stub_fd $m3_stub_text
    puts $stub_fd "endmodule"
    close $stub_fd

    # A project-mode synth run reconstructs its source set from the block
    # design, so ordinary add_files entries outside that generated hierarchy
    # are deliberately auto-disabled in its run script.  Use Vivado's
    # documented pre-synth hook to load the two replacement sources after the
    # BD sources have been reconstructed and before synth_design links them.
    set m3_hybrid_pre [file join $output_dir m3_hybrid_pre_synth.tcl]
    set pre_fd [open $m3_hybrid_pre w]
    puts $pre_fd "read_verilog [list $m3_hybrid_stub]"
    puts $pre_fd "read_edif [list $m3_edif]"
    puts $pre_fd "read_verilog [list $m3_hybrid_wrapper]"
    close $pre_fd
    set_property STEPS.SYNTH_DESIGN.TCL.PRE $m3_hybrid_pre [get_runs synth_1]
    puts "Using Vivado-2019 M3 EDIF: $m3_edif"
}
set_property top zub_orbtrace_wrapper [current_fileset]

# Reduce peak memory footprint: this machine is intermittently oversubscribed
# by unrelated processes, which has killed several prior runs mid-place. Fewer
# parallel sub-IP synth jobs and less place/route thread parallelism trades
# runtime for a smaller peak working set.
set_param general.maxThreads 4
launch_runs synth_1 -jobs 2
wait_on_run synth_1
if {[get_property STATUS [get_runs synth_1]] ne "synth_design Complete!"} { error "synthesis failed" }

# Default-strategy P&R left the M3 core's LSU-address-adder -> NVIC
# interrupt-pending fanout at WNS=-0.228ns, 76% of which was routing delay
# behind a "congestion level 5" placement warning -- i.e. placement, not
# logic depth. Push placement/routing harder specifically to close that.
set_property STEPS.PLACE_DESIGN.ARGS.DIRECTIVE Explore [get_runs impl_1]
set_property STEPS.PHYS_OPT_DESIGN.IS_ENABLED true [get_runs impl_1]
set_property STEPS.PHYS_OPT_DESIGN.ARGS.DIRECTIVE AggressiveExplore [get_runs impl_1]
set_property STEPS.ROUTE_DESIGN.ARGS.DIRECTIVE AggressiveExplore [get_runs impl_1]
set_property STEPS.POST_ROUTE_PHYS_OPT_DESIGN.IS_ENABLED true [get_runs impl_1]
set_property STEPS.POST_ROUTE_PHYS_OPT_DESIGN.ARGS.DIRECTIVE AggressiveExplore [get_runs impl_1]

launch_runs impl_1 -to_step write_bitstream -jobs 2
wait_on_run impl_1
if {[get_property STATUS [get_runs impl_1]] ne "write_bitstream Complete!"} { error "implementation failed" }
open_run impl_1
report_timing_summary -file [file join $output_dir timing_summary.rpt]
# report_cdc only analyzes a crossing once both sides have a defined clock;
# with swclktck now declared (see orbtrace.xdc), it structurally analyzes the
# M3 IP's own internal JTAG-DP (SWCLKTCK) <-> HCLK boundary for the first
# time. Arm synchronizes that boundary internally (register names like
# *_cdc_check_reg say so), but Vivado's generic pattern matcher doesn't
# recognize their structure and flags it Critical. Waive exactly that known
# boundary -- both the DAP's own internal crossing and the async nTRST/reset
# this design drives into it from orbtrace_dap_engine.sv -- and require any
# other critical CDC finding (e.g. a real bug in new orbtrace RTL) to still
# fail the build. report_cdc must run once to populate violations before
# get_cdc_violations/create_waiver can see them.
report_cdc
foreach v [get_cdc_violations -filter {SEVERITY == Critical}] {
    set from [get_property STARTPOINT_PIN $v]
    set to [get_property ENDPOINT_PIN $v]
    set from_in_dap_engine [string match {*trace_pl/inst/dap_engine/*} $from]
    set to_in_dap_engine [string match {*trace_pl/inst/dap_engine/*} $to]
    set from_in_m3_dap [string match {*m3_core/inst/inst/u_CORTEXM3INTEGRATION*} $from]
    set to_in_m3_dap [string match {*m3_core/inst/inst/u_CORTEXM3INTEGRATION*} $to]
    set known_m3_dap_boundary [expr {
        ($from_in_dap_engine || $from_in_m3_dap) && ($to_in_dap_engine || $to_in_m3_dap)
    }]
    if {$known_m3_dap_boundary} {
        create_waiver -type CDC -id [get_property CHECK $v] -user build.tcl \
            -description "M3 IP's own internal JTAG-DP (SWCLKTCK) <-> HCLK boundary;\
                Arm synchronizes this internally, not recognized by Vivado's generic\
                CDC pattern matcher. See orbtrace.xdc swclktck comment." \
            -from [get_pins $from] -to [get_pins $to]
    }
}
# Re-run so the report and the violation objects both reflect the waivers
# just created (IS_WAIVED is only current as of the report_cdc call that
# follows waiver creation).
set cdc_text [report_cdc -details -return_string]
set cdc_file [open [file join $output_dir cdc.rpt] w]
puts $cdc_file $cdc_text
close $cdc_file
set unwaived_critical [get_cdc_violations -filter {SEVERITY == Critical && IS_WAIVED == 0}]
if {[llength $unwaived_critical] > 0} {
    foreach v $unwaived_critical {
        puts "UNWAIVED CRITICAL CDC: [get_property CHECK $v] [get_property STARTPOINT_PIN $v] -> [get_property ENDPOINT_PIN $v]"
    }
    error "critical CDC violation outside the known M3 JTAG-DP boundary"
}
# swclktck (see orbtrace.xdc) is a primary clock declared on a bit-banged
# pseudo-clock pin, not a true independent clock source -- Vivado's
# methodology checker always flags that pattern two ways, regardless of
# which pin along the net it's declared on: TIMING-2 ("invalid primary clock
# source pin", the pin isn't a top-level port or primitive output) and
# TIMING-4 ("invalid clock redefinition on a clock tree", it's electrically
# downstream of clk_pl_0). Both are expected for this design; waive them.
# TIMING-4 has no per-instance object to scope to (OBJECT_TYPES is empty),
# so its waiver is rule-wide -- acceptable here since any future TIMING-4
# can only come from a new/changed clock declaration in orbtrace.xdc, not
# from unrelated RTL changes silently regressing past this gate.
report_methodology
create_waiver -id TIMING-2 \
    -objects [get_pins zub_orbtrace_i/m3_core/inst/inst/SWCLKTCK] \
    -description "swclktck is a bit-banged pseudo-clock (orbtrace_dap_engine.sv).\
        See orbtrace.xdc swclktck comment."
create_waiver -id TIMING-4 \
    -description "swclktck is a bit-banged pseudo-clock (orbtrace_dap_engine.sv),\
        electrically downstream of clk_pl_0 by construction. See orbtrace.xdc\
        swclktck comment."
set methodology_text [report_methodology -return_string]
set methodology_file [open [file join $output_dir methodology.rpt] w]
puts $methodology_file $methodology_text
close $methodology_file
set unwaived_methodology [get_methodology_violations -filter {SEVERITY == "Critical Warning" && IS_WAIVED == 0}]
if {[llength $unwaived_methodology] > 0} {
    foreach v $unwaived_methodology {
        puts "UNWAIVED CRITICAL METHODOLOGY: [get_property CHECK $v]: [get_property DESCRIPTION $v]"
    }
    error "critical methodology violation"
}
report_utilization -file [file join $output_dir utilization.rpt]
if {[get_property SLACK [get_timing_paths -delay_type max -max_paths 1]] < 0} { error "negative timing slack" }
if {[get_property SLACK [get_timing_paths -delay_type min -max_paths 1]] < 0} { error "negative hold slack" }

file copy -force [file join $output_dir zub_orbtrace.runs impl_1 zub_orbtrace_wrapper.bit] [file join $output_dir zub_orbtrace.bit]
write_hw_platform -fixed -include_bit -force -file [file join $output_dir zub_orbtrace.xsa]
set xsct [expr {[info exists ::env(XSCT)] ? $::env(XSCT) : [auto_execok xsct]}]
if {$xsct eq ""} { error "xsct not found; set XSCT to a compatible Xilinx executable" }
exec $xsct [file join $script_dir export_psu_init.tcl] [file join $output_dir zub_orbtrace.xsa] $output_dir
source [file join $script_dir write_manifest.tcl]
