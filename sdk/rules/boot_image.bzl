"""Declared BOOT.BIN construction for the AES-ZUB-1CG."""

_RPU_PLATFORM = Label("//sdk/platforms:rpu_r5_0")
_LOADER_MAIN = Label("//sdk/boards/zub_1cg/a53_loader:main.c")
_LOADER_STARTUP = Label("//sdk/boards/zub_1cg/a53_loader:startup.S")
_LOADER_LINKER = Label("//sdk/boards/zub_1cg/a53_loader:memory.lds")

def _rpu_transition_impl(_settings, _attr):
    return {"//command_line_option:platforms": str(_RPU_PLATFORM)}

_rpu_transition = transition(
    implementation = _rpu_transition_impl,
    inputs = [],
    outputs = ["//command_line_option:platforms"],
)

def _boot_image_impl(ctx):
    output = ctx.actions.declare_file(ctx.label.name + "/BOOT.BIN")
    command = """set -euo pipefail
work="$PWD/{name}.work"
root="$PWD"
mkdir -p "$work"
"{arm_objcopy}" -O binary "{firmware}" "$work/r5_firmware.bin"
(cd "$work" && "$root/{a64_objcopy}" -I binary -O elf64-littleaarch64 -B aarch64 r5_firmware.bin r5_firmware.o)
"{a64_gcc}" -ffreestanding -nostdlib -nostartfiles -Wl,-T,"{linker}" -Wl,--gc-sections \
  -o "$work/a53_loader.elf" "{startup}" "{main}" "$work/r5_firmware.o"
printf 'the_ROM_image: {{ [bootloader, destination_cpu=a53-0] %s }}\n' "$work/a53_loader.elf" > "$work/boot.bif"
"{bootgen}" -image "$work/boot.bif" -arch zynqmp -o "{output}" -w on
""".format(
        name = ctx.label.name,
        arm_objcopy = ctx.executable.arm_objcopy.path,
        a64_objcopy = ctx.executable.a64_objcopy.path,
        a64_gcc = ctx.executable.a64_gcc.path,
        bootgen = ctx.executable.bootgen.path,
        firmware = ctx.executable.r5_firmware.path,
        linker = ctx.file.loader_linker.path,
        startup = ctx.file.loader_startup.path,
        main = ctx.file.loader_main.path,
        output = output.path,
    )
    ctx.actions.run_shell(
        inputs = [ctx.executable.r5_firmware, ctx.file.loader_main, ctx.file.loader_startup, ctx.file.loader_linker],
        tools = [ctx.executable.arm_objcopy, ctx.executable.a64_objcopy, ctx.executable.a64_gcc, ctx.executable.bootgen],
        outputs = [output],
        command = command,
        mnemonic = "ZubBootImage",
    )
    return [DefaultInfo(files = depset([output]))]

_zub_boot_image = rule(
    implementation = _boot_image_impl,
    attrs = {
        "r5_firmware": attr.label(cfg = _rpu_transition, executable = True, mandatory = True),
        "loader_main": attr.label(default = _LOADER_MAIN, allow_single_file = True),
        "loader_startup": attr.label(default = _LOADER_STARTUP, allow_single_file = True),
        "loader_linker": attr.label(default = _LOADER_LINKER, allow_single_file = True),
        "arm_objcopy": attr.label(default = "@zub_cross_tools//:arm_none_eabi_objcopy", executable = True, cfg = "exec"),
        "a64_objcopy": attr.label(default = "@zub_cross_tools//:aarch64_none_elf_objcopy", executable = True, cfg = "exec"),
        "a64_gcc": attr.label(default = "@zub_cross_tools//:aarch64_none_elf_gcc", executable = True, cfg = "exec"),
        "bootgen": attr.label(default = "@zub_host_tools//:bootgen", executable = True, cfg = "exec"),
        "_allowlist_function_transition": attr.label(
            default = "@bazel_tools//tools/allowlists/function_transition_allowlist",
        ),
    },
)

def zub_boot_image(name, r5_firmware, **kwargs):
    _zub_boot_image(name = name, r5_firmware = r5_firmware, **kwargs)
