# Donor-comparison traces

These probes emit the same `ps1trace/v1` JSONL records as `ps1Runtime` so a
behavioural difference can be located before code is borrowed from a donor.

## Producers

- **ps1Runtime** — set `PS1_TRACE_JSONL=/tmp/crash3-recomp.jsonl`. Add
  `PS1_TRACE_GTE=1` only for a short GTE-focused capture. Add
  `PS1_TRACE_PGXP_VERTICES=1` to emit only rejected polygon provenance;
  optional `PS1_TRACE_PGXP_FRAME_FROM/TO` bounds keep that capture focused.
  Add
  `PS1_TRACE_GPU_WORDS=1`; for recompiled games, pair it with
  `PS1_TRACE_GPU_TRIGGER_PC=0x...` to capture the trigger frame and its
  successor. `PS1_TRACE_GPU_TRIGGER_FRAMES=N` widens that window when list
  submission is deferred; alternatively use inclusive
  `PS1_TRACE_GPU_FRAME_FROM/TO` bounds.
- **PCSX-Redux** — the executable oracle for an untouched Crash 3 disc. Apply
  `pcsx-redux-state-api.patch` to the pinned PCSX-Redux revision, rebuild it,
  then load `pcsx-redux-trace.lua` from its Lua console. The probe records the
  display and drawing registers exactly at `GPU::Vsync`; direct core callbacks
  also record GP1/GP0 state changes, CD commands, and SPU key-on/key-off masks
  at the point where the emulated hardware receives them.
- **SwanStation and Beetle PSX** — independent executable oracles driven by
  `libretro_headless.cpp`. The runner can record software frames, selected
  guest-RAM words and exact full-RAM checkpoints without modifying either
  core. SwanStation's optional `swanstation-cd-command-trace.patch` adds
  command-level CD anchors at the controller itself.
- **SlickAmogus/PsyCross** — a heavily Silent-Hill-specific implementation
  donor, not an emulator for the original executable. The probe is pinned to
  commit `d199cca8e460a8439f9de56b00aaec640c69420c`; apply
  `psycross-slick-trace.patch` when running a source port based on that exact
  tree. It covers raw VBlank ticks, latched display/draw state, present,
  `VSync`, `PutDispEnv`, `PutDrawEnv`, `DrawSync`, `DrawOTag`, and optionally
  every GTE command. This makes API and GTE semantics comparable, but it does
  not make PsyCross ground truth for Crash 3's boot sequence.

The patches are intentionally small and opt-in. No donor is vendored and no
game data is copied into this repository.

### Game-independent colour-GTE differential

`gte_color_oracle.py` compiles the actual local SwanStation GTE into a separate
instruction oracle, without modifying it or linking it into the runtime:

```sh
python3 tools/donor_trace/gte_color_oracle.py \
  --swan-source /home/xander/ps1-donors/swanstation \
  --samples-per-command 20000 --vulkan --output build/gte-oracle.bin
PS1_GTE_COLOR_ORACLE_VECTORS=build/gte-oracle.bin \
  build/ps1Test/ps1Runtime_tests --gtest_filter='GteColor*'
```

Requires a C++20 compiler; `--vulkan` additionally requires `glslc`, a Vulkan
loader/device with shaderInt64, and Vulkan headers (defaults to the donor's
loader includes; override with `--vulkan-headers`). The compute runner is an
independent one-buffer, binding-zero, 64-thread-workgroup diagnostic reusable
for other source arithmetic kernels. Device/capability errors are not skips.

The twelve covered colour commands compare all 64 register words; the Vulkan
adapter separately tests the production DPCS/DPCT sf=1 RGB operation. This is
not an oracle for all GTE commands or complete game images. Generated-vector
reports alone do not test the recomp: run the C++ vector test above too.

For real instructions, combine the existing bounded `PS1_TRACE_GTE=1` trace
(with an event filter admitting `gte.command`) and `PS1_GTE_COLOR_CAPTURE=PATH`.
Pass that file as `--capture PATH`, using a **different** `--output`. The driver
then directly compares recorded runtime results with the donor. GTECOL1 uses
an eight-byte `GTECOL1\0` header and little-endian 130-word records: PC,
instruction, 64 input words, 64 output words. It contains no game RAM/assets.
Truncated records fail; `--limit` is an explicit prefix-only experiment, never
an implicit repair of a damaged capture. Reports retain source/binary/input
hashes and donor revision/dirty state. See
[`ps1-gte-color-oracle-20260906.md`](../../evidence/renderer/ps1-gte-color-oracle-20260906.md)
for the repaired semantics, negative controls and actual coverage limits.

The headless libretro runner can also retain one software-rendered donor frame
for visual comparison. Set `PS1_DONOR_DUMP_FRAME=<n>` and
`PS1_DONOR_DUMP_PATH=/tmp/frame.ppm`; this is intentionally separate from the
JSONL trace and is currently most useful with Beetle PSX's software renderer.
Set `PS1_DONOR_BENCHMARK=1` to report wall time and frame throughput around the
`retro_run` loop only. This excludes core initialization, disc opening and an
optional state load, while retaining the core's complete emulation and raster
work plus the runner callbacks.
For a single reset-to-frame scan, set `PS1_DONOR_DUMP_DIRECTORY` together with
inclusive `PS1_DONOR_DUMP_FROM`, `PS1_DONOR_DUMP_TO`, and a positive
`PS1_DONOR_DUMP_STEP`. `PS1_DONOR_FRAME_STATS=/tmp/frames.csv` records the
dimensions, exact normalized-RGB8 FNV-1a, PS1-canonical RGB5 FNV-1a, and
non-black pixel count of every software framebuffer callback. Prefer the RGB5
hash when comparing different cores: their 5-bit GPU results can agree while
their frontend conversions expand those levels to 8-bit differently. Null,
duplicate, and hardware-frame callbacks are not synthesized into the CSV.
The core defaults crop borders (`swanstation_Display_CropMode=Borders` and
`beetle_psx_crop_overscan=smart`); use `None` and `disabled`, respectively,
when the oracle is intended to validate display ranges or overscan rather than
only the active image.

### Verified software-core boot

The official SwanStation and Beetle PSX libretro repositories were tested at
the revisions pinned in `donors.json`. From the repository root, the complete
software-core and runner build can be reproduced with:

```sh
DONOR_ROOT=/home/xander/ps1-donors

git clone https://github.com/libretro/swanstation.git \
  "$DONOR_ROOT/swanstation"
git -C "$DONOR_ROOT/swanstation" checkout \
  7f69c199ed88d5723f71dd3a6e9c1b7a45b535a6
make -C "$DONOR_ROOT/swanstation" -f Makefile.libretro -j2

git clone https://github.com/libretro/beetle-psx-libretro.git \
  "$DONOR_ROOT/beetle-psx-libretro"
git -C "$DONOR_ROOT/beetle-psx-libretro" checkout \
  ef51860dbd71ad6b7ce67115d4780c2ee321d968
make -C "$DONOR_ROOT/beetle-psx-libretro" -j2 \
  HAVE_HW=0 HAVE_OPENGL=0 HAVE_VULKAN=0

g++ -std=c++17 -O2 -Wall -Wextra \
  -I"$DONOR_ROOT/swanstation/dep/libretro-common/include" \
  tools/donor_trace/libretro_headless.cpp -ldl \
  -o "$DONOR_ROOT/libretro_headless"
```

SwanStation has no required submodules at the pinned revision. Beetle's build
uses the vendored dependency sources in its checkout; no submodule update is
needed for this software-only target.

### RAM-state and SwanStation CD oracles

The runner can sample any comma-separated set of PS1 RAM addresses immediately
after each `retro_run` boundary. This requires no donor-core patch and emits
the stable `diagnostic.ram_sample` event:

```sh
DONOR_ROOT=/home/xander/ps1-donors
DISC="/path/to/Crash Bandicoot 3.cue"
ADDRS=0x80069f80,0x80074f40

PS1_DONOR_RAM_TRACE=/tmp/swan-state.jsonl \
PS1_DONOR_RAM_ADDRESSES="$ADDRS" \
PS1_DONOR_RAM_TRACE_FROM=2100 \
PS1_DONOR_RAM_TRACE_TO=2350 \
PS1_DONOR_RAM_TRACE_STEP=1 \
PS1_DONOR_TRACE_SOURCE=swanstation \
  "$DONOR_ROOT/libretro_headless" \
  "$DONOR_ROOT/swanstation/swanstation_libretro.so" \
  "$DISC" 2350 \
  swanstation_GPU_Renderer=Software \
  swanstation_Console_Region=Auto
```

After selected words narrow a transition to adjacent frames, capture the exact
2 MiB system RAM on only those frames:

```sh
PS1_DONOR_RAM_DUMP_DIRECTORY=/tmp/swan-ram \
PS1_DONOR_RAM_DUMP_FRAMES=2264,2265,2278,2279 \
  "$DONOR_ROOT/libretro_headless" \
  "$DONOR_ROOT/swanstation/swanstation_libretro.so" \
  "$DISC" 2279 \
  swanstation_GPU_Renderer=Software \
  swanstation_Console_Region=Auto
```

Addresses select evidence only; they never enter runtime behavior. Match them
with the runtime's `PS1_TRACE_RAM_ADDRESSES`, then compare state-transition
spacing or a common semantic CD/GPU anchor rather than assuming equal reset
frame numbers. Repeat the same capture with Beetle so agreement is not an
implementation-specific accident.

For command-level SwanStation CD evidence, apply the pinned opt-in patch and
rebuild the core:

```sh
git -C "$DONOR_ROOT/swanstation" apply \
  "$PWD/tools/donor_trace/swanstation-cd-command-trace.patch"
make -C "$DONOR_ROOT/swanstation" -f Makefile.libretro -j2

PS1_DONOR_CD_TRACE=/tmp/swan-cd.jsonl \
  "$DONOR_ROOT/libretro_headless" \
  "$DONOR_ROOT/swanstation/swanstation_libretro.so" \
  "$DISC" 2400 \
  swanstation_GPU_Renderer=Software \
  swanstation_Console_Region=Auto
```

The patch records the command, parameter FIFO, resolved disc LBA and donor
frame at `CDROM::ExecuteCommand`; it is deliberately narrower than a general
SwanStation fork. Revert it in the donor checkout after capture if a pristine
tree is required. The repository's pinned patch must pass `git apply --check`
against the revision in `donors.json`.

The following reset-to-frame checks were run against the PAL Crash 3 CUE with
an empty runner system directory (no proprietary BIOS). SwanStation selected
its embedded OpenBIOS fallback after reporting that no PAL BIOS was present;
Beetle reported the missing external `openbios.bin` override and region BIOS,
then used its compiled-in OpenBIOS fallback. Both produced non-black software
frames: the piracy screen at SwanStation frame 1050 and Beetle frame 900, and
the Naughty Dog logo at frame 1800 in both cores.

```sh
DISC="/path/to/Crash Bandicoot 3.cue"

PS1_DONOR_DUMP_FRAME=1050 \
PS1_DONOR_DUMP_PATH=/tmp/crash3-swan-f1050.ppm \
"$DONOR_ROOT/libretro_headless" \
  "$DONOR_ROOT/swanstation/swanstation_libretro.so" \
  "$DISC" 1050 \
  swanstation_GPU_Renderer=Software \
  swanstation_Console_Region=Auto \
  swanstation_BIOS_PatchFastBoot=true

PS1_DONOR_DUMP_FRAME=900 \
PS1_DONOR_DUMP_PATH=/tmp/crash3-beetle-f900.ppm \
"$DONOR_ROOT/libretro_headless" \
  "$DONOR_ROOT/beetle-psx-libretro/mednafen_psx_libretro.so" \
  "$DISC" 900 \
  beetle_psx_override_bios=openbios
```

These frame numbers are only reproducible smoke-test checkpoints within each
donor. Do not use them to align the demo transition; use the CD/GPU semantic
anchors below. The unmodified cores provide visual frame oracles only. A
core-side probe is still required before either can emit `ps1trace/v1` JSONL.

### Crash 3 demo visual oracle

A reset-to-demo scan with the pinned software cores and their default active
image crop established these intervals:

| Core | Black transition | Visible demo | Visible length |
| --- | --- | --- | ---: |
| SwanStation | 4309--4668 | 4669--6002 | 1334 |
| Beetle PSX | 4238--4571 | 4572--5905 | 1334 |

The visible segment keeps the stable alignment `Swan = Beetle + 97`. At 23
aligned checkpoints spaced by 60 frames, non-black coverage agreed 23/23.
Canonical RGB5 comparison differed in only 92 of 3,014,656 pixels (0.00305%,
maximum 26 in one frame). RGB8 hashes should not be compared across the cores
because their frontend channel expansion differs.

For a recompiler gate, downsample the modern output to the native active
image, quantize each channel to five bits, and compare coverage plus RGB5
against both donors. Frame numbers are first narrowed with the CD/GPU semantic
anchors; they are not assumed equal between implementations.

`frame_compare.py` performs that gate directly on P6 captures without an image
library. Crop is applied in source coordinates before the integer reduction;
the default box filter makes a 4x render one native image, and the default
comparison is the same canonical `RGB8 >> 3` used by the runner's RGB5 hash.
The JSON keeps color mismatches and black/non-black coverage mismatches
separate. It also reports exact bounding boxes for all differences and for
reference-only (missing) versus candidate-only (extra) coverage. The optional
normalized diff map keeps exact pixels as a dim reference and marks missing
geometry magenta, extra geometry cyan and color-only differences yellow. Add
`--diff-mode coverage` to suppress true-color differences and inspect only
missing/extra geometry:

```sh
PS1_DISPLAY_DUMP_FRAME="$RECOMP_DEMO_VBLANK" \
PS1_DISPLAY_DUMP_PATH=/tmp/recomp-4x.ppm \
  build/ps1Runtime/ps1Runtime --config crash3_config.toml

tools/donor_trace/frame_compare.py /tmp/swan-demo.ppm /tmp/recomp-4x.ppm \
  --downsample 4 --output /tmp/swan-vs-recomp.json \
  --diff-output /tmp/swan-vs-recomp.ppm

tools/donor_trace/frame_compare.py /tmp/beetle-uncropped.ppm \
  /tmp/swan-uncropped.ppm --reference-crop 1,0,559,288
```

The runtime dump is the immutable VBlank-latched modern display aperture: it
keeps true-color samples and the configured internal scale while respecting
the guest GP1 display ranges. Select `RECOMP_DEMO_VBLANK` from the corresponding
semantic trace segment, not by copying a donor's absolute frame number.
Use `--candidate-crop X,Y,W,H` for host-window borders, or
`--downsample-mode nearest` when checking exact replicated native pixels rather
than the resolved 4x image. `--color-space rgb8` is available for same-frontend
true-color checks; cross-core gates should retain the RGB5 default. Run the
standalone tests with
`python3 -m unittest tools.tests.test_frame_compare`.

With border cropping disabled, the same entry scene is 559x288 in SwanStation
and 560x288 in Beetle. Removing Beetle's extra left column makes the images
equal at RGB5. Use the 512x256 default captures for active-image rendering
checks and the uncropped 288-line captures for GP1 display-range/overscan
checks; do not bake either frontend crop into guest rendering.

The Slick fork must not be read as a generic double-buffer reference. Its
`PutDispEnv` has a delayed active/current environment workaround and its
Silent Hill path reconstructs two PSX framebuffer pages from the host image.
Also, `USE_PGXP` and the runtime `g_PsxUsePgxp` gate are off by default in the
pinned tree. Borrow individual GTE/PGXP mechanisms only after a trace or test
vector agrees.

## Applying the PsyCross probe

```sh
git clone https://github.com/SlickAmogus/PsyCross.git
cd PsyCross
git checkout d199cca8e460a8439f9de56b00aaec640c69420c
git apply /absolute/path/to/tools/donor_trace/psycross-slick-trace.patch
```

Run the source port with `PS1_TRACE_JSONL=/tmp/psycross.jsonl`. Add
`PS1_TRACE_GTE=1` only for a short deterministic test: per-command register
hashing deliberately has non-trivial overhead. The probe reads the raw
`g_psxSysCounters` value instead of calling `PsyX_Sys_GetVBlankCount`, because
that getter increments time when the fork's fast-forward mode is active.

## Capture and compare

Runtime:

```sh
PS1_TRACE_JSONL=/tmp/crash3-recomp.jsonl \
  build/ps1Runtime/ps1Runtime --config crash3_config.toml
```

For heap-loaded Crash 2/3 native GOOL snippets, add
`PS1_TRACE_DYNAMIC_MIPS=1`; optional inclusive
`PS1_TRACE_MIPS_FRAME_FROM/TO` bounds keep entry/exit and code-hash probes
focused on one scene.

For hand-written regions containing code islands or mutable dispatch tables,
capture the same bounded `PS1_TRACE_MIPS_RANGE` in Beetle PSX and SwanStation,
then generate a donor-intersection profile:

```sh
tools/indirect_profile.py beetle.jsonl swan.jsonl \
  --exe game.exe --region 0x8003A000:0x8003B000 --toml
```

The output contains both `indirect_targets` and `indirect_source_pcs`. Adding
the pair to the owning `[[functions]]` entry makes overlapping emitted copies
inherit the same donor-confirmed local dispatch profile; JR and JALR are both
classified, while an ordinary `jr $ra` is excluded.

PCSX-Redux (from its Lua console):

```lua
loadfile("/absolute/path/to/tools/donor_trace/pcsx-redux-trace.lua")()
```

Load the probe while execution is paused, then hard-reset the emulated console
so frame zero includes the copyright/piracy screen and its early CD/SPU cues.

Then:

```sh
tools/trace_compare.py /tmp/crash3-recomp.jsonl /tmp/crash3-redux.jsonl
```

### Semantic demo-transition segments

Do not align an attract/demo transition by an absolute VBlank. BIOS choice,
frontend scheduling, and CD seek timing legitimately move it. Extract the same
segment from each trace with ordered CD/GPU anchors, then compare those
normalized segments. `semantic_segment.py` projects the input onto the event
names used by each pattern: unrelated events are ignored, but an unexpected
event on one of those anchor channels breaks the sequence. Ambiguous patterns
fail instead of silently selecting the first match.

For the current Crash 3 reference capture, the transition into the demo is the
read at LBA 13983 followed by drawing into the second framebuffer; the return
to the title begins with the read at LBA 1608. These are semantic disc/GPU
events, not frame 4868/5188 assumptions:

```sh
tools/donor_trace/semantic_segment.py /tmp/crash3-recomp.jsonl \
  --output /tmp/crash3-recomp.demo.jsonl \
  --metadata /tmp/crash3-recomp.demo.json \
  --start cdrom.command:command=2,parameters=3,lba=13983 \
  --start cdrom.command:command=14,parameters=1 \
  --start cdrom.command:command=6,parameters=0 \
  --start gpu.draw_area_start:x=512,y=0 \
  --end cdrom.command:command=2,parameters=3,lba=1608 \
  --end cdrom.command:command=14,parameters=1 \
  --end cdrom.command:command=6,parameters=0

tools/donor_trace/semantic_segment.py /tmp/crash3-redux.jsonl \
  --output /tmp/crash3-redux.demo.jsonl \
  --metadata /tmp/crash3-redux.demo.json \
  --start cdrom.command:command=2,parameters=3,lba=13983 \
  --start cdrom.command:command=14,parameters=1 \
  --start cdrom.command:command=6,parameters=0 \
  --start gpu.draw_area_start:x=512,y=0 \
  --end cdrom.command:command=2,parameters=3,lba=1608 \
  --end cdrom.command:command=14,parameters=1 \
  --end cdrom.command:command=6,parameters=0

tools/trace_compare.py \
  /tmp/crash3-recomp.demo.jsonl /tmp/crash3-redux.demo.jsonl --json
```

Some cores report the physical/current CD sector rather than the requested
Setloc target. In that case omit `lba` and select the verified command-sequence
match with `--start-occurrence N` or `--end-occurrence N`; occurrence is
relative to semantic matches (and the end count starts after the chosen start),
never to a frame. Keep at least one GPU state step in the start anchor so a
coincidentally similar BIOS CD sequence cannot select the scene.

The libretro runner does not synthesize trace records: SwanStation or Beetle
must be built with a probe that emits `PS1_DONOR_TRACE`. It also supplies no
hardware-render context, so use a software renderer. An uninstrumented core is
still usable for `PS1_DONOR_DUMP_FRAME`, but cannot drive this event comparison.

### Recomp streamed-section holdouts

`crash3_forward_level_slot6_extended.inputs` and
`crash3_new_section_slot3_extended.inputs` drive two independent Crash 3 saves
forward for 1,500 guest VBlanks. Their numbers reproduce the routes only; they
must never select runtime behavior. Capture compact resident transition data
with `PS1_TRACE_CRASH3_RESIDENT_PROJECTION=1` and an exact `PS1_TRACE_EVENTS`
allowlist containing only `game.crash3.resident_projection_catalog`,
`game.crash3.resident_projection_world`, and the opaque/semitransparent atomic
fallback events. Leave `PS1_TRACE_CRASH3_RESIDENT_FACES` unset for this audit;
enable it only when `crash3_resident_coverage.py` needs the large per-face
catalog. Validate either route with:

```sh
python3 tools/crash3_resident_transition_audit.py TRACE.jsonl \
  --output transition-audit.json
```

The v2 report reconstructs independent row admission/revocation and complete
cohort commits solely from intrinsic resource records. A new route extends the
corpus; it does not justify a frame-, slot-, level- or EID-specific exception.

For the first comparison, boot both runs from reset and stop at the same
screen. The report calls out clock rate, multiple display-origin writes in a
single VBlank, draw/display overlap, a latched-buffer/presented-buffer mismatch,
first SPU key-on, and the first differing stable event fields.

## Stable schema

Every line is one JSON object:

```json
{"schema":"ps1trace/v1","source":"ps1-recomp","seq":42,"frame":17,"host_us":340012,"event":"gpu.display_start","x":512,"y":0}
```

Required fields are `schema`, `source`, `seq`, `frame`, `host_us`, and `event`.
Producers can attach extra numeric fields. `tools/trace_compare.py` ignores
producer-specific fields and compares only the stable fields in its
`EVENT_FIELDS` table.
