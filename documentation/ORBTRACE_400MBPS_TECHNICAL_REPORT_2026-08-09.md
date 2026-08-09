# Orbtrace 400 Mbit/s Trace Logging: Engineering and Verification Report

- Date: 2026-08-09
- Hardware: Avnet AES-ZUB-1CG-ED-G, Zynq UltraScale+ ZU1CG
- Target firmware: A53 / ThreadX / NetX Duo
- Target address: `192.168.1.50`
- Control / trace ports: TCP `3401` / `3402`

## 1. Executive result

The Orbtrace implementation now sustains the required trace logging rate on
real hardware. The repository-owned hardware acceptance test captured exactly
3,000,000,000 bytes to a real host file in 37.431 seconds. Its end-to-end rate
calculation, which includes the host-side capture invocation overhead, reported
641,059,166 bit/s with no change in the device's dropped-byte, synchronization
loss, or DMA-fault counters.

```text
$ nix develop -c bazel test --config=host \
    //tests:orbtrace_throughput_test --test_output=streamed

captured 3000000000 bytes in 37.431s
orbtrace throughput: 641059166 bit/s, zero reported loss
//tests:orbtrace_throughput_test PASSED in 37.5s
```

The result exceeds the 400,000,000 bit/s requirement by 60.265% and completes
the 3 GB capture 22.569 seconds inside the 60-second limit. Expressed as
decimal bytes per second, the measured acceptance rate is 80.132 MB/s.

The project and acceptance test use **400 Mbit/s**, equivalent to **50 MB/s**.
The frequently used project shorthand “400MBps” must not be interpreted as
400 megabytes per second; that would be a different, 3.2 Gbit/s requirement
and is not the requirement verified here.

The accepted data traversed the complete production path:

```text
PL deterministic source
  -> Orbflow framing and COBS encoding
  -> 8-to-64-bit AXI Stream packing
  -> AXI DMA S2MM scatter/gather ring
  -> non-cacheable DDR-backed NX_PACKET buffers
  -> NetX Duo TCP
  -> GEM2 DMA descriptors and MAC
  -> RGMII PHY and Ethernet
  -> RTL8153 host adapter over USB 3.0
  -> Rust capture client
  -> real filesystem output
```

This was not achieved by a single throughput optimization. The investigation
first had to resolve independent failures in the programmable-logic design,
PS/DDR initialization, Ethernet frame ownership, interrupt/thread
serialization, cache coherency, TCP configuration, test arithmetic, and the
host I/O path. Several of these faults produced similar symptoms, especially
captures that stopped under load. Treating each stop as one recurring “DMA
hang” would have concealed the actual causes.

## 2. Acceptance contract and standard of evidence

### 2.1 Acceptance criteria

The final hardware test in `tests/orbtrace_throughput_test.sh` requires all of
the following:

1. The A53 control service must answer at `192.168.1.50:3401`.
2. The PL deterministic source is selected so the result is independent of a
   traced processor's workload.
3. The trace service must deliver exactly 3,000,000,000 bytes through TCP port
   `3402` into `${TEST_TMPDIR}/orbflow.bin`.
4. The measured rate must be at least 400,000,000 bit/s.
5. `dropped_bytes`, `sync_loss`, and `dma_faults` must be unchanged from the
   pre-capture values.
6. The command must complete rather than leave a partially written file after
   a timeout or broken connection.

The capture is intentionally written to a file rather than discarded through
`/dev/null`. Therefore the result includes host socket reads, Rust `write_all`
calls, page-cache/filesystem handling, and the real target-to-host transport.

### 2.2 Evidence classifications used in this report

To prevent a plausible hypothesis from being reported as a solved defect, the
findings below are classified as follows:

- **Hardware-proven:** reproduced or verified on the physical ZUBoard and host
  data path.
- **Wire-proven:** established by packet capture independently of the target's
  internal diagnostics.
- **Implementation-proven:** established directly from the relevant ownership,
  memory, or state-transition logic and verified by a focused build or test.
- **Eliminated hypothesis:** a reasonable explanation that was contradicted by
  measurements or by an A/B change.
- **Residual caution:** diagnostic or recovery code retained in the tree but
  not treated as part of the demonstrated root-cause fix.

This distinction matters because the work encountered several convincing but
incorrect interpretations. In particular, apparently alarming AXI DMA status
registers were normal idle-ring states, and an Ethernet link operating at
1 Gbit/s did not imply that the host's USB-Ethernet transport was operating at
SuperSpeed.

## 3. Initial architecture and failure surface

The starting system combined four independently asynchronous subsystems:

- An RTL trace source and Orbflow/COBS encoder clocked at 100 MHz.
- A scatter/gather AXI DMA engine writing PL output into A53-visible DDR.
- ThreadX and NetX Duo managing TCP packet ownership on the A53.
- GEM2 descriptor DMA transmitting through the board PHY to a USB-Ethernet
  adapter on the host.

The first implementation added the TCP trace endpoint on port `3402`, a
32-descriptor S2MM ring, and a dedicated `.trace_dma` region. That made the
intended data path structurally complete, but it exposed defects at multiple
boundaries:

1. PL bytes reached DDR while the AXI DMA descriptors never completed.
2. Once descriptors completed, TCP traffic became corrupt after a short burst.
3. A correct Orbtrace bitstream could be paired with a PS initialization script
   that hung DDR calibration.
4. After packet corruption was repaired, sustained captures froze the whole
   network stack.
5. After the freeze was repaired, the system plateaued at approximately
   42–45 MB/s, below the 50 MB/s requirement.

The diagnostic process therefore evolved from functional bring-up, to packet
integrity, to concurrency, to sustained throughput. Each stage needed a
different source of evidence.

### 3.1 State transition summary

The following table shows how each material change moved the observable system
to a new failure boundary. It is intentionally ordered by discovery, not by
source-file location.

| Stage | Observable boundary before change | Change | New proven boundary |
|---:|---|---|---|
| 1 | No deployable A53 control service | Bare-metal Rust FFI, ThreadX/NetX application, A53 Bazel toolchain | Firmware booted and exposed control/DAP TCP services |
| 2 | Board could not meet the target by construction | GEM forced from 100 Mbit/s to 1 Gbit/s | MAC/PHY rate could support the contract |
| 3 | ARP requests arrived but produced no response | Handled NetX ARP/RARP driver commands and built Ethernet headers | Link-layer replies reached GEM transmission |
| 4 | RX stopped after a fixed small number of frames | Cleared RX ownership, enabled RXUSED recovery | Ownership/recovery became explicit; a separate descriptor-walk fault remained |
| 5 | PHY tuning appeared to affect an intermittent one-frame path | Corrected 64-byte alignment versus 16-byte BD stride | GEM walked the descriptors software actually initialized |
| 6 | Every valid IP packet failed NetX checksum validation | Added GEM two-byte RX pad for four-byte IP alignment | TCP handshake could complete with zero checksum errors |
| 7 | TX worked once, then the ring wedged | Pre-armed all TX BDs with `USED`, handled TX status safely | TX completions tracked submitted frames |
| 8 | SYN-ACK could carry a transient wrong destination MAC | Added driver-local ARP cache around vendored NetX publication race | Real TCP application data exchanged correctly |
| 9 | PL payload reached DDR but no DMA completion was visible | Disabled unconnected SG status/control stream | A53 could consume completed trace buffers |
| 10 | Six good TCP segments were followed by shifted headers | Restored NetX prepend pointer and length after encapsulation | Retransmission no longer accumulated 14-byte header drift |
| 11 | Fresh Orbtrace PS init hung in DDR calibration | Applied board preset and selected correct Avnet board part | Fresh hardware export booted reliably |
| 12 | High-priority trace polling suppressed diagnostics | Scheduled bounded real sleeps based on completed work | Telemetry and recovery remained live under load |
| 13 | Sustained trace froze with one already-ACKed segment repeating | Protected TX publication and deferred packet release to NetX helper context | Exact 3 GB capture completed without freeze |
| 14 | Original encoder ceiling was estimated near 267 Mbit/s | Ping-pong encoder, 8,192-byte payload, 64-bit packer | PL production path had sufficient architectural rate |
| 15 | DMA/TCP path performed avoidable copies and cache operations | Non-cacheable descriptor/pool region and zero-copy `NX_PACKET` handoff | Stable multi-gigabyte logging at 357.969 Mbit/s |
| 16 | Diverse target optimizations all plateaued at 42–45 MB/s | Moved the same RTL8153 from USB 2.0 to USB 3.0 | 641.059 Mbit/s, zero-loss acceptance passed |

## 4. Development and investigation method

### 4.1 Reproducible build-flash-observe loop

The firmware and Rust client were built through Bazel in the Nix development
environment:

```bash
nix develop -c bazel build //applications/orbtrace/firmware/a53_app:a53_app
nix develop -c bazel build //applications/orbtrace/model:orbtrace
```

The actual Orbtrace PL image and A53 ELF were loaded together over JTAG using
the board-validated PS initialization script. A typical deployment was:

```bash
nix develop -c env \
  XILINX_ROOT=/home/v/opt/vitis \
  XSCT="$(nix develop -c which xsct)" \
  BITSTREAM=bazel-out/orbtrace-vivado/zub_orbtrace.bit \
  PSINIT=sdk/boards/zub_1cg/generated/psu_init.tcl \
  bash tooling/xsct/jtag_flash.sh \
    bazel-bin/applications/orbtrace/firmware/a53_app/a53_app
```

The target was observed simultaneously through:

- UART diagnostic counters and descriptor dumps;
- NetX/GEM internal state exported by the firmware;
- the control service's application statistics;
- host interface, neighbor, USB, and NIC counters;
- concurrent packet capture of the actual TCP exchange;
- exact host file size and wall-clock duration.

No one telemetry source was considered authoritative for every fault. UART
could itself suffer interleaved output, internal counters could describe the
driver but not prove what crossed the wire, and a host timeout could be caused
by an incorrectly budgeted outer test timeout. Packet capture and exact file
length provided independent checks.

### 4.2 Progressive test sizes

Changes were first tested with short captures to avoid spending a minute on an
obvious regression. Stable changes progressed through small captures, 100 MB
preflight runs, and finally the full 3 GB acceptance volume. This separated
startup correctness from sustained ownership or queue failures.

### 4.3 Controlled variables and retained negative results

The work deliberately varied one suspected bottleneck at a time: payload size,
cacheability, copy versus zero-copy, AXI width, DMA aggregation, TCP queueing,
and host USB topology. Results that did not improve throughput were retained.
The repeated convergence around 42–45 MB/s under radically different target
implementations eventually became positive evidence for a common external
limit.

### 4.4 Artifact provenance

The investigation repeatedly checked hashes and the role of each bitstream.
This was essential because the committed board bitstream was a valid design,
but the wrong valid design: it supported Ethernet loopback and did not contain
the Orbtrace AXI DMA instance. A successful JTAG programming operation alone
therefore did not prove that the expected PL hardware had been installed.

### 4.5 Foundational A53 and GEM2 network bring-up

Before trace streaming could be debugged, the A53 control plane and Ethernet
driver had to become reliable enough to exchange application data. This stage
contained several independent faults that later throughput work depended on.
They are included because a regression in any one of them can again resemble a
trace-path failure.

#### 4.5.1 Bare-metal Rust and NetX service integration

The existing Rust control model was converted into target firmware through an
explicit C FFI boundary. `orbtrace_control_feed` and `orbtrace_dap_feed` allow
the ThreadX TCP service to pass raw protocol bytes into the already-tested Rust
state machines. The Rust component is linked as a static library into the A53
ELF.

This required a manual `aarch64-unknown-none` Rust toolchain definition. The
used `rules_rust` release did not support fetching that target through
`extra_target_triples`, so the matching Rust standard library was pinned by
SHA-256 and registered through a repository-local Bazel toolchain. The result
remained inside the repository convention: Bazel, not standalone Cargo, owns
compilation and caching.

The first A53 application exposed TCP `3401` for control and `3240` for DAP.
The trace port `3402` was added later. Two build integration defects were
found at this stage: the NetX source glob omitted its `nxe_*.c` checked-API
wrappers, and the JTAG flash script still used pre-reorganization board paths.
Both were corrected before relying on the on-target service.

#### 4.5.2 Machine-local Xilinx tool execution

The licensed Xilinx tools were installed, but `xsct` initially failed before
executing any JTAG Tcl. Its FHS environment did not expose the libraries
bundled with Vitis, and the one genuinely absent compatibility name was
`libtinfo.so.5`. Pointing the wrapper at `Vitis/2023.2/lib/lnx64.o` resolved the
bundled dependencies; a compatibility symlink supplied the missing SONAME.

Non-interactive `xsct` also attempted to start `Xvfb` and exited silently in
the environment. The repository's non-interactive invocations were changed to
use `-nodisp`. These were tooling obstacles rather than board defects, but
until they were fixed a failed flash and a failed firmware boot were difficult
to distinguish.

#### 4.5.3 GEM was configured for an impossible link rate

The driver originally forced GEM2 to 100 Mbit/s while the KSZ9131 PHY and host
partner negotiated gigabit Ethernet. More fundamentally, a 100 Mbit/s MAC
could never meet a 400 Mbit/s trace requirement. Initialization now calls
`XEmacPs_SetOperatingSpeed(..., 1000U)`, configuring the GEM speed bits and the
appropriate reference-clock dividers.

The final driver deliberately requires a gigabit-capable point-to-point peer.
This is an architectural constraint derived from the performance contract, not
an incidental optimization.

#### 4.5.4 NetX link commands and Ethernet framing

NetX uses distinct driver commands for IP, ARP request, ARP response, and RARP
transmission. The initial driver did not route `NX_LINK_ARP_RESPONSE_SEND`,
`NX_LINK_ARP_SEND`, or `NX_LINK_RARP_SEND` into its transmit function. The
board consequently received ARP requests but silently discarded the stack's
attempt to answer. Dispatching all relevant commands to `gem2_packet_send()`
made the response path observable.

The driver also had to construct the 14-byte Ethernet header itself. NetX's
link-driver contract supplies the upper-layer packet and resolved physical
addresses; it does not place a complete Ethernet frame in the packet. Before
the driver implemented encapsulation, the GEM transmitted upper-layer bytes
as though they began with a MAC header. `tcpdump` parsed payload bytes as a
bogus 802.3 destination/source address.

This byte-stable malformed output had initially suggested RGMII timing. A real
timing fault would not reproduce the same payload-derived “MAC address” at
every PHY delay setting. Constructing destination MAC, source MAC, and
EtherType in driver-provided headroom corrected the actual layer-boundary
error. The later prepend-pointer restoration in section 6 is a separate bug:
the header first had to exist, and then its temporary insertion had to preserve
NetX packet metadata.

#### 4.5.5 RX descriptors were not returned to hardware

`XEmacPs_BdSetAddressRx()` changes address bits but deliberately preserves the
low `NEW` and `WRAP` ownership bits. The original refill code assumed that
setting a new buffer address also cleared `NEW`. Each descriptor therefore
remained software-owned after its first received frame. With the original
four-entry ring, RX stopped after exactly four frames.

The refill path now explicitly clears the RX `NEW` bit after installing the
buffer and preserves `WRAP` only on the final descriptor. The exact four-frame
failure boundary and continuous post-fix RX counter growth established this on
hardware.

#### 4.5.6 RXUSED was both masked and misunderstood

Cadence GEM halts its RX scan when its internal pointer encounters a descriptor
still marked used. This occurs whether or not the corresponding interrupt is
enabled. With `RXUSED` masked, the driver received no signal that the hardware
had stopped; other ThreadX diagnostics continued, making the condition look
like an external link failure.

The driver now enables `XEMACPS_IXR_RXUSED_MASK`. Recovery writes RXQBASE
directly to software's known-free `rx_tail` descriptor and reenables receive.
The direct write is intentional: `XEmacPs_SetQueuePtr()` becomes a no-op after
the MAC is started. Raw RXQBASE observations confirmed that the guarded helper
had not moved the hardware pointer during prior stalls.

#### 4.5.7 A zero interface MTU silently blocked outbound IP

The NetX interface structure was zero-initialized and the driver did not set
`nx_interface_ip_mtu_size`. ARP bypassed the IP MTU check and continued to
work, while outbound IP/TCP packets were rejected before reaching the GEM send
function. On the wire this was indistinguishable from a driver that received a
SYN and failed to answer it.

Setting the MTU to 1,500 restored ordinary IP transmission. It was later raised
to 9,000 as part of the throughput work. The key invariant is that the driver
must publish a valid nonzero MTU before NetX sends IP traffic.

#### 4.5.8 TXUSED did not mean what the first recovery assumed

The original driver did not enable or acknowledge the TX “used bit read”
condition. After the first successful transmission, completion progress could
stop while the software queue continued growing.

The first attempted fix reprogrammed TXQBASE and restarted transmission on
every TXUSED event. It caused an interrupt storm with millions of ISR calls per
second and visibly corrupted UART output through CPU starvation. ZynqMP GEM
checks the permanently `USED|WRAP` dummy descriptor in TX priority queue 1
before queue 0 on every normal send. That expected dummy scan raises TXUSED;
it is not by itself proof that queue 0 is stalled.

The safe handling acknowledges TX status without blindly resetting the queue.
Normal cleanup and later poll-based diagnostics deal with actual queue-0
progress. This distinction prevented a recovery mechanism from becoming a
higher-rate failure source than the original condition.

#### 4.5.9 Descriptor alignment was incorrectly used as descriptor stride

The driver allocated a 64-byte-aligned ring and indexed descriptors 64 bytes
apart. `XEmacPs_BdRingCreate()`, however, treats its alignment argument only as
a base-address requirement. Hardware descriptor separation remains
`sizeof(XEmacPs_Bd)`, which is 16 bytes on this target.

Only software slot zero coincided with a descriptor the GEM actually visited.
The GEM then advanced by 16 bytes into zeroed native descriptors while software
initialized locations at 64-byte intervals. RX and TX could therefore fail
after the first descriptor regardless of PHY configuration.

The decisive evidence was a live RXQBASE read: it advanced from `0x61E80` to
`0x61E90` to `0x61EA0`, exact 16-byte steps, and never reached software's
assumed `base + 64` second slot. `GEM2_BD_STRIDE` is now
`sizeof(XEmacPs_Bd)` for indexing and cache ranges; the 64-byte constant is
retained only for backing-array base alignment.

This finding also resolved a misleading hardware investigation. A full
KSZ9131 `tap_sel` sweep produced the same failure at all values because the PHY
was not the cause. The invariance under the physical-layer control eliminated
the skew hypothesis and made the descriptor walk the next target.

#### 4.5.10 RX IP checksum alignment

Even with continuous descriptor progress, NetX incremented its IP checksum
error counter for every received IP packet. Independent calculation over the
DMA-captured bytes folded to `0xffff`, proving the wire packet itself was
valid.

NetX's checksum implementation casts `nx_packet_prepend_ptr` directly to an
`ULONG *` and expects four-byte alignment. Packet payload storage began
four-byte aligned, but removing a 14-byte Ethernet header placed the IP header
at an address congruent to 2 modulo 4.

The driver now configures GEM `NWCFG.RXOFFS=2`, causing DMA to place two pad
bytes before the Ethernet frame. Removing two pad bytes plus the 14-byte
Ethernet header advances by 16 bytes, leaving the IP header four-byte aligned.
Subsequent hardware runs kept `ip_csum_err` and `ip_invalid` at zero.

#### 4.5.11 The TX ring was never initialized to an idle state

`XEmacPs_BdRingCreate()` clears TX descriptor memory but does not mark the
descriptors `USED`. A zeroed TX descriptor means “owned by DMA,” not “idle and
empty.” The original RX setup armed every descriptor before starting the MAC;
the TX setup had no equivalent step.

The result was a permanent ring wedge after the first legitimate transmission:
`tx_complete` stopped at one, `tx_count` grew to the ring limit, and the queue
never drained. Comparing the implementation with Xilinx's own interrupt-DMA
example exposed the missing `XEmacPs_BdRingClone()` call. The driver now clones
a cleared template with `XEMACPS_TXBUF_USED_MASK` into every TX slot and lets
the ring helper set `WRAP` on the final slot.

After the change, `tx_complete` tracked `tx_frames` and `tx_count` returned to
zero over sustained retransmission traffic. This was the real ring-startup
fix. Earlier TXQBASE recovery attempts were not credited with the result.

#### 4.5.12 Vendored NetX ARP-entry publication race

TCP initially generated SYN-ACK frames with an incorrect destination MAC even
though the host's ARP request carried the right sender address. The fault was
traced across four evidence layers:

1. A driver-side dump showed the incoming 28-byte ARP payload was byte-correct.
2. Direct JTAG reads of the live NetX ARP entry showed the final stored MAC was
   correct.
3. Disassembly of `_nx_ip_driver_packet_send()` showed a correct adjacent
   8-byte copy of the ARP entry's MAC fields into the driver request.
4. At actual send time, the request fields sometimes held bytes from the
   incoming TCP header, proving a transient partially initialized lookup rather
   than deterministic address conversion.

In the vendored `_nx_arp_packet_receive.c`, creation of a new entry publishes
IP address, MAC most-significant word, and MAC least-significant word as three
unprotected stores. The sibling update-existing-entry path protects the same
operation with `TX_DISABLE` / `TX_RESTORE`. A concurrent sender can observe the
new IP first, treat the lookup as a hit, and read stale pool contents from MAC
fields not yet written.

Rather than patch a fetched upstream dependency, GEM2 now maintains a four-
entry driver-local IP-to-MAC cache learned directly from correctly parsed ARP
frames. IP sends prefer this cache. A range check on the request's 16-bit MAC
prefix remains only as a fallback guard.

The first guard-only attempt was insufficient: a garbage value of `0xB974`
happened to fit within 16 bits and reached the wire. With the learned cache,
hardware captured the complete ARP, SYN, SYN-ACK, ACK, application payload,
board ACK, and teardown sequence using the correct host MAC. This was the
first verified application-data exchange over TCP.

#### 4.5.13 Build and test traps exposed during bring-up

Several process failures could produce false conclusions even after the code
was correct:

- A top-level Bazel invocation without the A53 platform transition could leave
  `bazel-bin/.../a53_app_elf` pointing to an older configuration's ELF. Two
  change-build-flash cycles appeared ineffective because the flashed artifact
  was stale. Checking `readlink`, modification time, and a distinctive string
  in the ELF exposed the mismatch. Artifact hashes remain the final check.
- `bash -c '</dev/tcp/host/port'` opens the redirection but does not perform a
  meaningful read/write exchange. A raw descriptor opened with `exec 3<>...`
  or the Rust client is the valid TCP preflight.
- ICMP was never enabled with `nx_icmp_enable()`. One hundred percent ping loss
  is therefore expected and cannot be used to diagnose this firmware.
- The KSZ9131 is not necessarily reset by an A53/JTAG reflash. PHY DLL control
  experiments can persist across firmware reloads and must be explicitly
  restored or cleared by the appropriate hardware reset.

These process controls became part of the later throughput workflow: validate
the artifact actually loaded, use TCP as the liveness probe, and corroborate
target diagnostics with wire traffic.

## 5. Hurdle 1: AXI DMA payload arrived, but descriptors did not complete

### 5.1 Symptom

The initial S2MM path placed recognizable trace payload in DDR, proving that
the PL stream and memory write path were active. Software nevertheless never
observed completed buffer descriptors. From the application, this looked like
a DMA engine that accepted data and then stopped.

### 5.2 Root cause

The AXI DMA was configured with its scatter/gather status/control stream
enabled even though that stream was not connected in the block design. In this
configuration, payload movement alone is insufficient to complete the expected
descriptor transaction.

### 5.3 Change

`applications/orbtrace/vivado/create_bd.tcl` now sets:

```tcl
C_SG_INCLUDE_STSCNTRL_STRM 0
```

The disabled stream matches the actual PL connectivity. Descriptor completion
then corresponded to the payload stream that the design really supplied.

### 5.4 Evidence and consequence

After rebuilding the PL design, the software could consume completed S2MM
descriptors and send their data through the trace TCP service. This changed the
failure from “payload in memory but no completion” to an observable Ethernet
packet-integrity problem. That progression is important: the later malformed
TCP frames were not a continuation of this DMA configuration defect.

## 6. Hurdle 2: Ethernet/TCP corruption after the first valid segments

### 6.1 Wire-level signature

The board initially completed the TCP handshake and transmitted six valid
segments totaling 4,380 bytes:

```text
1028 + 5 + 1032 + 1032 + 1032 + 251 = 4380 bytes
```

The host returned the correct cumulative ACK for sequence 4381. Approximately
one retransmission timeout later, the board emitted frames whose Ethernet and
IP addresses were still correct but whose TCP fields were displaced or
nonsensical. Checksums were internally valid for the corrupted byte layout.

That combination ruled against random PHY corruption: random link corruption
would not coherently preserve selected protocol layers and recompute valid
checksums for a shifted transport header.

### 6.2 Root cause: permanent mutation of a NetX packet

`gem2_packet_send()` needed to prepend a 14-byte Ethernet header. It did so by
decrementing `nx_packet_prepend_ptr` and increasing `nx_packet_length`, then
queued the packet for asynchronous GEM DMA. It did not restore the two NetX
fields before returning.

That violated the driver's ownership contract. NetX could present the same
packet for retransmission while the original transmit was still represented
in the driver's asynchronous path. Each re-entry prepended another Ethernet
header and shifted the apparent transport data by another 14 bytes. The
approximately one-second onset matched TCP retransmission timing.

### 6.3 Change

The driver now saves and restores the original prepend pointer and packet
length on every post-mutation return path, including both successful queueing
and the bad-destination/drop path. The Ethernet header remains visible to GEM
for the DMA operation, but the `NX_PACKET` metadata returned to NetX has the
same logical layout that NetX originally supplied.

### 6.4 Instrumentation and hardware proof

A `diag5` record was added to expose:

- repeated use of the same `NX_PACKET` pointer;
- prepend pointer before and after the driver call;
- packet length before and after the driver call;
- bad-destination drops.

On hardware, retransmissions occurred without prepend-pointer or packet-length
drift, and bad-destination drops remained zero. The original post-4,380-byte
header corruption disappeared. This was a real, independently fixed defect,
but it was not the later sustained-freeze root cause.

## 7. Hurdle 3: correct firmware paired with the wrong PL/PS artifacts

### 7.1 Wrong-design bitstream

The bitstream committed under `sdk/boards/zub_1cg/` is identified by its own
artifact manifest as the Ethernet-loopback-capable design. It is not the
Orbtrace design. Loading it with the Orbtrace A53 firmware correctly caused
`trace_dma_initialize()` to fail because no Orbtrace AXI DMA existed at the
expected PL address.

This was a provenance error, not a firmware regression. The real Orbtrace
bitstream existed only as a local/Bazel build artifact.

### 7.2 DDR initialization hang in a freshly exported design

Using an Orbtrace-generated `psu_init.tcl` initially caused a repeatable hang
during DDR initialization, including after complete power cycles. The PL
design creation script instantiated the Zynq UltraScale+ PS and set a few AXI,
interrupt, and clock properties, but it had not applied the ZUBoard's real
board preset. DDR therefore retained generic IP defaults instead of the
board's LPDDR4 geometry.

Calibrating physical LPDDR4 against those defaults caused the generated PS
initialization wait loop never to complete.

### 7.3 Permanent design-generation changes

The Vivado flow was corrected in two places:

- `create_bd.tcl` invokes the repository's `zub1cg_apply_ps_preset`, which
  asserts the board's LPDDR4 type, width, and capacity.
- `build.tcl` establishes the Avnet board repository before project creation,
  requires `AVNET_BDF_ROOT`, and selects the correct board part.

A fresh synthesis/implementation/export cycle then produced a PS-correct image
that initialized DDR and ran on the board. An intermediate verified build had
WNS +2.319 ns and WHS +0.010 ns. Later data-path changes produced the final
timing result documented in section 16.

### 7.4 Process lesson

Bitstream, XSA-derived PS initialization, firmware memory map, and board preset
form a versioned hardware/software unit. A JTAG script reaching “programmed”
does not establish compatibility among them. The build hash, artifact role,
DDR configuration source, and expected PL register map must all be checked.

## 8. Hurdle 4: diagnostic/recovery starvation under sustained load

### 8.1 Scheduling defect

The trace thread ran at a higher priority than the diagnostic thread and had no
time slice. `tx_thread_relinquish()` only yields usefully to a ready thread at
the same priority, so a tight trace polling loop could monopolize the A53 even
though it appeared to “yield.” Diagnostics and recovery logic then stopped
running, making a busy-loop scheduling failure resemble a total hardware
freeze.

### 8.2 First change and its limitation

The first correction inserted a one-tick sleep after a fixed number of polling
iterations. It restored diagnostic progress, but counting empty polls made the
sleep frequency depend on CPU speed rather than useful work. A 10 ms sleep
could also allow a 32-entry DMA ring to drain while the consumer was absent.

### 8.3 Final scheduling rule

The final loop yields briefly when no DMA completion is ready and performs a
one-tick sleep only after 4,096 completed transfers. This keeps lower-priority
maintenance runnable without turning idle spin rate into an accidental data-
path throttle.

### 8.4 Evidence

UART diagnostics continued during subsequent sustained tests. However, the
network still entered a deeper freeze. Scheduling was therefore a real defect
and a necessary fix, but it was explicitly rejected as the sole root cause of
the stale TCP retransmission behavior.

## 9. Hurdle 5: misleading DMA symptoms and recovery paths

### 9.1 PHY status ruled out link loss

Live MDIO BMSR reporting was added as `diag6`. The PHY continued to report link
up across the freeze. This eliminated physical link loss as the reason the
board stopped answering ARP and TCP.

### 9.2 DMA status was decoded incorrectly at first

Observed AXI DMA status values included `0x10009`, `0x1100A`, and `0x11009`.
They initially looked like fatal DMA states. Bit-level decoding instead showed
the engine had drained the currently posted ring and was halted or idle while
waiting for descriptors. The error-mask bits were not asserting the presumed
fatal fault.

The status was therefore a consequence or idle state, not proof of a DMA
hardware failure.

### 9.3 Recovery experiments

The GEM driver gained progressively stronger recovery diagnostics and paths:

- TXQBASE resynchronization and transmit re-kick;
- RX polling recovery;
- link polling and MAC/PHY reinitialization after repeated failed lighter
  recoveries;
- counters for attempted and completed recovery operations.

The stronger fallback was made reachable because the original gating flag
dropped before its three-second threshold could ever be satisfied. Hardware
confirmed that the fallback triggered, but it did not repair the stale TCP
state.

The full reinitialization path also releases in-flight packets wholesale. That
is potentially unsafe if NetX still references them. It remains diagnostic or
last-resort code and must not be represented as the demonstrated freeze fix.

### 9.4 ThreadX event tracing was counterproductive

ThreadX event tracing support was already built into the firmware, but enabling
the runtime trace call caused an even earlier and more severe board freeze in
an A/B hardware test. The enable call remains disabled under `#if 0`. This is a
useful negative result: instrumentation can perturb timing and ownership in a
system already failing through concurrency.

## 10. Hurdle 6: the sustained TCP freeze

This was the central correctness problem. It required two related driver fixes
and wire evidence to identify the actual cross-layer race.

### 10.1 Decisive packet capture

A concurrent `tcpdump` captured a clean transition into the frozen state. The
board repeatedly transmitted one TCP segment with sequence range
`72252:73284`. The host had already cumulatively ACKed through `78585`, and it
continued returning the same valid cumulative ACK after each retransmission.
The board resent the stale segment once per second indefinitely, without the
normal exponential backoff pattern.

This was wire-proven evidence that:

- the host received and acknowledged the data;
- the host TCP implementation was not withholding an ACK;
- the board's PHY and MAC could still place frames on the wire;
- the stale segment remained in the board TCP retransmission state after it
  should have been dequeued;
- the fault was above raw DMA completion and below application progress.

It also explained why the board eventually appeared ARP-dead: a corrupted or
wedged NetX ownership structure could stall the network service even while the
physical link remained up.

### 10.2 First concurrency defect: TX-ring bookkeeping race

`gem2_packet_send()` runs in thread context while `gem2_tx_cleanup()` formerly
ran in interrupt context. Both manipulated `tx_head`, `tx_count`, and
`tx_pkts[]`. Publication of a new descriptor and cleanup of a completed one
were not protected by interrupt masking.

The non-atomic sequence allowed classic lost-update and slot-reuse failures:

1. Thread context selected a free slot and began publishing its packet.
2. The GEM ISR cleaned another slot and decremented `tx_count`.
3. Thread context incremented a stale `tx_count` value or overwrote a packet
   pointer before the old owner was released.
4. Software ring occupancy and actual descriptor ownership diverged.

The driver already used `TX_DISABLE` / `TX_RESTORE` around an analogous shared
NetX state update elsewhere, making the missing protection especially clear.

The transmit publication sequence was placed inside a ThreadX interrupt-
masking critical section. Hardware regression tests showed no new destination
drops or prepend/length mutation. This fixed a genuine race, but the stale
already-ACKed retransmission could still be reproduced, so investigation
continued.

### 10.3 Final root cause: releasing NetX packets from the GEM ISR

The more fundamental violation was the execution context of cleanup itself.
`gem2_tx_cleanup()` called `nx_packet_transmit_release()` directly from the GEM
interrupt handler. At the same time, the NetX IP helper thread could process an
incoming ACK and walk the TCP transmitted-packet queue.

Those operations touch overlapping `NX_PACKET` state:

- `nx_packet_queue_next` and retransmission-queue linkage;
- driver-done ownership flags;
- prepend pointer and logical length;
- IP/TCP header location and header-length interpretation.

The ACK walker could read queue or header state while the ISR-side release
mutated it. The resulting stale entry matched the wire capture: a packet that
had completed at the driver and had been cumulatively ACKed remained reachable
as the TCP retransmission head.

This initially looked like a defect wholly inside the vendored NetX ACK
dequeue code. Reading the local NetX sources clarified the ownership contract:
the retransmit path only considers a packet marked `NX_DRIVER_TX_DONE`, so the
packet had necessarily passed through driver release. The missing fact was
that the driver performed that release concurrently with the ACK queue walker.

### 10.4 Correct serialization change

Hardware TX completion now requests `NX_LINK_DEFERRED_PROCESSING`. NetX invokes
the driver's deferred-processing case from its IP helper thread, and only that
thread runs `gem2_tx_cleanup()` and `nx_packet_transmit_release()`.

The relationship changed from:

```text
GEM ISR                         NetX IP helper thread
  nx_packet_transmit_release()    ACK queue walk
              \                 /
               concurrent mutation
```

to:

```text
GEM ISR
  record/acknowledge completion
  request NetX deferred processing
       |
       v
NetX IP helper thread
  gem2_tx_cleanup()
  nx_packet_transmit_release()
  ACK processing
  [serialized packet ownership]
```

TX-ring publication remains protected by `TX_DISABLE` / `TX_RESTORE`. Thus the
final design addresses both levels of concurrency: the driver's own ring
bookkeeping and NetX's higher-level packet ownership.

### 10.5 Hardware proof

After deferred cleanup was implemented, a 3 GB capture completed in 67.045
seconds with a file size of exactly 3,000,000,000 bytes and zero device error
counters. The former freeze had occurred in every sustained-load attempt; it
did not occur over the full 67-second, multi-gigabyte run.

That test proved the freeze fix even though its 357.969 Mbit/s rate did not yet
meet the acceptance target. Correctness and throughput were deliberately
reported as separate outcomes.

## 11. Hurdle 7: cache coherency and descriptor false sharing

### 11.1 Why cache maintenance alone was unsafe

GEM buffer descriptors are 16 bytes while the Cortex-A53 data-cache line is 64
bytes. Multiple descriptors therefore occupy one cache line. If software cleans
a line after DMA has updated an adjacent descriptor's ownership/status bits,
the clean can write an older cached copy of that adjacent descriptor back to
memory, erasing the hardware update.

This is false sharing between software-owned and DMA-owned fields. Correctly
cleaning the descriptor being submitted is not sufficient when the cache
operation covers neighbors owned by hardware.

### 11.2 Change

GEM RX/TX descriptors, the AXI DMA descriptors, and the dedicated trace packet
pool were moved into `.trace_dma` at `0x10000000`. The linker emits it as its
own writable PT_LOAD region, and platform memory attributes map the containing
2 MiB translation block as non-cacheable.

The final ELF contains:

```text
.trace_dma address 0x10000000, size 0x1b7040, alignment 64
```

`0x1b7040` is 1,798,208 bytes, leaving 298,944 bytes below the section's
2 MiB limit. The linker assertion requires `__trace_dma_end <= 0x10200000`, so
future pool growth cannot silently cross the translation-block boundary.

### 11.3 Ring and pool sizing

The final relevant sizes are:

| Resource | Final value | Purpose |
|---|---:|---|
| GEM TX descriptors | 64 | Absorb queued TCP segments and avoid the original four-slot saturation |
| GEM RX descriptors | 64 | Provide receive/ACK headroom under jumbo traffic |
| AXI DMA S2MM descriptors | 32 | Continuous PL-to-DDR rotation |
| AXI DMA buffer size | 16,512 bytes | Hold two worst-case encoded Orbflow frames |
| Trace NX packet payload | 18,688 bytes | Alignment and protocol headroom around DMA payload |
| Trace NX packet count | 96 | Descriptor replacements plus TCP ownership headroom |
| General cached packet payload | 10,304 bytes | Control/DAP and non-trace network traffic |
| General cached packet count | 192 | Preserve independent stack capacity |

Separating the trace pool also prevents sustained zero-copy trace ownership
from exhausting the general NetX packet pool.

### 11.4 Unsafe experiment rejected

A narrower, partial cache-clean optimization caused data loss or stalls and was
reverted. The non-cacheable ownership design was retained because it establishes
a simple invariant: neither the A53 cache nor a neighboring descriptor cache
line can conceal or overwrite DMA ownership changes.

## 12. Hurdle 8: copy-heavy DMA-to-TCP handoff

### 12.1 Previous path

The first firmware copied each completed S2MM payload from a dedicated DMA
buffer into a newly allocated NetX packet. The GEM driver then flattened or
copied that packet again for its own transmit staging. This added memory
bandwidth, cache maintenance, and packet-pool churn to every trace block.

### 12.2 True zero-copy handoff

Each of the 32 AXI DMA descriptors now owns an actual `NX_PACKET` data buffer
allocated from the non-cacheable trace pool. On completion, firmware:

1. validates the descriptor status and received length;
2. detaches the completed packet from that descriptor slot;
3. allocates a fresh trace packet;
4. rearms the descriptor with the fresh packet before advancing the ring tail;
5. sets the completed packet's NetX prepend/append/length fields;
6. marks it with the driver-private `GEM2_PACKET_NONCACHE` capability bit;
7. gives the completed packet directly to NetX TCP.

The GEM driver recognizes the private marker and skips redundant cache clean
operations. For a single-buffer NetX packet, it programs GEM directly from the
packet. Chained packets retain a flattening fallback so correctness does not
depend on every NetX caller producing a contiguous buffer.

### 12.3 Result interpretation

Zero-copy reduced unnecessary target work and removed a scalability risk, but
under the original host topology it did not lift throughput above the same
44–45 MB/s band. That negative result later helped show that the final ceiling
was external to the A53 copy path.

## 13. Hurdle 9: RTL encoder throughput and AXI width

### 13.1 Original serialized encoder ceiling

The original Orbflow encoder effectively performed packet load, COBS group
discovery, and encoded-data emission as sequential phases. Its approximate
cost was three cycles per input byte. At 100 MHz, that implied a ceiling near:

```text
100,000,000 cycles/s / 3 cycles/byte * 8 bits/byte
  ~= 267 Mbit/s
```

No network optimization could make a 267 Mbit/s producer satisfy a 400 Mbit/s
end-to-end requirement.

### 13.2 Ping-pong encoder

`orbtrace_orbflow_encoder.sv` was redesigned around two banks. Each bank stores:

- the raw channel/payload/checksum bytes; and
- the length of each COBS group.

The load side incorporates at most one new raw byte per clock and records a
group boundary on zero bytes or after 254 non-zero bytes. The emit side reads
the other full bank and generates COBS code bytes, group data, and the final
delimiter. Once a bank is complete, load switches to the alternate bank while
emit consumes the completed one.

This overlaps packet acquisition with encoding instead of serializing them.
The memories are deliberately represented as separate single-write-port
distributed RAM structures, avoiding an inferred multi-write memory that would
map poorly or ambiguously in synthesis.

The payload was increased to 8,192 bytes. A worst-case encoded frame remains
bounded at 8,228 bytes.

### 13.3 Verification of overlap and backpressure

The randomized pipeline test now sends two packets, varies downstream ready,
checks every encoded byte, and requires evidence that one bank was loading
while the other was emitting. A test that only compared final bytes would not
prove the intended throughput overlap.

### 13.4 8-to-64-bit stream packer

A new `orbtrace_axis_packer.sv` accumulates encoder bytes into 64-bit AXI Stream
beats. It supplies `TKEEP` for a partial final beat, propagates the correct
`TLAST`, and retains output unchanged under backpressure. The Vivado design
configures both the AXI DMA S2MM stream width and its memory-write width to
64 bits.

The packer can combine complete Orbflow frames into a larger DMA transfer. The
final configuration uses `FRAMES_PER_TRANSFER=2`, so two independently framed
and delimited Orbflow records share one 16,512-byte DMA buffer. Orbflow framing
is not altered; only the DMA/TCP transaction granularity changes.

A focused XSim test covers full beats, partial beats, backpressure stability,
and an intermediate frame boundary. The broader RTL suite also covers capture,
DAP behavior, and the randomized encoder pipeline.

### 13.5 Resource and timing impact

The implemented final design uses:

| Resource | Used | Available | Utilization |
|---|---:|---:|---:|
| CLB LUTs | 16,099 | 37,440 | 43.00% |
| CLB registers | 13,500 | 74,880 | 18.03% |
| Block RAM tiles | 5 | 108 | 4.63% |
| DSPs | 0 | 216 | 0.00% |

The changes retained substantial headroom and required no DSP resources.

## 14. Hurdle 10: NetX TCP and Ethernet configuration

### 14.1 Jumbo path

The private point-to-point network was moved to MTU 9000 end to end:

- the NetX interface MTU is 9000;
- GEM jumbo-frame handling is enabled;
- receive buffers use `XEMACPS_RX_BUF_SIZE_JUMBO`;
- the host NetworkManager profile applies MTU 9000;
- the trace socket advertises an MSS of 8960 bytes.

This reduced per-byte TCP/IP/Ethernet header cost and packet-processing rate.
Moving from roughly 1,450-byte payloads to 4,096-byte jumbo payloads improved a
representative observed rate from approximately 42.4 MB/s to 45.5 MB/s, but
larger payloads alone did not cross 50 MB/s.

### 14.2 Window scaling and queueing

NetX is built with:

- `NX_ENABLE_INTERFACE_CAPABILITY`;
- `NX_ENABLE_TCP_WINDOW_SCALING`.

The trace socket uses a 40-packet transmit queue and negotiates the 8,960-byte
MSS. Diagnostics showed approximately 230 KB outstanding with the full queue
active, disproving a stop-and-wait interpretation.

### 14.3 Checksum offload

The GEM interface advertises IPv4, TCP, and UDP transmit checksum capability.
NetX can therefore use the MAC's supported checksum path instead of consuming
A53 cycles for every trace segment.

These changes form the appropriate sustained-throughput configuration even
though no individual change explained the invariant USB 2.0 ceiling.

## 15. Hurdle 11: the host looked gigabit but was attached through USB 2.0

### 15.1 Target-side optimization plateau

After the freeze fix, substantially different target configurations converged
on approximately 42–45 MB/s:

| Experiment | Representative outcome | Interpretation |
|---|---:|---|
| ~1,450-byte payload | ~42.4 MB/s | Packet overhead was material but not the only limit |
| 4,096-byte jumbo payload | ~45.5 MB/s | Jumbo frames helped |
| 8,192-byte payload | ~44.3 MB/s | Larger payload alone did not remove the ceiling |
| Copy vs. zero-copy | Same band | A53 copy cost was not the active final cap |
| Cacheable vs. non-cacheable payload | Same band | Cache policy was not the active final cap |
| 8-bit vs. 64-bit AXI path | Same band | PL/DMA width was no longer the active cap |
| One vs. two Orbflow frames per DMA transfer | ~44.4 to ~44.9 MB/s | Per-send overhead was not the primary cap |
| TCP window/queue/MSS tuning | Same band | Connection was not stop-and-wait limited |

The exact 3 GB pre-acceptance run took 67.045 seconds:

```text
3,000,000,000 bytes / 67.045 s = 44.746 MB/s = 357.969 Mbit/s
```

It completed with zero target drops, synchronization losses, or DMA faults and
with zero host NIC receive errors or drops. The remarkable invariance across
target architectures indicated a shared downstream bottleneck.

### 15.2 USB topology evidence

The Ethernet link itself negotiated 1 Gbit/s, but the host RTL8153 adapter was
enumerated upstream at:

```text
USB speed   480 Mbit/s
USB version 2.10
driver      r8152
```

A USB-Ethernet adapter can report a gigabit Ethernet link while its USB parent
is limited to high-speed USB 2.0. Protocol and controller overhead make a
44–45 MB/s application plateau consistent with that transport.

### 15.3 Controlled physical change

The same adapter, identified by stable MAC `00:e0:4c:75:87:68`, was moved to a
different host port. It re-enumerated as:

```text
interface   enp0s13f0u1
USB speed   5000 Mbit/s
USB version 3.00
host IP     192.168.1.1/24
MTU         9000
```

The NetworkManager connection was bound to the adapter MAC rather than its
unstable interface name, so the static IP and jumbo MTU survived the port
change.

A 100 MB preflight immediately completed in 1.245 seconds, approximately
642 Mbit/s, with unchanged device error counters. No board-side code or
bitstream change was made between the USB 2.0-limited result and this
SuperSpeed result. The subsequent 3 GB acceptance test achieved 641.059 Mbit/s.

This A/B result proves that the final 357.969 Mbit/s ceiling was in the host USB
2.0 transport, not in the PL, DDR, A53, GEM, PHY, or TCP implementation.

## 16. Acceptance artifact verification

### 16.1 Final implementation timing

The final Vivado timing summary reports:

```text
WNS  +2.394 ns
TNS   0.000 ns, 0 failing endpoints
WHS  +0.012 ns
THS   0.000 ns, 0 failing endpoints
All user specified timing constraints are met.
```

This is the timing report for the final 64-bit, two-frame-coalescing design,
not an earlier functional bitstream.

### 16.2 Artifact hashes

The hardware-tested artifacts are:

```text
zub_orbtrace.bit
  sha256 3b6c16f2b1a3e6be8929d8d6df8b26e771385fc1a3b06fcdfd371d444d2424bf

a53_app ELF
  sha256 a8a80c4bbaded48572d208a5408245fde68b528d5b428da00246507b74c7440c
```

These hashes provide a stronger identity than a filename in a mutable Bazel
output tree.

### 16.3 Final device counters

After the acceptance sequence, the application reported:

```text
rx_bytes=6178979844
dropped_bytes=0
sync_loss=0
fifo_high_water=0
dma_faults=0
```

The acceptance script independently compares the before/after values of
`dropped_bytes`, `sync_loss`, and `dma_faults`; it does not merely assert that a
later snapshot happens to contain zero.

### 16.4 Supporting test coverage

In addition to the hardware acceptance test, the affected work was checked by
the repository's host/firmware tests and focused RTL simulations. Recorded
passing coverage includes:

- control firmware test;
- firmware common test;
- trace workload test;
- Orbtrace Rust model test;
- register schema test;
- capture RTL test;
- DAP RTL test;
- 64-bit AXI packer full/partial/backpressure test;
- randomized two-packet Orbflow pipeline test with required ping-pong overlap.

`git diff --check` also reports no whitespace errors in the current change set.

## 17. Change inventory by subsystem

### 17.1 A53 application

`applications/orbtrace/firmware/a53_app/src/main.c` now contains:

- the TCP `3402` trace service;
- a 32-descriptor AXI DMA S2MM ring;
- descriptor status validation and fault counters;
- a separate non-cacheable trace `NX_PACKET_POOL`;
- per-descriptor packet ownership and replacement;
- zero-copy completed-packet delivery to NetX;
- 16,512-byte transfers carrying two Orbflow frames;
- bounded cooperative sleeping based on completed work;
- expanded GEM, PHY, TCP queue, DMA, and recovery diagnostics;
- jumbo MSS and enlarged trace transmit queue configuration.

### 17.2 GEM2 driver

`third_party/os_abstraction_layer/ThreadX/ThreadXGEM2Driver.c/.h` now contain:

- 64-entry RX and TX rings;
- descriptor storage in `.trace_dma`;
- jumbo RX buffer and MTU configuration;
- IPv4/TCP/UDP checksum capability advertisement;
- direct transmit from a contiguous NetX packet;
- fallback flattening for packet chains;
- non-cacheable packet marker handling;
- restoration of NetX prepend pointer and logical length;
- protected TX-ring publication using `TX_DISABLE` / `TX_RESTORE`;
- deferred TX completion cleanup in the NetX IP helper thread;
- RX polling and TX/link recovery diagnostics;
- bounded descriptor dumps that do not walk beyond the ring.

### 17.3 RTL

The Orbtrace RTL now contains:

- an 8,192-byte ping-pong Orbflow/COBS encoder;
- simultaneous load and emit across alternate banks;
- recorded COBS group lengths in distributed memories;
- a backpressure-safe 8-to-64-bit AXI Stream packer;
- `TKEEP` generation for partial beats;
- two-frame DMA transfer coalescing;
- extended randomized and directed testbenches.

### 17.4 Vivado design

The Vivado block design now:

- disables the unused DMA SG status/control stream;
- configures 64-bit S2MM stream and memory data paths;
- applies the ZUBoard PS preset and correct board part before generating
  hardware artifacts.

### 17.5 NetX build and linker

The NetX Bazel configuration enables interface capabilities and TCP window
scaling. The A53 linker script gives `.trace_dma` a separate load segment at
`0x10000000` and asserts that it remains within the intended 2 MiB non-cacheable
translation region.

### 17.6 Host model and acceptance test

The Rust model's capture command streams socket data into a file with a 64 KiB
read buffer and `write_all`, tracking an exact `u64` byte count. The shell
acceptance test:

- performs a real control-service preflight;
- selects the deterministic PL source;
- captures exactly 3 GB;
- obtains the resulting file size with `stat`;
- avoids signed 64-bit shell overflow by reducing nanoseconds to microseconds
  before multiplying;
- compares pre/post error counters;
- enforces a 75-second capture timeout around a 60-second performance target.

The arithmetic correction is significant. The former expression multiplied
the 3 GB byte count by 8,000,000,000, which exceeds signed 64-bit shell
arithmetic and could invalidate the very test intended to prove the result.

## 18. Rejected explanations and why they were rejected

### 18.1 “The PHY dropped link”

Rejected because live MDIO BMSR remained link-up across the freeze and the
board continued transmitting the stale segment on the wire.

### 18.2 “The AXI DMA status proves a fatal hardware fault”

Rejected after decoding the observed status bits. The values represented an
idle/drained ring awaiting more descriptors, not the assumed fatal error.

### 18.3 “The driver simply never completed the TCP packet”

Rejected because NetX's retransmit path only selected a packet already marked
driver-TX-done. The issue was concurrent release versus ACK-queue traversal,
not absence of all release activity.

### 18.4 “TX-ring interrupt masking alone fixes the freeze”

Rejected by hardware reproduction after the critical section was added. The
critical section fixed a real bookkeeping race but not the higher-level NetX
ownership race.

### 18.5 “Full MAC reinitialization is the solution”

Rejected because the recovery was made reachable and observed to trigger
without repairing the stale TCP state. Its blanket packet release also makes
it unsuitable as evidence of a clean ownership fix.

### 18.6 “ThreadX trace will reveal the freeze without changing it”

Rejected by A/B hardware testing: enabling runtime event tracing caused a worse
freeze. It remains disabled.

### 18.7 “The A53 copy, cache policy, AXI width, or socket-send rate is the
remaining 45 MB/s bottleneck”

Rejected as the final explanation because large structural changes to each
factor converged on the same USB 2.0 plateau, while changing only the host USB
port immediately produced about 642 Mbit/s.

### 18.8 “Ping is the board liveness test”

Rejected because the firmware never enables NetX ICMP. A failed ping is normal.
TCP port `3401`, ARP neighbor state, UART diagnostics, and the Orbtrace client
are the valid liveness checks.

## 19. Technical lessons from the development process

### 19.1 Similar symptoms can arise from independent layers

“Capture stopped” described at least four distinct conditions during this
work: missing DMA descriptor completion, malformed retransmitted packets,
thread starvation, and a corrupted TCP retransmission queue. Only the final
wire capture distinguished the last case decisively.

### 19.2 DMA correctness is an ownership protocol

Physical addresses and cache flushes are necessary but not sufficient. Every
buffer and descriptor needs a single current owner, a defined handoff point,
and serialization against adjacent cache-line or interrupt-context mutation.
Moving shared DMA state to non-cacheable memory simplified that protocol.

### 19.3 RTOS interrupt legality is not the same as stack-level safety

A function may be callable from an ISR in isolation and still be unsafe when it
mutates an object simultaneously traversed by a protocol thread. Deferring
cleanup to NetX's helper thread aligned the driver with the stack's ownership
domain instead of merely protecting individual counters.

### 19.4 Negative performance results are diagnostic evidence

The failure of zero-copy, 64-bit AXI, and coalescing to lift the 45 MB/s limit
was not wasted work. Those changes established that very different board-side
cost structures shared one ceiling. That pattern directed attention to the
host transport and made the USB 2.0 finding testable.

### 19.5 Link speed must be checked at every bridge

The board PHY and host NIC reported gigabit Ethernet, yet the NIC's upstream
USB bridge was high-speed USB 2.0. End-to-end throughput is bounded by the
slowest negotiated segment, including bridges that are not visible in an
ordinary Ethernet link query.

### 19.6 Tests are part of the implementation

The original acceptance harness had a broken preflight and an overflowing rate
calculation. A successful data path could not produce a defensible claim until
the test itself used exact file length, safe arithmetic, deterministic input,
error-counter deltas, and a timeout budget larger than its required capture
window.

## 20. Reproduction procedure

The following sequence reproduces the final proof on the currently supported
lab topology.

### 20.1 Confirm host link and USB mode

Locate the interface by MAC rather than assuming its enumerated name:

```bash
ip -br link
ip -br addr
```

Require the adapter with MAC `00:e0:4c:75:87:68` to have host address
`192.168.1.1/24`, MTU 9000, and a USB parent speed of at least 5000. A reported
speed of 480 means the final acceptance result is expected to be host-limited.

### 20.2 Build

```bash
nix develop -c bazel build //applications/orbtrace/firmware/a53_app:a53_app
nix develop -c bazel build //applications/orbtrace/model:orbtrace
```

### 20.3 Verify artifact identity

```bash
sha256sum \
  bazel-out/orbtrace-vivado/zub_orbtrace.bit \
  bazel-bin/applications/orbtrace/firmware/a53_app/a53_app
```

Compare the output with the hashes in section 16.2 when reproducing this exact
result. Do not substitute the committed Ethernet-loopback bitstream.

### 20.4 Flash if required

Use the JTAG command in section 4.1 with the board-generated PS initialization
script. Confirm normal completion before attributing a later service failure to
firmware.

### 20.5 Use TCP, not ICMP, for preflight

```bash
timeout 5 bash -c \
  'exec 3<>/dev/tcp/192.168.1.50/3401 && echo OK'
```

### 20.6 Run the authoritative test

```bash
nix develop -c bazel test --config=host \
  //tests:orbtrace_throughput_test --test_output=streamed
```

Do not wrap the entire command in an outer timeout shorter than the sum of its
internal operations. Killing the sequence during capture can leave a torn-down
TCP session that resembles the historical board freeze.

## 21. Residual cautions and repository state

The objective is achieved, but several engineering cautions remain:

- The implementation and reports are committed in the release-preparation
  history. No release tag is implied by this dated engineering report.
- The final Orbtrace bitstream is a Bazel output/cache artifact rather than a
  committed board artifact. Its hash must be retained, or the image must be
  reproducibly rebuilt before cache garbage collection.
- The committed board bitstream remains the Ethernet-loopback design and must
  not be used for Orbtrace hardware tests.
- ThreadX runtime event tracing remains disabled because it perturbed the
  failing system severely in hardware testing.
- The full MAC/PHY reinitialization fallback remains a cautious recovery path,
  not the proven concurrency fix. Its treatment of in-flight packets deserves
  review before relying on it as a production guarantee.
- Existing root-owned background `tcpdump` processes were deliberately not
  terminated because doing so was outside the test operation and could require
  credentials. They are read-only but should be accounted for during lab
  cleanup.

None of these cautions invalidates the acceptance result. They concern artifact
retention, diagnostic perturbation, and maintainability rather than the
measured 641.059 Mbit/s data path.

## 22. Conclusion

The project progressed from a nominally complete but nonfunctional data path
to a timing-clean, sustained, hardware-proven trace logger. The decisive
technical achievements were:

1. correcting AXI DMA completion configuration;
2. restoring NetX packet metadata after Ethernet encapsulation;
3. making the Vivado PS configuration reflect the physical board;
4. protecting GEM TX-ring publication from interrupt races;
5. moving NetX packet release out of the GEM ISR and into NetX's serialized IP
   helper context;
6. eliminating descriptor/cache-line false sharing;
7. implementing zero-copy DMA-to-NetX packet ownership;
8. overlapping Orbflow encoding with ping-pong banks;
9. widening and aggregating the AXI Stream path;
10. configuring jumbo TCP and checksum capabilities; and
11. identifying and removing the final host USB 2.0 transport limit.

The strongest proof is not any single internal counter or estimated RTL rate.
It is the agreement among the final timing report, exact artifact hashes,
unchanged target error counters, a complete 3 GB host file, and a 641,059,166
bit/s Bazel hardware-test result over the full PL-to-filesystem path.
