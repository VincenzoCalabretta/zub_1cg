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

def _firmware_size_test_impl(ctx):
    launcher = ctx.actions.declare_file(ctx.label.name)
    ctx.actions.write(
        output = launcher,
        content = """#!/usr/bin/env bash
set -euo pipefail
elf="$TEST_SRCDIR/${{TEST_WORKSPACE:-_main}}/{firmware}"
sizes=$(arm-none-eabi-size "$elf" | tail -1)
text=$(echo "$sizes" | awk '{{print $1}}')
bss=$(echo  "$sizes" | awk '{{print $3}}')
ok=1
if [ "$text" -gt {text_limit} ]; then
    echo "size_test: .text $text bytes > budget {text_limit}" >&2; ok=0
fi
if [ "$bss" -gt {bss_limit} ]; then
    echo "size_test: .bss  $bss  bytes > budget {bss_limit}" >&2; ok=0
fi
if [ "$ok" -eq 1 ]; then
    echo "size_test: {name} OK (.text=$text .bss=$bss)"
fi
exit $((1 - ok))
""".format(
            firmware = ctx.executable.firmware.short_path,
            name = ctx.label.name,
            text_limit = ctx.attr.text_limit_bytes,
            bss_limit = ctx.attr.bss_limit_bytes,
        ),
        is_executable = True,
    )
    return [DefaultInfo(
        executable = launcher,
        runfiles = ctx.runfiles(files = [ctx.executable.firmware]),
    )]

_firmware_size_test = rule(
    implementation = _firmware_size_test_impl,
    attrs = {
        "firmware": attr.label(
            cfg = _firmware_platform_transition,
            executable = True,
            mandatory = True,
        ),
        "firmware_platform": attr.string(mandatory = True),
        "text_limit_bytes": attr.int(mandatory = True),
        "bss_limit_bytes": attr.int(mandatory = True),
    },
    test = True,
)

def firmware_size_test(name, firmware, firmware_platform, text_limit_bytes, bss_limit_bytes):
    """Fails if the firmware's .text or .bss section exceeds the stated byte limits."""
    _firmware_size_test(
        name = name,
        firmware = firmware,
        firmware_platform = firmware_platform,
        text_limit_bytes = text_limit_bytes,
        bss_limit_bytes = bss_limit_bytes,
    )

def _firmware_elf_test_impl(ctx):
    launcher = ctx.actions.declare_file(ctx.label.name)
    ctx.actions.write(
        output = launcher,
        content = """#!/usr/bin/env bash
set -euo pipefail
exec \"$TEST_SRCDIR/${{TEST_WORKSPACE:-_main}}/{checker}\" \\
     --machine {machine} --entry {entry} --range {memory_range} {required_symbols} \\
     \"$TEST_SRCDIR/${{TEST_WORKSPACE:-_main}}/{firmware}\"
""".format(
            checker = ctx.executable.checker.short_path,
            machine = ctx.attr.expected_machine,
            entry = ctx.attr.expected_entry,
            memory_range = ctx.attr.expected_memory_range,
            required_symbols = " ".join(["--require-symbol " + symbol for symbol in ctx.attr.required_symbols]),
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
        "expected_machine": attr.string(mandatory = True),
        "expected_entry": attr.string(mandatory = True),
        "expected_memory_range": attr.string(mandatory = True),
        "required_symbols": attr.string_list(),
    },
    test = True,
)

def firmware_elf_test(name, firmware, firmware_platform, expected_machine, expected_entry, expected_memory_range, required_symbols):
    """Validates one target-core firmware ELF on the host during presubmit."""
    _firmware_elf_test(
        name = name,
        checker = "//tools/elf_check:elf_check",
        firmware = firmware,
        firmware_platform = firmware_platform,
        expected_machine = expected_machine,
        expected_entry = expected_entry,
        expected_memory_range = expected_memory_range,
        required_symbols = required_symbols,
    )
