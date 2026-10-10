# Alpha: mandatory transitions and endurance checks, 2026-10-07

This closes the **isolated transition checks** for later Mantis, the Hind
roof return, and the jeep escape through the original ending handoff. It does
not certify complete boss fights, a continuous campaign, all difficulties or
physical Vulkan/SteamOS behavior. Receipts, binary identities, fixture writes,
input movies, native replay results and donor states are indexed in
[the evidence file](mgs-alpha-gameplay-soak-evidence.json). Retail RAM/states
remain private under `data/validation/completion/alpha-gates-20261007/`.

## Gameplay evidence and its boundaries

| Path | Verified behavior | Prepared conditions / remaining scope |
| --- | --- | --- |
| Hind roof | Authentic retail tower save, original ladder entry, roof traversal and stair return to `s11c`, story 190 | Input-only roundtrip. Earlier Hind death/film evidence remains separate; Stinger acquisition, full fight and bridge chain are not certified here. |
| Mantis | Nonlethal Meryl reactions, successive health gates, original port-2 recognition, death callback to story 133, Codec/film callbacks, reward to 512 max life and restored control at story 137, exit to initialized caves | Synthetic ancestry and explicit TARGET damage/stun boundaries. Actor state, callbacks and story are never patched to force a pass. Port recognition, film skipping and passage use actual inputs. Gun accuracy, a full fight and the statue alternative remain open. |
| Jeep | Garage navigation, companion readiness, boarding, aimed gunfire and original `s19b` load; both barricades; Liquid hit/recovery gates; original story-286 `s20a` handoff | Garage is input-only from a retail save. Tunnel barrels/guards/Liquid use source-bound post-hit TARGET conditions, followed by original explosions, collision, death, vulnerability recovery and scroll logic. Snake's health is not patched. This is not a survival/aim certificate for the full chase or the other companion branch. |

All accepted paired contracts require native and SwanStation success and an
exact native replay. Donor and native frame counts need not match. Inputs are
gated on actual conditions; damage fixtures operate on copied states, require
the expected prior values and preserve the originals. Failed exploratory runs
are retained and excluded from passed coverage.

Source authority is the private USA retail GCL/HZD/code and the local Foxdie
decompilation. Relevant source files are `chara/snake/sna_init.c`,
`chara/psyco/mempsy.c`, `chara/psyco/padmng.c`, `menu/radio.c`, and the `s19b`
`jeep_srl.c`, `jeep_emy.c`, `jeep_liq.c` and `jeep/jdrum.c` actors. Integral
decomp addresses are not assumed to equal USA addresses: receipts bind each
fixture to the live USA Act function and verified member offsets.

## Corrections that make these tests trustworthy

- The old Hind exit fixture inherited a set stair guard. Re-entering the roof
  through the original ladder from a retail tower save supplies the valid
  history; no guard bit is cleared by the test.
- The donor frontend delivered port-2 inputs but had never attached its second
  controller. It now explicitly attaches two digital controllers through
  `retro_set_controller_port_device`, matching native diagnostic input.
  The original Mantis `PLAYER_SECOND_CONTROLLER` branch passes in both engines.
- Port recognition now uses a direction and requires `PLAYER_MENU_DISABLE`
  clear. Using Triangle and immediately preparing a lethal hit left first-person
  input active, blocking the subsequent Codec in both engines. That was an
  invalid prepared hit, not evidence of a native Codec defect.
- The post-Mantis contract asserts the original 448-to-512 max-life reward;
  preserving the pre-boss maximum incorrectly rejected successful runs.
- Area arrival distinguishes current area from saved area. During the jeep's
  seamless load, `GM_CurrentStageName` is `s19b` while `GM_SaveStageName` remains
  `s19a`. Caves arrival also waits for the original initialization and live Snake;
  an early loader snapshot can still report the previous story value.
- The garage route waits outside the boarding trigger until the companion has
  set GCL global `01ea=2`. Entering earlier does not retrigger an entry-only trap
  merely by waiting inside it.
- The donor build recipe now links `metrics.cpp` and pthread support, required
  by the shared route implementation's loading diagnostics.

Read-only `work_…` observations bind a unique live actor filename, exact Act
address, member offset and integer width. Optional bounded pointer dereference
supports TARGET fields. Malformed actor lists, ambiguous instances, mismatched
implementations and invalid pointers produce **unknown**, which cannot satisfy
even a negative predicate. Both the C++ runner and Python offline reader are
tested against those cases. They never write guest RAM.

For fixtures with multiple instances, `where` selects an authored Work field,
then still requires one matching live actor. The barrel fixtures use original
CONTROL yaw 0/3800; guards use original CONTROL names 1466/1467/1468. Allocation
order is not an identity. The fixture's `source` must explain those bindings.

## Single-process endurance harness

`scripts/mgs_soak.py` authenticates the supplied session artifacts, creates an
isolated profile and runs all cycles in **one native process**. It reuses each
session's adaptive route when available, otherwise its digital input movie.
The ordinary guest VBlank latch delivers input. An explicit diagnostic TAS
rendezvous allows safe observation and timeline replacement; recorded TAS
movies retain their existing strict replay rules.

Each segment loads its initial checkpoint, acknowledges host pause, verifies
that RAM and guest time remain stopped, runs the route, checks the endpoint,
saves through the normal coordinator and reloads that save. RAM fingerprints
and guest time must agree after restoration. It rotates bounded backups rather
than retaining a new state per cycle. Source sessions and cards are hashed
again at the end; personal quick state and memory cards also passed their
pre-existing checksum check.

Measured runs:

| Run | Workload | Result |
| --- | --- | --- |
| `soak-mixed` | 12 cycles × 5 segments: Hind roundtrip, Codec, first person, disc change | 60/60; 123 s |
| `soak-endurance` | 30 cycles × 6 segments; adds Mantis transition/streaming | 180/180; 834 s, 180 saves and 360 loads, clean shutdown |
| `soak-final` | Final build, 8 cycles × 7 segments; adds garage escape and port-2 recognition | 56/56; 121 s |

The endurance run had zero warmed-up RSS growth and zero open-descriptor
growth at every comparable endpoint. Final-build RSS decreased after warm-up;
file descriptor counts remained stable. These are measured regression bounds,
not proof of no leaks. All three runs were **headless**. They do not measure
physical presentation performance or demonstrate Vulkan memory stability.

The renderer and MGS asset cache now expose a diagnostic ledger for their own
successful Vulkan memory allocations: live count, live allocated bytes and
peak bytes. Allocation errors, duplicate handles and untracked frees fail a
Vulkan soak. The ledger excludes swapchain, driver-private and ImGui-backend
allocations; it is not a device-wide VRAM total. Ordinary play leaves it disabled.
Free bookkeeping is ordered before a driver can recycle a handle concurrently.

RSS, open descriptors, host frame intervals and presentation CPU p95 are
sampled per case/cycle. After warm-up, the report compares like endpoints,
using first/last three-sample medians and a median pairwise slope. RSS fails
only on both >32 MiB growth and >1 MiB/cycle slope; descriptor limits are >3 and
>0.25/cycle. Vulkan allocation limits are >4 MiB and >64 KiB/cycle, or >2
allocations and >0.1/cycle. Presentation CPU p95 fails above both 1.5× baseline
and baseline +1 ms. Missing requested Vulkan measurements fail closed.
CPU duration is not GPU execution time; host intervals include diagnostic
pacing, logging and checkpoint work.

Example (use a new output directory):

```bash
python3 scripts/mgs_soak.py \
  --session data/validation/completion/alpha-gates-20261007/hind-retail-entry-r2/native/session \
  --session data/validation/completion/alpha-gates-20261007/hind-roof-return/native/session \
  --config data/validation/controllers-20261006/host-profile/runtime.toml \
  --output data/validation/local-endurance --cycles 30 --warmup 3
```

On a machine with a usable display and Vulkan device, add `--vulkan` and use
real-time pacing (`--period-us 16667`). Inspect the presentation alongside the
report. Physical SteamOS/Desktop/Big Picture acceptance remains an alpha gate.

Final code checks: **1,355 runtime tests passed, 31 opt-in skipped; 187 Python
tests passed**. Build logs, tests and source fingerprints are in the evidence
index. The failed fixed-input pilot motivated the adaptive VBlank rendezvous;
it is not counted as a successful run. The long-run receipts retain their
actual binary hashes rather than being relabeled as runs of the final build.
