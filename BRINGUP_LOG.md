# ThreadX Hello World — AES-ZUB Bring-up Log

Every command, every attempt, every fix is recorded here in chronological
order. Timestamps are in CEST (UTC+2), 2026-06-26.

---

## Iteration 1 — Environment audit  `22:11`

**Goal:** take stock of what is and is not available before touching any code.

### Results

| Item | Status | Notes |
|------|--------|-------|
| Bazel | ✅ 8.4.2 at `/usr/local/bin/bazel` | |
| arm-none-eabi-gcc | ❌ not installed | `extra/arm-none-eabi-gcc 16.1.0-1` available in pacman |
| bootgen | ❌ not installed | PATH entry `/tools/Xilinx/Vitis/2022.1/bin` is stale — directory does not exist |
| xsct / hw_server | ❌ not installed | same stale PATH |
| Board USB-UART | ❌ no ttyUSB/ttyACM | only ttyS0–31 (native UARTs) visible |
| SD card reader | ✅ `/dev/sda`, `/dev/sdb` | Generic USB3.0 Card Reader enumerated at 22:03 |
| sudo | ⚠️ requires password | user `v` in `wheel` group — can use `! sudo …` in terminal |
| udisksctl | ✅ available | can mount/unmount without sudo |

### USB device inventory

```
bus/003/002  SunplusIT FHD_Webcam
bus/003/005  Cooler Master MM710 Gaming Mouse
bus/003/006  Kinesis Adv360 keyboard
bus/003/045  Generic USB3.0 Card Reader  ← SD card for boot image
bus/004/040  GenesysLogic USB3.1 Hub
```

No Digilent/FTDI device (JTAG-UART) visible yet — board USB cable not plugged
in, or ttyUSB driver not loaded.

### Blockers identified

1. **`arm-none-eabi-gcc` missing** → user must `sudo pacman -S arm-none-eabi-gcc arm-none-eabi-newlib`
2. **`bootgen` missing** → will build from source (`github.com/Xilinx/bootgen`)
3. **FSBL missing** (`prebuilt/fsbl_r5.elf`) → must be built in Vitis or provided by user
4. **Board serial port not visible** → USB-UART cable likely not yet connected

### Action taken

- Started building bootgen from source (see Iteration 2)
- Documented blockers 1, 3, 4 as user actions required

---

## Iteration 2 — Build `bootgen` from source  `22:12`

**Rationale:** Xilinx published bootgen as MIT-licensed open source at
`github.com/Xilinx/bootgen`.  No sudo required — just `git clone && make`.

### Commands run

```bash
cd /home/v/projects
git clone --depth=1 https://github.com/Xilinx/bootgen.git
cd bootgen
# GCC 14+ treats -Wincompatible-pointer-types as error — suppress it:
make -j$(nproc) CFLAGS="-O -Wall -Wno-incompatible-pointer-types" \
                CXXFLAGS="-std=c++14 -O -Wall -Wno-reorder -Wno-deprecated-declarations \
                           -Wno-aligned-new -Wno-misleading-indentation -Wno-class-memaccess"
```

### Result

```
****** Bootgen v2026.1
```

Binary at `/home/v/projects/bootgen/build/bin/bootgen`.
Symlinked to `/home/v/bin/bootgen`.

---

## Iteration 3 — Bazel BUILD fixes  `22:14`

### Problem 1: wrong MODULE.bazel dependency versions

Bazel resolved `rules_cc@0.1.1` and `platforms@0.0.11` but MODULE.bazel
specified older versions → warnings during analysis.

**Fix:** Updated MODULE.bazel to `rules_cc = "0.1.1"`, `platforms = "0.0.11"`.

### Problem 2: missing root BUILD.bazel

`extensions.bzl` is referenced as `//:extensions.bzl` but the root package
had no BUILD file.

**Fix:** Created `BUILD.bazel` at repo root (empty, just marks the package).

### Problem 3: wrong ThreadX tag

`v6.4.1` does not exist.  Actual tags use the format `v{ver}_{date}_rel`.

**Fix:** Updated `extensions.bzl` to `v6.4.3.202503_rel`.

### Problem 4: port has no .c files

The glob `ports/cortex_r5/gnu/src/*.c` matched nothing → error.
The R5F GNU port contains ONLY `.S` files.

**Fix:** Removed `*.c` from port glob in `third_party/threadx/BUILD.bazel`.

### Problem 5: `common/inc/tx_port.h` does not exist

`tx_port.h` lives in `ports/cortex_r5/gnu/inc/`, not in `common/inc/`.

**Fix:** Changed `hdrs` to `glob(["common/inc/*.h", "ports/cortex_r5/gnu/inc/*.h"])`.

### Problem 6: filegroup name collision with source file

Bazel 8 reports a self-edge cycle when a `filegroup` is named `memory.lds`
(same as the file it contains).

**Fix:** Renamed target to `linker_script`; updated reference in
`apps/hello_world/BUILD.bazel`.

### Problem 7: wrong IRQ dispatch model in startup.S

By reading the actual ThreadX `tx_thread_context_save.S` and the official
`example_build/` files, the correct pattern is:

```asm
__tx_irq_handler:
    B  _tx_thread_context_save        @ ThreadX saves context
__tx_irq_processing_return:           @ ThreadX branches back HERE
    BL IRQHandler                     @ our C GIC dispatch
    B  _tx_thread_context_restore
```

`__tx_irq_processing_return` must be globally visible because
`_tx_thread_context_save.S` branches to it with `B __tx_irq_processing_return`.

**Fix:** Rewrote `startup.S` with correct entry/processing labels.  Added all
expected ThreadX exception symbols (`__tx_undefined`, `__tx_swi_interrupt`,
`__tx_prefetch_handler`, `__tx_abort_handler`, `__tx_reserved_handler`).

### Problem 8: `_tx_initialize_low_level` incomplete

ThreadX expects this function to set:
- `_tx_thread_system_stack_ptr` — system stack top (idle scheduler loop)
- `_tx_initialize_unused_memory` — first free RAM byte (for pool allocation)

**Fix:** Updated `board/zu_r5/timer.c` to assign both from linker symbols.

### Build status after all fixes

```
INFO: Analyzed target //apps/hello_world:hello_world (2 packages loaded, 10 targets configured)
ERROR: execvp(/usr/bin/arm-none-eabi-gcc): No such file or directory
```

**Analysis phase passes completely.**  The only remaining failure is the
missing cross-compiler binary — no more BUILD or logic errors.

---

## Iteration 4 — Waiting for user actions  `22:16`

Three things are needed from the user before the build can proceed:

### ACTION 1 (blocking) — Install arm-none-eabi toolchain

```bash
# Run in your terminal:
sudo pacman -S arm-none-eabi-gcc arm-none-eabi-newlib --noconfirm
```

After install, verify: `arm-none-eabi-gcc --version`

### ACTION 2 (blocking for flash) — Insert SD card

The USB3.0 card reader (`/dev/sda`, `/dev/sdb`) is attached but both
slots show size = 0 (no card inserted).  Insert an SD card ≥ 64 MB.

### ACTION 3 (blocking for UART monitor) — Connect board USB cable

No ttyUSB/ttyACM device is visible.  Connect the USB-UART cable from
the board's UART/JTAG header to the host.

### ACTION 4 (blocking for flash) — Provide FSBL ELF

`prebuilt/fsbl_r5.elf` is missing.  Build it in Vitis (New Application
Project → Zynq MP FSBL → destination cpu r5-0) or locate a pre-built
one from Avnet's board BSP.

---

## Iteration 5 — Nix hermetic devShell  `23:05`

**Goal:** provide all tools (cross-compiler, Bazel, bootgen, tio, …) via a Nix
flake so no host tools are ever used during the build.

### Files written

| File | Purpose |
|------|---------|
| `flake.nix` | Hermetic devShell using nixpkgs-unstable (for bazel_8); bootgen built from source |
| `toolchain/gcc_env_ext.bzl` | Repository rule + module extension: captures `ARM_GCC_BIN` from Nix shell |
| `toolchain/arm_none_eabi.bzl` | Updated: loads compiler path from `@arm_gcc_config` (no hardcoded host paths) |
| `MODULE.bazel` | Added `arm_gcc_ext` module extension |
| `.bazelrc` | Added `--action_env=ARM_GCC_BIN`, `--action_env=PATH`, `--sandbox_add_mount_pair=/nix` |
| `.gitignore` | Excludes `.bazelrc.user`, `BOOT.BIN`, `bazel-*`, `result*` |

### Issues fixed during Nix integration

**Problem 1:** `bootgen` derivation failed with:
- OpenSSL 3.x deprecates `SHA256_Init` / `RSA_free` → warnings treated as errors in g++
- `cdo-load.c` has `char*`↔`uint32_t*` cast, GCC 14 promotes to error
- The bootgen Makefile's rules don't all respect `makeFlags` from Nix

**Fix:** Use `env.NIX_CFLAGS_COMPILE` which Nix's cc-wrapper injects into every
compiler invocation regardless of what the Makefile does:
```nix
env.NIX_CFLAGS_COMPILE = "-Wno-deprecated-declarations -Wno-incompatible-pointer-types";
```

**Problem 2:** shellHook tried to write `.bazelrc.user` to
`${builtins.toString ./.}` which evaluates to the read-only Nix store path
when the flake is evaluated from outside the project directory.

**Fix:** Use `$PWD` in the shellHook (shell runtime, not Nix eval time):
```bash
cat > "$PWD/.bazelrc.user" <<EOF
build --action_env=ARM_GCC_BIN=$ARM_GCC_BIN
...
EOF
```

**Problem 3:** `gcc_env_ext.bzl` used `{gcc_bin!r}` Python-style format specifier
in a Starlark `.format()` call. Starlark does not support `!r` / `!s` conversion.

**Fix:** Use Starlark's `repr()` function:
```python
.format(gcc_bin = repr(gcc_bin), include_dirs = repr(include_dirs))
```

**Problem 4:** `uart.h` not found during compilation of `main.c`. The `bsp`
cc_library in `board/zu_r5/BUILD.bazel` listed headers but did not export its
directory as an include root.

**Fix:** Added `includes = ["."]` to `bsp`, causing Bazel to pass
`-Iboard/zu_r5` to all consumers.

**Problem 5:** Linker undefined references:
- `_txe_thread_create` — ThreadX error-checking wrappers are in `txe_*.c` files,
  not covered by the `tx_*.c` glob
- `memset` — no libc with `-nostdlib`

**Fix:** Added `"common/src/txe_*.c"` to ThreadX srcs glob.
Removed global `-nostdlib` from `.bazelrc`; added `--specs=nano.specs` and
`--specs=nosys.specs` to `hello_world` linkopts to pull in newlib-nano (tiny
libc with `memset`, `memcpy`, etc.) and nosys syscall stubs.

### Build result

```
$ nix develop
# inside devShell — ARM_GCC_BIN set to Nix store path
$ bazel build //apps/hello_world
INFO: Build completed successfully, 63 total actions

$ arm-none-eabi-size bazel-bin/apps/hello_world/hello_world
   text    data     bss     dec     hex  filename
  11208       4    5884   17096    42c8  hello_world

$ arm-none-eabi-readelf -S bazel-bin/apps/hello_world/hello_world
  .vectors   00000000  (0x40 bytes)   — Cortex-R5 reset table at addr 0
  .text      00000040  (0x2b88 bytes) — code
  .data      00002bc8  (4 bytes)
  .bss       00002bcc  (0x16fc bytes, no load)
```

**11 KB text, 17 KB total — well within 128 KB TCM.**
**All tools come from the Nix store; no host binaries used.**

### Status

- [x] Nix devShell working — all tools provided hermetically
- [x] `nix develop` exports `ARM_GCC_BIN` and writes `.bazelrc.user`
- [x] `bazel build //apps/hello_world` succeeds inside devShell
- [ ] FSBL still needed at `prebuilt/fsbl_r5.elf`
- [ ] BOOT.BIN creation (next step)
- [ ] Flash to board

---

## Iteration 6 — ELF verification  `23:20`

**Goal:** sanity-check the ELF before attempting to flash.

### Size

```
   text    data     bss     dec    hex  filename
  11208       4    5884   17096   42c8  bazel-bin/apps/hello_world/hello_world
```

11 KB text — fits comfortably in 128 KB TCM.

### Vector table (disassembly of .vectors at 0x0)

```
00000000 <_vectors>:
   0: e59ff018   ldr pc, [pc, #24]  → 0x00000118  reset_handler
   4: e59ff018   ldr pc, [pc, #24]  → 0x00000228  __tx_undefined
   8: e59ff018   ldr pc, [pc, #24]  → 0x0000022c  __tx_swi_interrupt
   c: e59ff018   ldr pc, [pc, #24]  → 0x00000230  __tx_prefetch_handler
  10: e59ff018   ldr pc, [pc, #24]  → 0x00000234  __tx_abort_handler
  14: e59ff018   ldr pc, [pc, #24]  → 0x00000238  __tx_reserved_handler
  18: e59ff018   ldr pc, [pc, #24]  → 0x00000218  __tx_irq_handler
  1c: e59ff018   ldr pc, [pc, #24]  → 0x00000224  __tx_fiq_handler
```

Correct Cortex-R5 layout.  IRQ (0x18) branches to `__tx_irq_handler` → ThreadX
context save → `IRQHandler` (GIC dispatch) → context restore.

### Section map

```
.vectors   0x00000000  0x40    — 8 ARM exception vectors
.text      0x00000040  0x2b88  — code
.data      0x00002bc8  0x4     — initialized data
.bss       0x00002bcc  0x16fc  — BSS (zeroed by startup.S)
```

Stack top: 0x00020000 (128 KB TCM = 0x0000_0000 + 0x0002_0000).
Total RAM used: 0x2bcc + 0x16fc = 0x42c8 ≈ 17 KB. ✅

### Status

- [x] ELF builds cleanly with Nix cross-compiler
- [x] Vector table correct
- [x] Sections fit in TCM
- [ ] **BLOCKED: board USB cable not connected** (no ttyUSB/ttyACM, no Digilent USB)
- [ ] **BLOCKED: no FSBL at `prebuilt/fsbl_r5.elf`**

### Next actions required from user

1. **Connect board USB cable** — plug the AES-ZUB USB-JTAG/UART connector to
   the host. The on-board FTDI chip should enumerate as ttyUSB0/ttyUSB1.

2. **Provide FSBL ELF** — copy to `prebuilt/fsbl_r5.elf`. Options:
   - Build in Vitis 2022.x: New App Project → Zynq MP FSBL → dest cpu r5-0
   - Download from Avnet's AES-ZUB board BSP (PetaLinux / prebuilt image package)

3. Once both are available, run:
   ```bash
   nix develop
   # inside devShell:
   bazel build //apps/hello_world
   bootgen -image scripts/boot.bif -arch zynqmp -o BOOT.BIN -w on
   xsct scripts/flash.tcl    # board in JTAG boot mode
   # set boot mode to QSPI, power-cycle
   tio -b 115200 /dev/ttyUSB1
   ```

---

## Iteration 7 — OpenOCD bring-up (JTAG direct load)  `23:40`

**Goal:** load ELF to R5 TCM via JTAG using OpenOCD, bypassing FSBL entirely.

### USB discovery

Board connected: `0403:6010` Xilinx "JTAG+Serial" (FT2232H).
- `/dev/ttyUSB0` = interface 0 = JTAG
- `/dev/ttyUSB1` = interface 1 = UART console

`ftdi_sio` auto-loaded; both ports visible immediately.

### OpenOCD target investigation

The OpenOCD 0.12.0 config for Zynq UltraScale+ is `xilinx_zynqmp.cfg` (not `zynqmp.cfg`).
Chip name is `uscale`. Available targets:
- `uscale.a53.{0..3}` — Cortex-A53 cores
- `uscale.axi` — AXI mem_ap at DAP AP 0 (direct memory r/w)
- **No R5 target defined** — not needed for our approach

### Strategy: pre-load ELF, then release R5 from reset

Since there's no OpenOCD R5 debug target, we avoid needing to halt/examine the R5:
1. Halt A53-0 (in JTAG boot mode, A53 is in BootROM wait loop)
2. Configure RPU: split mode, ARM state, TCM at 0x0
3. Write ELF sections to TCM via `uscale.axi` mem_ap with address offset:
   - R5 sees TCM at 0x00000000; A53/AXI sees same TCM at 0xFFE00000
   - `load_image $ELF 0xFFE00000 elf` shifts all ELF addresses by 0xFFE00000
4. Release R5-0 from reset → it fetches from 0x0 (our vector table) immediately

### Files written

| File | Purpose |
|------|---------|
| `scripts/openocd/aes_zub.cfg` | FTDI FT2232H interface + xilinx_zynqmp.cfg target |
| `scripts/openocd/load_r5.tcl` | RPU config + ELF load via AXI mem_ap + reset release |
| `scripts/60-openocd.rules` | udev: USB access + unbind ftdi_sio from interface 0 |

OpenOCD added to `flake.nix` packages.

### Dry-run result

```
$ openocd -f scripts/openocd/aes_zub.cfg --command "init; exit"
Open On-Chip Debugger 0.12.0
auto-selecting first available session transport "jtag".
Error: LIBUSB_ERROR_ACCESS    ← permission denied (udev rule not yet installed)
Error: no device found
```

Config syntax correct. Access denied because udev rule isn't installed yet
and ftdi_sio still holds interface 0.

### Pending: user actions before OpenOCD can run

**1. Install udev rule** (one-time, needs sudo):
```bash
sudo cp scripts/60-openocd.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules
sudo udevadm trigger --subsystem-match=usb --attr-match=idVendor=0403
```
After this: ttyUSB0 disappears (interface 0 unbound), ttyUSB1 stays for UART.

**2. Join uucp group** (for ttyUSB1):
```bash
sudo usermod -aG uucp v
newgrp uucp    # effective immediately in current shell
```

**3. Switch to JTAG boot mode**
Change all boot mode DIP switches to OFF (BOOT_MODE=0000).
Power-cycle the board after changing switches.

**4. Run** (inside `nix develop`):
```bash
nix develop
bazel build //apps/hello_world    # already built; cached
openocd -f scripts/openocd/aes_zub.cfg -f scripts/openocd/load_r5.tcl
# second terminal:
tio -b 115200 /dev/ttyUSB1
```

---


## Session 3 — OpenOCD JTAG Loading (2026-06-27)

Board connected. Boot mode: JTAG (all DIP switches OFF). USB-JTAG cable connected.

### Attempt 1 — First run after power cycle (clean state)

```
$ nix develop --command openocd -f scripts/openocd/aes_zub.cfg -f scripts/openocd/load_r5.tcl
```

Result (success through RPU config, failure at TCM write):
```
Info : JTAG tap: uscale.tap tap/device found: 0x5ba00477
Info : JTAG tap: uscale.ps tap/device found: 0x04688093
Info : gdb port disabled
RPU_GLBL_CNTL  = 0x00000050
RPU configured: split mode, ARM state, low vectors.
RST_LPD_TOP    = 0x00188fd7  (before)
R5-0 released from reset — TCM AXI slave now live.
Loading bazel-bin/apps/hello_world/hello_world into R5-0 ATCM (AXI 0xFFE00000)...
Info : DAP transaction stalled (WAIT) - slowing down and resending
Error: Timeout during WAIT recovery
Error: Failed to write memory at 0xffe00000
```

**Root cause analysis**: DAP DP STICKYERR (from failed WAIT/timeout) is now set in
hardware, persisting across reconnects.

**New finding**: `RPU_GLBL_CNTL = 0x00000050` — bit[4] = SLCLAMP (slave interface
clamp). Default=1 = TCM AXI slave gated. Our script cleared bit[0] (SLSPLIT) but NOT
bit[4] (SLCLAMP). The TCM AXI slave was STILL gated even after R5 was released from
module reset. The WAIT responses were caused by SLCLAMP being set.

### Fix: Clear SLCLAMP (bit[4]) in RPU_GLBL_CNTL

Load ELF while R5 is STILL in module reset (no CPU contention), then release reset:
1. Clear SLCLAMP → TCM AXI slave enabled (R5 still in reset)  
2. Write ELF to TCM — no WAIT, R5 cannot interfere
3. Release module reset → R5 starts from TCM[0x0]

Updated `load_r5.tcl`:
```tcl
mww 0xFF9A0000 [expr {$rpu_ctrl & ~0x11}]   ; # clear SLSPLIT(bit0) + SLCLAMP(bit4)
# Load ELF while R5 is still in reset
load_image $ELF 0xFFE00000 elf
# Then release R5
mww 0xFF5E023C $rst_r5_0_run
```

### Attempt 2 — After first power cycle (STICKYERR from attempt 1)

Ran new load_r5.tcl. STICKYERR still present from attempt 1 WAIT/timeout:
```
Error: JTAG-DP STICKY ERROR
Warn : target uscale.axi examination failed
```

### STICKYERR software clearing investigation

Tried multiple software clearing approaches in `aes_zub.cfg` setup event and in
`load_r5.tcl` itself:

1. **DP ABORT register** (IR=0x8, 35-bit DR=0xF8 = STKERRCLR|all flags): No effect
2. **DPACC write to CTRL/STAT** (IR=0xA, DR=0x280000102 = DATA=0x50000020 <<3 | A=01):
   CSYSPWRUPREQ|CDBGPWRUPREQ|STICKYERR(bit5 w1c): No effect
3. Added inline recovery in load_r5.tcl (after `init`, before `arp_examine`): Still fails

**Conclusion**: Software clearing of STICKYERR not working in OpenOCD 0.12.0 via raw
irscan/drscan. The exact failure mode is unclear — possible pipelining issue in
OpenOCD's DPACC queue, or re-triggering during dap_dp_init. **Power cycle required
after each failed run.**

### Pending: power cycle + test SLCLAMP fix

Power cycle board, then run:
```bash
nix develop --command openocd -f scripts/openocd/aes_zub.cfg -f scripts/openocd/load_r5.tcl
```

Expected: SLCLAMP=0 allows TCM writes while R5 is in reset → ELF loads → R5 starts.

---

## Session 4 — OCM load success  `2026-06-27`

### Key decision: switch from TCM to OCM

TCM (0xFFE00000 AXI) never became writable under any tested combination:
- Module reset ON + SLCLAMP=1 → WAIT (hardware default, gated on both)
- Module reset ON + SLCLAMP=0 → still WAIT (module reset independently gates TCM AXI slave)
- Module reset OFF + SLCLAMP=0 → WAIT + STICKYERR (CPU arbitration wins, AXI freezes)
- Module reset OFF + SLCLAMP=1 → same result

Root cause: TCM AXI slave has two independent gates (RPU module reset AND SLCLAMP).
When both are cleared the R5 CPU is live and takes ownership, blocking AXI before
we can load code. The fundamental problem: we cannot both allow AXI access AND have
the R5 safely idle.

**Solution: OCM (0xFFFC0000–0xFFFFFFFF)**
OCM is always AXI-accessible regardless of RPU module reset or SLCLAMP state.
With `RPU_0_CFG.VINITHI=1`, the R5 reset vector is at 0xFFFF0000 (top of OCM).

Changes made:
1. `board/zu_r5/memory.lds`: ORIGIN=0xFFFF0000, LENGTH=0xF000, `_stack_top=0xFFFFFFFC`
2. `board/zu_r5/startup.S`: no changes needed (all addresses via linker symbols)
3. `scripts/openocd/load_r5.tcl`: `mww 0xFF9A0100 0x00000001` (VINITHI=1), `load_image $ELF 0 elf`

### ELF layout (after rebuild)

```
Section   Address      Size
.vectors  0xFFFF0000   64 B   (LDR pc, =reset_handler at offset 0)
.text     0xFFFF0040   ~11 KB
.data     0xFFFF2BC8   4 B
.bss      0xFFFF2BCC   5884 B
Total: ~18 KB, fits in 60 KB OCM window (0xFFFF0000–0xFFFFEFFF)
_stack_top = 0xFFFFFFFC (top of OCM, grows downward)
```

### Script run — OCM load success

```
RPU_GLBL_CNTL  = 0x00000050
RPU configured: split mode, VINITHI=1 (vectors → OCM 0xFFFF0000), ARM state.
RST_LPD_TOP    = 0x00188fd7  (R5-0 in reset)
Loading bazel-bin/apps/hello_world/hello_world into OCM (0xFFFF0000, no AXI offset)...
OCM[0xFFFF0000] = 0xe59ff018  (reset vector, expect LDR pc insn)
Spot-check OK.
RST_LPD_TOP    = 0x00188fd6  (R5-0 running)
scripts/openocd/load_r5.tcl:94: Error: invalid command name "0xFFFF0000"
```

`0xe59ff018` = `LDR pc, [pc, #0x18]` — correct vector table entry. OCM write succeeded.
RST_LPD_TOP changed from 0x00188fd7 → 0x00188fd6 (bit[0] cleared → R5-0 executing).

The Tcl error on line 94 was a cosmetic `echo` using double-quoted string with `[0xFFFF0000]`
which Tcl evaluated as a command substitution. The critical `mww` on line 92 ran before the
error. Fixed by changing to curly braces: `echo {R5-0 executing from OCM[0xFFFF0000] = reset_handler.}`

**R5-0 is running.** Checking UART for "Hello, World!" output.

---

## Session 5 — R5 execution diagnosis, XMPU, lock-step, XPPU lockout  `2026-06-27`

### Goal
R5 was released from reset in Session 4 but produced no UART output and BSS was never
zeroed. This session traces the exact cause and works toward first instructions executing.

---

### S5-I1 — XMPU check (not the blocker)

`scripts/openocd/check_xmpu.tcl` — reads XMPU_OCM registers at 0xFF980000.

**Result:**
```
XMPU_OCM ISR    = 0x00000000
XMPU_OCM CTRL   = 0x000000ef  (bit0=defr_perm=1 = default ALLOW)
Regions 0..1:  garbage addresses, but CFG bit0=0 → disabled
Regions 2..3:  all zeros (inactive)
No XMPU violation recorded in last 50ms.
```
XMPU is pass-through (default-allow, all regions disabled). **Not the blocker.**

---

### S5-I2 — Cortex-R5 target added (MISTAKE — sets STICKYERR)

Added `cortex_r4` target at AP 4 to `aes_zub.cfg`:
```tcl
target create uscale.r5_0 cortex_r4 -dap uscale.dap -ap-num 4 -dbgbase 0xFE810000 -defer-examine
```

**Effect:** OpenOCD probes AP 4 during `init`, AP 4 does not exist or is inaccessible,
sets STICKYERR in the DAP.  From that point every AXI read returns 0 and every write
is dropped — silently, without raising a Tcl error.  **Reverted immediately.**

Added comment to aes_zub.cfg: do not add cortex_r4 without first finding the correct AP
number via a scan, because incorrect AP probing silently breaks the AXI mem_ap.

---

### S5-I3 — Lock-step mode identified (root cause hypothesis)

`load_r5.tcl` line 61 comment was wrong:
```tcl
# Clear bit[0]=SLSPLIT (split mode). Preserve other bits.   ← WRONG
mww 0xFF9A0000 [expr {$rpu_ctrl & ~0x1}]
```

Actual ZU+ bit layout:
- RPU_GLBL_CNTL bit[0]: TCM_COMB (not SLSPLIT)
- RPU_GLBL_CNTL bit[3]: SLSPLIT = 0=lock-step, 1=split mode

Code cleared bit[0] (TCM_COMB) but **never set bit[3] (SLSPLIT=1)**.  R5-0 was running in
lock-step mode (SLSPLIT=0) with R5-1 held in module reset.  In lock-step mode both cores
must run; a halted comparator partner causes R5-0 to stall or fault.

**Fix applied:** `mww 0xFF9A0000 [expr {($rpu_ctrl & ~0x1) | 0x8}]`

---

### S5-I4 — Write-before-reset bug (all RPU/CRL_APB writes silently ignored)

`halt_r5_inspect.tcl` run exposed that RPU_GLBL_CNTL and RPU_0_CFG readbacks show 0
even after writing non-zero values.  Root cause:

**RPU_0_CFG and RPU_GLBL_CNTL are write-ignored while R5 is running.**  The TRM documents
this: these strap registers must be written while the CPU is in module reset.

The original `load_r5.tcl` read RST_LPD_TOP at the start (= 0 when board not power-cycled
= R5 already running from previous session) and then attempted to write RPU registers.
All those writes were dropped.

**Fix applied:** `load_r5.tcl` now always asserts module reset first:
```tcl
set rst_cur [lindex [read_memory 0xFF5E023C 32 1] 0]
mww 0xFF5E023C [expr {$rst_cur | 0x3}]   ;# both R5s in reset
after 50
mww 0xFF9A0000 0x00000008  ;# SLSPLIT=1 while in reset
mww 0xFF9A0100 0x00000001  ;# VINITHI=1 while in reset
```

---

### S5-I5 — XPPU lockout (all CRL_APB + RPU writes fail after R5 released)

`write_test.tcl` and `diag_write_regs.tcl` confirmed:

| Register | Address | Write result |
|----------|---------|-------------|
| OCM[0xFFFF3000] | — | **WORKS** ✓ |
| RST_LPD_IOU2 | 0xFF5E0238 | FAILS (readback 0) |
| RST_LPD_TOP | 0xFF5E023C | FAILS (readback 0) |
| RPU_GLBL_CNTL | 0xFF9A0000 | FAILS (readback 0) |
| RPU_0_CFG | 0xFF9A0100 | FAILS (readback 0) |
| PMU_GLOBAL | 0xFFD80608 | FAILS (readback 0) |

**Conclusion:** After R5-0 was released from reset (Session 4 end), the XPPU (Xilinx
Peripheral Protection Unit) locked CRL_APB reset registers and RPU configuration registers
from the JTAG AXI master (AP 0).  OCM (0xFFFF0000+) remains writable.

In Session 4, RST_LPD_TOP WAS successfully written (read back 0x00188fd6 / 0x00188fd7).
This worked because R5 was still in reset at that point — XPPU permits writes to CPU reset
registers from JTAG only when the CPU is held in module reset.

`check_stickyerr.tcl` showed STICKYERR SET in DAP CTRL/STAT even immediately after ABORT
clear.  This is consistent with XPPU returning SLVERR for our CRL_APB write attempts,
which re-sets STICKYERR on each attempt.

**Unable to halt R5 via debug registers:** 0xFE810000 (Cortex-R5 debug in APB space) is
not accessible through AXI mem_ap (AP 0).  It requires APB-AP access and the correct AP
number, which we could not probe safely.

---

### S5-I6 — State and required next step

Current board state:
- R5-0: running, VINITHI=? (likely 1 from Session 4 latch), SLSPLIT=0 (lock-step)
- OCM: ELF from Session 4 still loaded (0xFFFF0000 = 0xe59ff018 confirmed)
- BSS: unzeroed (R5 stuck before BSS loop, probably in lock-step fault handler)
- UART: no output (never reached uart_init)
- CRL_APB/RPU/PMU writes: blocked by XPPU

**Required action: power-cycle the board.**

After power-cycle (RST_LPD_TOP resets to 0x00188FD7 = R5 in reset):
1. Run `openocd -f scripts/openocd/aes_zub.cfg -f scripts/openocd/load_r5.tcl`
2. The script now correctly: asserts reset, writes SLSPLIT=1 + VINITHI=1 while in reset,
   loads ELF, releases R5-0 only
3. Poll BSS at 0xFFFF2BCC — expect 0 within 500ms (BSS zeroed)
4. Check UART1 on /dev/ttyUSB1 at 115200 baud for "Hello, World!"

---

## Session 6 — UART confirmed, canonical boot path established  `2026-08-02`

Board power-cycled, USB-JTAG cable connected (FT2232H enumerated as
ttyUSB0/ttyUSB1). User in `uucp` group; udev rule `60-openocd.rules`
installed.

### Canonical boot sequence (no Vitis/xsct required)

```
nix develop --command openocd \
    -f scripts/openocd/aes_zub.cfg \
    -f scripts/openocd/psu_init_run.tcl \
    -f scripts/openocd/load_r5.tcl
```

`psu_init_run.tcl` sources the Vitis-generated `board/zub_1cg/psu_init.tcl`
directly via OpenOCD's AXI mem_ap, using `xsct_shim.tcl` to provide the
XSCT-compatible helper procs (`mrd`, `mwr`, `mask_write`).  No Xilinx
`hw_server` or `xsct` binary is required.

### Session 6 run log (first run, Path A — fresh reset)

Key observations from the first OpenOCD invocation:

| Register | Value | Meaning |
|---|---|---|
| RPU_GLBL_CNTL | 0x00000008 | SLSPLIT=1, SLCLAMP=0 ✓ |
| RPU_0_CFG | 0x00000005 | VINITHI=1 + NCPUHALT ✓ |
| CPU_R5_CTRL | 0x03000302 | CLKACT bit24 set ✓ |
| OCM[0xFFFF0000] | 0xe59ff018 | reset vector loaded ✓ |
| UART0_CR | 0x00000114 | TX+RX enabled by firmware ✓ |
| UART0 BAUDGEN/BAUDDIV | 124/6 | firmware set 115200 baud ✓ |
| Reset marker[0xFFFFFF00] | 0x52535431 | "RST1" — startup.S executed ✓ |

UART0 at 0xFF000000, MIO 10/11 (L3_SEL=6), routes to `/dev/ttyUSB1` via
FT2232H channel 1 at 115200 8N1.

### Three consecutive passes

```
bazel test --config=host --config=onboard //tests:rpu_hello_world_test
```

All three runs captured `[SERIAL] Hello, World!` and exited 0.  Path B
(software reset via RST_LPD_TOP) worked correctly on subsequent runs:
the test puts R5 back into module reset, re-runs `full_init_from_reset`,
and releases R5 afresh — no power-cycle required between test runs.

### Findings recorded

- **UART route:** UART0 (0xFF000000), MIO 10/11 (L3_SEL=6) → FT2232H
  channel 1 → ttyUSB1.  Confirmed via MIO register readback in psu_init.tcl
  and firmware BAUDGEN/BAUDDIV register state.
- **psu_init:** Two `mask_poll` timeouts (RPLL 0xFF5E0040, FPD PLL 0xFD1A0044)
  during DDR bringup.  Non-fatal for UART/R5; PS UART and RPU subsystem are
  fully functional.  DDR may need a psu_init update if DDR is used later.
- **XPPU:** Locks CRL_APB/RPU registers once R5 is released.  load_r5.tcl
  handles this by asserting reset before writing straps (Path A), or by
  software-reset via RST_LPD_TOP on the second and subsequent runs (Path B).
- **R5_STARTUP_TRACE:** Now enabled in `board/rpu:bsp` copts so
  `reset marker[0xFFFFFF00]=0x52535431` is the primary proof that
  reset_handler executed before uart_init.
- **No inherited Vitis state:** verified — psu_init runs entirely via OpenOCD
  AXI writes; no xsct/hw_server session required.

**ZUB-001 exit condition met.**
