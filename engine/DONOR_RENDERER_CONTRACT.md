# Donor-backed renderer contract

This file records the external evidence used to design the game-independent
Vulkan replay executor. Crash Bandicoot 3 is the reference trace, not a source
of rendering exceptions. Donor code is evidence for behavior; it is not copied
when its licensing or provenance is unclear.

## Frozen donor revisions and licensing

| Donor | Revision | Role | License/use |
| --- | --- | --- | --- |
| SwanStation | `7f69c199ed88d5723f71dd3a6e9c1b7a45b535a6` | Primary PS1 GPU semantic oracle | GPL-3.0, compatible with this project |
| Beetle PSX | `ef51860dbd71ad6b7ce67115d4780c2ee321d968` | Vulkan synchronization, native/scaled-domain and executable oracle | GPL-2.0 root; reuse semantics, do not copy GPL-2-only code |
| SlickAmogus/PsyCross | `d199cca8e460a8439f9de56b00aaec640c69420c` | Permissive PGXP propagation and native sampling donor | MIT |
| OpenDriver2/PsyCross | `e56e4cde1c2b8a15e0d4e38b26cdd9202e0d17e6` | Historical comparison | MIT; superseded for PGXP by Slick |

The executable donor pins live in `tools/donor_trace/donors.json`. Beetle's
Vulkan aggregation file declares MIT and the historical parallel-psx core ISC,
but the moved shader files have no clear per-file provenance after later
changes. Their behavior may be independently implemented; their source must
not be copied without a separate provenance check.

## Current implementation boundary

The live replay contract is `CrashReplayV1` schema 7. Capture, exact-latch FIFO
scheduling, structural preflight, per-vertex precision measurement and
transactional commit/fence ownership are connected to Vulkan. Presentation now
consumes the `DisplayScanoutPlan` paired with the immutable native snapshot. At
4x, `M0Reanchor` uploads that authoritative snapshot, seeds the persistent
full-VRAM color/mask/depth atlas, and resolves a progressive RGB555 active image
inside its complete Vulkan canvas.

The first M1 executor is now live but is not yet donor-equivalent for every
command family. It executes precise opaque and semi-transparent `Projected3D`,
semantically proven exact `Screen2D` DrawPackets, native 4/8/15-bit texture
sampling, ordered coalesced `PaintRect`/fill, depth epochs, donor-proved direct
non-overlapping copies and donor-safe scratch copies in the persistent 4x
RGBA8/mask/R32-surface/D32-occlusion atlas. Hybrid opaque packets use
fixed-function `ALWAYS` in
GP0/OT order and write genuine W to D32; Strict alone uses `LEQUAL` as an
opt-in research policy. Semi-transparent samples accepted after texture-zero/E6
checks write genuine surface R32 and far D32; `Screen2D` writes the explicit
R32 sentinel and far D32. `PaintPixels` remains a whole-frame M0
fail-close until its sparse executor exists. Exact ABR and E6 destination-mask
reads use bounded color/mask scratch snapshots, refreshed between the two
ordered halves of a feedback quad. Scratch CopyRect snapshots source RGBA8 and
R8 plus an independent pre-write destination R8 when check-mask is enabled.
Accepted fragments copy true color and source/forced mask, write the R32
sentinel and invalidate D32 to far; rejected fragments preserve every
destination plane. Accepted
`Screen2D` fragments likewise invalidate D32 only after texture-zero and
destination-mask rejection. Native VRAM writes are flushed only before the next
texture/copy reader and never moved past it. Schema 4 gives every `CopyRect` a
revisioned proof of its complete wrapped native source tile set. A0 Data uses
its authoritative accepted bitmap to update R16 and expand RGBA8/R8/R32/D32 through
Vulkan at the exact command ordinal. Ordered two-dimensional overlap and cyclic
horizontal overlap remain explicit bake misses and select the complete
`M0Reanchor`; unknown state, invalid transfer framing and weak identities still
fail closed to native `M0PresentOnly`. Only an actually submitted M1 frame whose
compatible replay atlas is also selected for presentation may be labelled
PGXP/true-color/Hybrid-D32 output. Hybrid's genuine-W attachment is depth
evidence, not a claim that unproven cross-packet `LEQUAL` is correct. Global
true-Z and universal coverage are not yet claimed.

The three 4096x2048 scratch images occupy 48 MiB at 4x in the worst case: 32 MiB
RGBA8 plus two independent 8 MiB R8 planes. An opt-in Vulkan integration test
passed on an NVIDIA GeForce RTX 4060 with exact expected RGBA8, source/destination
mask and D32 hashes for force-mask and horizontally overlapping check-mask
copies. `VK_LAYER_KHRONOS_validation` is not installed on that host, so the run
does not claim validation-layer coverage.

The sole canonical live checkpoint and focused attachment readbacks are recorded
in [RENDERER_EVIDENCE.md](RENDERER_EVIDENCE.md). This donor contract deliberately
does not duplicate counters. The record establishes the observed Crash 3
submission prefix and guest VBlank/submission cadence; scratch/overlap variants,
GPU-confirmed presentation, audio-underrun evidence, final scanout and
multi-title coverage retain separate gates.

## P0 fragment order: visibility before attachments

The executor must apply this order for every covered sample:

1. Sample the raw native texture/CLUT word.
2. Discard raw `0x0000`. Raw `0x8000` is visible black carrying STP.
3. If E6 check-mask is enabled and the destination mask is set, discard.
4. Decide color blending. On a textured semi-transparent packet, only texels
   with source STP blend; non-STP texels remain opaque. Untextured
   semi-transparent packets always blend.
5. Write color.
6. Write mask as `forceMask || sourceSTP` for textured samples and
   `forceMask` for untextured samples.
7. Perform a depth write only when the selected packet depth policy permits
   it. A discarded sample changes no attachment, including depth.

For Hybrid, step 7 means genuine W for a valid opaque packet and far for an
accepted semi-transparent or `Screen2D` sample. The valid opaque pipeline uses
`ALWAYS`, so an older D32 value left behind by a discarded texture-zero or E6
sample cannot reject the next OT-later opaque fragment.

SwanStation evidence:

- `src/core/gpu_hw_shadergen.cpp:1055-1160` implements zero texel, STP, mask
  output and the split transparent/opaque passes.
- `src/core/gpu_hw.h:370` and `src/core/gpu_hw.cpp:1506` require the two-pass
  textured semi-transparent path.
- `src/core/gpu_commands.cpp:275` flushes before an E6 force/check change, so
  E6 is an ordering boundary rather than mutable state sampled later.

Beetle evidence:

- `rhi/shaders_vulkan/vram.h:79` distinguishes zero and STP.
- `rhi/shaders_vulkan/primitive_feedback.frag:42` implements all four ABR
  equations, including saturating subtraction.

The ABR equations in 8-bit true-color space are `(B+F)/2`, `min(255,B+F)`,
`max(0,B-F)` and `min(255,B+F/4)`. A separate `R8_UINT` mask target is required:
encoding the mask in host depth cannot provide exact E6 behavior and true Z at
the same time.

### GP1(09) gates texture disable

GP0(E1).bit11 and the polygon-embedded tpage bit do not disable texturing
unless GP1(09).bit0 first permits that state change. The gate resets false;
writing GP1(09) does not retroactively change draw mode, while the next E1 or
embedded polygon tpage latches the gated value and GPUSTAT bit 15. SwanStation
implements this through `m_set_texture_disable_mask` in
`src/core/gpu.cpp:1147,1385`; Beetle independently resets and checks
`TexDisableAllowChange` in `mednafen/psx/gpu.c:341,1330,1663`.

This matters directly to the reference trace: Crash writes E1 `0x0A60` but
never enables GP1(09). Treating bit 11 unconditionally discarded 74,076
otherwise valid GP0(34/36) packets in the 1200-VBlank capture. The corrected
producer ignores that bit, retains the packets as textured and records zero
unsupported mutations. If software explicitly opens the gate, the encoded
textured packet layout is still parsed, but its effective raster operation is
untextured and creates no texture/CLUT dependency.

## Completeness-first OT/depth policy

The first live M1 mode applies this donor-informed policy to every supported
polygon packet in the observed Crash 3 prefix:

- a fully valid, opaque `Projected3D` packet follows GP0/OT order with no D32
  read, fixed-function `ALWAYS`, and a genuine interpolated-W write;
- a semi-transparent `Projected3D` packet retains painter visibility and does
  not test the previous D32 owner. Every fragment which survives texture-zero
  and E6 mask rejection writes the far sentinel. This records that the
  composite has no single physical W; it is not required to release later
  opaque visibility;
- if any vertex of an opaque packet lacks valid W, the whole packet uses the
  affine W=1 fallback, remains visible in painter order and does not write
  D32. A semi-transparent Hybrid fallback combines that same W normalization
  with accepted-only far invalidation;
- a live `Screen2D` DrawPacket never claims geometric depth; each accepted
  fragment invalidates prior D32 by writing far after texture-zero and mask
  rejection. Supported `PaintRect` writes carry the same accepted-only depth
  invalidation, while `PaintPixels` is not M1-capable;
- zero texels and mask rejects never write color, mask or depth. Even when they
  preserve an older near W, that value cannot hide the next valid opaque packet
  because its Hybrid pipeline is `ALWAYS` plus genuine-W write;
- E3/E4 draw-area changes, mean-W jumps and VBlank are not visibility clears.
  Hybrid emits no heuristic whole-D32 reset for those events;
- `true_depth_mode = "strict"` retains `LEQUAL` plus genuine-W write for valid
  opaque packets as an isolated research oracle, not the default presentation
  contract.

The implementation uses one global D32 domain for all accepted packets:
`0 < W <= 2^32`, normalized as `W * 2^-32`. It never rescales by a packet-local
maximum and never clamps an invalid W into the domain. Hybrid therefore leaves
comparable physical evidence for diagnostics and future proven groups without
letting unproven cross-packet W override GP0/OT visibility.

SwanStation establishes packet-atomic W fallback and enables PGXP depth testing
only for completely valid, non-transparent packets in
`src/core/gpu_hw.cpp:674-733`. Its Vulkan pipeline nevertheless couples the
`ALWAYS` semi-transparent path to a W depth write. Beetle independently
confirms packet-atomic W fallback in `mednafen/psx/gpu_polygon.c:1594` and
disables depth writes for semi-transparent Vulkan draws in
`rhi/rhi_lib_vulkan.c:9855`. Neither backend alone resolves the PS1 ordering
table versus host-Z contradiction: preserving old D32 can hide an OT-later
primitive, while writing the painter's W can make the painter itself a false
occluder.

The implemented resolution is visibility-first: valid opaque packets preserve
the native composition order while D32 remains a genuine-W sidecar. Accepted
far invalidation still gives translucent/2D/native ownership a truthful depth
lifetime; rejected samples remain bit-identical, but neither case controls the
next valid opaque visibility. The bounded Vulkan A/B/C oracle deliberately
covers one accepted and one E6-rejected B sample, then proves that OT-later C
wins at both pixels and writes C's genuine W with no executor downgrade or
depth clear. Universal optimality and stable object-level lifting remain future
claims with separate gates.

## Schema-6 per-vertex provenance and fail-closed gate

Donor-compatible visibility fallback is not evidence of complete precision.
Schema 6 retains the coordinate/depth provenance introduced in schema 3 and
adds intrinsic GTE/model/GP0 owner evidence. It records
it independently for every active vertex: packed `nativeX/nativeY`; effective
`x/y/w`; genuine `rawDepth` and `nearDepth`; homogeneous `qx/qy`; corrected
`u/v`; independent `preciseX` and `preciseY`; plus `rawDepthValid`,
`homogeneousQ`, `reconciledX/Y`, `inventedW` and `nearSaturated`. The packet
`validMask` remains the genuine XYZ/W proof. Packet-atomic W=32768 keeps
incomplete geometry visible in the donor/software oracle, but every such
projected vertex is marked as invented rather than counted as depth
provenance. Exact projection event/context and, when independently proved,
RT/TR writer lineage and contiguous model source follow the sidecar into the
replay. Submitted GP0 address plus DMA list, node and traversal ordinal are
latched directly per command word. Every field has separate evidence; absence
stays unknown. The Vulkan bake copies all three vertex records into a
deterministic per-triangle owner catalog without fitting identity from pixels
or W.

The live scheduler measures the complete immutable prefix, including invisible,
occluded and off-screen primitives. M1 is eligible only when capture is
complete, every Screen2D `DrawPacket` vertex is exact, every Projected3D vertex
has genuine precise X/Y and W, no component was reconciled, no W was invented,
no packet uses a fallback, every textured projected vertex has precise UV, and
every donor-depth-eligible packet carries the mode-specific state: no depth
read plus `ALWAYS`/genuine-W write in Hybrid, or `LEQUAL` read/write in Strict.
Failure is reported as `PrecisionCoverageIncomplete` and a structurally valid
prefix takes whole-frame `M0Reanchor`; native and precise fragments are never
combined.

Schema 6 also retains homogeneous Q and signed near-plane evidence. A genuine
signed W may be saturated without becoming missing provenance, but geometric
clipping remains unsupported by the Vulkan executor. Its capability bit stays
disabled and selecting that mode fails closed instead of inferring geometry
from packed SXY or silently changing policy.

### Exact signed projection versus the legacy screen-delta guard

The 8-pixel X/Y reconciliation threshold remains a conservative rule for
legacy XY/XYZ sidecars which do not carry complete homogeneous provenance. It
is not applied to an exact, finite, positive-W signed projection solely
because fractional transform W differs from the GTE's integer divider input.
Such a projection is normally retained only when both its stored X/Y and
reconstructed Q/W coordinates are inside the explicit +/-8192-pixel guard band.
Crossing that guard marks both components as reconciled, so schema-7 precision
coverage rejects the complete packet and the scheduler selects packet-atomic
`M0Reanchor`.

There is one narrower donor-proved exception. In `Saturate` mode with
`0 < W < near`, the GTE divider independently produces signed-11 clamped SXY;
raw Q/W is intentionally not the rendered position. `PreciseGteSaturated`
therefore retains finite SXY inside `[-1024, 1023]` even when Q/W crosses the
general guard, and the Vulkan ABI consumes that SXY with `W=max(rawW, near)`.
This still requires exact address/value matching, GTE origin, nonzero projection
ID, finite Q, positive finite near and positive W. It never applies to
`W >= near`, out-of-clamp SXY, stale/non-finite operands, `Screen2D` or the
unsupported `GeometricClip` mode. Independently valid positive legacy XYZ may
still use its established path without homogeneous Q; authentic zero or
negative W retains signed Q only for the later saturation/clipping policy.

The donors establish why the signed case must not use a small generic delta:

- SwanStation's `GetPreciseVertex` validates the exact address/value and keeps
  truncated signed-11 fractional X/Y plus W. Its optional tolerance is disabled
  by default (`gpu_pgxp_tolerance = -1`) in `src/core/settings.h:68` and
  `src/core/settings.cpp:119`; the check is in `src/core/pgxp.cpp:511-573`.
- Beetle's `PGXP_GetVertex` likewise accepts exact circular-buffer/value X/Y/W
  in `pgxp/pgxp_gpu.c:590-650`. Its optional 2D tolerance is evaluated only
  after packet `invalidW` in `mednafen/psx/gpu_polygon.c:1628-1666`, not as a
  rejection of an otherwise exact positive-W projection.
- SlickAmogus/PsyCross introduced the "keep every valid W>0" contract and the
  8192 edge knob in commit `84b2f86` (`pgxp: true-divide projection + per-poly
  affine consistency + edge knob`). Commit `019a117` changed beyond-guard
  behavior from position clamping at real W to an affine whole-polygon drop,
  providing the independent guard-band/fail-closed precedent. These are the
  introducing commits, not the later CRT-aspect HEAD change.

The reference trace supplied one concrete false reconciliation: producer
VBlank 4872, HLE ordering-table word `0x80076B3C` (packet head `0x80076B30`,
opcode `2Eh`, coordinate word index 3), packed/native `(-976,1023)`, precise
`(-967.323974609375,1023)`, raw W `152.896484375` and near W
`144.000244140625`. X differs by `+8.676025390625`, but its exact Q/W projection
is finite and far inside the donor guard band. The asynchronous replay latch
observed the same operand at frame 4989. Unit and capture regressions preserve
this tuple while proving no-Q, non-finite, stale, front-plane beyond-guard and
out-of-clamp saturated inputs still fall back.

The demo transition supplied the saturated counterpart: packet head
`0x80093704`, word 7 at `0x80093720`, opcode `2Eh`, packed/native
`(-1024,1023)`, raw W `8`, near `144.000244140625`, Qx `-209628.625` and Qy
`136427.3125`. Its Q/W exceeds the general guard while SXY is an authentic GTE
clamp result. The canonical evidence run retained this saturated class without
PGXP reconciliation fallback; exact checkpoint counters live only in
[RENDERER_EVIDENCE.md](RENDERER_EVIDENCE.md).

Rare cases remain attributable without a large full-run trace:
`pgxp.vertex.reconcile.last.*` records the latest producer tuple in compact
metrics, including exact Qx/Qy, near and projection identity. With reject schema
1, `PS1_TRACE_MODERN_REJECTS=1` emits aggregate, packet and vertex
precision-reject records, homogeneous-Q coverage and Qx/Qy at the
asynchronously latched replay frame. Its frame window is checked against each
immutable latch rather than mutable global emulation time. Neither diagnostic
weakens the scheduler gate or changes normal timing when disabled.

## Feedback, mask and native hazards

Every replay operation reads state produced by all earlier accepted samples.
The command planner must therefore end or split a segment at least at:

- every blended draw;
- every check-mask draw;
- a texture/CLUT read whose conservative tile set intersects a prior write;
- A0, fill or copy before a following intersecting native read;
- every overlapping `CopyRect`;
- each half of a quad when the second half can observe destination changes
  made by the first.

The portable path snapshots only the bounded destination rectangle required by
the next feedback command. SwanStation ends its render pass, copies the dirty
rectangle and performs explicit attachment-to-sampled transitions in
`src/core/gpu_hw_vulkan.cpp:3406`; its texture/CLUT hazard detection is in
`src/core/gpu_hw.cpp:1153,1343`. Beetle demonstrates a faster input-attachment
alternative plus a `COLOR_ATTACHMENT_WRITE -> INPUT_ATTACHMENT_READ`
`BY_REGION` dependency in `rhi/rhi_lib_vulkan.c:9506,9917`. That optimization
is optional; the ordering is not. The live Vulkan path implements the bounded
scratch route and refreshes it between a feedback quad's two triangle draws.

Beetle's persistent native-integer plus scaled-RGBA domains and tile resolve in
`rhi/shaders_vulkan/resolve.comp:17` support CrashReplay's revisioned
`NativeTileUpload` design: update only the conservative read set and never make
a later native snapshot visible to an older draw.

## Fill, A0 and CopyRect

`FillRect` follows GP0(02), not the image-transfer zero-size convention:

- align X down to 16 pixels and round width up to 16;
- zero width or height is a no-op;
- quantize RGB to native RGB555 and clear bit 15;
- E6 force/check mask does not apply.

SwanStation references are `src/core/gpu_commands.cpp:398` and
`src/core/gpu.cpp:1222`. Its true-color companion retains the RGB8 fill while
the native feedback surface remains RGB555 (`src/core/gpu_hw.cpp:1072`). The
general IR still needs accepted-field-row information before interlaced fill
can be claimed for games that use it.

For A0 CPU upload, zero dimensions expand to 1024/512. Every source word
advances the stream even when rejected by check-mask. Accepted writes store
`raw | forceMask`; an interrupted transfer exposes complete rows plus the
written prefix of the current row. `CpuUpload::acceptedBits` is authoritative:
the executor applies Data chunks immediately and must not repeat a mask test
against later state. SwanStation references are
`src/core/gpu_commands.cpp:421-470`; Beetle's comparable compute path is
`rhi/shaders_vulkan/copy_vram.comp:14`.

The live Vulkan path bakes maximal accepted one-row runs, splitting at reject,
transfer-row and native-X-wrap boundaries. Each run is tightly repacked at a
four-byte-aligned R16 offset with force-mask already applied. At the Data
ordinal the executor copies those runs into the native shadow, restores
shader-read visibility, then uses an absolute-coordinate fullscreen shader and
scaled scissor to write RGB555-expanded RGBA8, bit-15 R8, sentinel R32 and far
D32. It never
re-tests destination mask state and never touches rejected samples.

For `CopyRect`, zero dimensions expand to 1024/512. A same-source/destination
copy is a no-op only without force-mask. Source bit 15 is copied, force-mask is
ORed, and check-mask observes the current destination. Wrap, overlap and mask
require a specialized path.

Schema 4 makes source provenance explicit: every `CopyRect` carries a
`NativeReadDependency` hash over its complete wrapped source tile set and the
producer queues any stale source tiles before the copy. Preflight rejects a
missing, stale or reordered proof before recording Vulkan work. The current M1
executor consumes that proof in two paths. A disjoint, non-wrapped, unmasked copy
uses direct R16/RGBA8/R8 transfers. Every other accepted copy first partitions
source and destination wrap into at most nine in-bounds regions, snapshots all
source RGBA8/R8 regions and, for check-mask, snapshots the pre-write destination
R8. Only after every snapshot finishes does the fragment pass apply the copy.
An E6 reject uses `discard`, preserving destination RGBA8, R8, R32 and D32
together; an accepted pixel copies true color, applies source/force mask,
writes sentinel R32 and far D32. This atlas-only path leaves R16 stale until the next producer-owned tile
upload makes a native read authoritative.

The donors must be kept distinct because they do not expose one interchangeable
CopyRect oracle:

- SwanStation software executes rows from top to bottom and reverses X when the
  source begins left of the destination. Its source comments identify that X
  direction as console-verified, while its oversized/wrapped split is explicitly
  unverified (`src/core/gpu_sw_backend.cpp:790-830`). SwanStation hardware can
  instead sample a separately refreshed VRAM read texture, which behaves like an
  immutable source snapshot and therefore is not evidence that the software row
  order is irrelevant.
- Beetle/Mednafen software reads one horizontal block of at most 128 pixels into
  a temporary cache, writes that block, then advances in X and finally Y
  (`mednafen/psx/gpu.c:575-630`). Its overlapping Vulkan compute path mirrors
  the same 128-wide, top-to-bottom ordering in
  `rhi/shaders_vulkan/blit_vram_cached.comp`; the source still labels the exact
  console cache behavior uncertain.
- Slick PsyCross is a useful MIT PGXP and native-sampling donor, not a GP0(80)
  overlap oracle. Its PsyQ `MoveImage` shim
  (`src/psx/libgpu.c:141-144`) reaches `GR_CopyVRAM`, which performs top-to-bottom
  row `SDL_memcpy`, clamps rather than wraps and does not implement E6
  force/check semantics (`src/render/PsyX_render.cpp:5515-5560`).
- This project's native oracle follows SwanStation's console-verified horizontal
  direction and top-to-bottom rows, including the possibility that a later read
  observes an earlier write.

The Vulkan bake therefore accepts an immutable snapshot only when it is
observationally equivalent to that ordered native execution. A symbolic
horizontal pass records destination pixels and rejects any copy which later
reads one, covering circular/wrapped cycles such as a full-width rotation. Any
physical two-dimensional overlap with distinct source/destination row origins
is conservatively rejected as donor-unproven. Both cases report
`CopyRectOrderedOverlapUnproven` and atomically select M0. The future ordered
executor and executable-oracle matrix must still cover X/Y overlap, wrap,
widths 127/128/129, check-mask, force-mask and same-source/destination before
universal CopyRect coverage is claimed.

## True color and dithering

Software stays native RGB555 at 1x with architectural dithering. Vulkan 4x
true color does not dither the host RGBA8 target. If an authenticity mode is
added, the 4x dither matrix is anchored to native coordinates as
`(scaledCoord / scale) & 3`, not repeated independently at every scaled pixel.

SwanStation expands RGB5 to RGB8 before modulation; the current replay shader
retains the exact unquantized result of `tex5*shade8 >> 4`. Both are plausible
enhancements but are not bit-identical, so the choice belongs in the policy
fingerprint and conformance traces. Slick PsyCross's `v<<3` lookup is closer to
the current replay choice. Its custom 8-pixel/game-specific dither exceptions
are not a general donor.

## Display contract: implemented and remaining work

The CPU scanout plan separates four distinct concepts: output canvas,
origin/black padding inside that canvas, byte-addressed source VRAM region and
crop profile. SwanStation derives them separately in `src/core/gpu.cpp:570-726`
and passes canvas/origin/source separately in
`src/core/gpu_hw_vulkan.cpp:3112`. Beetle likewise separates output
rectangle/offset and source viewport in `rhi/rhi_lib_vulkan.c:7418,8350`.
Because their uncropped PAL conventions differ, both stay named profiles:
SwanStation maps Crash to 559x288 at (23,17), while Beetle maps it to 560x288
at (24,17). These are comparison oracles, not an unqualified hardware claim.

The live Vulkan consumer now takes canvas extent, destination origin, active
extent, byte source, row step, format and field from `DisplayScanoutPlan`.
It clears the full target and draws only the scaled active viewport. The
configured SwanStation no-crop reference is 559x288 with Crash's 512x256 image
at (23,17); at 4x this is a 2236x1152 Vulkan canvas with a 2048x1024 active
rectangle at (92,68). Beetle remains independently selectable and retains its
560x288/(24,17) convention.

PS1 packed-24 scanout is also implemented, but its authority is the latched
native `R16_UINT` VRAM image. The resolve reconstructs each RGB888 pixel from
three row-wrapped bytes at source scale one. It never interprets the replay
RGBA8 atlas as packed VRAM and never labels that native presentation as M1
true-color output; M1 execution may continue transactionally to preserve atlas
state for later compatible RGB555 frames.

The remaining interlace and replay-schema work is explicit. Schema 7's
`FrameLatch` still carries only a legacy aperture plus PAL/interlace flags; the
full live plan is derived from the atomically paired `GPU::DisplayState` and is
not silently part of the replay ABI. Because that state has no trustworthy CRTC
field parity, interlaced plans report `Unknown` and conservatively resolve
contiguous rows from native VRAM while preserving canvas, active placement,
format and byte phase. Future schema 8 must capture and preflight raw CRTC state
and explicit parity, then preserve the opposite field while applying `rowStep`.
No host-frame parity inference is permitted.

The existing opt-in Vulkan scanout test proves donor canvas dimensions,
placement, 4x allocation and the packed-24 native-route gate through live GPU
state. It does not read back final `scanoutColor` pixels. Full-canvas RGBA
readback and exact hashes for black padding, RGB555 placement and packed-24 byte
phase remain a future conformance gate; current RGBA8/R8/R32 readbacks target
the replay atlas instead.

## Transactional submitted coverage

`host.renderer.replay_coverage.*` describes the immutable prefix currently
being planned. The cumulative proof is
`host.renderer.replay_coverage.m1_submitted.*`, appended only after Vulkan
preparation, queue submission and a valid consuming commit. It covers complete
prefix/frame counts, capture and high-precision partitions, projected and
Screen2D vertices, homogeneous Q/UV/depth categories and the last committed
queue serial. Latch-only reuse frames remain in the denominator, while rejected
or atomically reanchored work cannot appear as partially submitted M1.

The operation inventory follows the same boundary. The renderer aggregates the
whole committed prefix locally and publishes it once under
`host.renderer.replay_ir.m1_submitted.*` or
`host.renderer.replay_ir.m0_reanchor_submitted.*`; the legacy
`host.renderer.replay_ir.*` namespace is their sum. These namespaces prove
executor submission coverage. Whether that submitted state was also the
presented high-precision surface is reported independently by the replay-atlas
presentation and scanout-format/deferred-interlace states.

## Release gates derived from donors

The inner visibility loop is deliberately smaller than the release gate.
`cmake --build build --target renderer_visibility_iteration -j2` builds the
focused renderer executable and runs its Hybrid/Strict scalar, capture,
planner, submission, bake and copy-contract tests;
`cmake --build build --target renderer_visibility_gpu_oracle -j2` then runs the
opt-in Vulkan attachment oracle. Generated recompilation units use the inline
`runtime/memory_access.h` front end plus the narrow `runtime/bios_access.h`
dispatch ABI. Hot RAM/scratchpad/PGXP work stays inline, while concrete device
headers exist only behind cold MMIO methods, so a GPU header edit does not
invalidate `recompiled_out.cpp`. A complete game build and the
boot/intro/repeated-demo run occur only after those two fast gates pass.

- Establish image completeness first: compare native hashes, semantic events,
  black-frame transitions, masks/transparency and visible coverage against
  pinned SwanStation and Beetle runs through boot, intro and repeated demo
  entry/exit.
- Establish full native cadence next, including VBlank debt, GPU-confirmed
  presentation and audio-underrun evidence. The canonical checkpoint
  establishes sustained guest VBlank/submission cadence for the implemented
  families, including A0 and direct copies; it does not by itself close the
  audiovisual cadence gate. Donor-unproven ordered overlap reanchors cannot
  become a hot path in a future workload.
- Compare complete M0 and geometric M1 output at native resolution before
  enabling each additional M1 command family at 4x. Capability rollout is
  atomic and must preserve both completeness and speed.
- Attribute every differing pixel to color, zero/STP, ABR, mask, feedback,
  coverage, depth, copy or display/crop; never accept an aggregate screenshot
  score as proof.
- Keep the native donor painter/OT result as the completeness baseline. Measure
  the project's visibility-first Hybrid genuine-W sidecar, Strict `LEQUAL`
  research mode and future proven-group true-Z policies separately so an
  occlusion regression cannot masquerade as a geometry or transparency bug.

The final target remains complete, full-speed 4x PGXP/true-color/true-depth
rendering. This priority order controls implementation and triage; it does not
waive the final 100% per-vertex and command-family coverage gate.
