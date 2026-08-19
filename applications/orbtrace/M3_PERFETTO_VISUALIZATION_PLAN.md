# Plan: M3 ITM trace capture → Perfetto-viewable trace

Goal: turn a `.bin` file produced by `orbtrace capture` (real M3 ITM traffic,
see `M3_TRACE_VERIFICATION_PLAN.md` Phases E/F) into a trace file viewable at
[ui.perfetto.dev](https://ui.perfetto.dev), with one track per ITM
stimulus port and real, correctly-decoded event content — entirely with new
Rust code in `applications/orbtrace/model`, no vendored C toolchain
(Orbuculum/orbetto) and no new Bazel/crate dependencies.

This document is the living plan for that work, in the same spirit as
`M3_TRACE_VERIFICATION_PLAN.md`. See section 0 for how to use/extend it —
that section is itself the deliverable the user asked for alongside the
plan, and applies to both documents.

## 0. How to hand off sessions on this document

This protocol was distilled from how `M3_TRACE_VERIFICATION_PLAN.md` was
actually used across many real sessions on this board. Follow it here too,
and for any future living plan document in this repo.

1. **The status table (section 1) is live state, not history.** Edit rows
   in place to reflect the current best understanding. Never leave a row
   saying "DONE" if a later finding invalidated it — go back and correct
   it, with a pointer to the section that explains why.
2. **Everything below the table is an append-only log.** Add a new
   `### Update YYYY-MM-DD` (or `continuation`) section for each session's
   findings; do not rewrite or delete earlier sections, even ones later
   proven wrong — mark them superseded in place (`**SUPERSEDED, see the
   2026-08-19 update below**`) so the document stays a truthful record of
   what was believed when and why it changed. Future sessions (and future
   memory recall) depend on that trail.
3. **"DONE" requires evidence, not confidence.** A phase is DONE only when
   backed by something checkable: a passing host test for pure-software
   work, or a real capture/log/register readback for anything touching the
   board. "Should work" or "looks right" is not DONE — say what's actually
   unverified.
4. **End every session with, explicitly:**
   - What's committed vs. what's scratch-only (and where the scratch files
     live, or a description precise enough to recreate them — this repo's
     convention is to keep one-off probe/diagnostic scripts out of the
     commit and describe them in-line instead; promote a script to
     "worth committing" explicitly if a future session would genuinely
     want to reuse it, don't leave that judgment implicit).
   - The exact hardware/board state left behind if the session touched
     real hardware (e.g. `ORBTRACE_REG_M3_CONTROL` value, which bitstream
     is flashed) — the next session should never have to rediscover this
     by trial and error.
   - A **Next steps for a future session** list, cheapest/lowest-risk item
     first.
5. **Cross-reference, don't duplicate.** This document should point at
   `M3_TRACE_VERIFICATION_PLAN.md` for anything that's really about capture
   quality/hardware bring-up (that document owns those phases), and vice
   versa. Keep each fact in exactly one place.
6. **Prefer this repo's existing style over new dependencies.** Every wire
   format in `applications/orbtrace/model/src/lib.rs` (COBS, length
   framing, Orbflow) is hand-rolled with zero external crates —
   `model/BUILD.bazel`'s `orbtrace_model` target has no crate deps at all,
   confirmed 2026-08-19. Match that: prefer hand-writing a format over
   pulling in a new dependency, unless a design decision below says
   otherwise for a specific, stated reason.

## 1. Status

| Phase | What | Status |
|---|---|---|
| 1 | Capture file → per-channel raw ITM byte stream | DONE (2026-08-19) — `reconstruct_channel_stream`/`channel_histogram` in `model/src/lib.rs`, host-tested |
| 2 | ITM SWIT packet decoder | DONE (2026-08-19) — `decode_itm_stream`/`ItmPacket` in `model/src/lib.rs`, host-tested incl. garbage-resync |
| 3 | Event → Perfetto track/slice semantic mapping | DONE (2026-08-19) — `classify_port0`/`build_perfetto_trace` in `model/src/lib.rs`, including the sync-gap counter track; classification is a documented best-effort heuristic, see the update below |
| 4 | Perfetto trace file writer | DONE (2026-08-19) — JSON Chrome-trace format only, `perfetto_json` in `model/src/lib.rs`; protobuf v2 stretch goal still not started |
| 5 | CLI wiring (`orbtrace decode-trace`) + end-to-end validation | DONE (2026-08-19) for host-only structural validation **and** a real-hardware capture run through `decode-trace` end-to-end; **NOT DONE** for the human-in-the-loop ui.perfetto.dev visual check — file handed to the user, see the update below |
| 6 | Capture throughput / sync-loss improvement | **Not owned by this document** — this is `M3_TRACE_VERIFICATION_PLAN.md`'s existing Phase E open item; this pipeline works with however much real content a capture yields today, sparse or not |

## 2. What already exists to build on (confirmed 2026-08-19)

- `orbtrace capture HOST FILE` (`model/src/main.rs`) writes **raw bytes
  straight off TCP 3402 to disk**, no host-side re-framing
  (`output.write_all(&buffer[..count])`). What's on that wire is this
  repo's own "Orbflow" transport, not raw ITM/TPIU: the FPGA already
  TPIU-demuxes the CoreSight formatter stream into per-source-channel
  bytes, then wraps `[channel][payload][checksum]`, COBS-encodes it, and
  terminates each frame with a `0x00` delimiter.
- **The unframe step is already implemented and reusable as-is**:
  `orbflow_unframe(frame: &[u8]) -> Result<(u8, Vec<u8>), ProtocolError>`
  (`model/src/lib.rs:338`) COBS-decodes, verifies the checksum, and returns
  `(channel, payload)`. `cobs_decode`/`cobs_encode`/`append_checksum`/
  `verify_checksum` (`lib.rs:265-323`) back it and already have unit
  tests. Because COBS encoding by construction never produces a literal
  `0x00` byte inside a frame, splitting a raw capture file on `0x00` bytes
  is an exact, safe way to recover frame boundaries.
- **No ITM SWIT (Software Instrumentation Trace) packet decoder exists in
  committed code.** Phase F of `M3_TRACE_VERIFICATION_PLAN.md`
  (2026-08-19) hand-rolled one in an uncommitted scratch script
  (`verify_phase_f.py`) to verify capture content against the firmware's
  `Workload` reference — that script proved the algorithm correct
  (found 9 real, order-preserving matches against an expected ~1-3 by
  chance) but was never ported into this repo's real Rust code. That
  algorithm — `header = (port << 3) | size_code`, `size_code` 1/2/3 for
  1/2/4-byte payloads — is exactly what Phase 2 below needs to become
  real, tested, committed code.
- The M3 firmware's real ITM traffic is a **synthetic, fully-known,
  deterministic pattern**, not application semantics — see section 3.
- `orbtrace_model` has zero external crate dependencies today
  (`model/BUILD.bazel`) — see section 0, item 6.
- The DBGEN/Phase-G JTAG-halt limitation documented in
  `M3_TRACE_VERIFICATION_PLAN.md` is **unrelated to this pipeline** — ITM
  trace comes from the TPIU/formatter over the trace port, not the debug
  AP, and works (to the extent Phase E/F's sync-loss rate allows)
  independent of whether halting is ever un-gated.

## 3. What the M3 firmware's ITM traffic actually means

Both the bare-metal firmware (`firmware/m3_app/src/main.c`'s
`emit_next()`) and its host-testable Rust reference model
(`firmware/m3/src/lib.rs`'s `Workload::next()`) implement the same
deterministic xorshift-driven event cycle, keyed on `sequence & 15`:

| `sequence & 15` | Event | Emitted as |
|---|---|---|
| 0 | `Timestamp(sequence)` | ITM stimulus port 0, 4-byte write |
| 1 | `Idle(width)` | ITM stimulus port 0, 4-byte write |
| 2 | `Malformed(byte)` | ITM stimulus port 0, **1-byte** write |
| 3 | `Fault(0xf001_0000 \| sequence)` | ITM stimulus port 0, 4-byte write |
| everything else (12 of 16 values) | `Itm{channel,value,width}` | ITM stimulus port `(n % 7) + 1` (i.e. ports 1-7), width cycling 1/2/4 bytes |

So a real capture, once decoded, is traffic on 8 ITM ports (0 through 7):
port 0 carries 4 distinguishable event kinds by value shape (a real
monotonic `sequence` counter for `Timestamp`, small values for `Idle`,
single-byte writes for `Malformed`, a fixed `0xf001_xxxx` pattern for
`Fault`); ports 1-7 carry generic pseudo-random values with no further
structure. This is a synthetic test pattern built for verification, not
real application telemetry — treat the v1 visualization goal as "prove
the pipeline decodes and renders real captured content correctly," not as
"produce a meaningful application trace." A future real firmware could
reuse the same pipeline with a different, real event vocabulary.

## 4. Phase 1 — Capture file → per-channel raw ITM byte stream

Almost entirely glue over existing code:

1. Read the capture file, split on `0x00` bytes to recover Orbflow frame
   boundaries (safe per section 2's COBS argument).
2. Call `orbflow_unframe` on each frame; skip (count, don't panic on)
   frames that fail checksum verification — a real capture's high
   sync-loss rate (see `M3_TRACE_VERIFICATION_PLAN.md` Phase E) means
   malformed/partial frames are expected, not exceptional.
3. **Confirm the real M3 orbflow channel value empirically against a
   fresh capture rather than assuming it's `1`** — Phase F's notes say
   "channel-1-only" content was found, but re-verify against whatever
   capture this work actually uses before hardcoding it; a full
   histogram of observed channel values from a real capture costs
   nothing and catches a wrong assumption immediately.
4. Concatenate the payload bytes of every frame on that channel, in frame
   order, into one `Vec<u8>` — this is the reconstructed raw ITM byte
   stream Phase 2 consumes. Frame order in the capture file is temporal
   order on the wire, so straightforward concatenation is correct (no
   reordering needed).

Suggested shape: `pub fn reconstruct_channel_stream(capture: &[u8], channel: u8) -> Vec<u8>`
in `model/src/lib.rs`, unit-tested by round-tripping synthetic frames built
with `orbflow_frame()` (already exists) — no hardware needed for this
test.

## 5. Phase 2 — ITM SWIT packet decoder

New code, porting the proven `verify_phase_f.py` algorithm into real,
tested Rust:

- Read one header byte: `port = header >> 3`, `size_code = header & 0x7`.
- `size_code` 1/2/3 → read 1/2/4 little-endian payload bytes respectively
  (per the ARMv7-M ITM SWIT packet format); other `size_code` values are
  reserved/other-ITM-packet-type headers, not SWIT — do not treat them as
  malformed SWIT, skip/resync past them (see below).
- **Must tolerate garbage, not just well-formed input.** Because real
  captures have a high false-lock/sync-loss rate, the reconstructed byte
  stream will contain resync noise and truncated frames interleaved with
  genuinely valid SWIT packets — a naive decoder that assumes every byte
  is the start of a valid header will misparse and cascade errors. Prefer
  a decoder that, on an unrecognized/incomplete packet, advances one byte
  and resynchronizes, the same tolerant-parsing posture
  `orbtrace_tpiu_demux.sv`'s channel-plausibility gate already takes at
  the framing layer.
- Suggested shape:
  ```rust
  pub enum ItmPacket {
      Swit { port: u8, value: u32, width: u8 },
      Unrecognized(u8),
  }
  pub fn decode_itm_stream(bytes: &[u8]) -> Vec<ItmPacket>
  ```
- Unit test: hand-encode a few `Workload` events using the firmware's own
  header formula, decode, and confirm exact round-trip (port/value/width)
  — this is a pure host-side test, no hardware needed, and should be the
  first thing that runs green before touching a real capture file.

## 6. Phase 3 — Event → Perfetto track/slice mapping

Design decision to make explicit (and revisit if it turns out wrong once
real output is visible) before writing Phase 4:

- **One Perfetto track per ITM port (0 through 7).** Track UUIDs derived
  deterministically from the port number so re-decoding the same capture
  twice produces byte-identical output (useful for diffing/testing).
- **Every decoded `Swit` packet becomes an instant event** (Perfetto
  `TYPE_INSTANT`, or `"ph":"I"` in the JSON Chrome format — see Phase 4)
  on its port's track, not a begin/end slice pair — nothing in the
  firmware's event model pairs naturally into a duration today.
  - Port 0: name the event by its recovered kind (`Timestamp`, `Idle`,
    `Malformed`, `Fault`) using the value-shape rules from section 3,
    with the raw value as an annotation/argument.
  - Ports 1-7: name the event by its raw value, e.g. `port {port} = {value:#x}`.
- **Surface capture quality directly in the trace, not just as an
  external stat.** Port 0's `Timestamp` events carry the firmware's real
  `sequence` counter — a gap between consecutive recovered `Timestamp`
  values is direct evidence of the sync-loss Phase E/F already
  documented. Consider emitting a counter track (`dropped_sequence_gap`)
  that steps up at each detected gap, so a viewer can *see* where/how
  often the capture lost lock, not just infer it from the overall
  `sync_loss` stat. Not required for a first working version, but cheap
  once Phase 2 exists and genuinely useful — revisit after Phase 5's
  first real capture is in hand.

## 7. Phase 4 — Perfetto trace file writer

**Recommendation: implement the JSON Chrome-trace format first, treat the
native Perfetto protobuf format as a v2 stretch goal.** Reasoning:

- ui.perfetto.dev fully supports both as drag-and-drop input — there is
  no visualization-quality reason to start with protobuf.
- The JSON format (`{"traceEvents":[{"name","ph","ts","pid","tid",...}]}`,
  `"ph":"I"` for instant events) is small, stable, and trivial to
  hand-write with no risk of a subtly-wrong encoding silently corrupting
  the file — plain text, no wire-format precision required.
- The native protobuf format (`Trace` = repeated `TracePacket`, with
  `TrackDescriptor`/`TrackEvent` submessages) is denser and supports real
  native counter tracks, which would make section 6's sync-gap counter
  idea cleaner — but hand-encoding protobuf varints/length-delimited
  fields requires getting exact field numbers right, and **field numbers
  must not be trusted from memory/training data** — a wrong field number
  doesn't error, it silently misparses or is silently dropped by the
  reader. Before attempting this, fetch the real, current
  `protos/perfetto/trace/trace.proto` (and `track_event.proto`) from the
  Perfetto project as a reference — do not hand-encode from recollection.
  This is real, valuable follow-up work, just riskier and not needed for
  a first working, viewable trace.
- Either format: zero new Bazel/crate dependencies (see section 0, item 6)
  — no `protoc`, no `prost`, plain string/byte writing.

## 8. Phase 5 — CLI wiring and validation

- Add `orbtrace decode-trace CAPTURE_FILE OUTPUT.json` to `main.rs`'s
  `run()` dispatch, alongside the existing `capture`/`replay`/`dap`/etc.
  subcommands — same pattern as every other subcommand there.
- Host-only validation (no hardware): unit/integration test built from
  synthetic capture bytes (`orbflow_frame()` + hand-encoded SWIT bytes),
  asserting the decoded event sequence matches what was encoded.
- Real-hardware validation: capture using Phase F's exact known-good
  procedure (`orbtrace load-m3 HOST m3_app.bin` immediately after
  starting a fresh capture, to pin `sequence` near 0 the same way Phase F
  did, rather than an unsynced capture against an unknown large offset),
  run `decode-trace` on the result, and sanity-check the output
  structurally (event count, port distribution, `Timestamp` values
  increasing monotonically modulo the expected sync-gaps) before
  declaring this phase done.
- **Actually opening the file in ui.perfetto.dev is a human-in-the-loop
  step** — there's no local Perfetto tooling in this repo/sandbox to
  automate that check. An agent session should get the structural
  validation above to pass and hand the file to the user (e.g. via
  `SendUserFile`) rather than claim visual confirmation it can't
  perform.

## Next steps for a future session

1. Start with Phase 1 + Phase 2 together (they're both small, host-only,
   and unlock everything else) — get both unit-tested before touching
   real hardware.
2. Get one real capture (reusing Phase F's procedure) and empirically
   confirm the real orbflow channel value (section 4, item 3) before
   hardcoding anything.
3. Phase 3's design is a first draft — revisit once Phase 2's decoder is
   run against real captured bytes and the actual event mix is visible,
   in case the port-0/ports-1-7 split needs adjusting.
4. Ship Phase 4 as JSON Chrome-trace first; do not start the protobuf
   version without first fetching the real current `.proto` field
   definitions from the Perfetto project as a reference.
5. After a first end-to-end file is produced and structurally validated,
   send it to the user for the actual visual check in ui.perfetto.dev —
   that's the real acceptance bar for this whole document, and it's a
   step no agent session can complete alone.

### Update 2026-08-19: Phases 1-5 implemented and host-verified; real-capture run and visual check still open

**What was done.** Items 1-4 above, all in this session, entirely in
`applications/orbtrace/model/src/lib.rs` (new code, no new crate deps,
consistent with section 0 item 6):

- `reconstruct_channel_stream(capture, channel)` + `channel_histogram(capture)`
  (Phase 1) — split-on-`0x00` framing, tolerant of bad-checksum frames.
- `ItmPacket` + `decode_itm_stream(bytes)` (Phase 2) — header
  `(port << 3) | size_code`, one-byte resync on anything not
  `size_code` 1/2/3, per the plan's tolerant-parsing posture.
- `Port0Event` + `classify_port0(value, width)` + `build_perfetto_trace(packets)`
  (Phase 3). Classification heuristic, since a decoder that only sees
  width+value shape can't perfectly disambiguate `Idle` from `Timestamp`:
  width 1 → `Malformed`; width 4 with high half `0xf001_0000` → `Fault`;
  width 4, nonzero, multiple of 16 → `Timestamp` (true by construction:
  `Timestamp`'s value *is* `sequence`, only emitted when
  `sequence & 15 == 0`); width 4 in `1..=1024` → `Idle`; anything else →
  `Unknown`. This misclassifies about 1-in-16 `Idle` values that happen to
  land on a multiple of 16 — acceptable for labeling a visualization, not
  claimed as exact. Also implements the `dropped_sequence_gap` counter
  track from section 6: steps by 1 each time two consecutive recovered
  `Timestamp` values on port 0 don't differ by exactly 16.
- `perfetto_json(trace)` (Phase 4) — hand-written JSON Chrome-trace format
  (`"ph":"I"` instants, one `tid` per ITM port under `pid` 1 named via
  `"ph":"M"` metadata, `"ph":"C"` counter track under `pid` 2 for the
  sync-gap series). Protobuf `TrackEvent` v2 stretch goal: still not
  started, per the plan's own caution not to hand-encode it from memory.
- `orbtrace decode-trace CAPTURE_FILE OUTPUT.json [CHANNEL]` wired into
  `model/src/main.rs`'s dispatch (Phase 5) — auto-picks the
  most-frequent channel from `channel_histogram` when `CHANNEL` is
  omitted, rather than hardcoding one.

**Verification performed (host-only, no board touched this session).**

- 7 new unit tests added to `model/src/lib.rs`'s existing `mod tests`
  (round-trip + tolerant-resync + classification + gap-detection +
  JSON-structure cases), all passing, run both ways:
  - `cargo test` (plain Cargo, no Bazel) inside the repo root's Nix dev
    shell (`nix develop`, which is how this repo's tooling — including
    `bazel` itself — is meant to be reached; ad hoc `cargo build`/`cargo
    test` outside that shell also happened to work here since the crate
    has zero deps, but isn't the supported path).
  - `bazel test //applications/orbtrace/model:orbtrace_model_test` inside
    `nix develop` — PASSED, 1/1, all 17 lib tests + 1 main.rs test green.
  - `bazel build //applications/orbtrace/model:orbtrace` and
    `bazel test //applications/orbtrace/model:register_schema_test` inside
    `nix develop` — both green, confirming the CLI addition didn't break
    the existing register-schema check.
- End-to-end smoke test with a synthetic capture file (built by a
  throwaway Python script replicating this crate's COBS/checksum/orbflow
  framing byte-for-byte, **not committed** — see below to recreate it):
  64 synthetic `Workload`-shaped events across 6 orbflow frames on
  channel 1 plus 2 frames of channel-2 noise plus one deliberately
  corrupted frame. Running the *Bazel-built* `orbtrace decode-trace`
  binary (not just the `cargo run` one) produced a byte-identical JSON
  file to the `cargo run` version; `channel_histogram` correctly picked
  channel 1 (6 valid frames) over channel 2 (2); of 7 channel-1 frames
  attempted (6 real payload chunks plus 1 deliberately corrupted one),
  exactly the corrupted one was silently skipped, matching the printed
  "6 valid frames"; all 64/64 packets decoded as SWIT; all 4 synthetic
  `Timestamp` events recovered as `Timestamp(16)`,
  `Timestamp(32)`, `Timestamp(48)`, `Timestamp(64)` with zero
  false-positive sequence-gap samples; the output parses as valid JSON
  via Python's `json` module.
- **Not done, and this is the real gap before this phase can be called
  fully DONE**: no real hardware capture was taken or run through
  `decode-trace` this session (no board work happened at all), and the
  file has not been opened in ui.perfetto.dev — per section 8, that
  visual check is an unavoidable human-in-the-loop step no agent session
  can perform.

**Committed vs. scratch.** Committed: the four new `lib.rs` items above,
the `decode_trace` function + `decode-trace` dispatch arm + updated
`usage()` string in `main.rs`, and the 7 new unit tests. Scratch-only, not
committed, describe-to-recreate: a Python script
(`gen_capture.py`, lived at the session's scratchpad path) that hand-rolls
this crate's COBS encoder, `append_checksum`, and `orbflow_frame` framing
in Python, then synthesizes 64 `sequence & 15`-cycled events (mirroring
`firmware/m3/src/lib.rs`'s `Workload::next()`) chunked across multiple
orbflow frames on channel 1, plus channel-2 noise and one corrupted
frame — recreate by porting that same logic if a future session wants
another synthetic capture without real hardware.

**Hardware/board state.** Unchanged — no hardware was touched this
session. Whatever board/bitstream state `M3_TRACE_VERIFICATION_PLAN.md`'s
most recent update left in place still applies; see that document, not
this one, for the current bitstream/flash state (section 0, item 5 of
*this* document: don't duplicate that fact here).

**Next steps for a future session:**

1. Cheapest: get one real capture using Phase F's exact known-good
   procedure (`orbtrace load-m3 HOST m3_app.bin` immediately after
   starting a fresh capture), then run
   `orbtrace decode-trace CAPTURE_FILE OUTPUT.json` on it (channel
   auto-detected) and sanity-check the structural stats it prints
   (frame counts per channel, recognized-SWIT ratio, event/gap counts)
   against what Phase F/E already know about this capture's sync-loss
   rate.
2. Send the resulting JSON to the user via `SendUserFile` for the actual
   ui.perfetto.dev visual check — that's the acceptance bar for this
   whole document and genuinely can't be done from an agent session.
3. If the visual check reveals the port-0 classification heuristic is
   misleading in practice (e.g. `Idle`/`Timestamp` collisions look bad
   at real capture density), revisit `classify_port0` — it's the one
   piece of this pipeline that's a judgment call rather than an exact
   decode.
4. Protobuf `TrackEvent` v2 stretch goal (section 7): still not started;
   fetch the real current `.proto` definitions before attempting it, per
   the plan's original caution.

### Update 2026-08-19 (continued): first real-hardware capture run through `decode-trace`

**Board state at start:** already flashed with the real Orbtrace bitstream
and running (`orbtrace info 192.168.1.50` → `ZUBoard-Orbtrace/1`
immediately, no reflash needed — the board had been left running since a
prior session). No `jtag_flash.sh`/reflash performed this session.

**Procedure (Phase F's exact known-good recipe, all via the Bazel-built
`orbtrace` binary, no JTAG/xsct involved):**
```sh
orbtrace configure 192.168.1.50 m3 tpiu4 2000000
orbtrace start 192.168.1.50
orbtrace capture 192.168.1.50 real_capture.bin 8388608 &   # backgrounded
orbtrace load-m3 192.168.1.50 m3_app.bin   # immediately after, to pin sequence near 0
# ~150s later:
kill %1   # stop the capture
orbtrace decode-trace real_capture.bin real_trace.json
```
`m3_app.bin` was rebuilt fresh this session
(`bazel build //applications/orbtrace/firmware/m3_app:m3_app`, then
`arm-none-eabi-objcopy -O binary` on the resulting `m3_app` symlink, per
`M3_TRACE_VERIFICATION_PLAN.md`'s recipe) rather than reused from a stale
artifact — see that document's own D2 "stale artifact" lesson for why
that distinction matters.

**Result:** `real_capture.bin` was 552 raw bytes. `decode-trace` printed:
```
channel 1: 76 valid frames
channel 1: 248 raw bytes, 66/116 packets recognized as SWIT
66 track events, 0 sequence-gap samples
```
— channel-1-only content, consistent with every prior session's finding
(Phase E/F). `channel_histogram` saw no other channel at all in this
capture (not just "channel 1 won", literally the only channel present).
44% of decoded packets were `Unrecognized` (resync noise from the
already-documented high false-lock rate) — expected, not a decoder bug.
Of the 66 recognized `Swit` packets, **all 66 landed on ports 1 and 5**
(40 and 26 respectively) — zero landed on port 0, so no `Timestamp`/
`Idle`/`Malformed`/`Fault` events were recovered in this particular
window, and the `sequence_gaps` counter's `0` is consequently not
evidence of a clean run — it's undefined-by-absence (no `Timestamp` was
recovered to compare against a next one), not "no gaps found". This is a
sparse-and-random sample, exactly as Phase E/F predicted: which specific
SWIT packets survive the sync-loss-heavy channel is close to random,
not something a fixed short window is guaranteed to cover port 0 at all.
A longer capture window (Phase E/F's own next-step item, not owned by
this document) would raise the odds of catching port-0 content too.
The output parses as valid JSON (`python3 -m json.tool`/`json.load`
both round-tripped it cleanly) with 66 `"ph":"I"` events + 8 `"ph":"M"`
track-name events, matching `decode-trace`'s own printed counts exactly.

**Handed to the user via `SendUserFile`** for the actual ui.perfetto.dev
visual check — see section 8, this is the one step this document has
always said no agent session can complete alone. That check has not
happened yet as of this update.

**Committed vs. scratch.** Nothing new committed this session (Phases
1-5's code was already committed as `2cf867a`/`2246009` in a prior
session). Scratch-only, in the session scratchpad, not committed:
`m3_app.bin` (rebuildable via the recipe above), `real_capture.bin`
(552 bytes, the raw capture), `real_trace.json` (the decoded output,
also the file sent to the user).

**Hardware state left behind:** M3 running (freshly reloaded this
session via `load-m3`), Orbtrace `start`ed with `source=m3 format=tpiu4
swo_baud=2000000` still configured, capture process killed (not the
board — `orbtrace stop` was **not** called, so the PL side is still
actively trying to trace; only the host-side `capture` reader that was
writing to `real_capture.bin` was stopped). `orbtrace stats` at the
point of stopping: `rx_bytes=1056 dropped_bytes=75479316056
sync_loss=3749455852 fifo_high_water=63 dma_faults=0` (the huge
dropped/sync_loss counters are cumulative-since-boot, not a regression —
consistent with every prior session's observation). No bitstream
reflash, no `ORBTRACE_REG_M3_CONTROL` changes, no JTAG/DAP work this
session — that register was untouched from whatever the last Phase-G
session left it at.

**Next steps for a future session:**
1. Once the user reports back on the ui.perfetto.dev visual check, note
   the outcome here — that closes out Phase 5's one remaining item.
2. If a port-0 (`Timestamp`/`Idle`/`Malformed`/`Fault`) sample is
   specifically wanted for a future check, either run a longer capture
   window or run `decode-trace` repeatedly across several fresh captures
   — no code change needed, this is purely a function of how much of the
   sync-loss-heavy channel gets sampled, which is `M3_TRACE_VERIFICATION_PLAN.md`
   Phase E's territory, not this document's.
