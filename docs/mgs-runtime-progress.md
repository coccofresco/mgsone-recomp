# MGS USA native boot

## Alpha gameplay transitions and endurance — 2026-10-07

Later Mantis phases now reach original victory, the 512-life reward and the
caves; the Hind roof roundtrip passes from a retail tower save; the jeep chain
reaches the original ending handoff. These paired/replayed checks retain
explicit post-hit fixtures and do not certify complete fights. The donor now
actually attaches its second controller, fixing a false failure in Mantis.

A new single-process endurance runner passed 180 segments with 180 saves and
360 loads; warmed-up RSS and descriptor counts stayed stable. A further 56
segments passed on the final build. Vulkan allocation telemetry is implemented
but physical GPU acceptance remains open. Final tests: 1,355 runtime passes,
31 opt-in skips and 187 Python passes. See the
[scope, sources and reproduction](mgs-alpha-gameplay-soak.md).

## Mandatory gameplay gates and callback coverage — 2026-10-06

Build `9916ff0d…` fixes a reproducible recompilation crash after the first
possessed-Meryl encounter: `padmng.c`'s original Die callback was referenced
but not registered because the previous function returned with `JR RA / SH`.
The callback scanner now accepts useful return-delay instructions, while
requiring an explicit pointer reference and rejecting control transfers in
that slot. Verified boot/menu overlays merge the current callback scan too.
Across 73 overlays, 13 missing callable entries in ten overlays are now
registered. This is static coverage, not 13 independently demonstrated bugs.

The same pre-crash checkpoint and input recipe now reach Mantis/story125:
3,016 native frames versus 3,058 SwanStation, with exact native replay. No
replacement no-op or forced story update is used. The ketchup prison route
also passes through Johnny's knockout, equipment recovery and s03a/story163;
the exit passes again on this build. Correcting the synthetic Hind entrance
from tower map f1f2 to the retail roof map7df9 resolves the previous shared
stall; full unskipped aftermath passes in 4,867/4,858 frames with exact replay.

151 Python tests pass; the unchanged runtime unit binary is rerun with 1,325
passed and 31 skipped. The regenerated main-dispatch audit has no unresolved
literal targets. Sanitizers and Vulkan visual checks were not repeated in
this iteration. Full boss combat, alternate prison branches and continuous
campaign remain open. See [scope and reproduction](mgs-required-paths.md) and
[per-run identities and evidence](mgs-required-paths-next-evidence.json).

## BIOS resource continuity — 2026-10-06

The preceding build (`d5c5168b…`) passes 1,325 runtime tests with 31 opt-in tests
skipped, both ordinarily and under ASan/UBSan; 142 Python tests pass. Nine new
runtime contracts cover BIOS allocator/file/directory/RNG continuation,
cross-controller memory-card writes, descriptor exhaustion and malformed states.
Three failing pre-fix tests document the missing resource and RNG continuity.

NSV12 retains both BIOS heap arenas, cached CD files, memory-card slots and
cursors, directory iteration and a per-BIOS RNG seed. Metadata and attachments
are validated before replacing the timeline. Legacy files remain readable;
absent resource history is reset, not reconstructed. No host pointers are saved.

Eight successful native executions include exact replays: Codec (146 frames),
fresh-process NSV12 first-person (48), seamless disc change (504), historical
NSV10 movie (1,460), plus retail CD2 load/control. Both UI probes also pass
against SwanStation. The final CD2 loader intervals are 4/4/4/5 VBlanks.
These are targeted headless regressions, not complete campaign or Vulkan visual
certification. Personal save/card hashes are unchanged.
See [design and limits](mgs-hle-state-verification.md) and
[build-specific evidence](mgs-hle-state-evidence.json).

## MMIO transport and input sampling — 2026-10-06

The preceding MMIO build (`9420f2b9…`) passes 1,316 runtime tests, with 31 opt-in
tests skipped, both normally and under ASan/UBSan. Twelve new matrix tests
cover timer access widths/flags, atomic DMA cancellation, ordered CD ports,
pad register access and serial snapshot boundaries, and coherent controller
type sampling during TAS hotplug. Ten tests reproduced failures before fixes.

Codec passes against SwanStation and replays exactly; seamless disc change
replays 504 frames, the legacy movie replays 1,460 frames, a CD2 retail load
keeps the gameplay loader at six VBlanks, and full movie 6137 decode/mix passes.
These checks add no completed campaign segment or Vulkan visual certification.
See [design, reproduction and remaining bus/serial limits](mgs-mmio-verification.md)
and the [build-specific evidence](mgs-mmio-evidence.json).

## Native asset loading — 2026-10-06

`game.fast_loading` is now enabled by default for MGS. The retail stage
consumer is interleaved with bounded CD/DMA transactions, eliminating data
seek/sector waits without advancing simulation or streaming clocks. All 24
retail save scenarios pass on both discs; 96 loader intervals take 4–7
VBlanks. Eight same-build A/B comparisons preserve archive sector counts and
gameplay endpoints, reducing gameplay stage loads by about 96–98%.

Seamless disc change/replay, Codec, the cleared-save restart, full STR movie
entry and the exact 1,460-frame legacy movie replay pass. The suite now has
1,304 passing runtime tests (31 skipped), also under ASan/UBSan, and 141 Python
tests. See [loading design, measurements and regression limits](mgs-fast-loading.md)
and [build-specific evidence](mgs-fast-loading-evidence.json). These are
headless loader measurements, not new Vulkan latency/visual certification.

## Systematic runtime checks — 2026-10-06

The preceding ordinary suite passes 1,297 runtime tests and 138 Python tests
(31 opt-in runtime tests excluded). Timer IRQ/clock fixes now preserve phase
and one-shot latches through NSV11, with older states still readable. The ISO
reader uses bounded little-endian reads instead of unaligned typed pointers;
its full retail directory/path lookup agrees with the independent parser for
all 22 file extents across both discs under ASan/UBSan. Oversized BIOS heap
requests can no longer wrap alignment and corrupt the allocation chain.
Codec, first-person, Disc 2 Liquid loading and seamless disc-change regressions
pass on their recorded builds. See the completion report for sanitizer results,
build identities and limits; previous evidence below is not relabelled.

All eight retail movie extents on both discs have
complete STR structure inventories. Their 11,571 XA sectors match SwanStation
sample for sample, including decoder snapshot restoration at every boundary.
Credits, the final phone call and rank complete with all 5,337 expected video
frames, 1,067,400 macroblocks and 15,699,600 stereo PCM frames consumed by SPU.
MDEC now rejects truncated blocks instead of inventing complete macroblocks;
empty/partial commands cannot certify successful frame decoding.

Nine additional native/donor paths now exercise all four Disc1 movie assets
and their scene variants, with source-derived video and audio counts. Corrupt
memory-card chains cannot modify neighbouring saves; invalid savestate
structure is rejected before changing the live timeline or disc. A new cleared
save and its reload pass, as do the exact NSV10 replay and a copied personal
NSV7 state. The separate GPU test attempt could not initialize SDL video in
this session, so it supplies no new Vulkan certification.

The [completion report](mgs-completion.md) records the current binary, source
identities, independent oracle, synthetic entry conditions and limits. This
does not certify the complete campaign or every Vulkan scene. The disc change
and writing/reloading a cleared-game save have separate successful tests;
older boot milestones below retain their historical scope.

## Vulkan effect adapters — 2026-10-04

All 60 overlapping effect dossiers now have renderer adapters. The shared
paths cover screen optics, original-geometry materials, semantic animation
particles, dynamic DG_MDL and affine interface geometry. Lighting is evaluated
per pixel and presentation color uses RGB8 without PSX dithering. Native
simulation, VRAM and guest readbacks remain authoritative.

Validation: 352 passing tests in 28 suites, including Vulkan, plus six real
checkpoint runs (1,680 VBlanks) at 3840×2160 with no renderer downgrades. The
heliport panorama exposed and now verifies a fix to affine source geometry
classification. The photo test verifies the exact shutter frame despite
multiple queued VBlanks. Personal savestate/card hashes are unchanged.

See [adapter evidence](mgs-effects-adapter-evidence.json) and
[effect contracts](mgs-effects-catalog.json). `mgs_effects_catalog.py --check
--require-adapters` passes. `--require-complete` still fails deliberately:
every boss phase and effect combination has not been visually certified.

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
The first verified overlay ranges were `title`, `opening`, `d00a`, `s00a` and
`s01a`. Compilation now includes 73 campaign candidates; see
[campaign validation](mgs-campaign.md) for actual progression and fixes.
The Extreme campaign now reaches Ocelot's defeat, the post-Baker sequence and
the return to the tank hangar through adapted original TAS inputs. PSXjin
provides the separately observed first-disc reference; this is not yet native
validation through the disc change. Independent retail LOAD GAME checks now
pass for 24 Normal saves across 20 stages on both discs, with matching
SwanStation semantic results and no native interpreter calls. Disc 2 adds ten
saves across eight stages and 180-VBlank Vulkan probes at 3840×2160 for each
distinct stage; see [scenario checks](mgs-tas-routes.md#disc-2-scenario-workflow).
The original final-save UI, fresh-process reload and automatic disc change
now have independent verification; see [completion](mgs-completion.md) and
[disc change](mgs-disc-change.md). Complete fights and continuous campaign
progression still need broader validation.
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
  file operations now have regression coverage. Retail loading is verified on
  both discs. The original cleared-game save UI and subsequent reload are now
  verified separately in the completion report.
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
Advanced original-asset Vulkan rendering is now implemented and has separate
adapter/visual evidence; the historical boot probes here do not certify it.
See [presentation](mgs-vulkan-presentation.md) for its current scope and
interpolation requirements.

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
