# Native MGS savestates

F5 saves the current slot, F9 loads it, and F6/F7 change slots. Default files
are `savestates/quick_slot_N.ps1s`; `paths.savestate_dir` overrides the directory.
The window title reports successful saves/loads and failures. Savestates are
independent of the game's memory-card save function.

## Native continuation reconstruction

Normal execution retains stackful `ucontext` fibers for BIOS threads. The file
contains no fiber stack bytes or host code pointers. Each suspended thread has
a guest TCB and a native resume PC. ChangeTh records its architectural return
PC; generated polling boundaries record the instruction before the branch and
its delay slot. Call-return PCs and reachable labels are emitted as native
continuations. This ordering prevents replaying an already executed delay-slot
side effect or depending on a temporary C++ branch variable.

A save request waits until all suspended threads have resumable boundaries.
The host parks the game thread and locks audio while saving/restoring hardware,
RAM, registers, device queues and BIOS state. Version 6 includes native thread
metadata and pending memory-card completion events. Loading unwinds onto the
host thread, discards the old fibers, and builds fresh fibers from the saved
TCBs/PCs. Restored guest returns follow the compiled continuation table.

The v6 payload has an FNV-1a checksum validated before live state changes.
Short headers, damaged payloads and truncated files are rejected. Writes use a
temporary file and rename; an unsuccessful write does not replace an existing
state. The header distinguishes v6 from legacy versions; versions 1–5 remain
readable by the generic loader. Earlier MGS builds could not produce native
thread savestates. Do not assume states are compatible with a different game
revision or with future incompatible runtime formats.

Machine-readable results: [mgs-savestate-evidence.json](mgs-savestate-evidence.json).

## Verified scope

USA v1.0, SLUS-00594, dock gameplay:

- Save around VBlank 12500, move Snake, load around VBlank 14500.
- Three consecutive loads of the same file in one process, followed by movement.
- Load into a fresh process, then move Snake and open the Codec.
- No interpreter counters in these diagnostic runs.
- Tests rebuild a suspended thread without its previous C++ stack, check
  register preservation, and reject damaged/truncated files without changing
  the live CPU/RAM. The code-generation regression checks that the polling
  checkpoint precedes the delay slot and branch temporaries.

The accelerated diagnostic runs use a 1 ms VBlank interval, so they are not
frame-rate measurements. The interactive validation uses Vulkan and normal
NTSC pacing. Later stages and arbitrary checkpoints throughout the full game
are not yet certified. Memory-card file persistence and in-game save/load are
separate features.

## Reproduction

From the repository root after building the native USA target:

```bash
SDL_AUDIODRIVER=dummy PS1_DIAGNOSTIC_HEADLESS=1 \
PS1_VBLANK_PERIOD_US=1000 PS1_EXIT_VBLANK=20000 \
PS1_AUTOSAVE_VBLANK=12500 PS1_AUTOLOAD_VBLANK=14500 \
PS1_AUTOLOAD_COUNT=3 PS1_EXIT_AFTER_LOAD_VBLANKS=3000 \
PS1_DIAGNOSTIC_SAVESTATE_DIR=data/native/states-test/states \
PS1_INPUT_SCRIPT=config/mgs-usa-gameplay.input \
PS1_METRICS=data/native/states-test/metrics.json ./scripts/run_native.sh
```

Create `data/native/states-test` first. To verify a fresh process, repeat with
`PS1_AUTOSAVE_VBLANK` omitted, `PS1_AUTOLOAD_VBLANK=1000`,
`PS1_AUTOLOAD_COUNT=1`, and `PS1_EXIT_AFTER_LOAD_VBLANKS=5500`, using the same
state directory. Metrics should report one save and three loads for the first
run, and one load for the second, with no `mips.*` interpreter counters.

States and all game-data artifacts are excluded from Git.
