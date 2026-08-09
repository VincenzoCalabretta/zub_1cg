# zub_ctl

Test driver for the AES-ZUB-1CG board: opens a serial port, matches
expected patterns with a timeout, and optionally orchestrates the JTAG boot
subprocess (`openocd` for R5, `xsct` for A53) so serial capture and boot
happen concurrently.

## Subcommands

### `serial-watch`

Open a serial port, print every received line as `[SERIAL] <text>`, and
exit 0 when all `--expect` regexes have matched. Exit 1 on timeout or if
any `--fail-on` regex matches.

```bash
zub_ctl serial-watch \
    --tty /dev/ttyUSB1 --baud 115200 --timeout 30 \
    --expect 'Hello, World!'
```

### `watch-r5`

Runs `openocd -f <cfg> -f <script>` concurrently with `serial-watch`.
openocd stdout/stderr streamed as `[OCD] ...`.

```bash
zub_ctl watch-r5 \
    --openocd-cfg    scripts/openocd/aes_zub.cfg \
    --openocd-script scripts/openocd/load_r5.tcl \
    --tty            /dev/ttyUSB1 \
    --expect         'Hello, World!'
```

### `watch-a53`

Runs `xsct <tmpfile.tcl>` with an auto-generated TCL that programs the PL
bitstream, runs `psu_init`, and loads the ELF onto A53#0. xsct output
streamed as `[XSCT] ...`.

```bash
zub_ctl watch-a53 \
    --xsct      /path/to/xsct \
    --elf       bazel-bin/apps/apu/hello_world/hello_world_a53.elf \
    --bitstream board/zub_1cg/design_1_wrapper.bit \
    --psinit    sdk/boards/zub_1cg/generated/psu_init.tcl \
    --tty       /dev/ttyUSB1 \
    --expect    'ThreadX RGB LED'
```

## Exit codes

| Code | Meaning |
|---|---|
| 0 | Every `--expect` regex matched before timeout |
| 1 | Timeout with unmatched expectations, `--fail-on` regex hit, or boot subprocess exited non-zero |
| 2 | Argument / I/O error (bad regex, missing TTY, cannot open port) |

## Building

```bash
# Bazel-built binary:
bazel build --config=host //tooling/zub_ctl:zub_ctl
# → bazel-bin/tools/zub_ctl/zub_ctl
```

## Design notes

- Serial reader runs in a dedicated thread and starts **before** the boot
  subprocess is spawned. This mirrors the original pyserial-based tests
  and guarantees no boot-time output is lost between opening the port and
  releasing R5 (or A53).
- `--expect` regexes are matched **in order** — one per expected line-of-
  interest. Use multiple `--expect` flags for a sequence.
- `--fail-on` regexes are checked on every line; any match exits 1
  immediately.
- Serial data is decoded UTF-8-lossy; binary noise on the wire won't crash
  the reader.
