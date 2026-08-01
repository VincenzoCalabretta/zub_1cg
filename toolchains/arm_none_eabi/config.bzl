"""cc_toolchain_config for arm-none-eabi-gcc targeting Cortex-R5F.

The compiler prefix is captured at Bazel fetch time by env_ext.bzl.
"""

load("@arm_gcc_config//:defs.bzl", "ARM_GCC_BIN", "ARM_GCC_INCLUDE_DIRS")
load("@bazel_tools//tools/build_defs/cc:action_names.bzl", "ACTION_NAMES")
load(
    "@bazel_tools//tools/cpp:cc_toolchain_config_lib.bzl",
    "feature",
    "flag_group",
    "flag_set",
    "tool_path",
)

_ALL_COMPILE_ACTIONS = [
    ACTION_NAMES.c_compile,
    ACTION_NAMES.cpp_compile,
    ACTION_NAMES.assemble,
    ACTION_NAMES.preprocess_assemble,
    ACTION_NAMES.cpp_header_parsing,
    ACTION_NAMES.cpp_module_compile,
    ACTION_NAMES.cpp_module_codegen,
]

_ALL_LINK_ACTIONS = [
    ACTION_NAMES.cpp_link_executable,
    ACTION_NAMES.cpp_link_dynamic_library,
    ACTION_NAMES.cpp_link_nodeps_dynamic_library,
]

_GCC_PREFIX = ARM_GCC_BIN + "/arm-none-eabi-"

# Cortex-R5F: hardware FP (VFPv3-D16), ARM state (ThreadX port requires ARM,
# not Thumb).
_CPU_FLAGS = [
    "-mcpu=cortex-r5",
    "-mfpu=vfpv3-d16",
    "-mfloat-abi=hard",
    "-marm",
]

def _impl(ctx):
    tool_paths = [
        tool_path(name = "gcc", path = _GCC_PREFIX + "gcc"),
        tool_path(name = "g++", path = _GCC_PREFIX + "g++"),
        tool_path(name = "ld", path = _GCC_PREFIX + "ld"),
        tool_path(name = "ar", path = _GCC_PREFIX + "ar"),
        tool_path(name = "cpp", path = _GCC_PREFIX + "cpp"),
        tool_path(name = "gcov", path = _GCC_PREFIX + "gcov"),
        tool_path(name = "nm", path = _GCC_PREFIX + "nm"),
        tool_path(name = "objdump", path = _GCC_PREFIX + "objdump"),
        tool_path(name = "strip", path = _GCC_PREFIX + "strip"),
        tool_path(name = "dwp", path = "/usr/bin/false"),
    ]

    default_compile = feature(
        name = "default_compile_flags",
        enabled = True,
        flag_sets = [flag_set(
            actions = _ALL_COMPILE_ACTIONS,
            flag_groups = [flag_group(flags = _CPU_FLAGS + [
                "-Wall",
                "-Wextra",
                "-ffunction-sections",
                "-fdata-sections",
            ])],
        )],
    )

    default_link = feature(
        name = "default_link_flags",
        enabled = True,
        flag_sets = [flag_set(
            actions = _ALL_LINK_ACTIONS,
            flag_groups = [flag_group(flags = _CPU_FLAGS + [
                "-Wl,--gc-sections",
                "-Wl,-Map,output.map",
            ])],
        )],
    )

    no_pic = feature(name = "supports_pic", enabled = False)
    no_dynamic = feature(name = "supports_dynamic_linker", enabled = False)

    return cc_common.create_cc_toolchain_config_info(
        ctx = ctx,
        features = [default_compile, default_link, no_pic, no_dynamic],
        cxx_builtin_include_directories = ARM_GCC_INCLUDE_DIRS,
        toolchain_identifier = "arm-none-eabi-cortex-r5",
        host_system_name = "x86_64-linux-gnu",
        target_system_name = "arm-none-eabi",
        target_cpu = "cortex-r5",
        target_libc = "none",
        compiler = "gcc",
        abi_version = "none",
        abi_libc_version = "none",
        tool_paths = tool_paths,
    )

arm_none_eabi_toolchain_config = rule(
    implementation = _impl,
    attrs = {},
    provides = [CcToolchainConfigInfo],
)
