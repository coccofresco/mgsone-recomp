# Modern renderer architecture and conformance plan

This document defines the game-independent rendering contract for ps1-recomp.
Crash Bandicoot 3 is the current reference workload, not a source of renderer
special cases. A change belongs in this path only when it is expressed in
terms of GPU, GTE, PsyQ or display semantics and is covered by a reusable test
or deterministic trace oracle.

The current Vulkan depth contract is projective, not screen-affine W:
`replay_depth_{vertex,fragment}.glsl` applies independently supplied affine
depth coefficients to `1 / gl_FragCoord.w`. The native projection XY/W and UV
remain intact. Opaque, transparent, GPU-pulled, interpolated and diagnostic
paths must share this contract. A sampled read-only D32 comparison is required
before fragment-interlock storage writes; late depth tests cannot undo them.
See `evidence/renderer/crash3-projective-depth-20260905.md` for the independent
GPU diagonal regression and for boundaries of the legacy scalar depth oracle.

Host GTE control writes have an explicit semantic origin through
`gte_control_provenance.h`. PsyQ matrix copies must not inherit a stale guest
`ctx->pc`, which would make ordinary native branches break temporal joins.
Stable odd host-origin tags share the provenance carrier with aligned native
writer PCs, but never alter the architectural PC. Matrix values, source
addresses and native instruction tracking remain unchanged. See
`evidence/renderer/crash3-hle-transform-history-20260905.md` for the portable
contract and the rotation/water regression evidence.

The runtime video backend is Vulkan. The authoritative software PS1 target is
permanently native 1x; `res_scale`, true color, PGXP and true depth belong only
to Vulkan. The old CPU `EnhancedSurface` remains an offline/unit-test
conformance oracle and is never the normal display renderer.

Implementation status is deliberately explicit:

- **M0 native presentation and live display plan (implemented):** a latched
  native `R16_UINT` VRAM image and the GP1 state captured with it produce one
  `DisplayScanoutPlan`. Vulkan allocates the plan's complete canvas at
  `res_scale`, clears its padding to black and resolves only the active image at
  the plan's destination origin before swapchain presentation. This is the
  native-complete/full-speed baseline and contains no CPU 4x allocation,
  expansion or raster loop.
- **Live `M0Reanchor` and 4x replay-atlas presentation (implemented):** when
  precision features are requested and the Vulkan resource gate passes, the
  runtime enables schema-9 capture, drains the non-dropping FIFO into the
  submission scheduler and targets the exact strong latch token. A visual
  prefix is replaced atomically by its authoritative native snapshot, which
  seeds the persistent full-VRAM RGBA8/mask/R32-surface/D32-occlusion atlas and
  presents the selected
  4x canvas in the same Vulkan submission when its scanout is progressive
  RGB555. Queue submission, scheduler
  commit and fence confirmation are wired transactionally.
- **Live M1 geometry/feedback/A0/copy/paint executor (implemented with an explicit
  ordered-overlap fail-close):** a
  transactional CPU bake and Vulkan preparation gate now execute ordered
  native-tile uploads, sparse `PaintPixels`, coalesced `PaintRect`/fill,
  whole-surface depth epochs,
  donor-proved non-overlapping copies, donor-safe scratch copies, opaque and semi-transparent
  `Projected3D`, and semantically proven `Screen2D` DrawPackets. Each accepted
  sparse pixel becomes an exact 1x1 native R16 mirror upload plus one
  scale-clipped true-color Screen2D triangle; source STP selects ABR per pixel,
  E6 acceptance remains the native raster's proved result, and mask/R32/D32
  updates use the same accepted-sample contract as other 2D work. Both
  untextured and native-VRAM
  textured 4/8/15-bit draws use precise XY/W/UV and generate RGBA8 true color at
  4x. In the default Hybrid policy, eligible opaque geometry follows GP0/OT
  order with fixed-function `ALWAYS` while writing genuine W to D32; the
  attachment therefore retains physical depth evidence without rejecting an
  OT-later primitive. Strict remains the opt-in `LEQUAL` research policy.
  Transparent geometry remains painter ordered. Accepted transparent fragments
  write genuine presented-surface R32 plus far occlusion D32; accepted
  `Screen2D` fragments write the explicit non-geometric R32 sentinel plus far
  D32, all only after zero/mask rejection. Exact ABR
  and E6 reads use bounded color/mask snapshots refreshed between a feedback
  quad's triangles. A scratch `CopyRect` first
  captures every scaled RGBA8 and source-mask texel, plus an independent
  pre-write destination-mask plane when E6 check-mask is active, before writing
  any destination region. Accepted fragments retain source true color, copy or
  force the mask bit, write the R32 non-geometric sentinel and invalidate D32
  to far; an E6 rejection discards all outputs and therefore preserves every
  destination plane.
  Wrap, force/check-mask and horizontal overlap execute through this path only
  when the native ordered copy cannot read one of its own prior writes. Vertical
  or other two-dimensional ordered overlap and circular horizontal overlap fail
  closed to a whole atomic `M0Reanchor`. Accepted A0 pixels are materialized as
  ordered horizontal R16 runs, expanded by Vulkan into color/mask at 4x, marked
  with the R32 non-geometric sentinel and invalidated in D32 only over those
  accepted samples. Texture/CLUT writes are
  flushed lazily but never past the first native reader. No frame mixes native
  and modern fragments. Unknown state or invalid transfer framing remains
  `M0PresentOnly`.

### Modern presentation target

The eight artefact families summarized in
[Pikuma's PlayStation graphics overview](https://pikuma.com/blog/how-to-make-ps1-graphics)
anchor this requirements checklist; overscan, PS1 destination-dependent
composition and streaming are additional project constraints. The article is
context, not an implementation specification. The modern profile removes
hardware presentation limits only after the original composed image is
complete and full-speed:

| Original limitation | General renderer response | Gate |
|---|---|---|
| Integer screen vertices and polygon jitter | Authentic PGXP/GTE sub-pixel XY rasterized by Vulkan at `res_scale` | Implemented for every observed Projected3D vertex |
| Affine texture wobble | Genuine W and perspective-correct UV, with packet-wide quad/domain rules | Implemented for every observed textured Projected3D vertex |
| No per-pixel Z and polygon-average OT errors | Preserve GP0/OT visibility in Hybrid while recording genuine W per sample; evaluate `LEQUAL` only in Strict or future proven presentation groups | Visibility-first Hybrid implemented; stable object lift planned |
| Near-plane polygon loss | Preserve authentic GTE saturation for compatibility; clip in homogeneous space only when complete Q provenance proves the operation | Instrumented; universal enhancement gate open |
| T-junction cracks and subdivision seams | Recover shared edge/topology groups, then stitch or conservatively cover only proven cracks in presentation space | Planned; never widen an unproven silhouette |
| RGB555 and visible dithering | RGBA8 true-color shading while bit 15 remains an independent R8 mask plane | Implemented on M1 |
| No mipmapping and distance shimmer | Build mutation-versioned, page/CLUT/window-aware mip views; never filter across transparent-zero, CLUT or atlas boundaries | Planned |
| Low framebuffer resolution and overscan assumptions | Vulkan internal 4x canvas, native 1x software oracle and a latched donor-canvas scanout plan | Implemented; final-canvas hash still open |
| Destination-dependent ABR/E6 tricks | Ordered bounded color/mask snapshots and accepted-only RGBA8/R8/R32/D32 updates | Implemented and GPU-readback tested |
| Fixed-point game arithmetic and small RAM/CD streaming | Preserve guest math, timing and streaming unchanged; retain higher precision only in renderer sidecars | Architectural invariant; cross-subsystem differential oracle planned |

Higher-polygon replacement meshes, texture replacement and material upgrades
are separate optional asset layers. They cannot be inferred universally from a
2D GPU packet and are never allowed to substitute guessed content in the
conformance profile.

M0 and `M0Reanchor` are not described as high-precision rendering: expanding a
complete native image on the GPU proves the backend, timing, scanout,
persistent-atlas and ownership boundaries. Only frames accepted and executed
through M1 and actually presented from a precision-compatible replay atlas may
count as high-precision output; total renderer coverage is not claimed until
every observed visual family passes that route.

The canonical Crash 3 attract-mode checkpoint, focused Vulkan readback and exact
scope limitations live in [RENDERER_EVIDENCE.md](RENDERER_EVIDENCE.md). Keeping
one evidence record prevents independently edited architecture documents from
drifting. It proves the observed submission and precision-provenance prefix,
not universal command coverage, a drained final GPU frame, full audiovisual
cadence or final-canvas pixel equivalence.

### CrashReplayV1 boundary

`CrashReplayV1` (current schema version 8) is a backend-neutral, immutable
command stream between the single-writer GP0 processor and Vulkan. A frame is
an ordered variant stream of native tile uploads, surface seeds, polygon
packets, fill/copy/A0/paint
mutations, depth epochs and reset state, terminated by exactly one
`FrameLatch`. Polygon state is fully baked: opcode, route, semantic/fallback
reason, submission and packet sequence, texture/CLUT/window state, ABR, mask
and depth policy.

The `CrashReplayV1` name is a legacy envelope identifier, not a claim that
Crash-specific behavior belongs in the renderer and not proof of multi-title
coverage. A title-neutral successor requires an explicit support matrix plus a
synthetic corpus spanning GP0/GP1 aliases, DMA framing, wrap/overlap, display
mode changes and interlaced parity; each new title then contributes traces, not
renderer exceptions.

Schema 6 retains the per-vertex coordinate provenance introduced in schema 3
and adds exact categorical projection provenance for visibility ownership. It
contains packed post-draw-offset `nativeX/nativeY`, effective raster
`x/y/w`, corrected
`u/v`, genuine `rawDepth`, `nearDepth` and homogeneous post-offset `qx/qy`.
`preciseX` and `preciseY` independently state genuine sidecar provenance;
`preciseXY` is their conjunction. `rawDepthValid` plus the packet `validMask`
prove genuine W, `homogeneousQ` proves the numerators, and `reconciledX/Y`,
`inventedW` and `nearSaturated` record transformations without disguising
their origin. Raw GTE W uses 32768 for normalized one. If any required W is
missing, the donor-compatible visibility path replaces effective W for the
complete packet and marks every affected vertex as invented; it is never
accepted as complete high-precision depth provenance. A finite signed W behind
the near plane remains genuine provenance and may be marked `nearSaturated`.
Each vertex also has independent evidence slots for its process-local
projection event/context, certified RT/TR writer lineage, contiguous
model-vertex source, submitted GP0 operand address and exact DMA list/node/
traversal tuple. Unproved slots stay unknown. The Vulkan bake preserves the
three records per source triangle and
assigns one deterministic owner token; it does not derive family identity from
rendered coordinates or from the likely family-count prior.

Every latch also carries the immutable capture policy: internal scale, PGXP,
true-color and depth mode. Preflight matches it against policy supplied
independently by the Vulkan consumer; it must never derive the expected value
from the producer frame itself. This makes a queued 1x capture fail atomically
after a 4x renderer reconfiguration instead of replaying scale-dependent UV
state under the wrong policy.

Frames are published through a non-dropping FIFO as
`shared_ptr<const CrashReplayFrame>`. Presentation may coalesce completed
latches, but it may not discard their mutations. A0 chunks retain the full
transfer geometry, row-major pixel offset and an accepted-write bitset, so a
VBlank in the middle of a row cannot duplicate or invent pixels. Each A0
header starts one transaction identified by its packet sequence; explicit
`Begin`, `Data`, `Complete` and `Abort` phases preserve that identity across
frame boundaries. Preflight requires contiguous offsets and identical geometry
and mask state, orders the A0 header with polygon packet sequences, rejects
interleaved mutations, and keeps an aborted zero-pixel transfer visible in the
stream. An active transfer cannot hide an additional packet sequence advance.
GP1 reset is separate from a full surface seed, and Screen2D writes invalidate
depth only where mask testing accepted a real write.

Schema 4 records the original homogeneous X/Y numerators needed by future
packet-atomic geometric clipping, but recording data is not executor support.
The live producer and Vulkan scheduler remain saturation-only: requesting
geometric clipping fails closed with `InvalidNearClipMode`, and a packet which
would require that path is not silently replayed as saturated geometry. The
reserved capability cannot be enabled until the Vulkan executor implements and
tests the complete packet policy. Schema 4 still admits only whole-surface
depth clears and resets which establish a new depth epoch.

Before recording any M1 Vulkan command, the renderer preflights the complete
batch through the selected latch. Every native reader carries its own sparse
revision proof. A textured packet proves its conservative 8x8 texture-page and
CLUT read set; `CopyRect` proves the wrapped source-tile set before the native
copy mutates overlapping state. Producer and consumer hash the ordered
`(tile index, native revision)` pairs. Missing, stale or reordered uploads, an
unsupported operation/route, a sequence gap or a latch mismatch selects the
complete M0 frame. Partial M0/M1 composition is forbidden. Producer and
preflight keep these tile sets sorted, duplicate-free and sparse; recording
cost scales with the affected/read region instead of scanning all 8192 native
tiles for every command. A tile upload must strictly advance every known
destination revision: an equal revision cannot prove that a different payload
is idempotent.

The live scheduler additionally requires `ReplayPrecisionCoverage` to be
`fullyHighPrecision()` before selecting M1. This gate is calculated from every
immutable IR vertex, not from samples which happened to survive clipping,
occlusion or the draw area. It requires a complete capture with no unsupported
mutation; exact Screen2D coordinates; genuine precise X and Y for every
Projected3D vertex; no reconciled component or fallback packet; genuine W for
every projected vertex with no invented W; precise UV for every textured
projected vertex; and matched route/depth-policy state on every eligible packet.
Homogeneous Q and near-saturation are reported separately: Q is required by
the future geometric-clip executor, while a genuine signed near-plane vertex
is not rejected merely because saturation was selected.

Depth lifetime is equally transactional. A `SurfaceSeed` establishes the
baseline epoch, every draw or paint command names that exact epoch, and
`ClearDepth` or `Reset` must advance it by exactly one. Missing baselines,
regressions, jumps and 64-bit overflow reject the complete target frame before
any Vulkan command can be recorded.

M0 and M1 therefore use distinct resources. The final native latch image and
canvas-sized RGBA8 target remain pristine native fallback sources. M1 owns a
separate ordered `R16_UINT` native shadow plus persistent full-VRAM RGBA8,
R8_UINT mask, `R32_SFLOAT` presented-surface and D32 occlusion targets. At 4x
the base M1 color/mask/surface/occlusion atlas is 104 MiB. Three persistent
scratch images add 48 MiB worst-case: a 32 MiB RGBA8 source/feedback image, an
8 MiB source/feedback mask image and an independent 8 MiB pre-write
destination-mask image. Each command still copies only its bounded regions. A
quad remains one IR packet, while its two raster halves execute in order and
refresh the destination snapshot between halves when self-overlap, blending or
mask testing can observe the first half.

The Vulkan resource foundation for that separation is implemented and remains
lazy when no precision feature is requested. One transactional allocation owns
the 1024x512 R16_UINT shadow and the scale-dependent color, mask, presented
surface and occlusion atlas outside both `FrameContext` and the swapchain. The
4x plan is 4096x2048 and 153 MiB of nominal texel payload including the 1 MiB
native shadow and the 48 MiB scratch set; Vulkan alignment may raise the actual
device-memory commitment. The M0Reanchor seed
pipeline, descriptor set, framebuffer and canonical initial layouts are
created transactionally. In one queue submit it uploads the authoritative
native snapshot, expands exact RGB555 plus mask into the color/R8 atlases,
initializes R32 to the non-geometric sentinel, clears D32 to far, resolves the
scaled display aperture from that atlas and presents it. The live M1 executor
reuses those persistent attachments through
LOAD/STORE render passes and dedicated opaque plus precise-feedback pipelines.
The native R16 mirror remains in shader-read layout between lazy transfer
flushes; a textured draw flushes only earlier native mutations. The direct
CopyRect path temporarily moves the same non-overlapping source/destination
image to `GENERAL`, copies R16/RGBA8/R8, restores attachment/sampling layouts
and clears destination D32 to far. The scratch CopyRect path instead ends the
pass, copies all wrapped source RGBA8/R8 regions into logical scratch coordinates
and, for check-mask, snapshots the corresponding pre-write destination R8 into a
third image. Explicit transfer-to-fragment barriers make all snapshots immutable
before one scaled-scissor draw per destination region. This path deliberately
leaves the read-through R16 shadow stale until the producer supplies an
authoritative tile upload before a later native reader. Before ABR or E6
destination reads, the executor uses the same bounded snapshot/barrier boundary;
a feedback quad repeats it between its two halves. Physical-device selection
qualifies every replay format, extent and usage up front; Vulkan 1.0 implicit
transfer features are normalized before that check. Late allocation failure is
not a valid route selector.

An A0 `Data` chunk does not re-read E6 state. The CPU bake consumes its
authoritative `acceptedBits`, splits accepted pixels at rejection, logical-row
and native-X-wrap boundaries, and repacks only maximal one-row runs into the
four-byte-aligned R16 arena with force-mask already applied. At the command's
exact ordinal Vulkan flushes preceding plus current native writes, transitions
the mirror back to shader-read, and draws one full-screen triangle per scaled
run under an exact scissor. `replay_native_expand.frag` derives absolute native
coordinates from `gl_FragCoord / internal_scale`, emits RGB555-expanded RGBA8
and bit-15 R8, and writes far D32. Rejected pixels therefore change no
attachment, and no CPU 4x surface is allocated or rasterized.

`PS1_VULKAN_REPLAY_SEED_SMOKE=1` remains a separate diagnostic. It repeats the
seed beside ordinary native M0 presentation without selecting the replay atlas.
In live replay mode the scheduler selects and presents `M0Reanchor` without
that environment variable. `layoutsInitialized` advances only after a
successful queue submit, so acquire/record/submit failure cannot publish a
false replay anchor.

### M0/M1 transition and recovery contract

The live replay renderer exposes three explicit frame routes; none may combine
native and precise fragments in one image:

- `M1Replay` preflights and submits the exact FIFO prefix through the requested
  latch. Supported mutations and opaque, feedback and semantically proven
  Screen2D DrawPackets update the persistent atlas in exact IR order; an empty
  latch simply preserves it.
- `M0Reanchor` submits no M1 command from a rejected prefix. It uploads the
  authoritative native snapshot into the ordered shadow, seeds full-VRAM
  color/mask, clears depth and advances replay metadata to that latch.
- `M0PresentOnly` presents the native snapshot without changing replay state
  when the target latch or swapchain image is temporarily unavailable.

The CPU-only `deriveNativeSnapshotAnchor` gate is implemented. It pairs an
independently supplied snapshot token and capture policy with the exact FIFO
prefix. Executor-capability and full-precision-coverage misses may reanchor;
semantic corruption may not. Its default remains complete-capture-only. The
live scheduler additionally opts into hard native recovery when the exact
strong latch token accompanies the snapshot: missing visual raster IR
(`02h`, `20h`-`9Fh`) is dominated by replacing the whole prefix, while an
`unrecoverableCount` separately rejects unknown GPU state, A0 transfer framing
and any other gap whose continuation is not encoded by color/mask pixels.
Strict M1 never inherits this exception. Timeline, A0, depth, tile revisions
and texture/CLUT proofs remain mandatory; on any error the input state is
returned unchanged. A successful result retains the transactional A0 suffix
and ordered-shadow revision labels while the complete native snapshot replaces
their pixel contents. Every tile must have a known revision baseline, normally
from a whole-surface `SurfaceSeed` (complete exact tile uploads are
equivalent).
The CPU-only three-route submission scheduler is now implemented: it owns a
non-dropping deque, targets the snapshot's strong frame token, preflights from
scheduled state, and consumes an exact prefix only through a no-throw commit
after successful queue submission. Submitted frame references live in the
fence commit; queue-ordered retirement advances confirmed state monotonically,
and presentation failure cannot roll state back. Live capture activation,
mailbox draining, exact-token scheduling, M0Reanchor/M1 submission,
replay-atlas presentation and fence-owned commits are wired. Capability
publication remains deliberately narrower than the schema; unsupported
feedback, 2D and general-copy batches downgrade before command-buffer
recording.

The native snapshot carries the exact strong `CrashReplayFrame` token sealed
under the same GPU/display latch; equal pixel, submission and sequence values
on consecutive empty VBlanks cannot alias an older frame. Capture policy is
permanently sticky after the first published latch. Supporting hot
reconfiguration later requires an explicit timeline generation in both the
snapshot and replay schema rather than resetting sequence numbers in place.

The full-VRAM replay atlas must survive swapchain recreation.
`nativeSnapshotImage` is the authoritative M0 and packed-24 presentation
source; `replayNativeShadow` belongs only to ordered replay, so a new native
snapshot upload can never expose future texture or CLUT data to an older M1
draw. The canvas-sized target is only `scanoutColor`. Both PS1 framebuffer
pages already coexist in the persistent 1024x512 atlas, so a `PutDispEnv` flip
changes source mapping and active placement, not host-surface ownership.

The renderer drains frames into a non-dropping pending deque and preflights
only the prefix ending at the exact target latch. It maintains a scheduled
state for successfully queued work and a confirmed state retired by fences.
Acquire or submit failure consumes nothing. After a successful queue submit,
the prefix moves into its `FrameContext` and remains strongly referenced until
the fence signals; an out-of-date present after submit does not undo that
commit. A capability or precision-coverage miss on a structurally complete
prefix uses `M0Reanchor`. A visually incomplete frame with a strong atomic
snapshot can use the same whole-prefix replacement and no longer wedges the
FIFO. Missing state/transfer framing, a weak scalar-only identity, or
structural timeline failure remains `M0PresentOnly` until a valid producer
anchor is available.

## Non-negotiable gates

Modern rendering is complete only when all of the following gates pass for a
full reference run, including boot, intro, demo entry, demo exit and repeated
demo cycles.

### Acceptance priority

Work is ordered by user-visible risk, not by how advanced a feature sounds:

1. **Image completeness and stability first.** The donor-aligned native image
   must retain every element, transparency/mask interaction and black-frame
   transition, without flicker or demo-entry regression. M0 remains the
   authority whenever a complete M1 result is not proved.
2. **Full native speed second.** The complete image must sustain PAL/NTSC
   cadence without VBlank debt, audio overlap or hidden catch-up. A live 4x
   `M0Reanchor` submission proves routing and atlas presentation, not this
   performance gate: reseeding the full atlas on visual frames is transitional
   work that M1 execution must eliminate.
3. **High precision only behind both protections.** PGXP geometry, true color,
   true depth and 4x coverage are the final objective, but no partial M1 subset
   may be enabled if it removes imagery or loses full speed. Each command-family
   capability becomes live atomically after conformance and timing evidence.

This ordering governs implementation and regression triage; it does not lower
the final requirement of complete, full-speed, 100% high-precision coverage.

### Fast renderer iteration loop

Renderer policy changes must not pay the cost of rebuilding or relinking the
generated game translation unit on every edit. The normal visibility loop is:

1. Run `cmake --build build --target renderer_visibility_iteration -j2`.
   This builds the small `ps1Renderer_visibility_tests` executable and runs the
   focused Hybrid/Strict scalar, capture, preflight, execution-plan,
   submission and Vulkan bake/copy-contract tests.
2. Run `cmake --build build --target renderer_visibility_gpu_oracle -j2` on a
   Vulkan-capable host. This enables the bounded attachment-readback depth
   oracles and exercises the actual Hybrid untextured, textured,
   texture-discard and destination-mask pipelines.
3. Build and run the game only after both focused targets pass. Boot, intro and
   repeated demo entry/exit are the final integration gate, not the inner
   implementation loop.

Generated code reaches RAM through the stable inline front end in
`runtime/memory_access.h`. The lightweight `Memory` definition retains the hot
RAM/scratchpad/BIOS, PGXP and watchpoint paths inline, while only cold MMIO
routing owns concrete device headers in the runtime library. BIOS dispatch uses
the narrow `runtime/bios_access.h` ABI. A GPU or replay header edit therefore
invalidates focused renderer/device objects, not the multi-megabyte generated
translation unit, without adding a call to every guest load/store. Changes to
these interfaces or to generated-code semantics still require the appropriate
recompiler and game-build validation. After changing the generated-code ABI,
regenerate once with
`./build/ps1Recomp/ps1Recomp <game_config.toml> ps1Runtime/src/recompiled_out.cpp`;
ordinary renderer iterations do not regenerate it.

The boundary itself is a regression gate: the generated object's dependency
file must contain no GPU/replay/renderer header, `nm -uC` must expose no
`runtimeMemory*` wrapper, and running either focused target must leave both
`recompiled_out.cpp.o` and the game executable unchanged. Compiler outlining
of pre-existing `Memory` helpers is a separate measured optimization problem;
do not force-expand them into the generated TU merely to satisfy this gate.

### Native equivalence

Native 1024x512 RGB555 VRAM is the sole guest-authoritative image. Enabling
PGXP, true color, true depth or internal scaling must not change any native
pixel, mask bit, texture or CLUT lookup, CPU read, VRAM copy, save state, or
framebuffer-feedback result.

For the same GP0/GP1 capture, native VRAM with the modern path enabled must be
bit-identical to native VRAM with it disabled. Native hashes are therefore a
release gate, not merely a diagnostic. PGXP may improve only the renderer-owned
presentation atlas; it may not alter the guest/native surface's painter order,
affine sampling result or any feedback visible to the game.

The replay atlas, its R32/D32 attachments and `replayNativeShadow` are
presentation sidecars and can never service guest reads,
guest/native-authoritative texture
or CLUT lookups, DMA, save states or game logic. M1 shaders may sample
`replayNativeShadow` solely to reproduce ordered texture lookup within the
presentation replay. The shadow may intentionally lag atlas-only scratch
copies; that is safe only because a revisioned native upload is mandatory
before any later native reader. Current oracles prove this boundary for
rendering/VRAM paths.
A title-independent release still needs event/hash differential coverage for
CPU, RAM, GTE, DMA, CD streaming and audio with modernization on and off.

### Coordinate and presentation-depth coverage

There are three independent coverage KPIs:

1. Every GP0 coordinate operand is classified as `Projected3D` or `Screen2D`.
   `pgxp.vertex.unknown.current` must be zero and classified coverage must be
   100%.
2. Every `Projected3D` polygon coordinate has genuine matched projective
   provenance. The precision sidecar carries homogeneous `(Qx, Qy, W)` and
   the high-precision GTE divider-saturated screen position.
   `pgxp.vertex.xyz.missing.current` must be zero, and
   `pgxp.vertex.xyz.valid / pgxp.vertex.xyz.required` must be 100%. A signed
   `W` at or behind the near plane is complete provenance, not missing depth;
   it remains precise and follows the selected near-plane policy. Packed XY
   coincidence is not proof of depth. `Screen2D` is reported as
   `pgxp.vertex.xyz.not_applicable` and never receives synthetic Z.
3. Every drawable 3D polygon packet takes the precise route. For each polygon
   family and each submission path, not just in aggregate, the
   `gpu.frame.precise_triangle` count divided by the sum of precise and
   `gpu.frame.compatibility_triangle` counts must be 100%. Partial-precision
   packets, modern setup fallbacks and unexplained route toggles must be zero.

Hardware-rejected oversized or degenerate primitives are conformance events,
not missing coverage, once their rejection agrees with the native oracle.
Compatibility rendering keeps incomplete packets visible while provenance is
being closed, but cannot be counted toward the 100% 3D KPI.

The packed architectural GTE result remains unchanged: its divider saturates
and SXY clamps exactly as on the console. The modern sidecar retains both the
fractional divider-saturated X/Y and double-precision pre-saturation numerators
`Qx = OFX*W + H*viewX` and `Qy = OFY*W + H*viewY`, the signed `W = viewZ`, and
the first unsaturated divider plane `W >= H/2 + 1/4096`. The saturated lane is
derived directly from the precise transform, never inferred from packed SXY;
the signed lane remains intact even when compatibility visibility clamps only
the companion raster depth. Monotonic 64-bit
projection IDs and a 64-bit context fingerprint bind split X/Y words to the
same RTPS result and the same `H/OFX/OFY` state. Stale words, XY-only values
and mixed projection contexts remain invalid provenance. Double precision is
required here: at large `H`, float32 cannot distinguish the exact `H/2`
saturation boundary from the following 4.12 depth tick.

### Visual and performance gates

The visual reference target is true-color rendering at `res_scale = 4`:
width and height are each multiplied by four. For Crash 3's programmed
512x256 aperture, the modern raster is therefore 2048x1024 samples before
aspect-correct presentation. Here **4x always denotes spatial resolution**;
it is not a request to run guest time four times faster.

Performance is a separate gate. The PAL guest must sustain its native rate of
at least 50 VBlanks per second through the complete intro-to-demo soak. The
20 ms budget includes native execution, modern replay, latching and
presentation. Passing average throughput while accumulating VBlank debt,
rapidly catching up guest time, dropping audio, or presenting incomplete
submissions does not pass. Optimizations that preserve the visual and native
contracts remain useful and are retained.

Other games must meet their configured native refresh rate. Lower scale is a
debug/fallback option, not a way to claim the 4x gate. Performance work cannot
waive native equivalence, coverage, transition stability, or rendering
conformance.

Before a release may claim full-speed M1, the capture-enabled producer and
continuously draining consumer must replay the pinned 5700-VBlank trace under a
wall-clock benchmark. The report must include capture/preflight CPU time, peak
FIFO memory, tile-read work, Vulkan submission time, GPU-confirmed presentation,
VBlank debt and audio underruns. A microbenchmark or the current
guest-submission checkpoint alone cannot satisfy the full-speed gate.

## Two synchronized render targets

Native VRAM and the modern host surface serve deliberately different
contracts:

```
                        decoded GP0 packet
                                |
                 +--------------+--------------+
                 |                             |
                 v                             v
    +---------------------------+   +---------------------------+
    | native PS1 executor       |   | modern packet replay      |
    | integer XY, affine UV,    |   | precise XY, perspective,  |
    | RGB555, dither, painter   |   | RGBA8, depth, mask         |
    +-------------+-------------+   +-------------+-------------+
                  |                               |
                  v                               v
    +---------------------------+   +---------------------------+
    | authoritative 1024x512    |   | scaled companion surface  |
    | RGB5A1 VRAM               |   | color + float Z + mask     |
    +-------------+-------------+   +-------------+-------------+
                  |                               |
         guest reads, textures,          latched display aperture
         CLUT, copies, feedback                   |
                                                  v
                                             presentation
```

The native executor always runs, including for a fully precise packet. It
uses packed integer coordinates, native affine interpolation, RGB555
quantization, optional dithering, PS1 blend equations, painter ordering and
native mask rules. True depth exists only in the companion surface.

The Vulkan companion surface is configured from `res_scale`. Native M0 owns
only its scanout RGBA8 target. The persistent replay atlas, used today by
`M0Reanchor` and reserved for M1 mutations, includes RGBA8 color, R32 presented
surface, D32 occlusion and exact R8_UINT mask state. PS1 bit 15 remains distinct
from ordinary host alpha
because it is both a destination write mask and a texture semi-transparency
selector.

The M1 executor must make 2D commands and compatibility packets mirror only
native writes that were actually accepted. A native mask rejection must reject
the companion update as well. Fully precise 3D packets replay directly at
scaled sample density using sub-pixel coordinates, genuine Z and
perspective-correct UVs; their later native execution writes the separate
authoritative native target and cannot overwrite that modern result.

## Packet-atomic execution

Semantic classification and precision routing are all-or-nothing properties
of one complete GP0 polygon packet. The decoder first collects all packet
vertices, classifies the packet as projected 3D or screen-space 2D, and builds
a genuine XYZ/W validity mask independently from rasterability. A 3D triangle
requires three finite selected XY positions; a 3D quad requires all four. PGXP
XY is retained when usable, otherwise the donor-selected position is packed
native XY converted to float. If any W is invalid, every vertex in the packet
uses normalized W=1, exactly as SwanStation does. The offline software oracle
can keep that packet visible through vector coverage, while the schema records
the fallback and invented-W provenance. The live full-precision gate rejects
it from geometric M1 and selects complete M0 output. A complete signed vertex
is not a fallback reason: it stays precise and follows the selected near-plane
policy. Mixing a precise half with a compatibility half is forbidden because
it creates seams, unstable depth and frame-to-frame flicker.

For a modern projected packet, execution order is:

1. decode the complete packet and latch its texture-page/ABR state;
2. replay the complete packet into the companion; texture, CLUT and native
   framebuffer-feedback reads use pre-packet native VRAM, while blend
   destinations follow companion submission order;
3. execute the packet through the exact native rasterizer;
4. expose the completed packet to subsequent commands and scanout.

This order makes the modern result independent of the native writes produced
by the same packet while retaining bit-identical guest state.

### Quad atomicity

A quad is one packet, not two independently routed triangles. One four-bit
validity mask selects one route for both halves. Both modern halves execute
before either native half, so their native texture, CLUT and feedback inputs
describe the same pre-packet state. The companion destination still follows
the defined first-half/second-half order. Only then do the two native painter
triangles run.

The shared diagonal follows the top-left rule and must never blend twice.
Intersections on a shared clipped edge use a canonical endpoint order so both
halves produce bit-identical vertices. A precise-degenerate, fully clipped or
draw-area-excluded half is a geometry outcome and never changes packet route
or reveals a packed-coordinate sliver. Flat, textured, Gouraud and
Gouraud-textured quads share this contract.

### Submission atomicity

One complete ordering-table/DMA traversal is bracketed as a submission. The
GPU timeline lock covers the complete linked list, including packet parsing,
native execution and companion replay. A VBlank latch can observe either the
state before a submission or the state after its final packet; it may never
capture a partially consumed ordering table.

PsyQ image transfers (`LoadImage`, `StoreImage`, `MoveImage`, `ClearImage`)
and GPU block DMA use the same boundary. This prevents scanout from observing
half an upload, half a copy, or a texture page whose rows belong to two
different moments. Multi-register `PutDrawEnv` and `PutDispEnv` updates are
atomic for the same reason.

Direct GP0 writes remain individually serialized. The future asynchronous
backend must preserve this same packet and submission ordering with explicit
sequence numbers and immutable replay data rather than weakening the atomic
boundary.

## Depth lifetime and primitive policy

VBlank is a scanout event, not a render-pass clear. Latching or presenting a
frame must not clear modern depth. Depth belongs to scaled VRAM coordinates
and persists across VBlanks and across horizontal/vertical double-buffer
selection until a guest-visible operation replaces the affected storage.

Depth is invalidated only by an operation with real VRAM ownership semantics:

- reset, save-state load, or modern-surface reconfiguration resets the full
  depth domain;
- fills, CPU uploads and VRAM-copy destinations invalidate their written
  regions;
- accepted semi-transparent or `Screen2D` writes invalidate the corresponding
  scaled samples to far because they establish a painter result without one
  physical surface W;
- a rejected mask write and a zero texture texel change neither color, mask
  nor depth.

This lifetime prevents wall-clock VBlank backlog from changing occlusion and
allows games that build or reuse a framebuffer across multiple VBlanks to
remain stable.

### Projected 3D

Projected polygons use scaled vector coverage. Fully valid packets retain
sub-pixel PGXP XY and perspective UVs; invalid-W packets use the donor's
packet-atomic affine W=1 fallback with PGXP XY where usable and native float XY
otherwise. The default `true_depth_mode = "hybrid"` is a donor-informed,
completeness-first project policy. A fully valid opaque packet uses
fixed-function `ALWAYS`, writes its genuine interpolated W to D32, and therefore
cannot be removed by an unproven cross-packet host-Z relation. GP0/OT order is
the Hybrid color-visibility authority. Smaller `W` is still nearer in the D32
representation, but only `true_depth_mode = "strict"` tests that relation with
`LEQUAL`. GTE divider saturation at the recorded near plane is valid depth,
not a painter fallback. The modern decision never alters native
painter-ordered VRAM.

Host depth and texture interpolation deliberately use different quantities.
Depth is the screen-space affine interpolation `sum(lambda[i] * W[i])`, as in
the donor shader after PGXP W is placed in clip-space Z. Perspective UV still
uses `sum(lambda[i] * UV[i] / W[i]) / sum(lambda[i] / W[i])`. Reusing the UV
reciprocal as depth is not equivalent and can incorrectly reject complete
geometry.

Every draw shares one D32 mapping: effective raw GTE W must be in
`(0, 2^32]` and maps linearly to `W * 2^-32`. The fallback value 32768 is
therefore an ordinary interior point, not the far plane. Packet-local
normalization would make the stored evidence incomparable; clamping an
out-of-domain value would fabricate valid depth, so the Vulkan bake rejects it
before recording any command.

Hybrid applies one packet-atomic visibility/depth decision:

- an eligible opaque triangle, or both halves of an eligible opaque quad,
  follows GP0/OT order with no depth read, fixed-function `ALWAYS` and a genuine
  W write;
- a semi-transparent packet also bypasses the prior D32 owner. Each fragment
  accepted after texture-zero and E6 rejection writes far D32 for occlusion
  hygiene and writes its genuine or profile-transformed projective W to the
  independent R32 presented-surface attachment. The R32 write records the
  contributing geometry; it is not the mechanism which makes a later opaque
  packet visible;
- if any opaque packet vertex lacks valid PGXP W, every vertex uses fallback
  W=1 for geometry and UV. The packet remains painter visible and preserves
  D32 rather than publishing fabricated depth. A semi-transparent fallback
  retains the same accepted-only far invalidation as other translucent work;
- a zero texture texel or E6-rejected fragment changes no color, mask or depth.
  Its stale D32 value still cannot hide an OT-later valid opaque packet because
  that packet uses `ALWAYS` and overwrites D32 with its own W;
- E3/E4 draw-area changes and mean-W discontinuities do not create Hybrid
  visibility epochs or heuristic whole-depth clears. VBlank likewise never
  starts a depth epoch.

SwanStation switches `use_depth_buffer` off for semi-transparent and invalid-W
batches, but its legacy hardware pipeline couples that state to `ALWAYS` with
depth writes still enabled. Reproducing that coupling for a painter composite
would publish a false physical occluder; preserving an older opaque D32 owner
and testing it would instead hide OT-later geometry. Hybrid resolves the
presentation conflict by applying `ALWAYS` to every valid opaque packet and
storing genuine W only after fragment acceptance. Accepted far writes remain
for translucent/Screen2D and explicit native ownership changes; rejected
fragments preserve all attachments without gaining veto power over the next
valid opaque draw.

Whole-surface resets can still use an O(1) depth-generation change. The former
draw-area and positive mean-W-jump clears are not Hybrid visibility rules and
must not return as repairs for missing imagery. `true_depth_mode = "strict"`
retains the earlier universal opaque `LEQUAL` experiment as an explicit
research/test mode; it is not the default presentation policy. Strict also
retains the old invalid-W normalized-far D32 write in the CPU oracle/capture.
That state is deliberately non-canonical: the replay execution planner rejects
the complete batch before Vulkan recording instead of silently weakening it to
Hybrid.

Original-game visibility defaults to the policy shared by SwanStation and
Beetle: the GTE projection uses its maximum divider ratio at and behind `H/2`,
then clamps X/Y to the architectural screen range. The companion uses the
fractional saturated X/Y and clamps only its raster depth to the recorded near
plane. Raw signed `(Qx,Qy,W)` remains available for diagnostics and future
geometry processing, so this policy does not fabricate or discard provenance.
It avoids erasing polygons which the native GPU can cover; the Crash 3
configuration selects this default with `pgxp_near_clip = false`.

Geometric clipping remains an explicit research/enhancement mode. With
`pgxp_near_clip = true`, the companion clips each triangle in homogeneous
screen space against its recorded near plane. Sutherland-Hodgman clipping
produces zero, one or two raster triangles while interpolating `Qx`, `Qy`, `W`,
UV, Gouraud color and all packet attributes on the same edge parameter.
Division and perspective interpolation happen only for the surviving positive
domain. Both policies leave the authoritative native polygon on the original
packed GTE output and PS1 rejection rules.

### Guest geometry and presentation geometry

Modernization must never feed reconstructed geometry back into the game. The
renderer therefore has two explicit domains:

- **guest geometry** is immutable evidence: packed SXY, authentic `(Qx,Qy,W)`,
  GTE projection context, draw offset, packet/submission order and eventually
  pre-GTE vertex/matrix provenance. Collision, animation, camera scripts and
  every CPU-visible result continue to use only this domain;
- **presentation geometry** is a renderer-owned embedding derived after
  capture. It may assign a presentation depth, visibility epoch and object
  grouping, but cannot change the source packet or write anything visible to
  the guest.

The next-stage model is a screen-preserving 2.5D lift, not an already proven
universal reconstruction. If a vertex receives a new positive presentation
`Wp`, its clip position can be constructed as
`(ndcX*Wp, ndcY*Wp, depth(Wp)*Wp, Wp)`. It then moves along the same camera ray
while retaining the captured high-precision screen position. A proven group
of vertices deliberately scaled or displaced by the game could be lifted
together, with presentation scale changed in proportion to depth, so apparent
size and placement remain unchanged. The “large box drawn small by OT” case is
the motivating constraint, not permission to infer an arbitrary world pose.

This lift is not uniquely recoverable from a final 2D polygon. The universal
resolver must therefore be constraint based and fail closed, not guess world
coordinates:

1. retain genuine W as immutable per-sample evidence and enable host-Z ordering
   only inside a coherent opaque group whose relation is independently proved;
2. add visibility constraints only for packets whose covered samples overlap;
3. treat OT order, accepted transparency, Screen2D and explicit framebuffer
   mutations as authoritative composition constraints;
4. group candidate objects only from complete categorical provenance: GTE
   transform-writer lineage, model/topology identity, GP0/DMA/OT submission
   lineage and stable temporal identity; final-image similarity is never an
   identity signal;
5. admit a scale/distance family only when exact guest dataflow or an exact
   fixed-point affine relation proves one common projective gauge, represented
   as a reduced positive rational; a `1/1` anchor is optional and may be
   required only when independently identified;
6. treat the likely count of four and integer/tenth gauges as advisory anomaly
   signals, never correctness bounds; require consistent discovery and holdout
   captures and a post-lift owner pass which reproduces OT visibility without
   introducing coverage divergence;
7. fall back to GP0/OT `ALWAYS` visibility while retaining genuine W in D32
   when evidence is insufficient, non-unique or cyclic.

The certainty gate for step 2 is defined in
[`VISIBILITY_AUDIT.md`](VISIBILITY_AUDIT.md). A conflict is admitted only when
exact OT-owner and strict-depth-owner renders disagree at the same accepted
sample and their replay ordinal plus physical-depth inequalities independently
confirm that disagreement. Bounding-box overlap and the F8 grayscale view are
candidate locators, never constraint evidence by themselves.

The depth-family certifier in `modern_replay_depth_family.*` deliberately does
not cluster. For a proved guest gauge `g=p/q`, it scales authentic
`(Qx,Qy,W)` by `q/p`; projected XY is therefore invariant while presentation
depth enters a common relative scale. Its likely-four and decimal-tenth checks
are report-only priors. The offline value-flow audit also splits packed GTE
rotation words into nine independently owned components: a factor is not a
full affine gauge unless all basis and translation components are proved, and
ambiguous bit packing rejects the proof. The current projection-context hash
contains only H/OFX/OFY/shift and is explicitly insufficient for object
identity. Schema 7 carries exact computed-model construction paths as
diagnostic evidence; the still-missing topology/temporal identity and
activation gate are detailed in the audit document.

Step 7 is the implemented visibility-first layer. A valid opaque packet cannot
be rejected by an older packet's W and replaces D32 with its own accepted W.
Accepted painter samples still write far, while rejected samples retain the old
owner, but neither state can veto the next valid opaque `ALWAYS` write. The
schema-8 IR already retains original OT/DMA provenance, GTE transform-write
lineage and direct/computed model construction evidence. A future activation
revision still needs independently proved topology/temporal identity and a
separate `presentationW`; it must be evaluated against the rejected-hole/
ordering oracle before any proven group replaces painter visibility with
`LEQUAL`. A claimed full-3D reconstruction additionally requires complete
pre-GTE model vertices and transforms. Final SXY/W preserves a screen ray, but
does not determine a unique surface or editable world model.

An explicitly configured WIP layer now exercises the next boundary without
claiming that the universal resolver is complete. A strict manifest maps only
categorical construction keys admitted by cross-validated topology edges. On
an exact opaque Projected3D hit, the Vulkan vertex stage derives D32 from
`W * presentationMultiplier` and the draw uses `LEQUAL`; it deliberately keeps
clip X/Y/W based on the original W. This depth-only formulation is visibility-
equivalent to the screen-preserving homogeneous lift while making coverage and
perspective UV independent of the partial hypothesis. Feedback, transparent,
unmapped, malformed and out-of-domain cases retain the Hybrid path per
triangle. The layer is named `wip_depth_manifest`, defaults to empty and is
not a substitute for the complete ownership/raster holdout gate.

Semi-transparent handling remains independent of per-texel STP blend
selection. It preserves effects explicitly ordered relative to opaque geometry
and is covered by the software oracle plus Vulkan RGBA8/R8/R32
presented-surface readback; dedicated visibility passes audit D32 separately.

### Texture coordinate domain

Perspective reconstruction is bounded by one domain prepared for the complete
packet. Each non-constant U or V axis excludes its upper endpoint before the
PS1 sampler applies 8-bit wrap and the E2 texture window. Both halves of a quad
share that domain, preventing a seam where independently prepared triangles
would disagree.

At scales above native resolution, quads whose first triangle has uniform
effective W use the donor's exact `BatchVertex` float plane derivatives, float
division and sign/zero tests for axis-flipped sprites. The calculation consumes
final PGXP/native float XY after packet-atomic W normalization; the fourth W
deliberately does not enter SwanStation's sprite test. A detected axis receives
the one-texel coordinate offset on all four vertices before the common domain
is built. Native scale and non-uniform W among vertices 0--2 do not take this
correction. The scalar oracle keeps a one-entry `(u,v)->texel` cache per raster
triangle; it changes no sampling semantics and includes transparent texels.

### Screen-space 2D

Lines, polylines, rectangles and sprites are classified as `Screen2D`; that
classification alone does not grant a live Vulkan capability. Their packed
signed coordinates plus draw offset remain authoritative. Semantically proven
`Screen2D` DrawPackets use exact native coverage expanded to the configured
scale and painter ordering. They have no geometric Z: after all texture-zero
and E6 rejection, an accepted sample invalidates the old D32 value to far. The
draw itself bypasses depth, and later valid opaque Hybrid work uses `ALWAYS`, so
the far write is attachment lifetime bookkeeping rather than a visibility
escape hatch. It is not fabricated vertex depth and not evidence used to
improve a precision coverage percentage. Coalesced `PaintRect`/fill mutations
are also live and ordered; only DrawPacket vertices enter the Screen2D
precision denominator.

The schema-8 producer represents every GP0 alias from `40h` through `7Fh`.
The variable-length mono and Gouraud polyline decoder waits for the hardware
`(word & F000F000h) == 50005000h` terminator and retains every segment. For the
current completeness bridge, the native raster records only pixels actually
accepted after clipping, texture-zero, ABR and E6 as one ordered
`PaintPixels`; sparse touched tiles advance once per command. Source RGB,
per-pixel STP, blend mode, mask provenance and the final native word are all
retained. The live Vulkan bake validates the execution plan, depth epoch,
native bounds, mask word and every feature dependency transactionally before
expanding the sparse payload. A malformed or unsupported operation still
fails the whole frame closed to M0 rather than publishing a partial M1 image.
The frequent unblended `68h` point keeps its allocation-free, coalescible
`PaintRect` representation.

Polygon-shaped composites require packet-level evidence rather than an opcode
guess. A flat, untextured, semi-transparent quad may be classified as a
full-target 2D composite only when its four distinct corners form the exact
half-open draw target, its local origin and draw offset agree, its dimensions
match an explicitly programmed GP1 aperture, and none of its vertices carries
GTE provenance. A smaller rectangle, a near match, a crossed quad, a
render-to-texture target, missing display ranges or any projected vertex keeps
the packet in the 3D domain. This covers fades without teaching the renderer
game addresses or mistaking an axis-aligned 3D face for UI.

### Compatibility polygons

A polygon packet without complete safe XYZ remains visible through the exact
native rasterizer and scaled mirroring. Each occurrence is attributed by
opcode, primitive family, source word, submission path, validity mask and
fallback reason. Compatibility is a safety path and software-debug aid, not a
permanent mixed rendering mode.

## Color, transparency and mask semantics

The native target preserves RGB555 quantization and optional PS1 dithering.
The modern target performs generated color, modulation and the four PS1 blend
equations in 8-bit channels and stores the result without framebuffer
dithering. Texture source precision remains exactly what the game uploaded.

The following rules apply to both targets and to every future backend:

- raw texel value zero is fully transparent and performs no color, mask or
  depth operation;
- on a semi-transparent textured packet, STP selects blending per texel;
  non-STP texels are color-opaque but remain part of the ordered
  semi-transparent packet for depth policy;
- on an opaque textured packet, STP does not request blending and cannot make
  a hole; the sample writes color, mask and modern depth normally;
- untextured semi-transparent packets use the currently latched ABR mode;
- the tpage/ABR embedded in a polygon is effective for that packet and is
  latched for following sprites and untextured semi-transparent commands;
- mask-check rejects the operation before color or depth changes;
- mask-force and source STP update the independent mask plane, never host
  alpha;
- source/destination blending observes the companion color established by
  all earlier samples in submission order;
- signed 11-bit coordinates, texture windows, fill alignment, rectangle size
  masks and polygon/line hardware limits are applied before rasterization.

## Framebuffer and texture hazards

Native VRAM is simultaneously framebuffer, texture store, CLUT store, copy
source and CPU-visible memory. A replay backend must therefore model hazards
explicitly; uploading only the final native frame at VBlank is incorrect.

Hazard-bearing operations include:

- a packet sampling a texture or CLUT region changed by an earlier packet;
- semi-transparency or mask-check reading its current destination;
- render-to-texture and framebuffer feedback within one submission;
- CPU uploads and fills becoming visible to later texture reads;
- overlapping VRAM copies, including their direction, mask and wrap rules;
- preserving high-resolution true-color samples across a modern VRAM copy
  while native copy results remain RGB555-authoritative.

The software oracle resolves these hazards synchronously and in packet order.
The Vulkan recorder attaches conservative read/write regions and
native-resource versions to commands. `CrashReplayV1` uses immutable pre-packet
dirty-tile uploads for texture/CLUT read sets, explicit barriers and a
versioned native shadow. It may not reorder commands across an unresolved
dependency. Copies transfer color and mask, invalidate destination depth, and
use immutable scratch storage only where that is equivalent to the native copy
order. A bake-time symbolic write/read check rejects circular horizontal copies;
physical two-dimensional overlap with distinct row origins is conservatively
rejected as donor-unproven. Either rejection selects the complete M0 reanchor,
never a partial copy.

## Display, overscan and latching

The visible image is derived from VBlank-latched GP1 state: display start,
horizontal dot-clock range, vertical scanline range and display mode. Nominal
256/320/368/512/640 by 240/480 dimensions are fallback values only when a
title has not programmed the corresponding range.

Crash 3's PAL setup programs H=608..3168 and V=37..293 in 512-wide mode. This
is a 512x256 aperture; a hard-coded 240-line crop removes part of the
Universal logo. The host presents the complete aperture in a centered 4:3
viewport, accounting for console pixel aspect without cropping guest data.
Frontend overscan is a separate policy: donor captures with their normal
border crop produce a 512x256 active image, whereas border-disabled captures
are 559x288 in SwanStation and 560x288 in Beetle. Removing Beetle's one extra
left column makes the uncropped demo-entry images equal at RGB5. Neither
dimension may be hard-coded as a replacement for the latched GP1 ranges.

The pure CPU `DisplayScanoutPlan` retains canvas, destination origin, active
extent, byte-addressed source, 15/24-bpp format, row step, display enable and an
explicit field value. Its two uncropped profiles reproduce the pinned donors
instead of presenting one disputed crop convention as hardware truth: Crash is
559x288 at (23,17) under SwanStation and 560x288 at (24,17) under Beetle, with
the same 512x256 active image.

The Vulkan consumer now uses that plan directly. It allocates the whole canvas,
clears it to black, and confines the resolve viewport/scissor to the scaled
active rectangle. With the configured SwanStation no-crop profile, Crash's 4x
target is therefore 2236x1152 and its 2048x1024 active image begins at (92,68).
Display-off and empty programmed ranges are clear-only frames; malformed plans
or device-limit/scale overflow fail closed instead of sampling a placeholder.
The native and replay resolves share a checked 32-byte push ABI carrying byte
source X, source Y, row step, source scale, active extent, format and field.
Progressive RGB555 may resolve from the persistent 4x replay atlas, including
VRAM row/column wrap.

Packed RGB888 is already implemented as an authoritative native scanout, not as
the renderer's true-color enhancement. The Vulkan fragment shader reads three
independently wrapped bytes from the latched `R16_UINT` snapshot at source scale
one. An accepted M1 prefix may still execute and advance the persistent replay
atlas, but that frame presents the native R16 decode and does not set the
precision-atlas presentation metric.

Interlaced scanout is deliberately still deferred. `GPU::DisplayState` cannot
provide trustworthy field parity, so `rawDisplayCrtcInput()` reports
`Unknown`; the live renderer preserves the complete canvas, active placement,
format and byte phase but resolves contiguous rows from the authoritative
native snapshot. It sets `host.renderer.scanout.interlace_deferred` and never
invents parity from a host frame counter. A future schema 8 must latch raw CRTC
state plus explicit guest field parity and preflight it. Only then can a
persistent load/preserve scanout target apply `rowStep` to one field without
erasing the other.

Native pixels, companion pixels, GP1 state, latch frame and completed
submission ID are captured under one GPU timeline boundary. The renderer
consumes only this immutable snapshot and never reads live VRAM. A display
flip issued after VSync therefore cannot pair frame N pixels with frame N+1
display coordinates.

Each materialized snapshot also carries a monotonic content revision. A later
VBlank with unchanged content still advances temporal/display state, but the
host leaves its CPU upload buffer untouched and skips the Vulkan staging/copy
for an already uploaded revision. This optimization cannot suppress a GP0 mutation,
display-origin change, aperture change or surface reconfiguration because each
of those advances the latched revision.

## Submission and flicker telemetry

Flicker must be reduced to a sequence/state problem before it is treated as a
screenshot problem. The implemented traces provide these correlations:

- `gpu.submission_end` records submission ID, start/end VBlank, packet count,
  precise/compatibility counts, partial packets, bounds rejects and route
  toggles;
- `gpu.vblank_latch` records latch frame, last completed submission,
  submissions since the prior latch, GPU mutation serial, mutations since the
  prior latch, snapshot-reuse state, native and modern aperture hashes, modern
  non-black sample count, draw/display geometry and route totals;
- `gpu.primitive_path_toggle` records opcode and source address together with
  previous/current submission, route and vertex-validity mask;
- `gpu.polygon_route` identifies compatibility packets and whether they had
  partial provenance or a fully precise setup fallback;
- `pgxp.vertex_reconcile` records the original and resolved components when a
  matched positive-depth or signed projective operand needs the per-component
  safety boundary;
- `pgxp.vertex_near_plane` records complete signed projective vertices in the
  saturated divider domain, including projection ID/context and homogeneous
  values, independently of the selected visibility policy;
- `gpu.vblank_latch` publishes covered, accepted, depth-rejected,
  mask-rejected and transparent-texel samples together with near-clipped,
  discarded, degenerate and draw-area-excluded geometry outcomes;
- an empty modern triangle is partitioned by its terminal sample cause as
  `depth_only`, `transparent_only`, `mask_only`, `invalid_depth_only` or
  `mixed`; these categories are mutually exclusive and sum to the empty
  total, so valid occlusion is not misreported as missing geometry;
- rejected modern geometry is checked, only on the cold reject path, against
  exact packed integer top-left coverage in the native draw area. The
  `native_coverage` and `native_empty` partitions distinguish a harmless
  high-precision outcome from a candidate visibility divergence without
  routing either one back to packed coordinates;
- PGXP breakdowns attribute every coordinate by opcode, primitive family,
  submission path, RAM lookup result and provenance-loss reason.

For a surgical provenance capture, `PS1_TRACE_PGXP_VERTICES=1` emits
`pgxp.vertex_reject` only for incomplete or unsafe projective provenance.
Optional
`PS1_TRACE_PGXP_FRAME_FROM/TO` bounds isolate one scene. Each record separates
missing XY, stale packed data, intentional XY-only projection, non-finite or
mixed-context values, and retains exact packet/source addresses plus sidecar
flags and float bit patterns. A finite signed `W` with matching `Qx/Qy` is a
near-plane event rather than a false missing-depth reject. This trace is
independent of the full metrics collector.

`PS1_TRACE_MODERN_REJECTS=1` enables
`gpu.modern_coverage_divergence` only for rejected modern triangles that the
packed native raster would cover. Optional inclusive
`PS1_TRACE_MODERN_REJECT_FRAME_FROM/TO` bounds isolate a transition. Each
record identifies the submission, guest packet address, opcode, native
triangle, high-precision coordinates/depth, projection context, draw area and
offset. It also separates a direct outcome from each fan triangle generated
after near clipping, because the latter is compared conservatively with the
whole original packed triangle and can otherwise look like a duplicate loss.
Outcome 1 is a near-plane discard in opt-in geometric-clip mode, 2 a precise
degenerate and 3 an empty intersection with the modern draw area. The event is
a triage signal, not permission to switch individual packets between packed
and precise geometry. Default saturation is reported separately as
`gpu.modern.near_saturated.triangle` and `.vertex`.

`gpu.modern.depth.packet.eligible`, `.invalid_w` and
`.blend_visibility_boundary` expose the CPU conformance oracle's Hybrid packet
policy. The two `visibility_boundary` names are retained for metrics
compatibility: they describe accepted far invalidation by Projected3D
translucency, not the authority for later opaque visibility.
`gpu.modern.depth.sample.visibility_boundary` counts accepted translucent
fragment writes, not covered samples, unique destination pixels or Screen2D
invalidations. CrashReplay stores the same decision in each immutable
`DrawPacket.state.depth`; planner validation is the authoritative Vulkan-side
gate. A valid Hybrid opaque packet must report no depth read/reject and must
write genuine W with `ALWAYS`.
`F8` post-correction exposes the persistent R32 presented-surface atlas as a
presentation-only RGB depth map. The pre-correction view independently
reconstructs strict raw-W D32; split view presents both for direct comparison.
The resolve samples the selected plane directly on Vulkan and restores its
attachment layout in the same command buffer; it neither changes replay state
nor treats the debug image as precision evidence. The requested and actually active
states are reported separately by
`host.renderer.depth_visualization.requested/.active`, because packed-24 or a
native fail-close frame has no compatible precision-atlas scanout.
Save-state loads preserve the selected F8 view but explicitly rebase the
renderer-side replay timeline after all submitted Vulkan work completes. The
next restored native seed can therefore re-anchor color and D32 even when the
guest frame number moves backwards; abandoned pending frames cannot keep the
view permanently requested-but-inactive. The rebase is counted by
`host.renderer.vulkan_replay_timeline_rebase`.
The stable mapping also makes the view a fast oracle for the presentation lift:
interior black holes, hard W jumps across otherwise continuous geometry and
temporally moving depth edges become directly visible. These observations
select trace candidates; they do not waive the constraint/provenance proof
required before changing presentation depth.

Automated runs can select the same view at startup with
`PS1_VULKAN_PRESENTATION_VIEW=pre`, `post` or `compare`; an absent or unknown
value retains normal colour. With `PS1_VULKAN_REPLAY_READBACK=all`,
`PS1_VULKAN_REPLAY_READBACK_DEPTH_DUMP_DIRECTORY` writes the selected R32
presented-surface region as RGB PPM (`near=red`, `far=blue`, invalid or the
far-clear value black). This dump is the uncomposited atlas depth, not a
screenshot of the F8 canvas. Native/M0 fail-close frames currently re-anchor
that atlas to far depth, so intermittent all-black dumps remain an explicit
depth-continuity gap rather than evidence that a scene contains no geometry.

### Crash 3 clamped-depth and procedural-surface evidence

Crash 3 supplies two distinct counterexamples to treating every OT value as a
metric depth. First, an object renderer node for a sky or distant family can
place every observed face in OT bucket 0 or 2047. Such an endpoint proves only
a one-sided inequality. Replacing its original, painter-ordered faces with a
host mesh at one synthetic depth allowed the sky to occlude terrain in some
demo levels. A wider temporal A/B then showed that even non-endpoint matched
replacement made Crash and other dynamic objects disappear in groups of
frames: per-frame geometric/material agreement did not prove temporal color
equivalence.

The operational presenter consequently never shadows a matched original
object packet. Decoded source-absent faces are clipped by Vulkan to the left
and right one-eighth margins created by the 3/4 horizontal projection; they
cannot touch the already complete central 4:3 interval `[64,448)`. The full
triangle and authentic projective W/UV/color reach the rasterizer, so scissor
clipping introduces no synthetic edge vertices. Diagnostic replacement
remains available explicitly: it still rejects an endpoint-only node and uses
packet per-vertex W rather than a face mean for a non-endpoint exact match, but
it is not selected by the checkpoint matrix.

Second, the aeroplane terrain and jet-ski water are emitted by two
structurally equivalent four-corner surface walkers. Their viewport reject is
not one of the ordinary scenery-polygon cull sites. Changing the early SXY
reads is unsafe because the first value is also cached for packet emission;
that experiment recovered the edge but produced transient strips. The shipped
Crash profile instead recognizes the four intrinsic AVSZ4 sites
(`80043C28`, `80043CDC`, `800446A8`, `80044764`), validates the expected
packed 512-by-257 bound, reconstructs all four original corners, and rebuilds
only the branch-local outside witnesses in the expanded presentation domain.
It does not modify the GTE FIFO, cached packet coordinates, camera, Vulkan
viewport or guest-visible simulation. No level ID or asset identity selects
the rule: the exact code shape and register/bound contract do, and any contract
mismatch fails closed.

The 2026-09-02 4x checkpoint matrix exercised 300 requested frames in every
captured demo scene. The aeroplane case prepared 105,921 procedural tests and
recovered 31,490 16:9 edge cases; the jet-ski case prepared 108,834 and
recovered 33,041. Both reported zero bound mismatch, zero fallback packet and
complete high-precision prefixes. Five other demo checkpoints passed while
reporting zero procedural preparation, proving that the targeted path stayed
dormant there. Across all seven scenes, original object shadowing and
replacement stayed zero; 37,329 opaque host draw groups and 3,007
semitransparent draws were all margin-confined.

A 90-frame warp-room A/B keeps Crash visible in every final sample. Over the
first 30 frames, the concatenated 1536x1024 RGB central images are byte-exact
against the object-presenter-off control, while 22 whole frames differ because
the host layer still adds lateral content. Two 320-frame aeroplane/jet-ski
sequences sampled every eighth frame cover the formerly corrupt transition
points: player vehicles and surface geometry remain continuous and the sky
stays behind the scene. The M0 depth-clear limitation described above is still
open and prevents claiming continuous post-correction depth for every
presented frame.

`gpu.modern.uv.limit.primitive`, `.flip_u.primitive`,
`.flip_v.primitive`, `.clamp.sample` and `.sample_cache.hit/.miss` expose the
packet UV domain without relying on screenshots. The legacy
`gpu.modern.depth.epoch.clear.draw_area` and `.clear.z_jump` counters remain
observable during migration but must stay zero for Hybrid visibility. A
non-zero value is a regression, not evidence that a missing primitive was
repaired.

`gpu.modern.metrics.schema_version=5` identifies the visibility-first Hybrid
metrics policy: valid opaque packets follow GP0/OT with `ALWAYS` and store
genuine W; accepted translucent samples write far. Metrics schema 4 used
cross-packet opaque `LEQUAL`, draw-area/mean-W heuristic clears and
accepted-sample far invalidation. Schema 3 preserved D32 across painter packets
and exposed the now removed `.blend_painter`; tools must not treat its absence
as zero. Schema 2
exposed the now-removed
`gpu.modern.depth.packet.blend_write`; tools must not interpret its absence as
a measured zero. The older schema-1 experiment used
`gpu.modern.depth.packet.fallback.saturated_w` and `.fallback.blend`; comparison
tools must likewise branch on the schema.
`precision_coverage_reject.coverage_schema=2` independently versions the
aggregate trace fields which now include configured Hybrid
`ot_visibility_boundary_packets/vertices`; the legacy field name counts
submitted translucent far-invalidation policy, not accepted fragments and not
opaque visibility control.
This metrics version is independent of the `CrashReplayV1` IR schema, which is
currently schema 8.
`pgxp.vertex_hit` remains a genuine XYZ/W hit. XY-only and native-W1 operands
remain misses and are additionally exposed through the packet validity mask,
`gpu.frame.partial_precision_packet` and `.depth.packet.invalid_w`.

The live replay gate publishes the schema-8 IR proof under
`host.renderer.replay_coverage.*`: separate projected X, Y, XY, genuine W,
homogeneous Q, reconciled components, invented W, near-saturated, textured UV
and exact Screen2D `DrawPacket` counts culminate in
`high_precision_complete`. These are per-vertex denominators over the selected
immutable FIFO prefix. A zero visible sample count, full occlusion or off-screen
primitive cannot remove a vertex from that proof.

That selected-prefix namespace is a planning snapshot, not the cumulative
submission proof. Only after Vulkan preparation, successful queue submission
and a valid consuming scheduler commit does the renderer append the complete
prefix to `host.renderer.replay_coverage.m1_submitted.*`. It counts prefixes,
frames (including latch-only reuse), complete/incomplete capture and precision
partitions, every vertex denominator and the last committed queue serial. A
failed preparation or atomic M0 replacement cannot leak partial M1 counts into
this namespace; fence confirmation remains a separate monotonically advancing
serial. These counters prove submitted executor coverage, not necessarily the
surface selected for scanout: packed-24 and deferred-interlace frames must also
be checked against `host.renderer.vulkan_replay_present_atlas` and the scanout
format/defer states.

Independent `owner_*_triangles` counters expose complete and coherent
projection, transform, direct-model, computed-model, GP0 and DMA traversal
evidence. A computed-model owner retains the ordered three-vertex fingerprint
tuple; it does not require the three program fingerprints to be equal. These
are diagnostics, not part of the current M1 precision gate and not a family
classifier.

Projected3D coordinate resolution distinguishes legacy XY/XYZ metadata from a
complete signed projection. Legacy values retain the 8-pixel native-delta
guard. An authentic finite GTE Q/W projection with positive W and positive near
depth may exceed that delta when both its stored and reconstructed coordinates
remain within +/-8192; this preserves authentic off-screen precision caused by
fractional transform W. The narrower `PreciseGteSaturated` lane handles
`0 < W < near` in donor `Saturate` mode: when exact GTE provenance is present
and finite SXY remains inside the architectural signed-11 clamp
`[-1024, 1023]`, that independently computed saturated SXY stays authoritative
even if raw Q/W crosses +/-8192. Vulkan consumes SXY with `W=max(rawW, near)`;
it does not reconstruct the position from extreme Q. The exception is forbidden
for `W >= near`, out-of-clamp SXY, false/stale provenance, non-finite operands or
the unsupported `GeometricClip` mode. Beyond the applicable guard both axes are
marked reconciled and the whole packet remains in M0.
Homogeneous Q itself additionally requires GTE origin, nonzero projection ID
and positive finite near depth, so false-origin/near metadata cannot leak Q
through CPU, RAM or GPU propagation. Independently valid legacy XYZ may remain
usable without homogeneous Q. Stale and non-finite inputs still fail closed,
and Screen2D continues to use exact packed/native coordinates independently of
this rule.

For surgical diagnosis, `pgxp.vertex.reconcile.last.*` retains the last exact
producer operand in compact metrics, including Qx/Qy, near depth and projection
ID/context bit patterns. `PS1_TRACE_MODERN_REJECTS=1`, optionally bounded by
its frame window, emits `replay.precision_coverage_reject`,
`replay.precision_packet_reject` and `replay.precision_vertex_reject` at the
replay latch frame with packet sequence, operation index and per-vertex reject
mask. The asynchronous filter is evaluated against each immutable latch rather
than the emulation thread's current frame. Schema 1 reject-mask bits are: 0
missing/non-finite X, 1 missing/non-finite Y, 2 incomplete XY, 3 invalid depth
provenance, 4/5 reconciled X/Y, 6 invented W, 7 incomplete textured UV, 8
inexact Screen2D, 9 packet fallback and 10 donor-depth state. Vertex records
also retain homogeneous-Q state and Qx/Qy. This separates asynchronous
producer/replay frame numbers without a multi-gigabyte general event trace.

The invariants used during diagnosis are:

- completed submission IDs are monotonic and a latch never exposes an active
  submission;
- a latch with no GP0/GP1 mutation reuses the already materialized native and
  modern buffers and retains stable hashes; a direct non-OT mutation advances
  the serial even when the completed submission ID is unchanged;
- repeated zero-submission latches or multiple submissions per latch are
  correlated with VBlank backlog rather than silently dismissed;
- an oscillating `modern_nonblack`, native/modern hash, validity mask or route
  at the same scene is investigated as a concrete disappearance/flicker
  cause;
- incomplete-provenance packets never produce partial modern geometry: one
  packet-atomic W=1 decision covers every half in the software donor oracle and
  remains explicitly counted. The live scheduler's full-precision gate rejects
  that fallback from geometric M1 instead of relabelling invented W as
  provenance.

Trace-only hashes and counters must remain optional so disabled telemetry
does not alter timing. Deterministic captures retain packet words, source
addresses, PGXP provenance, submission IDs and GP1 events.

`timing.vblank_debt_at_poll`, `timing.vblank_debt_peak`,
`timing.vblank_catchup_batch`, `timing.vblank_debt_ticks`,
`timing.vblank_slice_limited`, `timing.vblank_debt_after_slice`,
`gpu.frame.snapshot_reused`, `host.present` and `host.elapsed_us` provide the
performance side of the same diagnosis. `PS1_METRICS_FLUSH_VBLANKS=N` writes
recoverable checkpoints during a long run, and `PS1_EXIT_VBLANK=N` ends at an
exact guest-time boundary so before/after measurements cover identical work.
`host.texture_upload`, `host.texture_upload.bytes` and
`host.texture_upload.reused` distinguish presentation traffic from raster
cost. Expensive aperture hashes are opt-in through
`PS1_TRACE_FRAME_HASHES=1` and report `hash_valid` explicitly.

The default saturated projective path avoids allocating/copying near-clip work
arrays. In geometric-clip mode, the all-front path likewise avoids those arrays
and re-dividing three vertices; clipping is entered only when a vertex crosses
the recorded plane. Raster coverage is derived from
the five mutually exclusive terminal sample counters instead of incrementing
a sixth counter in the 4x inner loop. These are performance optimizations
only: they do not change the configured spatial scale, guest cadence,
coverage rules or native/modern results.

The scalar oracle additionally specializes textured and untextured kernels,
stops a scanline after its single convex coverage interval, derives UV-cache
hits outside the loop, and reuses raw/flat true-color texels. Perspective U/V
division uses one two-lane IEEE operation on SSE2 hosts, with an exact scalar
fallback elsewhere; it does not use a reciprocal approximation or fast-math.
At the fixed 1300-VBlank software-oracle 4x checkpoint, the pre-optimization baseline took
33.936 s with peak VBlank debt 397 and covered 671.7 M modern samples. The
final donor-routed CPU-oracle run took 32.033 s, debt 300 and covered 664.1 M samples:
normalized scalar cost is 48.24 rather than 50.52 ns per covered sample (4.5%
lower), while elapsed time is 5.6% lower. Earlier isolated kernel runs reached
30.717 s, but that figure predates the final donor routing and is not used as
the runtime claim. The retained optimization is useful; 32.033 s still exceeds
the 26 s PAL guest interval, so the 4x performance gate remains open and
requires accelerated replay.

The final Vulkan M0 smoke test on 2026-08-28 completed 180/180 PAL VBlanks and
presentations in 3.601857 s. Metrics reported `software_scale=1`,
`vulkan_scale=4`, an early programmed 512x256 aperture resolved to 2048x1024,
and ten 1 MiB native uploads; unchanged latches reused the resident image.
This establishes full-speed backend/presentation capacity only. It does not
count as the M1 PGXP/true-color/depth performance result.

The live schema-8 path captures and schedules the exact FIFO prefix, seeds or
reanchors the full 4x atlas, executes geometric M1 work and presents that atlas.
The canonical checkpoint in
[RENDERER_EVIDENCE.md](RENDERER_EVIDENCE.md) establishes the observed M1
submission/precision prefix without duplicating run counters here. Unobserved
overlap variants, the full M1 differential corpus and final-scanout validation
remain before universal replay coverage can be claimed. The donor-safe scratch
executor has a separate
opt-in hardware integration test on an NVIDIA GeForce RTX 4060 at 4x. It first
establishes genuine non-far D32, then verifies exact expected RGBA8, source and
destination R8 mask behavior, horizontally overlapping E6 acceptance and far
D32 invalidation by hash. The test passed with no executor downgrade. The host
does not have `VK_LAYER_KHRONOS_validation` installed, so this result is not a
validation-layer-clean claim; the shader separately passes SPIR-V validation.
The opt-in asynchronous replay-atlas readback is now live: each selected latch
copies RGBA8/R8/R32 presented surface into its own `FrameContext` and consumes
it only after that
frame's existing fence, without a queue-wide wait. This is not yet a readback of
the final `scanoutColor` canvas. A future scanout readback must hash the entire
canvas, including black padding, active placement and packed-24 byte phases.
The first 4x M0 seed probe compared 4,096 samples against the immutable native
snapshot with identical color, mask and depth hashes and zero invalid depth;
M1 capture currently publishes actual hashes until an offline atlas oracle is
provided.

Accepted prefixes also publish cumulative `host.renderer.replay_ir.*`
inventory counters (operation families, projected/2D/textured/translucent/mask
draws, vertices and upload/paint payloads). Inventory is measured locally over
the whole committed prefix, then published once after `commit.valid()` under
the route-specific `replay_ir.m1_submitted.*` or
`replay_ir.m0_reanchor_submitted.*` namespace; the legacy namespace is their
aggregate. This prevents rejected or partly prepared work from inflating the
executor inventory and selects coverage from the real Crash 3 workload instead
of from a hand-picked scene.

Wall-clock VBlank work is consumed in configurable bounded slices (one tick
per present by default). The clock thread never executes a guest callback.
Instead the main thread retains at most the newest single elapsed edge until a
game-thread-safe VSync boundary and coalesces any older backlog. Losing that
single edge makes a frame which crosses a 20 ms PAL boundary by microseconds
wait a second complete period; retaining several would expose a host stall as
guest fast-forward. Presentation and event processing therefore remain between
individual guest edges, and a persistently slow backend still runs in slow
motion rather than publishing an entire demo phase behind one frozen host
frame. `timing.vblank_host_pulses_coalesced` records discarded excess host
edges; `timing.vblank_debt_ticks` counts only additional guest ticks actually
consumed inside a configured multi-tick slice.

## Software oracle and Vulkan replay

The software modern surface is an offline semantic reference. It remains
available for unit tests, headless replay, donor comparison and differential
validation, but normal video never allocates or presents its 4x output.

The Vulkan backend receives a decoded, packet-atomic stream. A VBlank publishes
an immutable frame containing ordered draw/resource commands, display state
and completed submission ID. Vulkan maintains a persistent scaled VRAM target
with RGBA8 color, R32 presented-surface depth, D32 occlusion depth and an
independent R8 mask. It resolves the selected
active rectangle into the complete display canvas without copying the 4x
surface through the CPU. `M0Reanchor`
establishes the initial whole-surface baseline; the live M1 executor then
applies precise geometry, feedback, direct copies and A0 mutations
incrementally.

Presentation-rate expansion has an additional hard ownership boundary. Guest
execution and host CPU work may publish only authentic game endpoints plus
correspondence metadata. Every added image is constructed inside Vulkan: a
command-buffer copy starts from exact endpoint B, then an FP64 compute or raw
vertex pass derives only certified homogeneous X/Y/W between endpoints A and B.
Current UV, colour, material and state remain endpoint B. A primitive without
a complete identity proof snaps to B, and a device without the required Vulkan
FP64 path disables added frames. There is no CPU-generated intermediate-frame
fallback.

Vulkan conformance requires:

- native execution remains on the exact CPU path and never depends on GPU
  readback;
- scissor, top-left coverage, texture window, perspective interpolation and
  depth acceptance match the software oracle;
- textured semi-transparency can split STP and non-STP samples into ordered
  passes, while the four ABR equations and mask update remain exact;
- uploads, copies and render-to-texture hazards execute in stream order;
- intermediate frames may be skipped for presentation when late, but their
  state-mutating commands may not be discarded;
- modern color/mask comparisons are exact where operations are integer;
  any interpolation tolerance is explicit and depth accept/reject order must
  remain identical.

The donor-guided resource boundary is an ordered, versioned native VRAM shadow,
not a final VBlank upload. SwanStation flushes only when a dirty native region
intersects the next texture page or CLUT; Beetle independently tracks producers
in 8x8 tiles and resolves only tiles in the next read set. The replay stream
therefore records immutable `NativeTileUpload`, `DrawPacket`, `PaintPixels`,
`PaintRect`, `FillRect`, `CopyRect`, `CpuUpload`, `ClearDepth`, `Reset` and
`FrameLatch` commands. A textured draw uploads intersecting dirty tiles before
sampling; blend and mask-test snapshot only their destination bounds; an
accepted scratch copy snapshots source color/mask and the optional pre-write
destination mask before writing. An ordered or cyclic overlap which is not
equivalent to that immutable snapshot fails the complete bake. A partially
supported frame remains on the native-complete Vulkan M0 source—native and
precise fragments are never spliced into one incomplete image.

The persistent Vulkan resources are a native `R16_UINT` shadow, RGBA8 modern
color, exact mask/stencil, R32 presented-surface depth, D32 occlusion depth, and
three bounded-use scratch targets for RGBA8 source/feedback, R8 source/feedback
mask and R8 pre-write destination mask.
Integer fragment equations implement the four ABR modes; fixed-function UNORM
blending is not assumed bit-exact. Once conformance is established, only
adjacent opaque packets with identical state and no unresolved tile hazard may
batch. Transparent draws, mask tests, copies and texture dependencies remain
segment boundaries.

`CrashReplayV1` covers GP0 `02,20,2A,2C,2E,30,34,36,38,3C,3E,68,80,A0` plus
the baked E1--E6 state observed through VBlank 5700. This is the smallest
reference set with 100% observed visual-command-family coverage. The manifest
in [RENDERER_EVIDENCE.md](RENDERER_EVIDENCE.md) links the versioned metrics
artifact containing exact frequencies; an older incompatible capture is not
used as current evidence. The recorder represents zero-size/clipped no-ops,
exact E6 acceptance, wrapped copy geometry and transactional A0
`Begin/Data/Complete/Abort` phases split at arbitrary VBlanks; rejected or
unknown operations mark the complete latch unusable for M1. Screen-space 2D
packets are emitted even without PGXP and use integer XY and W=1; accepted
samples explicitly invalidate companion depth in their written region.
Higher scales and batching are allowed only behind the same capture/replay and
native-equivalence gates.

## Evidence and release procedure

Each meaningful renderer milestone includes:

- unit tests for every affected native and modern rule;
- a deterministic GP0/GP1/provenance capture around each failing scene;
- replay through native, software-modern oracle and Vulkan paths;
- native VRAM hash equality with modern rendering enabled and disabled;
- color, mask, depth-decision and display-aperture comparisons;
- per-family and per-submission-path 3D coverage reports;
- an intro-to-demo soak covering entry, exit and repeated cycles;
- sustained 4x/50 Hz timing plus VBlank backlog and audio metrics;
- comparison against independent implementations when semantics are
  ambiguous. SwanStation is the primary compatible donor, Beetle PSX HW is
  the independent oracle, and PsyCross supplies the precision-rendering
  model.

The reset-to-demo visual oracle uses the pinned software renderers rather than
a hand-selected screenshot. With their default active-image crop,
SwanStation is black on frames 4309--4668 and first shows the demo at 4669;
Beetle is black on 4238--4571 and first shows it at 4572. The visible segment
lasts exactly 1334 frames in both, with a stable 97-frame offset. Across 23
aligned checkpoints at 60-frame intervals, both donors reported identical
non-black coverage; after canonical RGB5 quantization only 92 of 3,014,656
pixels differed (0.00305%, at most 26 in one checkpoint). Cross-core gates
therefore downsample the modern result to native resolution, quantize to RGB5
and compare coverage against both donors. Raw RGB8 hashes are not a valid
cross-core oracle because SwanStation and Beetle expand 5-bit channels
differently.

The 2026-08-28 Crash 3 reference soak reached VBlank 5200, including demo
entry and execution, with all 5,762,657 required projected vertices carrying
complete provenance. Compared with the preceding capture, compatibility
triangles fell from 75 to 0, partial packets from 61 to 0, path toggles from
125 to 0, missing XYZ from 83 to 0 and modern setup fallbacks from 14 to 0.
Near-plane instrumentation recorded 1,992 signed vertices and 160 fully
hidden polygons, with no invalid projective value or projection-context
mismatch. Host elapsed time for the identical 5200-VBlank boundary improved
from 162.8 s to 145.1 s; this is retained as a useful optimization result,
not interpreted as the meaning of the 4x visual target.

A subsequent hash-enabled diagnostic soak reached the same VBlank 5200
boundary without crashing and attributed 1,680,353 empty modern triangles:
1,676,942 (99.797%) were depth-only occlusion, 1,074 transparent-texture
only, and 2,337 mixed. No mask-only or invalid-depth-only empty occurred.
Those depth-only counts predate the visibility-first Hybrid policy and are
retained only as historical localization evidence; they are not a current
occlusion acceptance baseline.
The packed-coverage oracle reduced the remaining geometric investigation to
125 near-plane discards, 52 precise degenerates and 383 draw-area exclusions.
Donor comparison showed that signed near clipping was not their original-game
policy, so those 125 cases directly motivated the default saturated lane and
its separate counters. The other outcomes remain source-addressable trace
candidates rather than aggregate "missing geometry" guesses. That diagnostic
deliberately enabled per-frame
hashing and a large GP0 trace, so its elapsed time and total submitted work
are not used as a performance comparison.

A screenshot is supporting evidence, never the sole acceptance test. A
visual improvement cannot waive a native mismatch, missing provenance,
partial packet, unstable submission, transition failure or performance gate.

### Vulkan-only temporal certification elision

The strict frame-ownership boundary also applies to performance work: the
recompilation produces authentic endpoints only, and every extra presentation
frame is constructed inside Vulkan. Once the complete resident transparent
prefix proves continuous raw coverage, immutable topology, matching scene
revision/resource instances and original vertex eligibility, the CPU no longer
inserts those vertices into its temporal hash or creates unused previous-vertex
records. Projected fallback faces still snap to endpoint B.

Ordinary metrics no longer enable the quadratic transparent OT-bucket audit.
That offline analysis now requires
`PS1_CRASH3_VULKAN_DIAG_SEMI_BUCKET_INVERSION=1`, so performance collection
does not silently alter renderer complexity. The exact readback and timing
evidence is recorded in
`evidence/renderer/crash3-vulkan-only-temporal-certification-elision-20260903.json`.

The same compact proof now covers mixed opaque streams: immutable raw faces
use one topology ordinal each, while projected near-plane fallbacks and faces
rejected by the original temporal eligibility contract are explicit endpoint-B
snaps. The CPU therefore stopped expanding the sampled `demo.code_65_e` opaque
layer into 38,577 temporal vertex records; only 795 non-raw endpoint samples
remained. This is metadata elision, not host interpolation. The recomp still
publishes only original endpoint frames, and Vulkan still creates every added
vertex and image.

At 4x, 185/185 colour and 185/185 depth readbacks remain byte-identical to the
pre-optimization Vulkan result. The four-scenario warp, streaming and demo
matrix retains the same 66 midpoint/endpoint pairs and has no incomplete M1
prefix. Two heavy 100-VBlank samples average 48.85 VBlank/s, 31.82% above the
preceding 37.06 result and 41.27% above the original 34.58 baseline. One
additional raw-layout boundary in that heavy sample deliberately skipped an
optional pair rather than inventing a cross-revision identity; the result is
therefore not presented as a perfectly frame-count-identical benchmark.
Detailed evidence is in
`evidence/renderer/crash3-vulkan-only-mixed-raw-topology-20260903.json`.

## Portability boundary

Game configuration may select timing, internal scale, true-color/depth
enablement, presentation policy and optional diagnostics. It may not redefine
blend equations, coordinate decoding, packet atomicity, depth lifetime,
draw-area behavior, mask semantics or display-register interpretation.

Per-game address maps belong to recompilation and overlay configuration. No
Crash address, scene name or asset rule may enter the renderer library. Crash
3 remains a demanding reference and regression workload; the architecture is
for every supported PlayStation title.
