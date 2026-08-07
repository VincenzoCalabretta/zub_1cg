"""Captures SDK command-line tools as declared Bazel executables."""

def _find(ctx, environment, program):
    configured = ctx.os.environ.get(environment, "").strip()
    if configured:
        return configured
    found = ctx.which(program)
    return str(found) if found else ""

def _wrapper(ctx, name, executable):
    if executable:
        body = "#!/usr/bin/env bash\nexec %s \"$@\"\n" % executable
    else:
        body = "#!/usr/bin/env bash\necho '%s is unavailable; enter the zub_1cg Nix shell or set its SDK environment variable' >&2\nexit 127\n" % name
    ctx.file(name, body, executable = True)

def _cross_tools_impl(ctx):
    arm_bin = ctx.os.environ.get("ARM_GCC_BIN", "").strip()
    a64_bin = ctx.os.environ.get("AARCH64_GCC_BIN", "").strip()
    _wrapper(ctx, "arm-none-eabi-objcopy", (arm_bin + "/arm-none-eabi-objcopy") if arm_bin else str(ctx.which("arm-none-eabi-objcopy") or ""))
    _wrapper(ctx, "arm-none-eabi-size", (arm_bin + "/arm-none-eabi-size") if arm_bin else str(ctx.which("arm-none-eabi-size") or ""))
    _wrapper(ctx, "aarch64-none-elf-objcopy", (a64_bin + "/aarch64-none-elf-objcopy") if a64_bin else str(ctx.which("aarch64-none-elf-objcopy") or ""))
    _wrapper(ctx, "aarch64-none-elf-gcc", (a64_bin + "/aarch64-none-elf-gcc") if a64_bin else str(ctx.which("aarch64-none-elf-gcc") or ""))
    ctx.file("BUILD.bazel", """
sh_binary(name = "arm_none_eabi_objcopy", srcs = ["arm-none-eabi-objcopy"], visibility = ["//visibility:public"])
sh_binary(name = "arm_none_eabi_size", srcs = ["arm-none-eabi-size"], visibility = ["//visibility:public"])
sh_binary(name = "aarch64_none_elf_objcopy", srcs = ["aarch64-none-elf-objcopy"], visibility = ["//visibility:public"])
sh_binary(name = "aarch64_none_elf_gcc", srcs = ["aarch64-none-elf-gcc"], visibility = ["//visibility:public"])
""")

_cross_tools = repository_rule(
    implementation = _cross_tools_impl,
    environ = ["ARM_GCC_BIN", "AARCH64_GCC_BIN"],
    local = True,
)

def _host_tools_impl(ctx):
    _wrapper(ctx, "bootgen.sh", _find(ctx, "BOOTGEN", "bootgen"))
    _wrapper(ctx, "openocd.sh", _find(ctx, "OPENOCD", "openocd"))
    ctx.file("BUILD.bazel", """
sh_binary(name = "bootgen", srcs = ["bootgen.sh"], visibility = ["//visibility:public"])
sh_binary(name = "openocd", srcs = ["openocd.sh"], visibility = ["//visibility:public"])
""")

_host_tools = repository_rule(
    implementation = _host_tools_impl,
    environ = ["BOOTGEN", "OPENOCD"],
    local = True,
)

def _tools_ext_impl(_ctx):
    _cross_tools(name = "zub_cross_tools")
    _host_tools(name = "zub_host_tools")

tools_ext = module_extension(implementation = _tools_ext_impl)
