#!/usr/bin/env bash
# Build BOOT.BIN using the A53-loader shim (no Vitis FSBL required).
#
# Steps
#   1. Build the R5 hello_world ELF via Bazel.
#   2. Strip it to a raw binary.
#   3. Wrap the raw binary as an aarch64 relocatable .o.
#   4. Compile the A53 loader (startup.S + main.c + embedded R5 .o).
#   5. Run bootgen against scripts/flash/boot_a53.bif.
#   6. Optionally copy BOOT.BIN to an SD card (if a removable FAT partition is
#      auto-detected).
#
# Invoke from anywhere; the script cd's to the repo root.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$REPO_DIR"

R5_TARGET="//apps/rpu/hello_world"
R5_ELF="bazel-bin/apps/rpu/hello_world/hello_world"
BUILD_DIR="scripts/flash/build"
LOADER_ELF="$BUILD_DIR/a53_loader.elf"

mkdir -p "$BUILD_DIR"

echo "=== 1/6  Build R5 hello_world ==="
bazel build --config=rpu "$R5_TARGET"

echo ""
echo "=== 2/6  Convert R5 ELF → raw binary ==="
arm-none-eabi-objcopy -O binary "$R5_ELF" "$BUILD_DIR/hello_world.bin"
echo "Size: $(stat -c%s "$BUILD_DIR/hello_world.bin") bytes"

echo ""
echo "=== 3/6  Embed R5 binary in AArch64 relocatable ==="
# The resulting object exports:
#   _binary_hello_world_bin_start
#   _binary_hello_world_bin_end
# consumed by board/a53_loader/main.c.
( cd "$BUILD_DIR" && aarch64-none-elf-objcopy \
    -I binary -O elf64-littleaarch64 -B aarch64 \
    hello_world.bin hello_world_data.o )

echo ""
echo "=== 4/6  Link A53 loader ELF ==="
aarch64-none-elf-gcc \
    -ffreestanding -nostdlib -nostartfiles \
    -Wl,-T,board/a53_loader/memory.lds \
    -Wl,--gc-sections \
    -o "$LOADER_ELF" \
    board/a53_loader/startup.S \
    board/a53_loader/main.c \
    "$BUILD_DIR/hello_world_data.o"
aarch64-none-elf-size "$LOADER_ELF"

echo ""
echo "=== 5/6  Package BOOT.BIN via bootgen ==="
bootgen -image scripts/flash/boot_a53.bif -arch zynqmp -o BOOT.BIN -w on
ls -la BOOT.BIN

echo ""
echo "=== 6/6  Optional: copy to SD card ==="
SD_PART=""
for dev in /dev/sd?; do
    [ -e "$dev" ] || continue
    if lsblk -no HOTPLUG "$dev" 2>/dev/null | grep -q 1; then
        for part in ${dev}?; do
            [ -e "$part" ] || continue
            if lsblk -no FSTYPE "$part" 2>/dev/null | grep -q vfat; then
                SD_PART="$part"
                break 2
            fi
        done
    fi
done

if [ -z "$SD_PART" ]; then
    echo "No removable FAT partition found."
    echo "BOOT.BIN is at $REPO_DIR/BOOT.BIN — copy it to an SD card manually,"
    echo "set the boot switches to SD, and power-cycle the board."
else
    echo "Found SD card at $SD_PART"
    MOUNT=$(udisksctl mount -b "$SD_PART" 2>/dev/null | grep -oP 'at \K\S+' || true)
    if [ -n "$MOUNT" ]; then
        cp BOOT.BIN "$MOUNT"/BOOT.BIN
        sync
        udisksctl unmount -b "$SD_PART" >/dev/null 2>&1 || true
        echo "BOOT.BIN copied to $MOUNT and card unmounted."
    else
        echo "Could not mount $SD_PART — copy BOOT.BIN manually."
    fi
fi
