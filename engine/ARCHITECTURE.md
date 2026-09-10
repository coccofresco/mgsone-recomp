# ps1-recomp -- Architecture

This document describes how the recompiler is put together: what each module
owns, what data crosses module boundaries, and which decisions are deliberate.
It is meant as a starting point for contributors and as a frozen reference for
the design choices behind the project.

The high-precision GPU path, its measurable 100% coverage gates and its
native-VRAM/modern-surface split are specified in
[MODERN_RENDERER.md](MODERN_RENDERER.md).

For build instructions see [README.md](README.md). For day-to-day contributor
workflow see [CONTRIBUTING.md](CONTRIBUTING.md). For a side-by-side comparison
against N64Recomp / PS2Recomp / psxrecomp, see the audit notes in `docs/`
(local working tree).

---

## What this is

`ps1-recomp` is a **static recompiler** for PlayStation 1 games. It reads a PS1
ELF or PS-EXE binary, decodes every MIPS R3000A instruction, emits an
equivalent C++ function for each MIPS function, and links the result against
an in-tree runtime that simulates the PS1 hardware. The output is a native
x86-64 executable that runs the original game code at full CPU speed without
an interpreter loop.

The architectural lineage is explicit: N64Recomp pioneered this technique for
N64 titles, PS2Recomp adapted it for the PS2's R5900, and `ps1-recomp` is the
PS1 analog. The three-binary layout (Analyzer + Recompiler + Runtime) follows
PS2Recomp's model; the in-tree runtime + TOML configs follow both.

---

## Three-phase pipeline

```
        +-------------+
disc ->  | ps1Analyzer | -> game.toml -+
        +-------------+              |
                                     v
                              +-------------+
                              |  ps1Recomp  | -> recompiled_out*.cpp
                              +-------------+              |
                                                           v
                                                  +----------------+
                                                  |   ps1Runtime   | -> ./game (native binary)
                                                  +----------------+
```

Each binary has one job. They communicate via files (TOML config, generated
C++ source), not shared memory or IPC. Each phase can be re-run independently.

### Phase 1 -- `ps1Analyzer/`

**Input:** a PS1 disc image (CUE/BIN or ISO 9660) and a region tag.
**Output:** a TOML config describing every function in the executable.

The analyzer reads the disc, extracts the boot executable, parses its ELF
sections, and walks the `.text` segment looking for function prologues
(`addiu $sp, -N` and friends). Detected functions are matched against the
PsyQ signature database in `ps1Analyzer/data/psyq_signatures.toml` -- 3463
SHA-256 hashes covering 14 PsyQ SDK releases and the five core libraries
(libapi, libgpu, libcd, libgte, libetc). Matches are emitted as
`[[hle_functions]]` entries so the recompiler can skip them and the runtime
can route them to native C++ stubs.

The hash-based detection is the project's main divergence from prior PS1
recompilers (which use either manual stub lists or per-game annotations).
Generation, hashing and collision handling all live in `tools/`:

| Step | Tool |
|------|------|
| Parse SN Systems `.LIB` archives | `tools/psyq_lib_extract.py` |
| Hash every function body | `tools/extract_psyq_signatures.py` |
| Match at analysis time | `ps1Analyzer/src/psyq_signatures.cpp` |

### Phase 2 -- `ps1Recomp/`

**Input:** `game.toml`.
**Output:** `recompiled_out.cpp` plus optional `recompiled_out.partNNN.cpp`
translation shards -- one C++ function per MIPS function.

The recompiler decodes each MIPS instruction (`mips_decoder.cpp`) and emits a
1:1 C++ translation (`instruction_emitter.cpp`). Every recompiled function
takes `(uint8_t* rdram, recomp_context* ctx)` and mutates `ctx` (the register
file). GTE coprocessor instructions are emitted by `gte_emitter.cpp` against
the runtime's GTE state.

Jump tables are detected by the canonical MIPS pattern
(`LUI/ADDIU/ADDU/LW/JR`) and emitted as C++ `switch` statements. Functions
flagged `hle_functions` in the config are replaced by a call to the
corresponding `ps1::psyq::hle_*` runtime stub.

Set `recompiler.translation_units` to a value greater than one to emit a small
dispatch/support source, a shared declaration header and byte-balanced function
shards. Sharding bounds compiler memory, permits useful optimization in normal
iteration builds and lets independent sources compile in parallel. A value of
one retains the compatible monolithic output. Unchanged generated files are
not rewritten, so regenerating identical input does not invalidate objects.

All `recompiled_out*` game outputs are **gitignored** -- they are regenerated
per game from the TOML config. CI builds use `recompiled_out_stub.cpp` (a no-op
placeholder) so the build is green without any ROM present.

### Phase 3 -- `ps1Runtime/`

**Input:** the generated `recompiled_out*` sources and a game TOML.
**Output:** the running native binary.

The runtime is the platform layer: it owns the simulated PS1 hardware, loads
the game's data from the disc, hosts the SDL2/Vulkan window, and dispatches
into recompiled functions. Subsystems live in `ps1Runtime/src/<unit>/`:

| Subsystem | Source dir | What it owns |
|-----------|------------|--------------|
| BIOS HLE | `bios/` | Syscall tables A/B/C, heap, events, file I/O |
| CD-ROM | `cdrom/` | ISO 9660 / CUE+BIN reader, drive state machine |
| GPU | `gpu/` | GP0/GP1 command stream, 1MB native VRAM, Vulkan renderer |
| SPU | `spu/` | 24-voice ADPCM audio (SDL2 callback) |
| DMA | `dma/` | 7-channel DMA controllers |
| GTE | `gte.cpp` | Geometry transform coprocessor |
| MDEC | `mdec/` | Motion picture decoder (stubbed, FMV skipped) |
| Timers | `timers/` | Root counters 0/1/2/3 |
| Input | `input/` | Controller + memory card SIO |
| PsyQ HLE | `psyq/` | Native implementations of detected PsyQ funcs |

`main_host.cpp` wires it all together: it loads the disc, hands control to
the recompiled entry point, and runs a VBlank timer thread that increments
`psyq_state().vsyncCounter` at 60Hz.

---

## Architectural decisions

These are intentional and should not be revisited without strong evidence.

### Static, not JIT
All translation happens at compile time. There is no interpreter loop and no
runtime code generation. The trade-off is a per-game recompile step in
exchange for full native speed and trivial debugger support.

### HLE for both BIOS and PsyQ
The runtime contains zero original Sony code. The PS1 BIOS A/B/C syscall
tables are implemented as C++ stubs in `bios/syscall_a.cpp`, `syscall_b.cpp`
and `syscall_c.cpp`. PsyQ middleware (VSync, DrawSync, DrawOTag,
PutDispEnv, PutDrawEnv, the libcd state machine, etc.) is implemented
natively in `psyq/`. Detection of PsyQ functions is done by hash at analysis
time, so the same runtime stubs work across games that happen to link the
same SDK version.

### Three binaries, not one
Splitting Analyzer / Recompiler / Runtime into three executables makes the
pipeline incrementally debuggable. You can rerun the analyzer without
recompiling, dump intermediate TOML, swap in a hand-edited config, etc.
N64Recomp ships a single CLI; PS2Recomp ships three. We follow PS2Recomp.

### Runtime in-tree, not external
The runtime ships in the same repository as the recompiler. Generated game
code links directly against `ps1Runtime_lib`. This keeps the contributor
loop short: one `cmake --build build` rebuilds the whole pipeline.
N64Recomp publishes its runtime in a separate repo (`N64ModernRuntime`); we
deliberately avoid that split for a one-author project.

### Single `recomp_context` passed by pointer
All recompiled functions receive `(uint8_t* rdram, recomp_context* ctx)` --
explicit, not global. This pattern is N64Recomp's and is preserved so that a
future multi-threaded recompilation harness has a clean handoff. Do not move
the context to a thread-local global without rethinking that.

### TOML configs, not CLI flags
Each game has a `configs/<game>.toml` that lists every recompiled function,
every HLE override, every overlay, and the disc path. This file is the only
input the recompiler needs. CLI flags are limited to overrides and
debugging tools.

### Bounded translation shards
`ps1Recomp` supports either the historical single `recompiled_out.cpp` or a
configurable number of independently compiled sources. Shards are packed by
generated byte size so duplicated hand-written control-flow islands cannot
concentrate several multi-megabyte functions in one compiler process. The
dispatch table remains in `recompiled_out.cpp`; function bodies live in
`recompiled_out.partNNN.cpp` and share `recompiled_out.generated.h`.

The shard count is a build property, not game behavior. It does not change
addresses, emitted operations, dispatch ownership or the runtime ABI.

### Why `ps1Runtime/src/gpu/gpu.cpp` is one file
The GPU class owns all GPU state (VRAM, draw-area registers, texture window,
dither and blend state) and every primitive emitter touches that state
directly. Splitting the file would just spread the same class across more
translation units without reducing coupling. The 1.6k-line file is
intentional. The same is **not** true of `bios.cpp`: there the three
syscall tables are independent and were split into `syscall_a/b/c.cpp` in
Phase 5.

---

## Data flow at a glance

```
                                +------------------------+
disc (CUE/BIN) --ISO9660------->|  cdrom::VirtualFs      |
                                +-----------+------------+
                                            | extract boot.exe
                                            v
ELFIO ----parse---->   .text / .data / .bss in rdram[]
                                            |
                                            v
                          +-------------------------+
                          |  recomp_dispatch(addr)  |  <-- runtime entry
                          +------------+------------+
                                       | jumps into
                                       v
              func_801XXXXX(rdram, ctx)  <-- generated C++
                                       |
              +------------------------+------------------------+
              |                        |                        |
              v                        v                        v
       BIOS::executeA0()       ps1::psyq::hle_VSync()     GPU::writeGP0(...)
       (syscall_a.cpp)         (psyq_libgpu.cpp)          (gpu/gpu.cpp)
              |                        |                        |
              +----------- mutate recomp_context + hardware -----+
```

The scheduler and audio callback run outside the game thread, but neither may
mutate `recomp_context`. VBlank state is published through `psyq_state()`
atomics. CD-ROM timing crosses threads only as atomic cycle credit; controller
state, disc reads, its single sector buffer, IRQ delivery and libcd callbacks
are game-thread-owned.

### Save-state timeline boundary

A checkpoint is more than guest RAM. `SaveStateCoordinator` parks the game
thread at a resumable VSync continuation while the host serializes hardware
and HLE state. Format version 4 also captures the BIOS event registry,
pending callbacks, deferred CD IRQs, custom-exception buffer, and the last
DMA/VBlank generations already delivered to PsyQ. Loading then unwinds the
abandoned native stack and dispatches from the saved continuation.

Older checkpoint formats do not contain those BIOS fields. Their compatibility
path discards callbacks belonging to the abandoned future and rebases the host
delivery epochs to the restored DMA and VBlank counters. Both delivery loops
also reject implausibly large unsigned deltas, so a future counter rewind can
never expand into billions of queued callbacks.

### Single-owner asynchronous CD streaming

Games such as Crash Bandicoot issue long `CdlReadN` transfers and poll
`CdReadSync(1)` while simulation continues. The host scheduler therefore calls
only `CdromController::accrueClockCycles()`. At cooperative guest yield points,
`serviceClock()` exchanges that credit and advances the drive on the game
thread.

Each matured sector follows one ordered transaction:

```
clock credit -> read LBA -> publish single buffer -> INT1 -> copy/DMA -> ACK
                                                               |
                                                               +-> next LBA
```

The next sector cannot reuse the buffer until ACK releases the latch. If a
no-callback HLE transfer completes copy+ACK inline, the service may consume up
to eight already-matured sectors iteratively; callback and register-DMA paths
stop after one pending INT1. This preserves 2x throughput (three sectors per
PAL VBlank) without recursive callbacks or concurrent controller mutation.

### PGXP provenance and conservative true depth

High-precision geometry is a non-architectural side channel. The integer GTE
still produces the exact PS1 registers and packed SXY words; in parallel, RTPS
keeps its pre-shift 4.12 transform and true floating-point projection. That
metadata follows the word through the paths Crash actually uses:

```
GTE SXY FIFO -> COP2/GPR shadow -> RAM shadow -> DrawOTag/linked-list -> GPU
```

Every shadow entry includes the corresponding packed word. A consumer accepts
precision only if address, value and validity still match, otherwise the
triangle falls back atomically to the native integer rasterizer. CPU writes,
device-to-RAM DMA, OTC and save-state loads invalidate affected shadows so CD
streaming or recycled primitive buffers cannot inherit stale geometry.

The enhanced rasterizer requires valid XYZ on all three vertices. It uses
sub-pixel screen coordinates, perspective-correct texture interpolation and a
host-only depth surface. Depth applies only to opaque non-STP polygon pixels;
semi-transparent effects, sprites, UI, framebuffer feedback and incomplete
provenance preserve PS1 ordering-table/painter semantics. VRAM remains the
authoritative color/mask surface.

This is the compatibility layer for a later high-resolution GPU backend. The
current software raster still targets native 1024x512 VRAM; `res_scale` scales
presentation, not internal raster resolution.

---

## Known limitations

The project is honest about its scope. As of this writing:

- **One title fully validated**: Rayman (USA) ran end-to-end at ~59fps with
  correct framebuffer content through Phase 3. The supporting imperative
  patch script (`tools/patch_rayman.py`) was retired during open-source
  preparation; the regen-fresh path now relies entirely on the PsyQ HLE
  coverage and has not been re-validated headed since.
- **Crash Bandicoot 1**: experimental. Boots through PsyQ init, then stalls
  in a game-side hash table walk before the title screen. A signature
  collision in short libcd wrappers is the next known blocker.
- **MDEC / FMV**: the decoder stubs out -- full-motion video is skipped, not
  decoded.
- **SPU accuracy**: ADPCM envelope, reverb and pitch modulation are
  approximations. Most music plays; precise effects do not.
- **No save/load state UI**: the underlying machinery exists in
  `save_state.cpp` but is not surfaced in `ps1Interface`.
- **Single-thread game loop**: recompiled code, CD-ROM controller state and
  guest IRQ/callback delivery run on one owner thread. The scheduler and audio
  callback remain separate; they communicate only through bounded/atomic
  handoffs.

For active follow-up items, see `PLANNING.md` in the working tree.

---

## Where each piece of code lives

```
ps1-recomp/
+-- ps1Analyzer/        ELF + disc parsing, function detection, PsyQ matching
|   +-- data/           psyq_signatures.toml, psyq_metadata.toml
|   +-- include/
|   +-- src/
+-- ps1Recomp/          MIPS -> C++ translation
|   +-- include/
|   +-- src/            mips_decoder, instruction_emitter, gte_emitter,
|                       overlay_handler, hle_emitter, main
+-- ps1Runtime/         PS1 hardware simulation + SDL2/Vulkan host
|   +-- include/runtime/   public headers
|   +-- src/
|       +-- bios/       syscall_a/b/c, heap, event_system, file_io, bios
|       +-- cdrom/      cue_parser, bin_reader, iso9660, virtual_fs,
|                       cdrom_controller, exe_extractor
|       +-- gpu/        gpu (state machine + primitives), renderer_vulkan
|       +-- spu/        spu (24-voice ADPCM)
|       +-- dma/        dma (7 channels)
|       +-- mdec/       mdec (currently stubbed)
|       +-- timers/     timers (root counters)
|       +-- input/      input (controller + SIO)
|       +-- psyq/       psyq_state, psyq_registry, hle_VSync etc.
|       +-- interrupts/ interrupt controller
|       +-- gte.cpp     geometry transform engine
|       +-- main_host.cpp   game loop, VBlank thread, SDL2 host
|       +-- recompiled_out{,_stub}.cpp   generated game code
+-- ps1Interface/       optional ImGui studio (off by default)
+-- ps1Test/            557 Google Test cases
|   +-- analyzer/
|   +-- recompiler/
|   +-- runtime/
|   +-- pipeline/
+-- third_party/        ELFIO, toml11, fmt, googletest (git submodules)
+-- tools/              PsyQ signature extraction, diagnostics, MCP server
+-- .github/workflows/  CI: stub-mode build + ctest on Ubuntu
+-- configs/            per-game TOMLs (gitignored except example_game.toml)
```
