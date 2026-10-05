# Native MGS TAS and incremental playability checks

`scripts/mgs_tas.py` records and replays the input **consumed by the native
runtime**, starting from an isolated `.ps1s` checkpoint. Vulkan remains the
normal 3840×2160 presentation. Guest RAM, original logic and the PSX framebuffer
remain native-resolution underneath it.

This is an instrument for building a complete playthrough, not a declaration
that the whole game works. See [evidence](mgs-tas-evidence.json) for the routes
actually exercised and [donor review](donor-updates-2026-10-03.md) for upstream
changes. The USA catalog now compiles 73 story overlay candidates. See
[campaign validation](mgs-campaign.md) for the distinction between compiled code
and exercised stages. Boss encounters, in-game card saves and disc swapping
require their own recorded validation.

## Record, replay and branch

From the repository root, start a new interactive recording from the user's
existing dock checkpoint:

```bash
python3 scripts/mgs_tas.py record data/tas/my-playthrough \
  --state savestates/quick_slot_0.ps1s
```

The destination must be new. The launcher copies the initial checkpoint, uses
its own memory-card files, and records SHA256 fingerprints for the executable,
disc tracks/CUE, runtime, configuration and presentation cache. Existing user
savestates and cards are not overwritten. The state itself contains its saved
card data; the new backing files remain inside the session directory.

For a run from startup, first create the earliest portable checkpoint:

```bash
python3 scripts/mgs_tas.py bootstrap data/tas/boot-seed
python3 scripts/mgs_tas.py record data/tas/from-start \
  --state data/tas/boot-seed/initial.ps1s
```

Bootstrap uses isolated blank cards, or copies the two raw 128 KiB card images
from `--cards DIRECTORY` before starting the game. Its manifest fingerprints
those source cards and reports `initial_vblank`; it never replaces user cards.
The few startup VBlanks before that first
resumable checkpoint are not part of the movie; it is not a power-on emulator
state. No console BIOS movie format is implied.

- Normal game controls are unchanged.
- **F5** requests a new checkpoint in the session's `states/` directory.
- **F10** pauses at a native clock boundary; **F12** advances one guest VBlank.
  One VBlank is not necessarily one rendered game frame (MGS uses 30/20 fps).
- **Esc** ends a recording after its terminal state witness is written.
- Automatic checkpoints default to every 3,600 recorded VBlanks. If not all
  native thread continuations are resumable, saving waits for a later boundary.
- **F9** is intentionally unavailable inside a movie. Loading arbitrary state
  without also replacing the movie cursor would corrupt its history. Start
  a separate branch from an immutable checkpoint instead:

```bash
python3 scripts/mgs_tas.py record data/tas/my-alternative-route \
  --state data/tas/my-playthrough/states/CHECKPOINT.ps1s
python3 scripts/mgs_tas.py replay data/tas/my-playthrough
```

A branch has its own initial state and input timeline; it is not an automatic
splice into its parent's movie. In particular, no claim is made that restoring
a checkpoint reproduces the parent's suffix without rerecording it.

If a process crashes before writing its terminal witness, recover the verifiable
prefix into a **new** session, keeping the original failure intact:

```bash
python3 scripts/mgs_tas.py recover-prefix data/tas/interrupted data/tas/recovered
python3 scripts/mgs_tas.py replay data/tas/recovered --headless
```

Recovery excludes the last complete input sample and the incomplete tail. Its
pre-input RAM/PC becomes the terminal witness for the preceding samples. The
recovered manifest explicitly records that reduced coverage; recovery itself is
not a successful replay or evidence that the crashing section works.

For unattended checks:

```bash
python3 scripts/mgs_tas.py record data/tas/dock-check \
  --state savestates/quick_slot_0.ps1s \
  --input-script scripts/fixtures/mgs-dock-cache.input \
  --frames 1500 --min-game-ticks 600 --headless --period-us 1000 --timeout 90
python3 scripts/mgs_tas.py replay data/tas/dock-check \
  --headless --period-us 3000 --timeout 90
python3 scripts/mgs_tas.py replay data/tas/dock-check \
  --hidden --capture-every 300 --timeout 90
```

`record --end-at-checkpoint` extends a requested frame count until the runtime
can save an exact portable terminal checkpoint. The extra input samples remain
in the movie; the manifest records the requested count and termination policy.
Replays always use the original terminal witness. Campaign steps enable this
option automatically so the next segment can inherit the exact final state.

`--headless` exercises the actual native game and PSX GPU without Vulkan;
`--hidden` uses Vulkan in a hidden window. Omit both for interactive playback.
`--period-us` is diagnostic acceleration, not an overclock or FPS measurement.
`--min-game-ticks` asserts minimum progression of the USA `GV_Time` actor clock
and is retained in the recording manifest. This catches a clock/renderer stall
even if a motionless route would otherwise replay with identical RAM.
For a regression after rebuilding, explicitly pass `--allow-runtime-change`;
all other asset/configuration fingerprints and recorded RAM witnesses remain
mandatory. A logic fix can legitimately invalidate an old expected state: keep
the old failing recording, explain the change, and record a new accepted route.

## Importing an existing PSXjin TAS

The active state-gated campaign now restarts on **Extreme** with copies of the
reference's completed cards. Its original menu unlock required a BIOS device
discovery fix. Original GCL conditions, HZD patrol paths and the decompilation's
actor layouts now guide the adaptation; see [Extreme alignment](mgs-tas-routes.md#extreme-startup-and-original-script-alignment).
The raw-import limitations below remain distinct from that adapted campaign.

`scripts/mgs_tas_import.py` reads PJM v2 text and binary movies with two digital
controllers. It preserves both ports, inclusive press/release boundaries, source
SHA256, disc IDs, and embedded memory cards in a new isolated directory. Text
commands are exponents (`2` means disc-lid toggle), not binary command masks.
Savestate-start, PAL, analog, multitap, cheat and hack movies are rejected.
System events inside the selected range are rejected rather than discarded.

The downloaded reference is [theenglishman's USA v1.0 TAS](https://tasvideos.org/3462S),
recorded with PSXjin 2.0.2: 330,540 frames, Extreme difficulty, preinitialized
cards and two controller ports. This is a speedrun route, not exhaustive coverage
of every gameplay alternative. Its disc-lid events are frames 191,147 and
191,150; the explicit prefix ending at 191,146 excludes those events.

```bash
python3 scripts/mgs_tas_import.py inspect PATH/TO/reference.pjm
python3 scripts/mgs_tas_import.py convert PATH/TO/reference.pjm data/tas/import \
  --last-frame 191146 --first-vblank 3 --source-url https://tasvideos.org/3462S
python3 scripts/mgs_tas.py bootstrap data/tas/import-boot \
  --cards data/tas/import/cards
```

`--first-vblank` is an explicit mapping, not an inferred synchronization. The
example value 3 follows the observed native bootstrap at VBlank 2. Check the
new bootstrap's `initial_vblank` before recording; if different, convert again
into another directory with the intended mapping. `--first-frame` and
`--last-frame` can also rebase a source segment to an aligned native checkpoint.

```bash
python3 scripts/mgs_tas.py record data/tas/import-probe \
  --state data/tas/import-boot/initial.ps1s --cards data/tas/import-boot/cards \
  --input-script data/tas/import/source.input --frames 3000 \
  --headless --period-us 1000 --end-at-checkpoint --native-every 300
python3 scripts/mgs_tas.py replay data/tas/import-probe \
  --headless --period-us 1500
```

Two 3,000-frame imported probes and their native replays complete with
matching RAM witnesses, zero interpreter calls and zero game exceptions.
They **do not reproduce the reference playthrough**. Mapping source frame 1
to native VBlank 3 leaves the game in the title menus. All 3,000 consumed
samples on both ports match the PJM input exactly. An earlier offset-7 trial
chooses Easy instead of Extreme and stays in the opening Codec while later
movement inputs arrive. Trials rebased at the title also fail to reproduce the
reference menu sequence, including a one-VBlank phase shift. Input import and
native replay work; cross-emulator alignment is unresolved. A constant startup
offset has not been sufficient in these trials. Local recordings are under
`data/tas/reference/`; hashes and outcomes are in
[the import evidence](mgs-tas-import-evidence.json).

No whole-disc validation is implied by a completed recording or by importing
191,146 input frames. The existing native campaign remains separately documented
in [campaign validation](mgs-campaign.md).

## What makes the replay verifiable

Host key events are accumulated until the VBlank sample. Short press/release
pairs still survive for one sample; analog axes are latched too. Playback owns
both pad samples and ignores live gameplay keys/controllers. Host window,
pause and exit controls remain available.

The movie also records **which native cooperative boundary receives each
VBlank**, identified by an ordinal and a guest resume PC. The host parks the
game thread there before ticking devices, hashing RAM and publishing input.
Replay delivers the clock at exactly the same boundary. A frame number alone
would allow host/render timing to change the number of iterations through
translated polling loops, even with identical input.

HLE clock queries/waits participate too, using their guest return PC as the
boundary identifier; those points cannot themselves serialize the suspended
C++ HLE stack. RAM hashing and checkpoint I/O are excluded from the next clock
deadline. Accumulating their wall time as guest clock debt would starve the
native game between VBlanks, producing a repeatable but almost frozen movie.
Time parked inside a TAS rendezvous is also excluded from the HLE VSync
timeout, so a long pause does not shorten a multi-VBlank wait.

Clock delivery and full savestate eligibility are separate. A thread waiting
for a CD response must continue to receive hardware time even when another
thread has not yet reached a portable continuation, or when no HLE VSync/CD
waiter has published a clock-demand flag. This rendezvous is enabled only for
TAS sessions; ordinary gameplay retains its existing pacing path.

`movie.mgst` is a versioned text stream:

```text
MGSTAS 1
F boundary vblank ram_fnv1a64 resume_pc pad0_fields pad1_fields
E count terminal_boundary terminal_vblank terminal_ram_fnv1a64 terminal_resume_pc
```

Each pad has six decimal fields: active-low PSX buttons, LX, LY, RX, RY,
pad type. Every frame checks all **2 MiB of main RAM**, plus the native resume
PC, before advancing. The terminal witness checks the result after the last
input. FNV is a fast state witness, not cryptographic authentication; SHA256
protects the session files against accidental replacement.

On the first divergence the runtime returns **42**, retains the failing RAM
in `final-ram.bin`, reports the movie frame/VBlank/boundary and expected/actual
hash/PC, and saves `states/desync.ps1s` when native continuations permit it.
`result.json`, `runtime.log` and `metrics.json` report completion, timeout,
interpreter counters, exceptions and checkpoint hashes. A valid input prefix
without a terminal footer is not a successful replay.
`movie_frames` is the recording's total length; `completed_frames`, when the
runtime exits far enough to report it, is the number actually consumed. Replays
also request a final native framebuffer image in `native/`; Vulkan captures
are separately enabled with `--capture-every`.

The witness does **not** cover every device register, CPU register, scratchpad,
audio buffer or Vulkan pixel. Equal RAM is strong regression evidence for the
recorded route, not a general proof of hardware determinism or playability.
Screenshots and oracle comparisons remain separate checks. No video encoding
or movie editor is included.

## SwanStation oracle

For digital recordings with centered axes:

```bash
python3 scripts/mgs_tas.py export-donor data/tas/dock-check \
  data/tas/dock-check/donor.input --first-frame 1
```

This exports effective button intervals in the existing donor fixture format.
It rejects analog/controller-type changes instead of silently discarding them.
Align a **separate SwanStation state and clock** before comparing a route.
Native `.ps1s` files cannot be loaded into SwanStation; an exported frame index
does not establish equivalent emulator/game progress.

The instrumented libretro harness supports `PS1_DONOR_MOVIE_LOG=FILE`. It writes
an independent `MGSORACLE 1` stream containing the frame, complete main-RAM FNV
hash and both active-high **libretro-order** masks after each `retro_run`, plus
an end marker. It refuses to overwrite an existing log. Use distinct
`PS1_DONOR_WORK_DIRECTORY` directories so repeatability checks do not share
mutable cards. The oracle remains at the pinned revision with its established
165% CPU setting. Native and donor RAM digests are not directly comparable.
The tested oracle uses the core's built-in OpenBIOS. Reproduce its configuration
through `scripts/run_oracle.sh` (software GPU, interpreter, fast boot, 165%),
using an absolute input fixture path and a fresh working directory:

```bash
mkdir -p data/tas/oracle-check
PS1_DONOR_WORK_DIRECTORY="$PWD/data/tas/oracle-check/work" \
PS1_DONOR_MOVIE_LOG="$PWD/data/tas/oracle-check/movie.txt" \
PS1_DONOR_INPUT_SCRIPT="$PWD/config/mgs-usa-gameplay.input" \
  scripts/run_oracle.sh \
  'data/discs/usa/disc1/Metal Gear Solid (USA) (Disc 1) (v1.0).cue' 4000
```

Core options and BIOS identity matter: direct harness calls with its default
core options are not equivalent to this oracle configuration.

## Building coverage of the title

See [campaign progression and fixes](mgs-campaign.md) for stage compilation,
exact checkpoint ancestry, diagnostic telemetry, and the active route.
For efficient progression, [adaptive routes](mgs-tas-routes.md) keep one native
process alive, arm imported TAS actions on observed game conditions, and replay
the completed milestone once. This is the default route-driving workflow.

Keep a recorded segment and a checkpoint at each stage transition, boss,
inventory/weapon mechanic, Codec interaction, death/continue and card operation.
Replay completed routes after each runtime fix. A failing route remains a
regression fixture until the cause is understood; do not erase a desync by
turning off the witness. Mark unvisited stages explicitly unverified. Full-game
coverage also requires both discs and the disc-change interaction.

Existing public TAS runs can guide routes. The current
[any%](https://tasvideos.org/3164M) and
[all-bosses](https://tasvideos.org/3161M) movies by **theenglishman** use Integral
and BizHawk 1.11.4, starting from saved state/SRAM. The any% route skips the
Disc 1 bosses, so completing it would not cover those encounters. Their
timings, region and initial state are not interchangeable with USA v1.0 on our
runtime or SwanStation. The [game listing](https://tasvideos.org/Games/1004)
also records an older USA v1.0 movie. These are route references, not imported
or certified native inputs.
