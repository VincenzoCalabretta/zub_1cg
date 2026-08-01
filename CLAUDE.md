# zub_1cg — Hardware notes

## Board

- **Hardware:** Avnet AES-ZUB-1CG-ED-G (Zynq UltraScale+ ZU1CG)
- **Boot mode (bring-up):** JTAG — all DIP switches OFF (BOOT_MODE=0000)
- **USB:** FT2232H → `ttyUSB0` = JTAG (OpenOCD), `ttyUSB1` = UART console

## Two boot targets

This mainline covers both cores:

| Core | Cross-toolchain | Bazel platform | Config |
|---|---|---|---|
| APU (Cortex-A53) | aarch64-none-elf | `//platforms:apu_a53` | `--config=apu` |
| RPU (Cortex-R5F) | arm-none-eabi | `//platforms:rpu_r5_0` | `--config=rpu` |

## R5 memory map (runtime)

| Region | R5 address | AXI address | Notes |
|---|---|---|---|
| OCM (code + data + BSS) | 0x00000000–0x000042C8 | 0xFFFC0000–0xFFFC42C8 | via OCM remap |
| Stack (grows down) | 0x00010000–0x00020000 | 0xFFFC0000+offset | all modes |
| UART1 | 0xFF010000 | 0xFF010000 | physical, not remapped |

ELF is linked at VMA `0x00000000` and written into OCM at AXI `0xFFFC0000`.
OCM remap register (`0xFF960000` bit 0 = 1) makes R5 address `0x00000000` hit OCM.

## Key hardware facts (R5)

- **UART1 ref clock:** IOPLL (FBDIV=50 × 33.333 MHz / DIV0=24) = 69.44 MHz
  → CD=43, BDIV=13 → 115200 baud (0.1% error).
- **RPU_GLBL_CNTL** (`0xFF9A0000`) power-on default = `0x00000050`.
  bit[3] SLSPLIT=0 = lock-step mode — **must write 0x8 (split) while R5 is
  in reset**.
- **XPPU rule:** CRL_APB and RPU registers are writable from JTAG only while
  R5 is held in module reset (`RST_LPD_TOP` bit0=1). After R5 is released
  they are locked. **Power-cycle is the only reset.**

## OpenOCD scripts

| Script | Purpose |
|---|---|
| `scripts/openocd/aes_zub.cfg` | FT2232H interface + ZU+ TAP chain + STICKYERR clear on init |
| `scripts/openocd/load_r5.tcl` | Full boot: OCM remap → RPU config → UART init → ELF load → R5 release |
| `scripts/openocd/scan_aps.tcl` | Safe AP scan (run before adding any cortex_r4 target) |
| `scripts/openocd/check_xmpu.tcl` | Dump XMPU_OCM registers |
| `scripts/openocd/check_stickyerr.tcl` | Read DAP CTRL/STAT to check STICKYERR bit |

**Never add a `cortex_r4` target to `aes_zub.cfg`** without first finding
the correct AP number via `scan_aps.tcl`. A wrong AP number sets
STICKYERR and silently corrupts all subsequent AXI reads/writes until the
next power cycle.

## Diagnosing a failed R5 run

1. Check OpenOCD log: `RPU_GLBL_CNTL = 0x00000008` and `UART1_SR` TX_EMPTY set.
2. If UART shows "BOOT" but no "Hello": R5 executing but stuck — check BSS:
   ```
   read_memory 0xFFFC2BCC 32 4   # should be all zeros once startup.S clears BSS
   ```
3. If nothing on UART: verify MIO routing and UART1 clock.
4. If all CRL_APB writes fail: power-cycle the board (XPPU lockout from
   previous run).

## Testing without picocom

`zub_ctl serial-watch --tty /dev/ttyUSB1 --expect '...' --timeout 30`
is a scriptable replacement for `picocom` that emits each line prefixed
with `[SERIAL] ` and exits 0 / 1 based on regex matches. Used by the
`//tests/…` sh_tests.

## Regenerating board artifacts

`board/zub_1cg/design_1_wrapper.bit` and `psu_init.tcl` are opaque blobs
regenerated in the Xilinx workflow:

1. Open the Vivado project (`../vivado_workspace/zub_hello_world_ethernet/`).
2. Generate bitstream → export hardware handoff (`.xsa`).
3. Vitis: platform from `.xsa` → extract `psu_init.tcl` and the `.bit`.
4. Copy both into `board/zub_1cg/`.
