# USA input and persistent memory cards

Implementation uses the MIT-licensed RecompOne revision
`0409bc227ab41149e27985383eafd0492f2e6afc`; see
[provenance and retained notice](../third_party/recompone/PROVENANCE.md).
No OpenGTPS1 implementation was imported.

## Input

RecompOne's PsyQ signatures identify the USA v1.0 SDK entries below.
The recipe connects them to the runtime's pad HLE:

| Routine | Address |
| --- | --- |
| PadInitDirect | `0x8009EE48` |
| PadStartCom | `0x8009D524` |
| PadStopCom | `0x8009D5F0` |
| PadGetState | `0x8009CDF4` |
| PadInfoMode | `0x8009CEB4` |
| PadInfoAct | `0x8009CFAC` |
| PadSetAct | `0x8009D1A8` |
| PadSetActAlign | `0x8009D128` |
| PadSetMainMode | `0x8009D160` |

SDK ports are `0x00` and `0x10`. The USA direct buffers are `0x800C3BE0`
and `0x800C3C04`. MGS copies latched host input into these buffers on the
native game thread before VBlank callbacks. The default pad is digital;
actuators/rumble are not exposed.

Keyboard: arrows move, Enter is Start, Backspace/right Shift is Select;
Z = Cross, X = Circle, A = Square, S = Triangle;
Q/W = L1/R1 and E/R = L2/R2. Circle confirms the New Game menu.

Scripted Start/Circle input was observed advancing the real native title
menu into New Game, dock cinematics and the initial Campbell Codec dialogue.
Subsequent Right/Up input visibly moves Snake through the dock, and Select
opens the Codec. This was inspected in the accelerated diagnostic captures
under `data/native/gameplay-native/frames`. The final 20,000-VBlank run
recorded no interpreter counters; see [gameplay evidence](mgs-gameplay-evidence.json).
The compiled stage set includes `title`, `opening`, `d00a`, `s00a` and
`s01a`. The last is prepared for the heliport; its transition is not yet verified.
Leaf actor callbacks, including `d00a:0x800E38E4`, `s00a:0x800D661C` and
`s00a:0x800E3264`, require explicit function entries because they lack both a
stack prologue and direct JAL callers. The recipe covers 38 such referenced
entries and validates their preceding return boundaries. The complete lists
are in [overlay evidence](mgs-usa-overlay-evidence.json).
The recompiler now supports validated `additional_functions` for overlays.
Movement also calls the main executable at `0x80052288`, inside `0x80052254`.
It is configured as a native entry alias after the initial gameplay probe
reported private-entry interpretation there. A complete pass over direct calls
in the generated code identified 17 such entries, including `0x80027A94`.
All are mapped natively; the owner/entry list is recorded in
[mgs-usa-main-entry-aliases.json](mgs-usa-main-entry-aliases.json).

## Persistence

The host opens `data/memory_cards/card1.mcd` and `card2.mcd` (128 KiB raw
images), creating formatted images if missing. `paths.memcardDir` overrides
the directory. Wrong-sized existing images are rejected without replacement.
Writes replace the image through a temporary file; failures roll back the
in-memory mutation. This is not a guarantee against power loss.

BIOS file operations support `bu00:`/`bu10:`, allocation chains, reads,
writes, seek, close, directory iteration, erase and format. BIOS sector writes
also persist. MGS asynchronous completion requires one HwCARD event per
128 bytes followed by SwCARD completion; these are delivered cooperatively
after the initiating call returns.

Tests verify a cross-block BIOS write survives object destruction/reopening,
file enumeration and deletion, full-card allocation, malformed-size protection,
cycle rejection and directory checksums. A BIOS test verifies four hardware
completion events followed by software completion for a 512-byte write.

F5/F9 savestates remain unsupported for active native MGS threads: suspended
C++ stacks are not serializable. The window now explains that no savestate
was saved/loaded. Use the game's memory-card save flow. Unit-test persistence
does not establish that an actual MGS gameplay save/load has succeeded.
A real Codec call to 140.96 from the initial dock produced "NO RESPONSE";
no MGS save was written by that test. In-game save/load remains unverified.

## Reproduce the gameplay input probe

With the built USA runtime, run from the repository root:

```bash
SDL_AUDIODRIVER=dummy PS1_DIAGNOSTIC_HEADLESS=1 \
PS1_VBLANK_PERIOD_US=1000 PS1_EXIT_VBLANK=20000 \
PS1_INPUT_SCRIPT=config/mgs-usa-gameplay.input \
PS1_METRICS=data/native/gameplay-metrics.json ./scripts/run_native.sh
```

The 1 ms VBlank period is diagnostic acceleration, not a frame-rate
measurement. Omit it for normal NTSC pacing. The input fixture skips startup,
starts a game, skips dialogue, moves Right/Up and opens the Codec.
