# PS1Recomp

**Static recompiler for PlayStation 1 games -- translates MIPS R3000A machine code to native C++ for PC.**

## Overview

PS1Recomp converts PS1 game binaries into native PC executables through static recompilation. Instead of interpreting each MIPS instruction at runtime like a traditional emulator, it translates the entire game binary to C++ at compile time and links it against a hardware simulation runtime. The result is a standalone native executable that runs at full CPU speed.

The base runtime remains a title-neutral PS1 recompilation platform. The
current development branch deliberately puts universal depth inference aside
and treats Crash Bandicoot 3 PAL (SCES-01420) as a first-class, game-specific
target. Every exception is gated by the exact `crash3_sces_01420` profile and
lives below `runtime/games/crash3`; a configuration which does not select that
profile receives none of its projection, culling, depth or transparency
policy. This keeps the accelerated title work usable without silently defining
future games by Crash-specific observations.

For this target, image completeness and native cadence come first, followed by
Vulkan 4x internal resolution, complete PGXP projection coverage, true-color
shading, 16:9 projection/culling and presentation-only depth reconstruction.
The implementation boundary, measured results and known gaps are in
[CRASH3_GAME_PROFILE.md](CRASH3_GAME_PROFILE.md). The artefact families in
[Pikuma's PS1 graphics overview](https://pikuma.com/blog/how-to-make-ps1-graphics)
remain the visual checklist, extended here with exact PS1 blending, masks,
framebuffer feedback, scanout/overscan and streaming invariants.

The architecture and its proof boundaries are documented in
[MODERN_RENDERER.md](MODERN_RENDERER.md), donor-derived native behavior in
[DONOR_RENDERER_CONTRACT.md](DONOR_RENDERER_CONTRACT.md), and reproducible run
artifacts in [RENDERER_EVIDENCE.md](RENDERER_EVIDENCE.md). Enhancements inferred
from incomplete evidence fail closed to the complete native presentation;
renderer sidecars never feed reconstructed coordinates, depth or color back to
the guest.

CrashReplay schema 8 can carry independently certified GTE transform, direct
or computed-model, submitted-GP0 and DMA/OT traversal provenance into the
Vulkan bake. The full MIPS value-flow DAG remains a bounded audit mode; a
separate opt-in compact construction tracker computes the same versioned
Merkle pair while retaining only live register/GTE/RAM fingerprints. Runs
which enable neither mode leave computed-model lineage explicitly unknown.
The bake assigns a deterministic owner token to every triangle for
OT-versus-depth auditing, and conflict traces can carry the corresponding raw
categorical owner tuple into an offline discovery/holdout audit. Recurrent
tuples remain triangle candidates, not object families. Exact shared-projection incidence can prove
partial topology across captures, but disconnected conflict subsets are not
assumed to be separate objects. Equivalent light/heavy captures can also
cross-validate the frame-local equality partition of complete numeric GTE
affines while discarding the matrices themselves as persistent identities;
split/merge is rejected. The platform deliberately does not infer a family
count or scale factor from any component count. “Few families” and simple
decimal factors remain uncertain, non-binding priors until independent
evidence closes the complete object and gauge proofs.

```
PS1 ISO/BIN
    |
    v
ps1Analyzer  ->  game_config.toml   (function boundaries, PsyQ signatures)
    |
    v
ps1Recomp    ->  recompiled_out*.cpp (MIPS -> C++, optionally sharded)
    |
    v
ps1Runtime   ->  Native PC executable (SDL2 + Vulkan)
```

## Architecture

| Component | Directory | Description |
|-----------|-----------|-------------|
| **ps1Analyzer** | `ps1Analyzer/` | Parses PS1 ELF/BIN, detects function boundaries, identifies PsyQ SDK signatures, generates TOML config |
| **ps1Recomp** | `ps1Recomp/` | Decodes MIPS R3000A instructions and emits literal C++ translation (1:1 per instruction), handles GTE coprocessor and jump tables |
| **ps1Runtime** | `ps1Runtime/` | Simulates PS1 hardware: BIOS HLE, GPU (Vulkan), SPU (SDL2 audio), CD-ROM, DMA, GTE, MDEC, Timers, Input |
| **ps1Interface** | `ps1Interface/` | GUI studio for loading ELFs, exploring functions, editing configs, and previewing recompiled C++ (ImGui + SDL2) |
| **ps1Test** | `ps1Test/` | Unit and integration tests covering all runtime subsystems |

## Project Structure

```
ps1-recomp/
+-- ps1Analyzer/       ELF parser, function finder, PsyQ signature DB, disc reader
+-- ps1Recomp/         MIPS decoder, C++ emitter, GTE emitter, overlay handler
+-- ps1Runtime/        Full PS1 hardware simulation + SDL2/Vulkan host
|   +-- src/
|       +-- bios/      BIOS HLE (syscall tables A/B/C, heap, events, file I/O)
|       +-- cdrom/     CD-ROM emulation (ISO 9660, CUE/BIN, virtual FS)
|       +-- gpu/       GPU state machine + Vulkan renderer
|       +-- spu/       Sound Processing Unit (24-voice ADPCM)
|       +-- mdec/      Motion Picture Decoder (FMV video)
|       +-- dma/       DMA controllers (Ch0-Ch6)
|       +-- timers/    Hardware timers
|       +-- psyq/      PsyQ middleware HLE (VSync, DrawSync, DrawOTag, etc.)
|       +-- main_host.cpp  SDL2 host: game loop, input, VBlank thread
+-- ps1Interface/      GUI studio (ImGui docking): ELF explorer, function inspector,
|   |                  C++ preview, config editor, Ghidra CSV import
|   +-- include/
|   |   +-- StudioState.hpp    Async state management (analysis, config, exports)
|   |   +-- GUI.hpp            4 dockable panels + menu bar
|   |   +-- ui/StyleManager.hpp  Dark/Light/Custom themes and font scaling
|   +-- src/
|       +-- main.cpp           SDL2 + OpenGL3 entry point with ImGui docking
|       +-- StudioState.cpp    ps1Analyzer backend integration
|       +-- GUI.cpp            Panel rendering and keyboard shortcuts
|       +-- ui/StyleManager.cpp
+-- ps1Test/           Unit and integration tests (GTest) across all subsystems
+-- third_party/       External dependencies: ELFIO, toml11, fmt, googletest
+-- tools/
|   +-- extract_psyq_signatures.py   Build the PsyQ signature DB from PsyQ SDK .LIB files
|   +-- psyq_lib_extract.py          Split SN Systems .LIB archives into per-function .OBJ
|   +-- ps1_mcp_server.py            MCP server for AI-assisted debugging
|   +-- run_and_report.py            Automated game diagnostics (VRAM, frame rate, logs)
|   +-- validate_game.py             Pixel-content validation against a frame budget
|   +-- demo_build.sh                Quick-start demo script
|   +-- ghidra/                      Ghidra integration scripts
|   |   +-- ExportPS1Functions.py    Export function list from Ghidra to CSV
|   |   +-- ExportPS1Functions.java  Same, Java version (headless mode)
|   |   +-- start_ghidra_mcp.sh      Launch Ghidra with GhidraMCP on port 8080
|   +-- pcsx_redux_mcp/              PCSX-Redux emulator integration (MCP server)
+-- .github/workflows/ci.yml   CI/CD: build + test on every push
+-- CMakeLists.txt             C++20 / CMake 3.20+ build system
+-- LICENSE                    GPLv3
```

## Building

### Prerequisites

```bash
# Ubuntu / Debian
sudo apt install cmake build-essential libsdl2-dev libvulkan-dev glslc

# macOS
brew install cmake sdl2 vulkan-headers shaderc molten-vk
```

### Clone and Build

```bash
git clone --recurse-submodules https://github.com/Dellareti/ps1-recomp.git
cd ps1-recomp
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

### Build with GUI Studio (optional)

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DPS1RECOMP_BUILD_INTERFACE=ON
cmake --build build -j$(nproc)
./build/ps1Interface/ps1Interface
```

### Run Tests

```bash
ctest --test-dir build --output-on-failure -j$(nproc)
```

## Running a Game (Rayman example)

> **Note:** Game disc images (ROMs/ISOs) are not included in this repository for legal
> reasons. You need your own legal copy of the game. The disc path is configured per-game
> and can point to **any folder on your system** -- there is no required directory name.

1. Place your disc image anywhere and note the path:
   ```
   # Example paths -- use whatever works for you:
   /home/user/games/Rayman (USA).cue
   ~/roms/Rayman (USA).cue
   ./my_games/Rayman (USA).cue
   ```

2. Analyze the binary and generate config:
   ```bash
   ./build/ps1Analyzer/ps1Analyzer "/path/to/Rayman (USA) (Track 01).bin.boot.exe" rayman_config.toml
   ```

3. Edit `rayman_config.toml` to set your disc path:
   ```toml
   [disc]
   cue_path = "/path/to/Rayman (USA).cue"
   ```

4. Recompile MIPS -> C++:
   ```bash
   ./build/ps1Recomp/ps1Recomp rayman_config.toml ps1Runtime/src/recompiled_out.cpp
   ```

5. Build and run:
   ```bash
   cmake --build build -j$(nproc)
   ./build/ps1Runtime/ps1Runtime --config rayman_config.toml
   ```

   During Vulkan execution, `F8` cycles the normal image and three live RGB
   depth views: post-correction presented-surface R32, independent pre-correction
   strict-W depth, and an aligned `pre | post` split. Near surfaces are red,
   increasing depth crosses yellow/green/cyan to far blue, and invalid depth
   is black. The views are presentation-only and are available on
   precision-atlas RGB555 frames.

### Runtime controls and checkpoints

Keyboard bindings come from `[input]` in the game TOML. The default layout is
arrow keys for the D-pad; `Z/X/A/S` for Cross/Circle/Square/Triangle;
`Q/W/E/R` for L1/R1/L2/R2; Return for Start; and Backspace or right Shift for
Select (`right_shift` is accepted as a TOML-safe SDL key name). SDL
GameController devices use the standard platform mapping,
including both sticks, D-pad, shoulders and analog triggers. Set
`input.analog = true` to expose an analog PS1 pad to the game.
Host button edges are latched at the platform VBlank boundary: even when SDL
delivers a short press and its release in the same host-loop batch, the guest
observes the press for at least one controller sample. The same sampled state
feeds BIOS buffers, PsyQ `PadInitDirect`/`PadRead`, and direct SIO transfers.

Runtime hotkeys are independent from guest input:

- `F5`: save the current checkpoint slot;
- `F9`: load the current checkpoint slot;
- `F6` / `F7`: select the previous/next slot (0 through 9);
- `F8`: cycle color, RGB depth `post`, RGB depth `pre`, and `pre | post`;
- `Escape`: quit.

Checkpoint files are written below `paths.savestate_dir`. Save and load
rendezvous at the end of a guest VSync, after callbacks have been delivered.
The game thread is parked while the host captures CPU, RAM, scratchpad, native
VRAM/GPU registers, DMA, IRQs, timers, the streaming CD state (including LBA,
FIFO and pending sector), MDEC command/FIFO state, memory cards, SPU voices/XA
queues, the complete controller/SIO transaction state, `PadInitDirect` buffer
registration, PsyQ state, and the BIOS HLE event timeline (registered events,
pending callbacks, CD IRQ queue, custom-exception buffer, and DMA/VBlank
delivery epochs). Input, memory cards and checkpoints are PS1
platform services; game configurations only identify SDK entry addresses.
Version 4 checkpoint files contain that BIOS timeline. Versions 1 through 3
remain loadable: their unavailable host queues are discarded and delivery
epochs are rebased to the restored hardware generations, preventing a rewind
from being misread as a multi-billion-event unsigned wrap.
Loading unwinds the old native recompilation stack and
starts a fresh dispatch at the saved continuation PC; it never resumes stale
host call frames over restored RAM. The recompiler registers every MIPS call's
`PC+8` as a native continuation. A load jumps past the already-executed
prologue, then chains architectural `$ra` returns until a live native call
stack has been rebuilt. This avoids both replaying guest side effects and
leaving normal execution trapped in the compatibility interpreter.
The Vulkan 4x atlas is deliberately reconstructed from restored authoritative
native VRAM and subsequent replay packets rather than serialized as guest
state. A successful load first waits for submitted Vulkan work, discards the
abandoned replay prefix, and rebases frame ordering before sealing the restored
VRAM snapshot. This also preserves an active F8 depth-view request across F9
instead of leaving it stranded behind the pre-load frame timeline.

The current renderer/Crash 3 handoff, reproducible evidence hashes, validation
commands and next proof gates are collected in
[RESUME_CHECKPOINT.md](RESUME_CHECKPOINT.md). The depth-view semantics and
their diagnostic cost are detailed in
[VULKAN_REPLAY_DIAGNOSTICS.md](VULKAN_REPLAY_DIAGNOSTICS.md).

For repeatable checkpoint diagnostics, `PS1_AUTOSAVE_VBLANK` and
`PS1_AUTOLOAD_VBLANK` request slot 0 at the selected guest VBlank; combine
them with `PS1_EXIT_VBLANK` and `PS1_METRICS` for bounded automated runs.
`PS1_DIAGNOSTIC_SAVESTATE_DIR` redirects that slot to a disposable directory,
so an automated capture cannot overwrite an interactive checkpoint.
`PS1_EXIT_AFTER_AUTOSAVE=1` exits only after that checkpoint has reached a
safe guest boundary and was written successfully; use `PS1_EXIT_VBLANK` only
as an outer safety ceiling.
`PS1_TRACE_ARM_AFTER_LOAD=1` keeps opt-in GTE, GP0 and MIPS value-flow
producers dormant until a successful timeline load, avoiding both boot cost
and an assumed saved VBlank number.
`PS1_PRESENT_VSYNC=0` removes only host Vulkan swap pacing without editing the
game configuration; the guest scheduler retains its configured PAL/NTSC
cadence. `PS1_VBLANK_PERIOD_US` deliberately changes that guest cadence and is
therefore reserved for scheduler stress tests, never fidelity or donor
captures.
`PS1_DIAGNOSTIC_IGNORE_INPUT=1` ignores keyboard and controller events during
such a bounded run (window close still works), preventing an accidental F9 or
guest button from contaminating a fresh-boot capture.

For bounded OT/depth diagnosis, combine
`PS1_VULKAN_REPLAY_READBACK=all` with
`PS1_VULKAN_VISIBILITY_AUDIT=1` and a small inclusive frame range. The
diagnostic creates independent OT-owner and strict-W-owner passes only for
selected M1 latches. Its default region follows submitted geometry rather than
the displayed page, which is required for double-buffered games. Full usage,
memory cost and fail-closed interpretation are documented in
[VISIBILITY_AUDIT.md](VISIBILITY_AUDIT.md) and
[VULKAN_REPLAY_DIAGNOSTICS.md](VULKAN_REPLAY_DIAGNOSTICS.md).
`PS1_VULKAN_VISIBILITY_CANDIDATE_IDENTITY=1` optionally adds a third independent
4x owner/depth/evidence pass as a calibration control. It must reproduce strict
and does not alter presentation or select a gauge.
`PS1_VULKAN_VISIBILITY_CANDIDATE_MANIFEST=/path/to/file` selects the mutually
exclusive reloadable hypothesis mode. Its exact construction-lineage dispatch
never falls back to identity: an invalid file or unknown owner invalidates that
candidate coverage, and the result still cannot alter normal presentation.
`tools/visibility_candidate_trace.py` joins every rejected OT/strict/lifted
winner triple to that runtime key and can independently relabel it with stable
shared-projection topology. Those partial components expose distinctions hidden
by a common GTE affine without being counted as objects or families.
For topology work,
`PS1_VULKAN_VISIBILITY_TRACE_CATALOG=1` additionally traces every eligible 3D
owner in those same latches with an audited completeness record; it remains
opt-in and does not affect presentation.

## How It Works

### Phase 1 -- Analysis (`ps1Analyzer`)
- Reads the disc image (ISO 9660 / CUE+BIN) and extracts the PS1 executable
- Parses ELF sections: `.text` (MIPS code), `.data`, `.bss`
- Detects function boundaries via prologue patterns and call graph analysis
- Identifies PsyQ middleware calls via signature matching
- Outputs a TOML config with all function addresses and BSS layout

### Phase 2 -- Recompilation (`ps1Recomp`)
- Decodes every MIPS R3000A instruction (32-bit fixed-width)
- Emits equivalent C++ for each instruction using a `recomp_context` struct as register file
- Detects jump tables (switch statements) via LUI+ADDIU+ADDU+LW+JR pattern
- Handles GTE coprocessor instructions separately via `gte_emitter`
- Produces one C++ function per MIPS function; large games can use configurable,
  byte-balanced `recompiled_out.partNNN.cpp` shards for parallel optimized builds

### Phase 3 -- Runtime (`ps1Runtime`)
- **BIOS HLE**: implements PS1 syscall tables A/B/C without a BIOS ROM
- **GPU**: interprets GP0/GP1 commands, keeps authoritative RGB555 VRAM at native 1x, and presents/upscales through Vulkan; high-precision packet replay is tracked in [MODERN_RENDERER.md](MODERN_RENDERER.md), with emulator-derived behavior in [DONOR_RENDERER_CONTRACT.md](DONOR_RENDERER_CONTRACT.md)
- **CD-ROM**: emulates drive state machine, reads sectors from CUE/BIN files via ISO 9660
- **SPU**: 24-voice ADPCM audio, mixed via SDL2 audio callback
- **DMA**: transfers between RAM, GPU, SPU, MDEC, and CD-ROM
- **GTE**: geometry transform engine for 3D vertex processing
- **PsyQ HLE**: replaces PsyQ middleware functions (VSync, DrawSync, DrawOTag, SetDefDispEnv, PutDispEnv, PutDrawEnv) with native implementations

### GUI Studio (`ps1Interface`)
- **Explorer panel**: lists all functions with color-coded strategy (PsyQ=yellow, Stub=red, Skip=gray, ForceRecompile=green), filterable by name
- **Inspector panel**: shows function details (address, size, disassembly summary) and lets you change the recompilation strategy per function
- **Workspace panel**: tabbed view with C++ preview, hex dump, config.toml editor, and Ghidra CSV importer
- **Logs panel**: real-time analysis progress bar and error markers
- Keyboard shortcuts: `F5` = Analyze, `Ctrl+O` = Open ELF, `Ctrl+S` = Save config

## Key Design Decisions

**Static vs. dynamic recompilation**: All translation happens at compile time -- no JIT, no interpreter loop. Game code runs as regular C++ function calls at native CPU speed.

**HLE over LLE**: BIOS and PsyQ middleware are implemented as High-Level Emulation (HLE) stubs rather than running the original ROM code. This avoids legal issues and simplifies the runtime.

**Hash-based PsyQ detection**: `ps1Analyzer` matches each function body against a SHA-256 database of PsyQ SDK functions extracted from 14 SDK versions (3463 signatures, 1943 unique after collision filtering). Detected functions are routed to native HLE implementations at runtime instead of being recompiled. This avoids per-game imperative patches and is the project's main divergence from prior PS1 recompilers.

**Double-buffered framebuffer**: PS1 games typically use double-buffering in VRAM (e.g. Buffer A at (0,0) rendered while Buffer B at (0,256) is displayed, swapping each VBlank at 60Hz). The runtime infers the active display window from the GPU command stream.

## Tooling

### GUI Studio
```bash
./build/ps1Interface/ps1Interface
```
Opens the visual studio for loading ELF files, exploring functions, editing game configs, and previewing the generated C++ output before running the full pipeline.

### Ghidra Integration
The project includes Ghidra scripts for static analysis validation:
- Install the `ghidra_psx_ldr` plugin (PSX-specific loader with GTE + PsyQ support)
- Run `tools/ghidra/ExportPS1Functions.py` inside Ghidra to export function boundaries to CSV
- Run `tools/ghidra/start_ghidra_mcp.sh` to start Ghidra with the MCP bridge on port 8080

### Automated Diagnostics
```bash
python3 tools/run_and_report.py --duration 15 --output /tmp/report.json
```
Runs the game for N seconds and outputs a JSON report with frame rate, VRAM pixel analysis, GPU command log, and CD-ROM callback counts.

## Dependencies

| Library | Purpose | How included |
|---------|---------|--------------|
| [ELFIO](https://github.com/serge1/ELFIO) | ELF binary parser | Git submodule |
| [toml11](https://github.com/ToruNiina/toml11) | TOML config parser | Git submodule |
| [fmt](https://github.com/fmtlib/fmt) | String formatting | Git submodule |
| [GoogleTest](https://github.com/google/googletest) | Unit testing framework | Git submodule |
| [Dear ImGui](https://github.com/ocornut/imgui) | GUI (docking branch) | FetchContent (ps1Interface only) |
| [ImGuiColorTextEdit](https://github.com/BalazsJako/ImGuiColorTextEdit) | Syntax-highlighted code editor | FetchContent (ps1Interface only) |
| [ImGuiFileDialog](https://github.com/aiekick/ImGuiFileDialog) | Native file open/save dialog | FetchContent (ps1Interface only) |
| SDL2 | Audio (SPU) + window + input | System package |
| Vulkan loader + headers | Runtime rendering backend | System package; pinned official header fallback when headers are absent |
| glslc | Vulkan shader compiler | System package |
| OpenGL 3.3 | Optional GUI studio backend | System package (`PS1RECOMP_BUILD_INTERFACE=ON` only) |

## Inspired By

- [N64Recomp](https://github.com/N64Recomp/N64Recomp) -- pioneered the static recompilation approach for retro consoles
- [PS2Recomp](https://github.com/ran-j/PS2Recomp) -- direct architectural reference (ps2Analyzer/ps2Recomp/ps2Runtime structure)
- [psxrecomp](https://1379.tech/i-built-a-ps1-static-recompiler-with-no-prior-experience-and-claude-code/) -- PS1 static recompiler reference implementation
- [Nocash PSX Specs](https://problemkaputt.de/psx-spx.htm) -- definitive PS1 hardware reference
- [ps1-bare-metal](https://github.com/spicyjpeg/ps1-bare-metal) -- PS1 hardware programming examples

## Status

The project is the subject of an ongoing undergraduate thesis. **It is not a finished
emulator** -- it is a recompiler whose architecture works end-to-end, plus a runtime that
covers enough of the PS1 hardware to boot real games. The current state, honestly:

- **Pipeline**: stable. Analyzer -> recompiler -> runtime -> native binary works for any PS1
  ELF/EXE. Stub-mode build (no ROM) is the CI baseline (557/557 tests green).
- **Rayman (USA)**: was the primary validation target through Phases 0-3. With prior
  imperative HLE patches it ran at ~59fps with correct VRAM content. Those patches were
  retired during open-source preparation; the regen-fresh path now relies entirely on the
  PsyQ hash-based HLE coverage and has a brightness gap (see `ISSUES.md` #2 -- not
  re-validated end-to-end since patch removal).
- **Crash Bandicoot 1 (USA)**: experimental but boots. Reaches the main game loop and
  runs the full PsyQ render pipeline (`DrawOTag` ~435 calls/sec, `MulMatrix` ~870/sec,
  `VSync` ~58 fps) with an env-gated workaround (`PS1_SKIP_31BF8=1`) that NOPs a GOOL
  state machine call which would otherwise hang in an unpopulated hash table walk.
  Permanent fix (porting the NS chunk subsystem) is the next roadmap item in
  `PLANNING.md`. The bypass keeps the engine running so we can study the next layer
  while the proper fix is in progress.
- **Other games**: not exercised yet. The architecture is meant to be game-agnostic; adding
  a new title is mostly a matter of providing a TOML config and running the analyzer.

For an honest, gap-by-gap account of what works and what does not, see `ARCHITECTURE.md`.
For specific game blockers and verification metrics, see `ISSUES.md`.

## License

GPLv3
