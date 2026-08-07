"""Public firmware macros for the AES-ZUB-1CG A53 and R5 cores."""

_APU_PLATFORM = Label("//sdk/platforms:apu_a53")
_RPU_PLATFORM = Label("//sdk/platforms:rpu_r5_0")

_APU_BSP = Label("//sdk/bsp/apu:xilinx_runtime")
_APU_LINKER_SCRIPT = Label("//sdk/bsp/apu:linker_script")
_RPU_BSP = Label("//sdk/bsp/rpu:bsp")
_RPU_LINKER_SCRIPT = Label("//sdk/bsp/rpu:linker_script")

def _platform_transition_impl(_settings, attr):
    return {"//command_line_option:platforms": attr.platform}

_platform_transition = transition(
    implementation = _platform_transition_impl,
    inputs = [],
    outputs = ["//command_line_option:platforms"],
)

def _firmware_target_impl(ctx):
    binary = ctx.executable.binary
    output = ctx.actions.declare_file(ctx.label.name)
    ctx.actions.symlink(output = output, target_file = binary, is_executable = True)
    return [DefaultInfo(
        files = depset([output]),
        executable = output,
        runfiles = ctx.runfiles(files = [output]),
    )]

_firmware_target = rule(
    implementation = _firmware_target_impl,
    attrs = {
        "binary": attr.label(
            cfg = _platform_transition,
            executable = True,
            mandatory = True,
        ),
        "platform": attr.string(mandatory = True),
        "_allowlist_function_transition": attr.label(
            default = "@bazel_tools//tools/allowlists/function_transition_allowlist",
        ),
    },
    executable = True,
)

def _metadata(visibility, tags, testonly):
    result = {}
    if visibility != None:
        result["visibility"] = visibility
    if tags != None:
        result["tags"] = tags
    if testonly != None:
        result["testonly"] = testonly
    return result

def a53_firmware(
        name,
        srcs,
        deps = [],
        bsp = None,
        linker_script = None,
        extra_copts = [],
        visibility = None,
        tags = None,
        testonly = None,
        **kwargs):
    """Builds an A53 ELF and exposes it through an A53 platform transition."""
    bsp = bsp or _APU_BSP
    linker_script = linker_script or _APU_LINKER_SCRIPT
    binary_name = name + "_elf"
    native.cc_binary(
        name = binary_name,
        srcs = srcs,
        additional_linker_inputs = [linker_script],
        copts = ["-ffreestanding", "-fno-builtin"] + extra_copts,
        linkopts = ["-T$(location %s)" % linker_script],
        deps = [bsp] + deps,
        tags = (tags or []) + ["manual"],
        visibility = ["//visibility:private"],
        **kwargs
    )
    _firmware_target(
        name = name,
        binary = ":" + binary_name,
        platform = str(_APU_PLATFORM),
        **_metadata(visibility, tags, testonly)
    )

def r5_firmware(
        name,
        srcs,
        deps = [],
        bsp = None,
        linker_script = None,
        extra_copts = [],
        visibility = None,
        tags = None,
        testonly = None,
        **kwargs):
    """Builds an R5 ELF and exposes it through an R5 platform transition."""
    bsp = bsp or _RPU_BSP
    linker_script = linker_script or _RPU_LINKER_SCRIPT
    binary_name = name + "_elf"
    native.cc_binary(
        name = binary_name,
        srcs = srcs,
        additional_linker_inputs = [linker_script],
        copts = [
            "-mcpu=cortex-r5",
            "-mfpu=vfpv3-d16",
            "-mfloat-abi=hard",
            "-marm",
            "-ffreestanding",
            "-fno-builtin",
        ] + extra_copts,
        linkopts = [
            "-T$(location %s)" % linker_script,
            "-nostartfiles",
            "--specs=nano.specs",
            "--specs=nosys.specs",
        ],
        deps = [bsp] + deps,
        tags = (tags or []) + ["manual"],
        visibility = ["//visibility:private"],
        **kwargs
    )
    _firmware_target(
        name = name,
        binary = ":" + binary_name,
        platform = str(_RPU_PLATFORM),
        **_metadata(visibility, tags, testonly)
    )
