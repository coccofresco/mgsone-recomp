# Native campaign validation

The campaign is being exercised with input-only native TAS segments. The active
TAS alignment campaign now restarts on **Extreme**, matching the published
reference and its embedded completed cards. A continuous 17,588-frame run from
startup through the dock to controllable heliport (`s01a`, story 18) passes its
complete replay, with full health and no alert. The historical milestones below are
**Normal** and are retained separately. See [adaptive routes](mgs-tas-routes.md)
for the device-discovery fix and script/patrol synchronization.

Compiling
a stage is not evidence that it is playable. The disc-change milestone has not
yet been reached. The dock elevator ascent, following cutscenes, and heliport
Codec, upper hangar duct (`s02b`), tank hangar (`s02a`) and B1 prison
(`s03a`) have been exercised. The selected route proceeds through the DARPA
sequence to victory in the prison ambush with Meryl (`s03d`), confirmed by the
original story variable reaching 29. Ocelot and disc change remain unvalidated. The first progression failure
was fixed in dock patrol initialization.

## Compilation coverage

`scripts/build_native.sh` now prepares 73 story overlay candidates from the
original USA archive, including `change`. The five previously verified ranges
retain their fingerprints and callback lists. Other text extents and callback
entries are candidates derived by `mgs_overlay_catalog.py`; the local report is
`data/overlays/usa/campaign-catalog.json`. Retail resources remain untracked.

Four variant pairs share their actor-table identity but differ in code:
`s02c/s02e`, `s04b/s04br`, `s10a/s10ar`, and `s17a/s17ar`. Their dispatch predicates
also check distinct instruction words. Both ordinary and savestate-resume
dispatch apply the same checks. Synthetic fixtures verify these distinctions
and reject invalid identity offsets.

## First progression failure: dock patrol initialization

The original dock script requires Snake inside `liftup`, the arriving guard
outside `lup_enemy`, and both other guards outside that volume. Walking inside
the lift and waiting did not satisfy the guard condition.

In USA `s00a`, `ReadNodes` begins at `800C7040`, but its stack adjustment occurs
at `800C7068`. Prologue discovery split it at that later address. The first
generated function read the route length and returned implicitly from C++,
instead of continuing into the fragment that copies the patrol points. Two
guards retained zero-length routes and started near the origin. A later route
change populated another guard's route, but could not restore its initial pose.

An independent SwanStation run (software GPU, interpreter, OpenBIOS, 165%) reads
the original routes correctly: route 4 has 2 points, route 17 has 14, and route
13 has 11. Its arriving guard starts at `(1000, 9000, -29000)`; the affected
native checkpoint had `(0, 0, 0)` and no patrol. Diagnostic evidence is kept in
`data/tas/campaign/guard-initialization-evidence.json`.

The generator now dispatches a reachable sequential continuation across a
discovered boundary, preserving the guest return address. It emits that edge
before external branch labels, also avoiding accidental execution of a taken
branch when its condition is false. There are 269 such fragments in the current
campaign compilation, including two in `s00a`.

`scripts/tests/test_native_codegen.py` compiles and executes a synthetic C++
fixture with a split routine, both branch outcomes, and a restored continuation.
The recompiler suite passes 129 tests; one retail Crash fixture is unavailable
and skipped. A fresh native segment from before dock initialization now produces the correct
patrol lengths and initial elevator-guard position. Its 5,989-frame replay
passes all RAM and terminal witnesses. Old checkpoints already contain the
incorrectly initialized actors; this validation therefore uses a fresh entry.

## Chaining recorded steps

The runtime writes a checkpoint at a resumable terminal TAS boundary, in
addition to periodic checkpoints. A new recording clears inherited host button
holds while preserving the saved guest-latched input until the first new sample.
The neutral-input regression restores a checkpoint holding Up and verifies an
unpressed first movie sample (`65535`) on both ports.

```bash
python3 scripts/mgs_campaign.py step data/tas/campaign/next-step \
  --parent data/tas/campaign/previous-step \
  --sequence 'right:20,wait:10,up:90,wait:600' --expect-stage s01a
python3 scripts/mgs_tas.py replay data/tas/campaign/next-step --headless
```

The parent must have an intact movie and an intact checkpoint at its exact
terminal witness. Final memory-card files are inherited into the new isolated
session. `route-step.json` records ancestry, input sequence, state/card hashes,
and final telemetry. A stage expectation failure, interpreter fallback, game
exception, death, or missing exact terminal checkpoint returns nonzero even when
recording itself completed. A boundary can be deterministic for TAS but not
portable for savestates; branch explicitly from an earlier numbered checkpoint
when a historical segment terminates inside such a transition. New campaign
steps enable `--end-at-checkpoint`: recording may append fully witnessed frames
until the terminal boundary is portable. The input script ends normally, and
the manifest records this termination policy. Replays retain the exact original
terminal boundary, including movies made before this option existed.
Use `--allow-death` for deliberate death/continue tests. Earlier checkpoint
branches use `--state` explicitly.

`mgs_tas.py --native-every N` captures inexpensive native diagnostic frames;
`--capture-every N` continues to capture the Vulkan presentation. Diagnostic
native captures do not change the presentation's rendering resolution.
`--runtime` and `--config` select and fingerprint explicit builds. Replays can
permit a changed runtime or compilation catalog with `--allow-runtime-change`
and `--allow-config-change`; all input and RAM witnesses remain mandatory.

## Read-only telemetry

`mgs_campaign.py inspect final-ram.bin` reports stage, pause state, life,
inventory, player/guard positions, and dock script flags. Addresses are verified
against the USA executable, not copied from the Integral symbol map:

| Data | USA address | Evidence |
|---|---|---|
| GCL script variables | `800B6440` | `GCL_GetVar` at `800227B8` |
| Linked game state | `800B7510` | same routine; offsets from `linkvar.h` |
| Stage name | `800B7500` | loaded stage string |
| Player control | `800AE16C` | USA `GM_InitGameSystem`, GP `800AD9FC` |
| Player position | `800AE188` | same routine, confirmed against CONTROL |
| Game status / load request | `800ADB3C` / `800ADB40` | same routine |
| Pause level | `800AE0A0` | USA `gvd.c` actor at `80014C30` |
| Pad camera origin | `800ADAA8` | USA setter `8001728C`, getter `80017298`; GP + `00AC` |

The decoded GCL and HZD data are diagnostic inputs only. No progression flag,
actor pose, collision, health, or inventory value is written to advance play.

## Dormant BIOS threads across successive saves

The heliport Codec reactivated a dormant scheduler thread at `8008CDE4`.
After restoring a snapshot, saving again before that thread ran omitted its
continuation: the old snapshot writer included only allocated host fibers.
The original replay remained deterministic but entered the private interpreter
twice, including the scheduler loop at `8008DB60`. A debugger backtrace traced
the calls to `KernelThreads::Impl::entry`.

Snapshots now retain dormant guest continuations independently of host stack
allocation. When reading older snapshots with omitted metadata, restore takes
the architectural PC from the live BIOS TCB. Tests cover a second save before
worker activation and recovery from the TCB. The 1,021-frame original Codec
movie replays with every RAM/terminal witness unchanged and zero interpreter
calls on runtime `c13ca87af2636a68e339dba11df76bb649bc79f8e269e7619d4f43258f097048`.
The 22 selected BIOS/thread/savestate tests pass. Evidence is local in
`data/tas/campaign/heliport-entry-02-native-replay`.

## Recorded milestones (still before disc change)

| Segment | Observed result | Replay evidence |
|---|---|---|
| `dock-fresh-fixed-01` | Correct patrol initialization, life 256 | 5,989 frames plus terminal RAM witness |
| `dock-fixed-lift-02` | Elevator ascending normally, life 256, no alert | 980 frames plus terminal RAM witness |
| `dock-death-continue-01` | Death and Continue restore the dock | Recorded 1,251 frames |
| `heliport-entry-02-native-replay` | Native Codec scheduler continuation | Original 1,021 frames unchanged |
| `heliport-upper-01` | Upper walkway, life 256, no alert | Recorded input path |
| `heliport-upper-vent-01` | Vent transition requested; guard damage leaves life 160 | Recorded 496 frames |
| `hangar-duct-entry-01` | `s02b`, controllable crawl in upper duct | 900-frame replay at a different host pacing passes, zero interpreter calls |
| `hangar-duct-exit-03` | Original Circle action opens the grate into `s02a` | Recorded input path |
| `hangar-socom-safe-04` | SOCOM and 12 rounds collected, cameras avoided while crawling | Recorded input path |
| `hangar-to-prison-01` | Original elevator selector loads B1 `s03a` | 916-frame replay at different pacing passes, zero interpreter calls |
| `prison-ration-use-02` | Collected ration used through inventory; health restored from 160 to 256 | Recorded input path |
| `prison-ladder-climb-01` | Original ladder action enters the upper prison duct | Recorded 418 frames, zero interpreter calls |
| `prison-postdarpa-codec-01` | DARPA sequence completed, card level 1 acquired, control restored in `s03d` | Recorded input path |
| `prison-cell-ration-02` | Original crawl under the cell table collects one ration | Recorded input path |
| `prison-ambush-armed-02` | First three attackers defeated using the SOCOM | Recorded input path |
| `prison-ambush-meryl-01` | Original ammunition pickup and Meryl cut-in reached | Branch point for the remaining fight; later attempts include ordinary deaths |
| `../routes/b1-milestone-01` | Adaptive shooting and ammunition pickups complete the fight: story 29, life 256, SOCOM 12, one ration | Single 2,539-frame recording and complete replay; zero interpreter calls or game errors |
| `../routes/b1-post-milestone-01` | Post-fight films and Codec complete; controllable `s03a`, story 36, life 256 | 1,216-frame recording and complete replay; zero interpreter calls or game errors |

The selected heliport route uses the eastern edge and waits for the ground
patrol. Failed western/searchlight approaches and premature stair approaches are
retained as separate branches. These are ordinary gameplay deaths, not runtime
exceptions. The upper vent route takes damage and does not certify a no-alert
run. No claim of whole-disc or whole-game playability is made by these milestones.

An existing USA PSXjin TAS is importable, but its original frame timing does not
reproduce the same route in the native runtime. Its actions now drive the B1
milestone through [state-gated native macros](mgs-tas-routes.md). This adapts the
Extreme reference to the historical Normal campaign; the active campaign has
since restarted in Extreme. Neither establishes raw
movie synchronization or a complete automatic route to disc change. See
[import tests](mgs-tas.md#importing-an-existing-psxjin-tas).

The terminal-checkpoint regression requests 623 frames at a cutscene transition
and records 625, obtaining an exact terminal save. Its replay passes. The older
623-frame movie also replays on the new runtime, still ending at its original
nonportable boundary. These are separate recordings; their host-selected
boundary ordinals are not expected to match each other.
