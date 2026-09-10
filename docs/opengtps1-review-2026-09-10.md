# OpenGTPS1 review — 2026-09-10

Inspected commit `d9e6af5dccf704383f28faf8d1b50633e2539046`
(2026-09-10, “Ship release notes as plain text”), cloned into
`.local/opengtps1`. Source review only: no GT2 build or gameplay test performed.
Project: https://github.com/GTTeancum/OpenGTPS1

## Most useful for our current MGS blockers

1. **Explicit pad-buffer registration.** `tools/prepare_reference.py` installs
   a pre-hook at GT2's `0x80087148` to `LibPad.PadInitDirect`; Arcade has its
   own address. `vendor/RecompOne/RecompOne.Runtime/Runtime.cs:218` calls
   `Sdk.LibPad.Refresh(Mem)`. `sdk/LibPad.cs` writes connection/type, active-low
   buttons and analog axes into the registered original-game buffers, with
   optional input traces. Relative to our earlier RecompOne snapshot, this
   fork adds analog identity, per-port rumble indices and state-change tracing.
   **MGS action:** identify and verify USA PadInitDirect and the buffers MTS
   consumes; trace host buttons → raw pad buffer → MTS input. We already have
   HLE pad-writing code, but the USA recipe does not map PadInitDirect.
   This is a strong lead, not yet proof of the precise MGS input failure.
   GT2's addresses must never be copied into the MGS recipe.

2. **Persistent memory cards plus asynchronous completion.**
   `Hardware/MemoryCard.cs` models 128 KiB cards, 128-byte frames, a 15-entry
   directory, allocation chains, checksums and file operations. Writes flush
   to a host file. `Bios/BiosB.cs` separates lower I/O completion
   (`0xF0000011`) from higher device completion (`0xF4000001`), queues card
   events and delivers them via `TickCards`. `Bios/BiosA.cs` flushes file writes.
   **MGS action:** implement durable card images and the missing directory/file
   operations, then validate original-game save → process exit → reload.
   Do not copy the direct whole-file overwrite policy blindly: our persistence
   should use a checked temporary write and replacement. This solves an
   in-game memory-card contract, not F5 native-stack snapshots.

3. **Tests exercise gameplay and persistence.** `tests/fixtures` includes
   `validate-test-save.input` and many menu/race scripts; `tools` contains
   intro, soak, loose-runtime and rendering checks. The useful lesson is to
   test actual game-state changes and a fresh-process reload rather than
   accepting a window, elapsed VBlanks or a logged save request as success.

## Other useful findings

- `docs/PORT_PLAN.md` records exact native continuation boundaries, game-owned
  callback addresses, overlay relocation and non-local `longjmp` unwinding.
  These reinforce our explicit aliases and RAM-identified overlay dispatch.
- CD callbacks own their sector FIFO. Advance the completed read position
  before invoking a callback that may issue a new seek/read. This is relevant
  to our native CD IRQ transaction and split-header/payload DMA work.
- The same document calls out fractional NTSC/XA pacing: approximating the
  display cadence as exactly 60 Hz can accumulate audio drift. Treat its
  `60000/1001` choice as a reference implementation to compare with SwanStation
  and our target timing, not an automatically verified MGS clock constant.
- `native/src/world_interpolation.cpp`, native scene/topology code and
  `docs/MODERN_RENDERER.md` are useful architectural references for original
  geometry, separate authored 2D composition and later presentation interpolation.
  GT2 course/vehicle formats and visibility assumptions are not MGS KMD/OAR;
  these are not drop-in asset loaders or a ready MGS Vulkan backend.

## What it does not provide

The vendored `BiosB.cs` still has `case 0x10: break` for ChangeTh.
Its OpenTh allocator is not a native MTS context-switch implementation.
It therefore supplies no replacement for our stackful scheduler and no
solution for serializing its suspended native stacks. Do not infer that
GT2 memory-card support means our F5 savestates can work.

## Provenance and reuse

The current README explicitly says no license has been granted for original
OpenGTPS1 code. The vendored RecompOne tree retains its MIT license, but fork
modifications need provenance checking before copying. No implementation was
imported into MGS in this review. Use this repository as a concrete source of
behavioral contracts, experiments and architecture; retain SwanStation as
our execution oracle.

Priority: pad delivery, durable original-game saves, later CD timing audits;
advanced scene presentation and interpolation remain the subsequent phase.
