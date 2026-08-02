"""Starlark helpers for building BOOT.BIN for the AES-ZUB."""

def _rpu_platform_transition_impl(_settings, _attr):
    return {"//command_line_option:platforms": "//platforms:rpu_r5_0"}

_rpu_platform_transition = transition(
    implementation = _rpu_platform_transition_impl,
    inputs = [],
    outputs = ["//command_line_option:platforms"],
)

def _r5_firmware_for_flash_impl(ctx):
    return [DefaultInfo(files = depset([ctx.file.elf]))]

r5_firmware_for_flash = rule(
    implementation = _r5_firmware_for_flash_impl,
    attrs = {
        "elf": attr.label(
            allow_single_file = True,
            cfg = _rpu_platform_transition,
            mandatory = True,
            doc = "R5 firmware ELF; built with the RPU platform transition.",
        ),
        "_allowlist_function_transition": attr.label(
            default = "@bazel_tools//tools/allowlists/function_transition_allowlist",
        ),
    },
    doc = "Expose an R5 firmware ELF built via platform transition for BOOT.BIN packaging.",
)
