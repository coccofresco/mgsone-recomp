# Adaptive native TAS routes

`scripts/mgs_tas_route.py` reuses actions from the published USA PSXjin movie
inside **one continuous native session**. The C++ runner observes game state
at each TAS VBlank, arms the appropriate action, and releases both controllers
as soon as the goal is reached. It records the actual consumed inputs as MGST,
with the existing RAM/continuation witnesses and a portable terminal checkpoint.

The [PSXjin reference workflow](mgs-tas-reference.md) supplies observed original
movie states through the first disc change, plus experimental game-update input
retiming and local prefix recovery. That reference completion is separate from
the native campaign's measured progress.

The workflow is organized around milestones: a stage transition, a completed
fight, a required pickup or restored player control. Use the original HZD/GCL
and decompilation to identify their conditions. Try a complete macro from the
nearest checkpoint; change only the failing macro. Run one complete replay
after reaching the milestone. The older `--checkpoint-driver` is an explicit
diagnostic option and is much slower because it relaunches for every action.

## Reproduction

The local reference is theenglishman's [USA v1.0 movie](https://tasvideos.org/3462S),
SHA256 `9375a2bf8e9406d79edf7c0eec64f9cf32b3226536396514786c0b638240e757`.
The continuous alignment campaign uses **Extreme**, with copies of the
reference's embedded completed memory cards, but is currently deferred in favor
of the independent scenario checks below. Historical B1 recipes and results
below belong to the earlier **Normal** campaign. Source frame numbers are
inclusive and start at 1. Retail
data, source movies, checkpoints and recorded sessions remain local and ignored.

```bash
python3 scripts/mgs_tas_route.py \
  scripts/fixtures/mgs-usa-b1-ambush.route.json \
  data/tas/routes/b1-next-check \
  --movie .local/tas-reference/metalgearsolid-bigbossextreme-tas-v3-theenglishman.pjm \
  --parent data/tas/campaign/prison-ambush-meryl-01 --verify
```

`--parent` names a native recording directory. For a preceding adaptive route,
use its `session/` subdirectory. `--boot` accepts a TAS bootstrap directory.
After a safe adaptive abort, an explicit `--state CHECKPOINT --cards DIRECTORY`
can branch from the retained checkpoint. Keep the unsuccessful session intact.

The output includes the original JSON recipe, its compiled `route.mgsr`, source
and build fingerprints, effective-input recording under `session/`, event logs,
final observations and terminal checkpoint. `--verify` adds `verified-replay/`
and checks every recorded RAM/PC witness at a different host pacing. Without
that option the output explicitly reports that no replay was performed.

## Recipe conditions

Each step has a name and a required `until` goal. Conditions within each object
are conjunctive; scalar values mean equality. Numeric predicates accept `eq`,
`ne`, `min`, `max` and an optional bit `mask`.

- `when` arms an action only in the appropriate state.
- `until` ends the step immediately, including during a held button.
- `stable_frames` optionally requires 1–3600 consecutive VBlank observations;
  keys are released while the goal holds and any false sample resets the streak.
  This requires the persistent runner and prevents transient loading states
  from being certified as controllable gameplay.
- `guard` must remain true throughout the step.
- `abort_when` stops the route if all its predicates match.
- `attempts` bounds repetitions; `max_frames` includes both actions and waits.
- `source_frames` imports both pads in their original order. `minimum_hold`,
  `minimum_release` and `settle_frames` explicitly adapt short input edges.
- `run_adjustments` adds/removes time from a specific selected source run using
  `first_frame` and `extra_frames`, without deleting an edge or changing pads.
- `sampled_game_runs` imports `[updates, pad0, pad1]` from an observed reference
  trace and uses `CLOCK GAME`; this requires the persistent runner.
- `sequence` allows documented local input adaptations. No action means wait.

Guards and aborts take priority over goals. A step expecting a stage change must
not require the old stage throughout. Missing actors never satisfy a predicate,
including `ne`. Budgets and guard failures release both controllers, retain a
checkpoint when native continuations permit it, and return runtime status 43.
Neither observer nor route runner writes gameplay RAM, inventory or progression.

`actor_<basename>_count` observes how many linked actors of an original source
file are alive, for example `actor_bakudan_count`, `actor_blast_count` or
`actor_kogaku2_count`. Unlike a named actor field, an absent actor has count zero
when **all nine GV lists are structurally valid**. Broken links, cycles and
invalid or unterminated names yield an unknown observation; even `ne` then
fails. Stale unlinked heap allocations never count. Both the Python inspector
and native runner validate the same bounded, reciprocal lists without writes.
Counts gate creation/destruction of effects, not visual visibility: an actor
may exist off screen or during a fade. Avoid requiring it to remain alive
longer than the original effect's lifetime.

For an adaptive recording, a completed route can stop before `--frames`, which
is its safety budget. A missing, failed or incomplete route is not success.
For a fixed recording, the requested length remains mandatory;
`--end-at-checkpoint` permits only an extended portable tail.
Combat routes should also guard `game_over_timer == 0` (signed USA
`GM_GameOverTimer`, `0x800ADB44`). Cutting Ocelot's wires or killing the
president can end the mission with Snake still alive; health alone misses this.

`gcl_XXXXXXXX` reads the original four-byte GCL variable operand. For example,
`gcl_11800002` reads linked difficulty, `gcl_1100000a` reads story progress,
`gcl_16000040` reads the dock lift state, and `gcl_1403002d` reads the original
Snake-inside-lift flag. Signed shorts, unsigned identifiers, bytes and individual
flags retain their original semantics. Only bounded global/linked operands are
accepted. The C++ runner and Python inspector use the same USA addresses. Each
action, completion and failure log includes coordinates, stance, life, story,
difficulty and all observations used by that step.

At the end of the heliport introduction, an allocated Snake actor alone does
not mean input is available. The original script can leave `STATE_PADRELEASE`
set after story reaches 18. The intro recipe additionally requires pause=0,
`game_status & 0xDC080000 == 0` and `player_status & 0x20000000 == 0`, following
`game/g_define.h`. This excludes script-controlled/released/masked pad states,
game over, the menu lock and `PLAYER_PAD_OFF` before subsequent gameplay inputs.

`watcher_1466_x`, `watcher_1466_target_z`, `watcher_13e8_facing`, etc. read a
named patrol actor using the original `CONTROL`/`WatchWork` layout. Available
members are `x`, `y`, `z`, `facing`, `pitch`, `route`, `target_x`, `target_z`,
`vision_facing`, `vision_angle`, `vision_range`, `visible`. A valid
callback, `watcher.c` filename and reciprocal actor-list links are required;
missing or duplicate actor names never satisfy a condition. These predicates
can wait for the patrol to leave a crossing before starting a movement macro.
`--checkpoint-interval 600` retains intermediate branch points during route
development, avoiding a repeat of earlier waits after a failed crossing.

Observations are pinned to the verified USA executable. The title actor uses
USA callback `800D26E0` and offsets `A48`, `A4C`, `24B8`; Integral's different
layout is not used. B1 guard counts inspect live `zako.c` actors and their target
health. `guards_in_room == 0` alone does **not** certify a wave or fight victory:
attackers can be outside the room and new groups can spawn without a zero gap.
The B1 victory condition is original GCL procedure `#4538` setting story 29.

## Extreme startup and original-script alignment

`mgs-usa-extreme-start.route.json` starts a new game through the retail menu.
Its `start-03` run reaches controllable `s00a`, difficulty 3, story 6 and life
256. All 1,998 VBlanks replay with matching witnesses. This is a new campaign;
Normal checkpoints are not converted into Extreme checkpoints.

This exposed a BIOS compatibility defect: USA PsyQ's `firstfile` wrapper at
`8009C628` scans the device table at `0x150/0x154`, looking for `bu`, before it
invokes B0:42. The HLE file service already exposed the completed saves, but the
empty table prevented the game from reaching it, so Extreme remained locked.
The BIOS constructor now publishes a 0x50-byte memory-card device descriptor
below its syscall tables. B0 file services still perform the operations; the
descriptor preserves the guest's temporary firstfile hook. This implements
device discovery, not a complete BIOS device callback subsystem. The layout
follows the [documented BIOS control blocks](https://psx-spx.consoledev.net/ps1/kernelbios/control-blocks/).
The card-unlock decision remains in the original menu code.

Use a **fresh bootstrap** after this fix: an older snapshot restores its old
RAM/device table. The validated local bootstrap is `data/tas/extreme/boot-01`,
seeded from `data/tas/reference/usa-pjm-disc1/cards`. User cards and quicksave
remain untouched. The source card hashes are recorded in each manifest.

```bash
python3 scripts/mgs_tas_route.py \
  scripts/fixtures/mgs-usa-extreme-intro.route.json data/tas/extreme/intro-next \
  --movie .local/tas-reference/metalgearsolid-bigbossextreme-tas-v3-theenglishman.pjm \
  --boot data/tas/extreme/boot-01 --timeout 480 --verify

python3 scripts/mgs_tas_align.py \
  'data/discs/usa/disc1/Metal Gear Solid (USA) (Disc 1) (v1.0).bin' \
  data/tas/extreme/alignment-next.json --decomp .local/mgs_reversing/source \
  --movie .local/tas-reference/metalgearsolid-bigbossextreme-tas-v3-theenglishman.pjm
```

The catalog decodes **137 scripts across 96 stage entries** in this disc's
archive, yielding **6,455 structural candidates**: stage loads, trigger volumes,
radio/demo calls and constant story assignments. It also indexes the reference's
15,864 input runs and keeps its disc-lid events. Script byte offsets, resource
hashes, lexical branch/option context and original HZD volumes accompany each
candidate. Decoder provenance includes Foxdie's `libgcl` sources and linked
variable layout. Original HZD patrol routes and the decompilation's actor layout
are included as well. Generated catalogs contain local retail data and stay ignored.

This catalog does **not** infer which movie frame belongs to which stage or
prove that a lexical condition was true. That correspondence requires an
observed run. The dock macro uses original `ea54.g` procedures `053d/053e` and
the `liftup`, `lup_enemy`, `move01A` volumes. A movement blocked by a crate and
an insufficient lift-arrival budget are route failures, retained as such;
neither is repaired by writing flags or positions.

The adapted west-aisle segment `data/tas/extreme/dock-12` reaches `d00a`,
story 7, difficulty 3 and life 256, without triggering the original alert flag.
Its 4,601-frame recording takes 89.904 seconds and its complete replay takes
77.350 seconds, with zero interpreter calls or game errors. It begins at a
retained dock checkpoint; those figures do not include startup or the earlier
wait for the lift. The main synchronization condition at the north crossing
combines the two patrols: the western guard has moved south of z=-20000, while
the lift guard is travelling east. Waiting only for the lift guard allows the
western guard to spot Snake during its turn at the north corner.

`mgs-usa-extreme-dock.route.json` combines the opening reference movement with
this adaptation. `mgs-usa-extreme-intro.route.json` also includes the menu and
heliport introduction, so the complete milestone can be exercised in one
native process. Full-run verification is recorded separately from the earlier
component runs in [mgs-tas-extreme-evidence.json](mgs-tas-extreme-evidence.json).

The combined `intro-milestone-02` run now passes **all 17,588 VBlanks from the
fresh bootstrap through the original menu, dock, films and Codec to controllable
heliport**. Recording takes 144.848 seconds; the complete replay takes 145.591
seconds. Final state is `s01a`, story 18, difficulty 3, life 256, alert 0,
game status 0, pause 0 and player status 0. Both runs report zero interpreter
calls and game errors. The single session uses 23 state-gated steps. The first
combined attempt incorrectly armed skip inputs only in `d00a`; the original
heliport introduction also reloads `d01a` several times. Its timeout is retained,
and the corrected final step follows the entire film/Codec chain.

A separate 28-frame input check branches from this terminal checkpoint and
moves Snake from x=-4605 to x=-3985 with the Right button, retaining full health
and pause=0. This confirms that the observed ready state actually accepts input;
it is not included in the 17,588-frame replay count.

## Measured result, 4 October 2026

`data/tas/routes/b1-milestone-01` starts at the existing Meryl cut-in checkpoint
and completes all remaining attackers, including original ammunition pickups.
It records **2,539 VBlanks in 28.613 seconds**, then replays all 2,539 in
28.970 seconds with matching RAM/PC witnesses. Final state: `s03d`, story 29,
life 256/256, SOCOM 12, one equipped ration, zero live guards. Both runs report
zero interpreter calls and zero game errors. Evidence fingerprints are in
[mgs-tas-route-evidence.json](mgs-tas-route-evidence.json).

`mgs-usa-b1-post-ambush.route.json` then follows the original transitions through
`s03d`, `s03b` and `s03a`, skips the films and closes Codec. Its successful
`b1-post-milestone-01` recording takes 1,216 VBlanks / 10.531 seconds, with a
complete verified replay. Snake regains control in `s03a`, story 36, life 256,
at `(3000, 1016, 500)`. Earlier incorrect stage guards/goals remain recorded as
failed recipe attempts; they were not runtime failures.

`mgs-usa-tas-start.route.json` is a startup recipe candidate. Its complete
persistent run has not yet been validated; it is not part of the milestones.

This measures the native diagnostic path with inexpensive framebuffer captures.
It does not benchmark or certify Vulkan effects in these encounters. The normal
4K presentation remains the launcher default. The raw PSXjin movie is still
unsynchronized; these are adapted, state-gated inputs. Ocelot and disc change
have not yet been validated.

Ten Python route tests cover source preservation, compiler validation, state gates,
budgets, retail GCL operands and USA actor offsets. Four catalog tests cover
branch context, byte offsets, procedure tables and malformed input. Seven native route tests cover immediate release,
both controller ports, retries, missing actors and malformed programs; the five
existing native movie tests also pass.

## Independent scenario checks

The current focus is independent scene validation, with the continuous Extreme
campaign deferred. `scripts/mgs_scenarios.py` reads unmodified USA retail saves,
validates both the card directory checksum and MGS payload CRC/layout, and makes
two isolated cards per case. Only the chosen single-block save is retained;
the original 8192-byte save block and its directory entry remain byte-identical.
Integral's different layout is rejected. The player's own cards and quicksave
are never used as test backing storage.

The local source bank comes from the author's
[DuckStation save-file post](https://www.reddit.com/r/metalgearsolid/comments/qyv2ma/metal_gear_solid_1_save_files_for_duckstation/).
Its archive fingerprint and the measured results are in
[scenario evidence](mgs-scenario-evidence.json). Retail saves remain ignored.

Measured on 2026-10-04: **14 Normal saves on disc 1, spanning 12 stages**, pass
in both the native runtime and SwanStation interpreter at 165% CPU. Both use
the same C++ `TasRoute`, not two independently retimed input scripts. Each run
boots, selects LOAD GAME, loads its only save, waits for 30 consecutive
controllable VBlanks, crouches, and stands again. Native runs have zero private
interpreter calls and no reported game errors. Final stage, story, difficulty,
disc, life, maximum life, stance, equipped items, complete item/weapon arrays,
X/Z coordinates and game-over timer agree exactly across engines. Frame counts
and RAM hashes are recorded separately and are not expected to match engines.

| Save blocks | Stage | Saved → controllable story |
| --- | --- | --- |
| 1 | s01a | 18 |
| 2 | s03a | 18 |
| 3 | s04b | 36 |
| 4, 5 | s08c | 75; 111 |
| 6 | s07c | 119 |
| 7, 8 | s09a | 137 → 141; 149 |
| 9 | s06a | 150 |
| 10 | s10a | 151 |
| 11 | s11b | 173 → 174 |
| 12 | s11c | 190 |
| 13 | s12b | 204 |
| 14 | s12c | 204 |

The two automatic story transitions are grounded in the original USA scripts:
`s09a/ea54.gcx` main assigns 138, starts a demo, and its `fafa` callback assigns
141; `s11b/ea54.gcx` main assigns 174 on entry when story is below 174. Their
hashes and byte offsets are retained in `control_expected`. These cases stop
menu confirmation as soon as Snake is allocated and wait without input through
the subsequent scene. `abst.c` explains why checking pause alone is insufficient:
the mission-log UI can require confirmation while unpaused. Earlier failed
contracts remain on disk and are not counted as passing tests.

The suite exposed and verified two runtime corrections:

- The retail load menu reaches EXE leaf `0x80044D2C`. It now has a native entry
  alias instead of one private-interpreter call per load.
- `s12c` crashed at indirect callback `0x800D7A00`. The overlay catalog now also
  discovers standalone leaves referenced in raw pointer tables, with the same
  aligned, in-text, preceding `JR RA/NOP` boundary requirement as other leaves.
  This adds 197 callback candidates across the campaign catalog, including
  seven for `s12c`. Compilation coverage alone is not gameplay coverage.

Reproduce one case with unique destination directories:

```bash
python3 scripts/mgs_scenarios.py catalog path/to/disc1.mcd
python3 scripts/mgs_scenarios.py prepare path/to/disc1.mcd 4 data/scenarios/example
python3 scripts/mgs_scenarios.py run data/scenarios/example data/scenarios/example/native-01 --engine native
python3 scripts/mgs_scenarios.py run data/scenarios/example data/scenarios/example/donor-01 --engine donor
python3 scripts/mgs_scenarios.py compare data/scenarios/example/native-01/result.json data/scenarios/example/donor-01/result.json
```

`build_donor.sh` links the shared route runner into the harness. The tested
harness is installed at `.local/libretro_headless`; core rebuilding is not
needed for route-only harness changes. `PS1_DONOR_ROUTE` and
`PS1_DONOR_ROUTE_OUTPUT` also expose it directly, with terminal RAM and a full
SwanStation state. They are mutually exclusive with fixed input-script playback.
The native runner retains effective MGST inputs, witnesses, periodic checkpoints,
terminal state, metrics and private-entry traces. `compare` rejects different
cards or route contracts and never upgrades a failed execution to a pass.

Two additional terminal-checkpoint restores (`s08c` story 75 and `s12c` story
204) ran 180 VBlanks each with hidden Vulkan presentation. Both resumed without
interpreter use or runtime errors and produced 3840×2160 presentation captures.
This initially found a presentation gap: `s12c`'s fog actor replaces
`DG_BOUND_CHANL`, bypassing the usual source-scene capture hook. The scene is
rendered through the original packet path: zero source-scene captures versus
90 in the Ninja corridor. The common-dispatch fix and exact replays below now
resolve this gap; the earlier captures and zero counters remain as baseline
evidence. Complete visual equivalence is still outside the scope of these probes.

These checks certify load/control behavior at the listed saves. They do not
certify complete boss fights, traversal, disc swapping, a continuous campaign,
or full visual equivalence. The original focused checks passed: 36 Python TAS tests,
3 overlay catalog tests, 5 scenario tests and 16 native route tests.

## Disc 2 scenario workflow

The USA disc 2 card contains ten gameplay saves and one completed-game `rank`
save. The gameplay cases cover `s13a`, `s14e`, `s15a`, `s16a`, three variants of
`s16d`, `s17a`, `s18a`, and `s19a`. `rank` is excluded from control checks.
The executable and all 73 compiled campaign overlays are byte-identical on
both USA v1.0 discs; media contents remain separate. The scenario runner chooses
the CUE from the save's disc number and creates a local native configuration
for bootstrap and recording, leaving the normal launcher configuration intact.
Both native and donor reports fingerprint the actual selected data track.

The same `prepare`, `run`, and `compare` commands work on the disc 2 card:

```bash
python3 scripts/mgs_scenarios.py prepare path/to/disc2.mcd 3 data/scenarios/raven
python3 scripts/mgs_scenarios.py run data/scenarios/raven data/scenarios/raven/native-01 --engine native
python3 scripts/mgs_scenarios.py run data/scenarios/raven data/scenarios/raven/donor-01 --engine donor
python3 scripts/mgs_scenarios.py compare data/scenarios/raven/native-01/result.json data/scenarios/raven/donor-01/result.json
```

Three scene-specific contracts avoid false failures or unintended inputs:

- `s17a` saved story 253 becomes 255 through the original `ea54.gcx` entry
  script, before the REX introduction. Input remains neutral after Snake's
  allocation until stable control becomes available.
- `s18a` uses `snake18.c`, which stores posture in its own `f8FE` rather than
  the ordinary linked stance variable. The test checks `PLAYER_SQUAT` becoming
  set and cleared after two Cross inputs. The character's own C/assembly
  routines `800CD768` and `800CDE8C` establish this distinction.
- `s19a` also releases menu confirmation when Snake is allocated, preventing
  a late Circle press from moving him with a punch as the opening ends.

The `s15a` failure exposed an overlay function-discovery bug. A stack allocation
at `0x800D5D84` is in a branch delay slot, inside the routine starting at
`0x800D5D70`. Treating it as another prologue skipped that delay instruction
on the taken branch and dispatched to the unregistered interior `0x800D5D90`.
The scanner now rejects prologue guesses following control-transfer instructions;
explicit JAL and configured entries remain authoritative. A synthetic test
executes the emitted C++ on both branch paths and checks the restored stack.
The USA catalog loses 44 false prologue entries across 35 overlays.

The presentation interception now runs at the common USA channel dispatcher
`0x80018780`, before loading `DG_ChanlUnits` through `s2`. Slot 1 captures the
source scene before bounds; slot 6 prepares its presentation before sorting.
This covers both the default callbacks (`0x8001919C`, `0x8001E0AC`) and the
fog replacements observed in `s12c` (`0x800D83E0`, `0x800D7D7C`). The verified
argument-setup instructions are checked against the USA EXE before codegen.
The original callbacks and instructions still execute normally.

### Measured Disc 2 results

All **10 saves across 8 distinct stages pass** on the corrected runtime
(`native-04`), with no interpreter calls, no game errors and no differences in
the compared gameplay fields. Both engines use the same isolated cards and
route contract. The original saves and failed attempts are preserved. Results,
binary/media hashes and capture references are in
[Disc 2 evidence](mgs-disc2-scenario-evidence.json).

| Card block | Stage | Saved → controllable story | Vulkan source instances matched |
| --- | --- | --- | --- |
| 1 | s13a | 204 | 49 / 49 |
| 2 | s14e | 204 | 16 / 16 |
| 3 | s15a | 219 | 96 / 96 |
| 4 | s16a | 237 | 18 / 18 |
| 5, 6, 7 | s16d | 240; 246; 246 | 16 / 16 (block 5 probe) |
| 8 | s17a | 253 → 255 | 33 / 33 |
| 9 | s18a | 277 | 8 / 8 |
| 10 | s19a | 283 | 27 / 27 |

The Vulkan probes resume terminal native checkpoints for 180 VBlanks per
distinct stage with neutral input and disc 2 mounted. Each produces 90 source
scene captures, original-format GPU submissions, complete capture of all
visible instances, and 3840×2160 presentation images. The final images were
visually inspected. These probes establish source-path coverage at the sampled
positions, not completion of the fights or coverage of every disc 2 room.

The original 180-VBlank Ninja and `s12c` recordings also replay exactly after
the dispatcher change, passing their RAM and continuation witnesses. Ninja
retains 13/13 matched instances and 106,521 original-format GPU submissions.
Fog now records **90 source captures instead of 0**, **94/94 matched instances**,
**3,870/3,870 complete visible-instance captures**, and **20,114 original-format
GPU submissions instead of 0**. The original game callbacks continue to run.

Focused regression checks passed: 7 Python code-generation tests, 36 Python TAS
tests, 7 Python scenario tests, and 77 C++ runtime tests, including four using
the locally extracted USA executable/cache. Four opt-in GPU unit tests were not
run in this batch; the actual-scene Vulkan probes above ran separately.
The user's quick savestate and both personal memory cards remain unchanged.

## Checkpoint situations

`scripts/mgs_situations.py` branches directly from the terminal native and
SwanStation states of a passed scenario pair. Each short check executes a new
input-only route, without replaying the title/load menus or the preceding
campaign. Both engines receive the same conditions and controller actions;
their frame counts and raw RAM hashes are not expected to match each other.
The starting RAM, states, cards, route and execution results are fingerprinted.
Cards are copied into each test directory and parent states remain untouched.

Built-in probes exercise pause, Codec, item and weapon menu entry/exit,
first-person entry/exit, and unequip/re-equip of the current item or weapon.
Each requires an initially false transition goal, observes its entry and exit,
then requires 30 consecutive VBlanks of restored control. Gameplay invariants
include stage, story, life, inventory and position. These are short interaction
checks, not prolonged menu navigation, item-effect or weapon-firing coverage.
Unavailable equipment produces `not-applicable`, never a passed test.

```bash
python3 scripts/mgs_situations.py \
  data/scenarios/usa-normal/cases/d2-03-s15a-219/native-04/result.json \
  data/scenarios/usa-normal/cases/d2-03-s15a-219/donor-01/result.json \
  data/scenarios/situations/example-codec --probe codec --vulkan --verify

python3 scripts/mgs_situations.py matrix \
  docs/mgs-disc2-scenario-evidence.json data/scenarios/situations/example-matrix \
  --probes pause codec items weapons first-person --jobs 2
```

`matrix` selects one save per distinct stage from the evidence and runs at most
two isolated checks concurrently. Its report separates passes, inapplicable
cases and failures. Individual output directories are immutable: repeat only
the failing check into a new destination after diagnosis. `--verify` replays
native RAM/continuation witnesses with different host pacing; `--vulkan` adds
presentation captures. Neither option is necessary for every routine check.

Custom `mgs-situation-1` recipes declare initial `requires`, fields to `preserve`,
fields to `compare`, and a normal `mgs-tas-route-1` route. Read-only GCL/actor
predicates can gate steps. Two measured examples are included:

- `scripts/fixtures/mgs-usa-s14e-return-s13a.situation.json` follows the original
  `move_to_s13a` HZD volume and `ea54.g` load procedure. Controller movement
  triggers the door, stops at the real load request, waits for overlay loading,
  and verifies stable control in `s13a`. Native/donor complete in 365/373
  VBlanks with matching final fields; the native recording replays exactly.
- `scripts/fixtures/mgs-usa-liquid-damage.situation.json` waits for Liquid's
  attack, then presses Cross when Snake is downed. Both engines lose exactly
  84 life (768 → 684) and exit the damaged/downed state. Native/donor complete
  in 1,502/794 VBlanks; the native replay is exact. Combat positions and enemy
  phase are not compared: the source load/control checkpoints do not synchronize
  those phases. The initial neutral-recovery contract timed out in both engines;
  `snake18.c`'s `800D0AF8` confirms that getting up requires a button other than
  Triangle. That failed attempt is retained and the recipe now uses that input.

Use a fixture with `--recipe PATH` in place of `--probe`. Changing a route
contract after consulting retail behavior is recorded separately from fixing
the runtime; matching failures in both engines are not automatically port bugs.

Measured follow-up: **48 distinct situation checks pass**, comprising 40 menu/
first-person checks across eight Disc 2 stages, five quick-item checks (including
a second `s16d` story state), one quick-weapon check, the room transition and
Liquid damage/recovery. There are 50 successful executions including the Codec
Vulkan run and a repeated quick-item check. Five equipment cases were explicitly
inapplicable. The Codec Vulkan images show menu entry, exit and return to the
source-mesh scene at 3840×2160; its native witnesses replay exactly.
See [situation evidence](mgs-situation-evidence.json) for individual results,
hashes, preserved failures and exact scope.

The same mechanism can cover pickups, lifts, alerts, death/continue, PAL-card
conditions and selected boss phases once their checkpoint preconditions and
original-script goals are defined. Those cases are not certified by the checks
above. Artificial fixtures that initialize a special phase would need separate
labeling: they can validate that phase, but cannot prove it is reachable through
normal campaign progression.
