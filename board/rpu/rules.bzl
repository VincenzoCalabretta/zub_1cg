"""Starlark helpers for Cortex-R5F firmware targets on AES-ZUB.

Usage in an app BUILD.bazel:

    load("//board/rpu:rules.bzl", "r5_binary")

    r5_binary(
        name = "my_app",
        srcs  = ["main.c"],
        deps  = ["//my_lib"],
        visibility = ["//visibility:public"],
    )

For tests that do not use ThreadX, pass bsp = "//board/rpu:bsp_bare":

    r5_binary(
        name = "bsp_test",
        srcs  = ["main.c"],
        bsp   = "//board/rpu:bsp_bare",
        deps  = ["//board:test_proto"],
        visibility = ["//visibility:public"],
    )
"""

# CPU/ABI flags shared by the BSP and every firmware binary.
# Keep in sync with _R5F_COPTS in board/rpu/BUILD.bazel.
R5F_COPTS = [
    "-mcpu=cortex-r5",
    "-mfpu=vfpv3-d16",
    "-mfloat-abi=hard",
    "-marm",
    "-ffreestanding",
    "-fno-builtin",
]

def r5_binary(name, srcs, deps = [], bsp = "//board/rpu:bsp", extra_copts = [], **kwargs):
    """Build a Cortex-R5F firmware binary with the standard R5F ABI and linker script.

    Args:
        name:        target name
        srcs:        C/assembly source files
        deps:        additional dependencies (BSP is added automatically)
        bsp:         BSP library label; defaults to //board/rpu:bsp (ThreadX-capable).
                     Use //board/rpu:bsp_bare for tests without ThreadX.
        extra_copts: additional compiler flags appended after R5F_COPTS
        **kwargs:    forwarded to cc_binary (e.g. visibility, tags)
    """
    native.cc_binary(
        name = name,
        srcs = srcs,
        additional_linker_inputs = ["//board/rpu:linker_script"],
        copts = R5F_COPTS + extra_copts,
        linkopts = [
            "-T$(location //board/rpu:linker_script)",
            "-nostartfiles",
            "--specs=nano.specs",
            "--specs=nosys.specs",
        ],
        deps = [bsp] + deps,
        **kwargs
    )
