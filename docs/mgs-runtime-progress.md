# MGS USA native boot

## Verified target and result

Primary target: the user's USA v1.0 ZIPs, extracted under `data/discs/usa`
without modifying the archives. CD1 is SLUS-00594; CD2 is SLUS-00776.
The main executables and their `/MGS` copies share SHA-256
`3342f75717c5134aec832547691c2354e67a253abfcfa9e0e823e2133bb1fc32`.
Entry is `0x8009B9A8`; executable code ends at `0x8009FA28` exclusive.
`prepare_native.py` verifies the hash and patched instruction words.

The native build displays the Konami and KCE Japan logos, introductory titles,
“Alaska - Bering Sea”, and the start of the underwater opening. A normal NTSC
run presented through Vulkan on the RTX 4060 completed 3,000 VBlanks in
50.004234 seconds (`data/native/usa-boot-ntsc`). Metrics recorded 13,494 BIOS
thread switches and 56,793 Gouraud-textured quad commands, with no interpreter
fallback counters. The corresponding screenshots were inspected. An earlier
accelerated diagnostic reached 4,000 VBlanks; it is not the timing reference.

Initial boot is established, and the input follow-up reaches controllable
dock gameplay with manual Codec opening. This is not a fully verified port.
The compiled overlays now include `title`, `opening`, `d00a`, `s00a` and `s01a`.
The heliport transition, in-game saves and disc switching still need validation.
The final accelerated gameplay probe completed 20,000 VBlanks with no
interpreter fallback counters; movement and manual Codec captures were inspected.
See [input and memory-card implementation](mgs-input-memory-card.md) and
[gameplay evidence](mgs-gameplay-evidence.json).
Audio output is nonzero in runtime metrics; listening quality is not certified.

The default 16:9 profile also passed the Vulkan smoke test: 3,000 VBlanks
in 50.002783 seconds, six visible captures, 1,669 adjusted render matrices
and no interpreter fallback counters (`data/native/usa-wide-smoke`).
Machine-readable results are in [mgs-native-boot-evidence.json](mgs-native-boot-evidence.json).

## What was required

- Native POSIX `ucontext` continuations preserve translated C++ stacks through
  BIOS OpenTh/ChangeTh and MGS's MTS scheduling. Guest TCBs remain in guest RAM.
  A test switches twice between guest tasks and checks both native locals and
  guest registers. Savestate version 6 reconstructs these continuations from
  guest TCBs and native resume PCs, rather than serializing host stacks.
  Same-process repeated loads and a fresh-process gameplay load passed; see
  [native savestates](mgs-savestates.md).
- The CD FIFO retains a shared cursor across CPU reads and DMA. MGS reads a
  12-byte sector header and its payload in separate transfers; discarding the
  sector after the first DMA prevented boot. Snapshot version 5 includes this
  cursor and continues reading older versions.
- Native PsyQ CD handlers retain ownership of each IRQ transaction. Callback
  dispatch blocks recursive CD delivery; ResetCallback preserves registered
  handlers when later sound initialization calls it again.
- The register-only MTS idle loop yields to pending interrupts. Native-thread
  VSync callbacks are delivered once per elapsed VBlank, including idle waits.
- BIOS kernel allocation, stable syscall table pointers and minimal memory-card
  presence/sector transactions cover startup. Persistent card images and BIOS
  file operations now have regression coverage; in-game save/load is pending.
- The original CD, GTE, DrawOTagEnv and ResetGraph routines are compiled.
  ResetGraph must initialize SDK-private GPU information: the inherited HLE
  no-op caused native SetDrawEnv to emit an invalid drawing area.
- Corrected ClearOTag/ClearOTagR link direction and 24-bit tag addresses. The
  inherited reverse-table implementation skipped the actual geometry. Tests
  now exercise GPU traversal through an inserted middle-bucket command.
- Overlay jump tables may lie after executable code in the same resource.
  Detection reads the whole resource, while code generation stays within the
  verified code extent. Runtime dispatch selects overlapping overlays by
  immutable identity bytes in loaded RAM instead of overwriting one global map.
- SDK entry `0x80093174` is a configured native alias, eliminating the observed
  private-label interpreter fallback from the measured opening.

## Cutscene timing

SPU IRQ registration and the audio-driven stream clock are corrected. Audio
IRQ slot 9 no longer aliases VBlank, and guest audio progresses independently
of SDL buffer size. Version 7 states retain this clock and migrate existing
v6 MGS states. See [stream clock validation](mgs-stream-clock.md).

## Donor and overlay evidence

SwanStation is rebuilt from the revision in `donor/source.json`, with the
included instrumentation patch and snapshot header. The original Crash donor
is unchanged. The harness uses `PS1_DONOR_WORK_DIRECTORY` for isolated state.
`run_oracle.sh` defaults to interpreter instrumentation, software rendering,
fast boot and **165% CPU**. USA reaches main entry at donor frame 686.

All five raw `.b` overlays load at `0x800C5968`. The title resource is byte-identical
to the donor snapshot at its actor entry (frame 1164); opening is byte-identical
to donor RAM at frame 2000. The executable ranges and 64-byte identities of
`d00a` and `s00a` match donor RAM at frames 7000 and 10000 respectively; mutable
actor-table fields and trailing data are excluded from that code comparison.
Hashes, lengths and code boundaries are in
[mgs-usa-overlay-evidence.json](mgs-usa-overlay-evidence.json). Identity matching
is a stage-selection mechanism, not a checksum of every instruction at runtime.

## Requested defaults

`game.profile = "mgs_usa_v1_0"`, `widescreen_16_9 = true` enables the initial
16:9 implementation and matching Vulkan aspect ratio. At verified
`DG_AdjustOverscan` entry `0x80017B38`, it scales the horizontal render-matrix
row and translation by 3/4. The original vertical overscan adjustment remains.
This preserves the vertical field of view across rotated cameras. Tests cover
several orientations, disabled behavior and large signed translations.
Set `widescreen_16_9 = false` in the generated configuration for reference 4:3;
this runtime toggle does not require rebuilding the translated code.

The published US AR patch instead forces the instruction at `0x80017A78` to
`addiu v0,zero,0x0c00`; its author reports camera-angle limitations:
https://forums.pcsx2.net/Thread-PSOne-Widescreen-Patches?pid=559122
https://forums.pcsx2.net/Thread-PSOne-Widescreen-Patches?pid=559272
Our render-matrix adjustment is a derived implementation, not a claim that
this published code works universally. HUD composition, edge culling and
later cameras still require game-wide validation.

**165% is the donor CPU setting.** The native runtime executes translated code
at host speed and has no guest CPU cycle throttle; therefore it does not expose
a fictitious native 165% clock. NTSC VBlank and CD timing retain their nominal
rates, and the game's 30/20 fps divisors remain in original code. A calibrated
CPU-budget model is still pending if exact overclock timing is required.
2x interpolation and advanced original-asset Vulkan rendering are deferred.

## Reproduction

```
python3 scripts/prepare_usa_discs.py
./scripts/build_native.sh 'data/discs/usa/disc1/Metal Gear Solid (USA) (Disc 1) (v1.0).cue'
./scripts/run_native.sh
python3 scripts/smoke_native.py
```

The smoke test uses normal NTSC pacing, 3,000 VBlanks, Vulkan presentation,
metrics and multiple visible frame captures. `--headless` bypasses the presenter
for diagnostics. Output lives under ignored `data/native/`; no retail assets,
generated translations or screenshots are committed.

The diagnostic exit stops the host after its target VBlank. The inherited host
shutdown waits two seconds for the still-running guest and then exits without
destructors; graceful cancellation of suspended guest continuations is pending.
This exit behavior is not used as proof of boot: rendered output, native
coverage and elapsed timing are checked separately.

## Earlier Italian work

Italian EXE SHA-256:
`43cecb21d128b93c882e5eede031bc9d2a4c95a50167198f2b9f8f5fd0514ce6`.
Entry `0x8009A35C`, code end `0x8009E3DC`. Donor entry was frame 1008.
The original Italian asset audit remains useful, but its addresses and PAL
assumptions are not applied to the active USA build.
