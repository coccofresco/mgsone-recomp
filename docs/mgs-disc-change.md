# Seamless USA disc transition

The USA runtime mounts Disc 2 automatically when the original `change.c`
actor requests it. It removes the insert-disc/Start screen, its logo and its
cosmetic fade delay. The game still performs its original disc validation,
rebuilds its filesystem tables, runs the original GCL continuation and loads
`s13a`. It does not modify story flags, inventory or player position.

## Disc set

`prepare_native.py` accepts `--disc2-cue` and otherwise uses the unique CUE in
`data/discs/usa/disc2`. The generated runtime configuration contains:

```toml
[disc]
exe_path = "/path/to/SLUS_005.94"
cue_path = "/path/to/Disc 1.cue"
additional_cues = ["/path/to/Disc 2.cue"]
```

The two USA boot serials are `SLUS_005.94` and `SLUS_007.76`. Registration
validates the additional images without changing the active drive. A switch
prepares the new reader and filesystem before replacing the mounted medium;
a missing/replaced image leaves the current medium and device buffers intact.
The active disc is shared by native sector reads and BIOS file access.

## Source-backed adapter

The source reference is FoxdieTeam's
[`source/onoda/change/change.c`](https://github.com/FoxdieTeam/mgs_reversing/blob/master/source/onoda/change/change.c).
The actual bindings use the local USA overlay, SHA-256
`d06543015baf02c8b4e6f30fe39e6bcd4786d7a0b6e6e2c80e2b482e1ead2844`,
and are scoped to `overlay_usa_change__*`, because other stages reuse the PCs.

| USA entry/data | Purpose |
| --- | --- |
| `800C699C` | Change actor Act; preserves the GCL callback and destructor |
| `800C63A0` | Original disc state machine |
| Work `+6B0` | Disc-change phase |
| Work `+6AC`, `+6A4` | Native success flag and cosmetic fade age |
| `800AF368` | Original zero-based `FS_DiskNum` |
| `800B7518` | Script-facing `GM_Disk`; never written by the adapter |
| `800C7404` | Logo callback with instructions before its stack prologue |

States 0/1 still set the CD mode and stop the drive. The adapter replaces only
states 2–4 (lid open/close and Start), selecting the validated second image and
resuming at state 5. States 5–9 execute through the static recompilation. The
native filesystem, rather than the adapter, identifies Disc 2 and changes its
file locations. An unsuccessful original validation fails explicitly.

The callback at `800C7404` was absent from the original static dispatch table.
It begins before the prologue detected at `800C7418`, following a return with
a non-NOP delay slot. The overlay catalog now supplies its fingerprint-verified
entry. Legacy checkpoints can already contain the logo actor: the adapter
schedules its native destruction before the filesystem replaces the overlay,
preserving the original destructor, memory release and frame-rate reset.
Skipping the fade without retiring that actor left its callback dangling.

The transition exposed an independent controller error: INT1 data-ready could
overwrite a still-pending INT3 command acknowledgement during Setloc/ReadN.
Data delivery now retains that acknowledgement until the driver consumes it.
The generic PsyQ HLE driver also acknowledges its own completed commands;
the registered native IRQ handler retains ownership of raw register responses.

## Savestates

Version 8 stores the medium identity, independent of installation paths, and
selects the required image before restoring guest RAM and the saved controller
buffers. Returning to a Disc 1 checkpoint and reopening a Disc 2 checkpoint
therefore select their respective images automatically. Old MGS states infer
their disc from `FS_DiskNum`; their CPU/RAM layout is unchanged. Details:
[native savestates](mgs-savestates.md).

TAS manifests now fingerprint every configured CUE and referenced BIN, including
Disc 2. The savestate identity uses the boot serial, executable and volume
descriptor; the TAS SHA-256 identities cover complete image contents.

## Reproduction

The locally retained input-only seed is
`data/native/disc-change-walk-03/states/checkpoint_000000200_vblank_7790.ps1s`.
It was reached from the Normal s12c scenario through ordinary controller input.
No user memory card or quicksave is overwritten by the runner.

```bash
python3 scripts/mgs_disc_change.py data/native/disc-change-check \
  --state data/native/disc-change-walk-03/states/checkpoint_000000200_vblank_7790.ps1s \
  --replay
```

Add `--vulkan` for a hidden 3840×2160 presentation run. Use a fresh destination
per run. The [route](mgs-disc-change.route.json) follows the warehouse corridor,
triggers the original exit, waits for controllable s13a and moves Snake along
its entrance staircase. The retail `cac1.hzd` identifies the return trigger at
Z=11000..12500, so the route moves down the stairs toward Z=9500. It waits for
loading/fade actors to disappear and requires 60 stable VBlanks after movement;
an endpoint during the fade back to s12c is explicitly rejected.
Its audit requires one automatic switch, one original
validation, no CD timeouts, no Start presses and no interpreter calls. It also
checks that health, difficulty and inventory survive the transition.

This is a focused transition test; it does not certify a continuous complete
campaign. Original file loading still occurs during the stage transition.

## Measured results (2026-10-05)

[Machine-readable evidence](mgs-disc-change-evidence.json) retains runtime,
movie, route, seed and media identities for seven successful runs:

| Check | Result |
| --- | --- |
| Input-only s12c exit, automatic switch, s13a movement; Vulkan 3840×2160 | 1,540 VBlanks, one switch and one original validation |
| Headless replay of the same recording | Same boundary, game ticks and final gameplay state |
| Vulkan replay with the final legacy-logo fix | Same witnesses and final state; no route outcome change |
| Legacy v7 state already inside `change`, with an existing logo | 1,344 VBlanks to stable s13a; native logo destruction completed |
| Disc 2 mounted, restore a v8 Disc 1 state | s12c remains controllable; 60 game ticks over 120 VBlanks |
| Restore v8 during validation, with CD2 mounted but `FS_DiskNum=0` | Original validation resumes and reaches controllable s13a |
| Disc 1 mounted, restore a v8 Disc 2 state | s13a remains controllable; 60 game ticks over 120 VBlanks |

The full transition ends at `(7727,17758,8904)`, with `FS_DiskNum=1`, one
Snake actor and no loader, change or fade actor. Health remains 96/704,
difficulty remains Normal, and inventory is unchanged. No Start press, CD
timeout or interpreter call occurs. The screenshot at
`data/presentation/disc-change/s13a-vblank-9300.png` is retained locally.

The C++ suite passes **1,252 tests**, with 31 opt-in GPU/golden tests skipped;
all **95 Python tests** pass. Regression cases cover failed media switches,
disc identity and saved read buffers, missing-media load rejection, command
acknowledgement ordering, overlay-scoped adapters and false gameplay endpoints.
The user's quicksave and both personal memory cards retain their prior SHA-256.
