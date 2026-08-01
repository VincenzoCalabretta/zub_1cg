"""cc_toolchain_config for aarch64-none-elf-gcc targeting Cortex-A53.

Tool paths and built-in include directories are captured at Bazel fetch time
by //toolchains/aarch64_none_elf:env_ext.bzl, which reads AARCH64_GCC_BIN from
the Nix devShell environment.

The default link flags pull in the vendored Xilinx BSP (libxil, libxilstandalone,
libxiltimer) plus Xilinx.spec so ELFs link against the ZynqMP BSP without each
binary needing to repeat these flags. BSP header inclusion still goes through
the normal cc_library dep on //third_party/xilinx_bsp:headers.
"""

load(
    "@bazel_tools//tools/cpp:cc_toolchain_config_lib.bzl",
    "feature",
    "flag_group",
    "flag_set",
    "tool_path",
)
load("@bazel_tools//tools/build_defs/cc:action_names.bzl", "ACTION_NAMES")
load(
    "@aarch64_gcc_config//:defs.bzl",
    "AARCH64_GCC_BIN",
    "AARCH64_GCC_INCLUDE_DIRS",
    "BSP_LIB_DIR",
    "BSP_SPEC",
)

_ALL_COMPILE_ACTIONS = [
    ACTION_NAMES.assemble,
    ACTION_NAMES.preprocess_assemble,
    ACTION_NAMES.linkstamp_compile,
    ACTION_NAMES.c_compile,
    ACTION_NAMES.cpp_compile,
    ACTION_NAMES.cpp_header_parsing,
    ACTION_NAMES.cpp_module_compile,
    ACTION_NAMES.cpp_module_codegen,
    ACTION_NAMES.lto_backend,
    ACTION_NAMES.clif_match,
]

def _impl(ctx):
    tp = AARCH64_GCC_BIN + "/aarch64-none-elf-"
    tool_paths = [
        tool_path(name = "gcc",      path = tp + "gcc"),
        tool_path(name = "g++",      path = tp + "g++"),
        tool_path(name = "ar",       path = tp + "ar"),
        tool_path(name = "ld",       path = tp + "ld"),
        tool_path(name = "nm",       path = tp + "nm"),
        tool_path(name = "objcopy",  path = tp + "objcopy"),
        tool_path(name = "objdump",  path = tp + "objdump"),
        tool_path(name = "strip",    path = tp + "strip"),
        tool_path(name = "gcov",     path = tp + "gcov"),
        tool_path(name = "cpp",      path = tp + "cpp"),
        tool_path(name = "dwp",      path = "/usr/bin/false"),
        tool_path(name = "llvm-cov", path = "/usr/bin/false"),
    ]

    default_compile_flags = feature(
        name = "default_compile_flags",
        enabled = True,
        flag_sets = [flag_set(
            actions = _ALL_COMPILE_ACTIONS,
            flag_groups = [flag_group(flags = [
                "-O2",
                "-g",
                "-Wall",
                "-DSDT",
            ])],
        )],
    )

    # Pull in the Xilinx BSP: libxil (peripheral drivers incl. GEM/UART/GIC),
    # libxilstandalone (standalone platform runtime), libxiltimer (portable
    # timer abstraction). --start-group / --end-group resolves circular refs
    # between the three archives.
    default_link_flags = feature(
        name = "default_link_flags",
        enabled = True,
        flag_sets = [flag_set(
            actions = [ACTION_NAMES.cpp_link_executable],
            flag_groups = [flag_group(flags = [
                "-specs=" + BSP_SPEC,
                "-L" + BSP_LIB_DIR,
                "-Wl,--start-group",
                "-lxil",
                "-lxilstandalone",
                "-lxiltimer",
                "-lgcc",
                "-lc",
                "-Wl,--end-group",
            ])],
        )],
    )

    no_pic     = feature(name = "supports_pic",            enabled = False)
    no_dynamic = feature(name = "supports_dynamic_linker", enabled = False)

    return cc_common.create_cc_toolchain_config_info(
        ctx = ctx,
        toolchain_identifier       = "aarch64-none-elf",
        host_system_name           = "x86_64-linux-gnu",
        target_system_name         = "aarch64-none-elf",
        target_cpu                 = "aarch64",
        target_libc                = "none",
        compiler                   = "gcc",
        abi_version                = "none",
        abi_libc_version           = "none",
        tool_paths                 = tool_paths,
        cxx_builtin_include_directories = AARCH64_GCC_INCLUDE_DIRS,
        features = [
            default_compile_flags,
            default_link_flags,
            no_pic,
            no_dynamic,
        ],
    )

aarch64_none_elf_toolchain_config = rule(
    implementation = _impl,
    attrs = {},
    provides = [CcToolchainConfigInfo],
)
