# Original-emulator reference for the native TAS

PSXjin **2.0.2** now runs theenglishman's original
[USA Extreme movie](https://tasvideos.org/3462S). This is an additional TAS
reference; SwanStation remains the recompilation donor/oracle. PSXjin reaching
a milestone does not certify that the native runtime reached it.

## Observed first disc

The reference reached stage `change`, story **204**, difficulty **3**, at movie
frame **190651**, with life **576**. The trace covers frames **1–191146** with
no gaps and no zero-life observations in gameplay stages. Nine duplicate boot
rows are indexed by their movie frame labels, not by CSV row number. The index
contains **192 stage/story/difficulty intervals**, joined to original script
resource hashes from the existing alignment catalog. This establishes an
observed frame correspondence; script branches still require live predicates.

Local evidence:

- `.local/tas-reference/psxjin-2.0.2/reference-disc1.tsv`
- `data/tas/extreme/reference-disc1-index.json`
- `.local/tas-reference/psxjin-2.0.2/reference-189000.pxs`
- `.local/tas-reference/psxjin-geometry/reference-ticks.tsv` (second pass from
  the retained frame-14078 reference state, including sampled game pads).

The first pass stopped at the disc-lid dialog. Closing the emulator flushed
the remaining rows through frame 191146. Its periodic slot file uses the
**movie basename**, not `SLUS00594.000`; only the retained frame-189000 state
was archived from that pass. The maintained collector stops before the modal
disc-lid event.

## Local execution

The [archived PSXjin executable](https://files.tasvideos.org/emulatordownloads/psxjin/Deprecated/psxjinv2.0.2.zip)
runs through the existing Steam Proton Wine binary, in an isolated Wine prefix
and Xvfb display `:94`. Its directory is
`.local/tas-reference/psxjin-2.0.2/`; the prefix is
`.local/tas-reference/wine-prefix`. The process uses the user's USA v1.0 disc
and their SCPH-5502 BIOS, with the original movie's embedded cards. The actual
BIOS identity is preserved even though the game is USA. User cards and quicksave
are not emulator backing files. BIOS, discs, videos, RAM and states stay ignored.

The reference source checkout is `.local/psxjin-reference/psxjin`, from
[TASVideos/psxjin](https://github.com/TASVideos/psxjin). Relevant implementation
references are `Win32/WndMain.cpp` (CLI), `LuaEngine.cpp` (telemetry/state API)
and `movie.cpp` (PJM input and system commands).

Copy `scripts/mgs_tas_reference.lua` into the isolated emulator directory and
pass that relative filename to PSXjin's `-lua`, together with
`-runcd DISC.cue -play MOVIE.pjm -readonly`. An absolute Lua filename changes
PSXjin's working directory and can break its relative BIOS path. Disc/movie
Wine paths use the `Z:` mapping. The collector waits for queued CLI playback
before loading a movie slot.
`MGS_TAS_TRACE_PREFIX` chooses a fresh output prefix;
`MGS_TAS_TRACE_STOP` selects the last observed frame (default 191146).
`MGS_TAS_TRACE_SLOT` optionally resumes an existing PSXjin slot (1–10) from
the same movie, avoiding another startup replay. Existing trace outputs are
refused. Native PS1S files are not PSXjin slots and are never imported here.
Maximum speed suppresses reference rendering; it does not overclock the guest.

```bash
python3 scripts/mgs_tas_reference.py index \
  .local/tas-reference/psxjin-2.0.2/reference-disc1.tsv \
  data/tas/extreme/reference-next.json \
  --movie .local/tas-reference/metalgearsolid-bigbossextreme-tas-v3-theenglishman.pjm \
  --catalog data/tas/extreme/alignment-catalog-03.json

python3 scripts/mgs_tas_reference.py match TRACE.tsv CHECKPOINT.ps1s \
  --first 18300 --last 18800 --tolerance 80
```

`match` retains separate candidate intervals when a pose recurs. It compares
stage, story, difficulty, stance, pause, actor existence and position. A close
position alone does not establish synchronization: compare orientation,
inventory, actor state and upcoming original inputs as well.

## Input adaptation and current limits

The original heliport inputs start before Snake is allocated. The native intro
checkpoint is already ready, so the source playhead must be rebased near frame
13720. Applying the earlier source range from a ready checkpoint moved Snake
too soon. Short weapon-menu edges also require explicit native sampling time.

Native progress includes the lower heliport, ducts, hangar elevator, DARPA
Chief, the Meryl ambush and B2 armory in Extreme. The ambush reached story 29
with all 18 guards defeated; the post-battle sequence reached story 36.
Snake used a normally collected ration through the real inventory to recover
from 32 to 256 life, collected four C4 charges, and demolished all three walls
on the published route to Ocelot. Ocelot is defeated (story 39), followed by
Baker's sequence (story 52), the Meryl call (story 58) and return to the hangar.
The final Ocelot segment and post-Baker sequence have exact witness replays;
these do not certify all earlier battle prefixes as one uninterrupted replay.
The native frontier and separately verified
segments are recorded in `mgs-tas-reference-evidence.json`. This is not native
validation through the disc change. Failed route tails are not milestones.

PSXjin's per-VBlank trace exposes differences in game update cadence. The
extended collector reads USA `GV_Time` at `0x800ADA48` and `GV_PadData` at
`0x800B2D38`/`0x800B2D48`; pad masks are byte-swapped from PsyQ to wire order.
The USA Snake pad pointer is at `work+0x9AC`, unlike Integral's `+0x9B0`.
The decompilation's `libgv/gvd.c`, `libdg/dgd.c` and `libgv/pad.c` establish
the update order. `GV_Time` advances before the frame wait/pad read, so the
last observation of a tick is used to retain short interactions that appear
after its first observation. This is a per-VBlank observation, not a trace of
each CPU input read: an interaction can also be masked within the same tick.
For example, source frame 20454 contains Up+Circle for the B1 ladder, but
20455 has zero pads and a new script status without advancing GV_Time.
The retimer rejects such transitions; use the original PJM interaction with
a game-state gate. Scripted `STATE_PADDEMO` movement must finish before
replaying player-controlled movement, or the adapter can repeat it twice.

`mgs_tas_reference.py retime` creates `sampled_game_runs`, preserving both
controllers. The native `CLOCK GAME` route consumes these runs only when
`GV_Time` advances, retaining the pad through intervening VBlanks. Guards,
goals and timeouts still run at every VBlank. Missing/reset game ticks fail
explicitly; stage loads can execute multiple actor updates between observed
video frames and must be split at state gates. This adapter remains under
validation: filtered game pads and state-controlled animations still need
appropriate entry/exit conditions. It is not yet a full-disc native replay.

The duct exit illustrates this requirement: `PLAYER_INTRUDE` clearing does
not immediately mean the stand-up edge is accepted. Wait for the exit
animation/control state, and rebase subsequent source input to the *completed*
stand-up state, rather than replaying its animation's movement a second time.

Other explicit adjustments are available in `run_adjustments`, keyed by the
first source frame of a button run. They change duration while preserving its
pads and order; they cannot delete an input edge. Read-only `camera_XXXX_facing`,
`pitch`, `phase`, `timer` predicates follow `enemy/camera.c`; camera and patrol
IDs are separate and require live, reciprocally linked actors. `yaw` observes
interpolated orientation; `turn_yaw` observes `CONTROL.turn.vy`, which is the
appropriate gate for the duct's 32-unit steering input.

`evpanel_XXXX_phase`, `timer`, `door_phase`, `selection`, and `occupants`
observe the live named actor in `game/evpanel.c`. The hangar panel is `7858`.
On a call, its arrival delay is `(GV_Time % 6)*30`, except that ticks divisible
by 8 choose 300. Native input timing need not produce the reference's zero
delay. Wait for `door_phase == 2`, enter using the source doorway/selector
positions, then issue the original Down+Circle. On the destination floor,
wait for panel phase 3 and `PLAYER_PAD_OFF` clearing; the stage name changes
before the entrance animation releases the controls.
The selector itself can take control before Snake reaches an exact coordinate:
panel phase 6 is the meaningful goal for entering floor selection.

The original TAS preserves Big Boss rank using human shields and enemy/Meryl
kills. Our validation adapts that choreography to ordinary firing and real
pickups; it does not enforce the movie's rank constraints. In `s03d`, read-only
`ambush_phase` and `ambush_kills` observe the commander, while `zako_XXXX_life`
dereferences each live guard's target. Phase 12 can overlap the final grenade
blasts; it alone is not proof that crossing the room is safe. Item actors also
remain alive to display pickup text: `item.c` state 2 means already collected,
not an available second box.

In the armory, the physical movie's R1 edges can leave a different selected
weapon after an adapted placement. Gate equipment and the actual C4 decrement,
then the original demolition flags (`global[0x4e]` bits 3 and 4 in `s04b`).
Only use movement waypoints from the source geometry after the preceding door
or wall is open; direct steering does not route around crates. Ocelot's
`revolver_09fb_{x,y,z,facing,life,phase,timer}` predicates use his linked battle
actor in `onoda/s04b/revolver.c`; life is the normalized 0–1024 display value.
They exclude the unrelated torture actor with the same filename.

Two new main-executable entry aliases eliminate private interpreter calls:
`0x8003B090` in owner `0x8003B028` (Ocelot loader/updates), and `0x80024C64`
in owner `0x80024B0C` (post-Ocelot demo). The original recordings replay with
matching witnesses and zero interpreter counters after these corrections.
The generator now preserves function placement in existing translation shards;
body/entry changes no longer rebalance all 160 campaign shards. The subsequent
fix required 8 build operations, compared with 163 for the preceding fix.
Fresh builds retain size balancing; new declarations can still invalidate the
shared header.

Short menu edges require a real release before the next press. The original
B2-to-1F selection includes a neutral interval between the two Up edges; the
adapted action retains and lengthens it to six VBlanks. A one-VBlank release
at a route goal can be missed by the menu and select B1 instead. For Codec,
arm physical Cross pulses against story completion rather than assuming the
sampled dialogue span will finish at the source's final frame.

For a successful span preceding a failure, `mgs_tas.py recover-prefix
SESSION DEST --frames N` seals exactly those N inputs with frame N+1's existing
RAM/PC witness. Replay that prefix once to validate it and obtain its portable
checkpoint. This neither changes game RAM nor repeats earlier campaign stages.
