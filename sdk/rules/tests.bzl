"""Public host-side checks for transition-aware firmware targets."""

_ELF_CHECKER = Label("//tooling/elf_check:elf_check")
_RPU_PLATFORM = Label("//sdk/platforms:rpu_r5_0")
_SIZE_TOOL = Label("@zub_cross_tools//:arm_none_eabi_size")

def _firmware_transition_impl(_settings, attr):
    return {"//command_line_option:platforms": attr.firmware_platform}

_firmware_transition = transition(
    implementation = _firmware_transition_impl,
    inputs = [],
    outputs = ["//command_line_option:platforms"],
)

def _elf_test_impl(ctx):
    launcher = ctx.actions.declare_file(ctx.label.name)
    required = " ".join(["--require-symbol " + s for s in ctx.attr.required_symbols])
    ctx.actions.write(
        launcher,
        """#!/usr/bin/env bash
set -euo pipefail
exec "$RUNFILES_DIR/zub_checker" --machine {machine} --entry {entry} \\
  --range {memory_range} {required} "$RUNFILES_DIR/zub_firmware"
""".format(
            machine = ctx.attr.expected_machine,
            entry = ctx.attr.expected_entry,
            memory_range = ctx.attr.expected_memory_range,
            required = required,
        ),
        is_executable = True,
    )
    return [DefaultInfo(
        executable = launcher,
        runfiles = ctx.runfiles(root_symlinks = {
            "zub_checker": ctx.executable.checker,
            "zub_firmware": ctx.executable.firmware,
        }),
    )]

_firmware_elf_test = rule(
    implementation = _elf_test_impl,
    attrs = {
        "checker": attr.label(default = _ELF_CHECKER, executable = True, cfg = "exec"),
        "firmware": attr.label(cfg = _firmware_transition, executable = True, mandatory = True),
        "firmware_platform": attr.string(mandatory = True),
        "expected_machine": attr.string(mandatory = True),
        "expected_entry": attr.string(mandatory = True),
        "expected_memory_range": attr.string(mandatory = True),
        "required_symbols": attr.string_list(),
        "_allowlist_function_transition": attr.label(
            default = "@bazel_tools//tools/allowlists/function_transition_allowlist",
        ),
    },
    test = True,
)

def firmware_elf_test(
        name,
        firmware,
        expected_machine,
        expected_entry,
        expected_memory_range,
        required_symbols = [],
        firmware_platform = None,
        **kwargs):
    _firmware_elf_test(
        name = name,
        firmware = firmware,
        firmware_platform = str(firmware_platform or _RPU_PLATFORM),
        expected_machine = expected_machine,
        expected_entry = expected_entry,
        expected_memory_range = expected_memory_range,
        required_symbols = required_symbols,
        **kwargs
    )

def _size_test_impl(ctx):
    launcher = ctx.actions.declare_file(ctx.label.name)
    ctx.actions.write(
        launcher,
        """#!/usr/bin/env bash
set -euo pipefail
sizes=$("$RUNFILES_DIR/zub_size" "$RUNFILES_DIR/zub_firmware" | tail -1)
text=$(echo "$sizes" | awk '{{print $1}}')
bss=$(echo "$sizes" | awk '{{print $3}}')
test "$text" -le {text_limit} || {{ echo ".text $text exceeds {text_limit}" >&2; exit 1; }}
test "$bss" -le {bss_limit} || {{ echo ".bss $bss exceeds {bss_limit}" >&2; exit 1; }}
""".format(text_limit = ctx.attr.text_limit_bytes, bss_limit = ctx.attr.bss_limit_bytes),
        is_executable = True,
    )
    return [DefaultInfo(
        executable = launcher,
        runfiles = ctx.runfiles(root_symlinks = {
            "zub_size": ctx.executable.size_tool,
            "zub_firmware": ctx.executable.firmware,
        }),
    )]

_firmware_size_test = rule(
    implementation = _size_test_impl,
    attrs = {
        "firmware": attr.label(cfg = _firmware_transition, executable = True, mandatory = True),
        "firmware_platform": attr.string(mandatory = True),
        "size_tool": attr.label(default = _SIZE_TOOL, executable = True, cfg = "exec"),
        "text_limit_bytes": attr.int(mandatory = True),
        "bss_limit_bytes": attr.int(mandatory = True),
        "_allowlist_function_transition": attr.label(
            default = "@bazel_tools//tools/allowlists/function_transition_allowlist",
        ),
    },
    test = True,
)

def firmware_size_test(name, firmware, text_limit_bytes, bss_limit_bytes, firmware_platform = None, **kwargs):
    _firmware_size_test(
        name = name,
        firmware = firmware,
        firmware_platform = str(firmware_platform or _RPU_PLATFORM),
        text_limit_bytes = text_limit_bytes,
        bss_limit_bytes = bss_limit_bytes,
        **kwargs
    )

def _onboard_test_impl(ctx):
    launcher = ctx.actions.declare_file(ctx.label.name)
    ctx.actions.write(launcher, "#!/usr/bin/env bash\nexec \"$RUNFILES_DIR/zub_test_script\" \"$@\"\n", is_executable = True)
    runfiles = ctx.runfiles(
        files = [f for d in ctx.attr.data for f in d[DefaultInfo].files.to_list()],
        symlinks = {"zub_firmware": ctx.executable.firmware},
        root_symlinks = {"zub_test_script": ctx.file.test_script},
    )
    return [DefaultInfo(executable = launcher, runfiles = runfiles)]

_onboard_firmware_test = rule(
    implementation = _onboard_test_impl,
    attrs = {
        "test_script": attr.label(allow_single_file = True, mandatory = True),
        "firmware": attr.label(cfg = _firmware_transition, executable = True, mandatory = True),
        "firmware_platform": attr.string(mandatory = True),
        "data": attr.label_list(allow_files = True),
        "_allowlist_function_transition": attr.label(
            default = "@bazel_tools//tools/allowlists/function_transition_allowlist",
        ),
    },
    test = True,
)

def onboard_firmware_test(name, test_script, firmware, data = [], firmware_platform = None, tags = None, **kwargs):
    _onboard_firmware_test(
        name = name,
        test_script = test_script,
        firmware = firmware,
        firmware_platform = str(firmware_platform or _RPU_PLATFORM),
        data = data,
        tags = tags or ["manual", "exclusive", "requires-hardware", "local"],
        **kwargs
    )
