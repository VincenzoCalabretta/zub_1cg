"""Supported Starlark API for zub_1cg consumers."""

load(":boot_image.bzl", _zub_boot_image = "zub_boot_image")
load(":firmware.bzl", _a53_firmware = "a53_firmware", _m3_firmware = "m3_firmware", _r5_firmware = "r5_firmware")
load(":tests.bzl", _firmware_elf_test = "firmware_elf_test", _firmware_size_test = "firmware_size_test", _onboard_firmware_test = "onboard_firmware_test")

a53_firmware = _a53_firmware
r5_firmware = _r5_firmware
m3_firmware = _m3_firmware
zub_boot_image = _zub_boot_image
firmware_elf_test = _firmware_elf_test
firmware_size_test = _firmware_size_test
onboard_firmware_test = _onboard_firmware_test
