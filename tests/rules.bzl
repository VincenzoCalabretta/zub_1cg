"""Bazel rules for host-side tests that consume cross-compiled firmware."""

def _firmware_platform_transition_impl(_settings, attr):
    return {"//command_line_option:platforms": attr.firmware_platform}

_firmware_platform_transition = transition(
    implementation = _firmware_platform_transition_impl,
    inputs = [],
    outputs = ["//command_line_option:platforms"],
)

def _onboard_firmware_test_impl(ctx):
    launcher = ctx.actions.declare_file(ctx.label.name)
    ctx.actions.write(
        output = launcher,
        content = """#!/usr/bin/env bash
set -euo pipefail
exec \"$TEST_SRCDIR/${{TEST_WORKSPACE:-_main}}/{script}\" \"$@\"
""".format(script = ctx.file.test_script.short_path),
        is_executable = True,
    )

    files = [ctx.file.test_script, ctx.executable.firmware]
    for target in ctx.attr.data:
        files.extend(target[DefaultInfo].files.to_list())
    return [DefaultInfo(
        executable = launcher,
        runfiles = ctx.runfiles(files = files),
    )]

_onboard_firmware_test = rule(
    implementation = _onboard_firmware_test_impl,
    attrs = {
        "test_script": attr.label(allow_single_file = True, mandatory = True),
        "firmware": attr.label(
            cfg = _firmware_platform_transition,
            executable = True,
            mandatory = True,
        ),
        "firmware_platform": attr.string(mandatory = True),
        "data": attr.label_list(allow_files = True),
    },
    test = True,
)

def onboard_firmware_test(name, test_script, firmware, firmware_platform, data, tags):
    """Creates a host test with firmware built for its target-core platform."""
    _onboard_firmware_test(
        name = name,
        test_script = test_script,
        firmware = firmware,
        firmware_platform = firmware_platform,
        data = data,
        tags = tags,
    )

def _firmware_elf_test_impl(ctx):
    launcher = ctx.actions.declare_file(ctx.label.name)
    ctx.actions.write(
        output = launcher,
        content = """#!/usr/bin/env bash
set -euo pipefail
exec \"$TEST_SRCDIR/${{TEST_WORKSPACE:-_main}}/{checker}\" \\
     \"$TEST_SRCDIR/${{TEST_WORKSPACE:-_main}}/{firmware}\"
""".format(
            checker = ctx.executable.checker.short_path,
            firmware = ctx.executable.firmware.short_path,
        ),
        is_executable = True,
    )
    return [DefaultInfo(
        executable = launcher,
        runfiles = ctx.runfiles(files = [ctx.executable.checker, ctx.executable.firmware]),
    )]

_firmware_elf_test = rule(
    implementation = _firmware_elf_test_impl,
    attrs = {
        "checker": attr.label(
            executable = True,
            cfg = "exec",
            mandatory = True,
        ),
        "firmware": attr.label(
            cfg = _firmware_platform_transition,
            executable = True,
            mandatory = True,
        ),
        "firmware_platform": attr.string(mandatory = True),
    },
    test = True,
)

def firmware_elf_test(name, firmware, firmware_platform):
    """Validates one target-core firmware ELF on the host during presubmit."""
    _firmware_elf_test(
        name = name,
        checker = "//tools/elf_check:elf_check",
        firmware = firmware,
        firmware_platform = firmware_platform,
    )
