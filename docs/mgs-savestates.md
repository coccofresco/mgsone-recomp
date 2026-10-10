# Native MGS savestates

F5 saves the current slot, F9 loads it, and F6/F7 change slots. Default files
are `savestates/quick_slot_N.ps1s`; `paths.savestate_dir` overrides the directory.
The window title reports successful saves/loads and failures. Savestates are
independent of the game's memory-card save function.

## Alpha durability and recovery (2026-10-07)

Player saves use a unique temporary in the target directory, sync its bytes,
copy and sync the previous file into `<slot>.ps1s.backups`, then atomically
rename and sync the parent directory. Three previous distinct versions are
retained after successful publication; identical saves do not rotate history.
Failed writes/syncs before publication leave the current file in place. An
interruption can leave an ignored `.pending-*` temporary or extra backups;
neither is treated as the current save. A successful rename followed by a
failed directory sync produces a persistent host-menu warning and retains
all backups: the published bytes cannot honestly be reported as rolled back.
This improves durability on Linux filesystems honoring fsync; it cannot
promise survival of faulty hardware or arbitrary power loss.

The host menu lists all ten slots with local modification date and file-format
version (currently v12), and exposes previous versions for explicit loading.
These are header/date previews; checksum and full structural validation still
happen on load. F9/player loads recover the newest readable backup if the
primary is missing or fails its integrity check, pause the session, and show a
persistent recovery notice. The damaged primary is left untouched until the
next save. All backups failing validation is a load failure, never a fresh
blank state. Unsupported future formats are neither overwritten nor silently
rolled back; a valid state requiring an unavailable CD also fails explicitly.
TAS/diagnostic loads remain strict and never recover to a different timeline.

Memory cards retain their pre-session disk image before the first changed
write, so a multi-part BIOS save cannot consume the entire backup history.
Loading a savestate does not write cards to disk; it re-arms protection for
the next card write, preserving newer on-disk progress even when the guest
has returned to an older timeline. See [card persistence](mgs-input-memory-card.md).
The launcher's ZIP backup includes committed histories, excludes symlinks and
unfinished transactions, and publishes a completed archive without replacing
an existing ZIP.

The state payload is still NSV12, without added host paths or fiber stacks.
The native update check writes with the previous `dev.3` executable, loads and
saves with the new executable, then loads that new file with `dev.3` in another
process. It verifies the old state remains in history and both disk cards are
unchanged. This certifies this build pair, not every future alpha migration.
Reproducible checks: `scripts/mgs_host_lifecycle_check.py`; exact results and
limits: [shutdown/save evidence](mgs-shutdown-saves-evidence.json).

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

The v6–v12 payload has an FNV-1a checksum validated before live state changes.
Since the 2026-10-06 structural validation fix, the complete payload is also
parsed into temporary storage before replacing CPU, RAM, VRAM, sound RAM or
devices. Exact file length and the existing input/BIOS snapshot predicates
are checked first; selecting the recorded medium is the last fallible step.
A checksum-valid file with an invalid XA field, missing bytes or unexpected
extra payload therefore leaves the running timeline and mounted disc intact.
That structural fix required no on-disk layout/version change. Regression tests now cover eight
structural mutations and disc preservation in both directions; live probes
pass the NSV10 movie replay and a copy of the user's NSV7 state.
Short headers, damaged payloads and truncated files are rejected. Writes use a
temporary file and rename; an unsuccessful write does not replace an existing
state. Version 7 adds the separate SPU IRQ callback and pending audio sample credit.
The loader migrates the known MGS v6 callback misregistration automatically;
see [stream clock](mgs-stream-clock.md).

Version 8 appends a 64-byte, NUL-terminated medium identity without moving the
CPU/RAM prefix (RAM still begins at byte 476). The identity combines the
normalized boot serial and a content fingerprint of the volume descriptor and
boot executable. Loading selects that medium from the configured disc set
before replacing guest state, then restores the saved CD buffers, IRQs and
read position. Missing or replaced media rejects the load before CPU/RAM or
controller changes. No host path is stored in the savestate.

Version 9 adds the uploaded 64-entry MDEC IDCT matrix before the medium
identity trailer. It preserves the unread pixel FIFO, pending input and DMA
state; the CPU/RAM prefix and final 64-byte medium identity remain unchanged.
The loader migrates old DMA interrupt registration/epochs instead of replaying
stale flags left by the earlier unconditional-callback implementation. This
cannot reconstruct video pixels or ring buffers already corrupted in an old
checkpoint. Start those cases before the affected film begins.

Version 10 uses the `NSVA` flag and appends 163 bytes between the v9 MDEC
matrix and final 64-byte medium identity. Field order: four signed 32-bit XA
predictor values, two 32-entry signed 16-bit interpolation rings, position and
six-step phase bytes, current file/channel bytes, three one-byte booleans
(current stream set, command mute, ADPCM mute), four active and four pending
CD volume bytes (LL/LR/RL/RR), then signed 16-bit SPU CD gains L/R. Existing
SPU PCM queues/read positions remain in their previous serialized section.
The loader validates interpolation indices and preserves the CPU/RAM prefix.

Version 11 (`NSVB`) appends four bytes after the v10 extension and before the
64-byte medium identity: three one-shot IRQ latch bytes (timer 0, 1, 2, each
0 or 1), then the timer-2 divide-by-eight remainder (0–7). The existing timer
register prefix retains the IRQ toggle level and reached-target/overflow
flags. Saving uses an observational snapshot: it must not acknowledge the
read-to-clear MODE flags. Loading restores internal timer state directly,
without MODE writes that would re-arm a consumed one-shot interrupt.
Invalid latch/remainder values are rejected before replacing the timeline.

Versions 1–11 remain readable. Versions 2–10 have no one-shot latch or
fractional clock history; migration starts those absent fields at zero and
sets IRQ inactive, preserving the old re-armed behavior. It cannot reconstruct
lost timer history. RAM still starts at byte 476, and the medium trailer remains
the final 64 bytes. Tests preserve v6/v8 migration and explicitly remove the
four-byte extension to check v10 compatibility. A 640-case snapshot matrix
checks all 16 IRQ combinations at 40 saved phases; a file round trip checks
pending fractional credit, consumed one-shot and repeated-toggle continuation.

Version 12 (`NSVC`) additionally retains BIOS HLE resource ownership and the
BIOS random generator. After the v11 timer bytes, if IOS4 declares a BIOS state,
it appends five u32 fields (user heap base/size/root and kernel heap next/end),
16 file descriptors, the directory iterator, and the u32 RNG seed. A descriptor
is kind u8, path length u32 plus bytes, cached data length u32 plus bytes,
then cursor/first-block/card-slot u32. Kind 0 is closed, 1 is CD, 2 is card;
descriptors 0–2 remain reserved. Directory layout: u32 count, repeated
(name length u32, name bytes, file size u32), u32 cursor. An otherwise empty
resource extension occupies 368 bytes. The final 64-byte medium identity and
RAM offset 476 are unchanged; there is no resource extension when IOS4 says
that no BIOS was saved.

Heap restoration only restores metadata, preserving the block list in guest
RAM. Card descriptors rebind to the current input controller's slot, keeping
host backing paths local. Restoring does not close/flush abandoned descriptors
or reopen cached CD files. No host pointers or native fiber stacks are saved.
Structural bounds and required card attachments are checked before RAM commit
or medium selection. Cached CD data has a combined 512 MiB serialization limit;
paths are limited to 4096 bytes, directory scans to 15 entries of up to 20 name
bytes. Read lengths must also fit the actual remaining file before allocation.

Older versions lack these resource fields. They reset heaps/descriptors/directory
iteration and start the BIOS RNG from seed 1 instead of inheriting the abandoned
host timeline. That cannot recover an open file, heap arena or RNG history never
recorded by the older build. The verified historical MGS continuations remain
covered separately. See [HLE state verification](mgs-hle-state-verification.md)
for the regression matrix, same-process and fresh-process runs, and limitations.

Earlier states have no recoverable XA predictor, interpolation history or CD
gain. Their decoder starts cleared and SPU CD gain defaults to unity until the
retail driver rewrites it. For exact audio continuity, make a new v10-or-later save;
loading an older checkpoint mid-film is not an audio-equivalence guarantee.

The paired USA `d18a/e80c` v10 test saves at movie frame 120 and resumes in a
fresh process through the original end procedure (story 267). Native decoding
totals 120 + 360 video frames and 150 + 451 XA sectors, equal to the uninterrupted
run (480 frames, 601 XA sectors, 1,413,552 stereo PCM frames at 44.1 kHz). Unit
checks additionally compare the next sector's PCM exactly after restoring
predictor/ring state, and verify file serialization of that state, the mixer
and pending PCM. Both donor legs complete. See [completion evidence](mgs-completion.md)
for artifacts and limits; this does not certify arbitrary saves in every FMV.


For legacy MGS states the USA `FS_DiskNum` variable selects the medium; the
script-facing `GM_Disk` can still refer to the previous disc during validation.
See [seamless disc change](mgs-disc-change.md) for configuration and regression
coverage. Versions 1–5 remain
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
