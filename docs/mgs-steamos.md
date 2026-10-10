# SteamOS: desktop, Big Picture, AppImage and Flatpak

**alpha.1 — UNTESTED** packages are published at
https://github.com/coccofresco/mgsone-recomp/releases/tag/alpha.1.
AppImage and Flatpak availability does not certify physical SteamOS, Gaming
Mode, controller transport or complete gameplay on these release artifacts.

The final release targets **both AppImage and Flatpak**, with one native SDL
launcher navigable by keyboard, mouse or controller. `--big-picture` opens the
launcher and game fullscreen without changing the saved desktop preference.
The game scene stays fixed at 16:9; the launcher uses the available screen.
720p is selectable for Deck, while the existing default remains 4K. These are
presentation settings, not changes to original simulation timing.

## Player flow

1. Start the AppImage, or install the `.flatpak` bundle from the desktop.
2. Select both extracted USA v1.0 BIN/CUE pairs in **Import original discs**.
   The native browser supports controller navigation, including directories
   and SD storage. Flatpak allows read-only access to Downloads and
   `/run/media`; it does not request the entire home directory.
3. Set resolution, audio and window preferences, then select **PLAY**.
4. In game, **R3** (right-stick click) or **Esc** opens the host pause menu.
   D-pad/left stick navigate, A selects, B returns. Save, load, slot selection,
   original controller port 1/2 and confirmed quit are accessible here.
5. Keep original Codec saves as durable progress and use **Saves and support**
   to back up cards/states or export diagnostics. Exports are under the user
   profile, never inside the AppImage mount or Flatpak installation.

Add the AppImage to Steam as a non-Steam game and use launch option
`--big-picture`. For Flatpak use target `/usr/bin/flatpak` and arguments
`run io.github.mgsrecomp.MGSRecomp --big-picture`. Select Steam's Gamepad
controller layout. These are launch instructions, not a Deck Verified claim.
The application does not edit the user's Steam shortcuts automatically.

AppImage uses normal XDG directories. An optional `portable.txt` beside the
actual AppImage selects adjacent `mgs-recomp-userdata/`; nothing is written to
its transient read-only mount. Flatpak uses its own XDG profile under
`~/.var/app/io.github.mgsrecomp.MGSRecomp/`. The two formats deliberately use
separate profiles; existing data is not silently migrated or overwritten.
The existing application ID is retained for alpha.1 so that previously created
Flatpak profiles remain accessible; the public project is mgsone-recomp.

See [controller compatibility and Steam Input](mgs-controllers.md) for the
shared controller path, device selection, tested mappings and physical test matrix.

## Safe host behavior

Host pause shares the native save-state rendezvous. It first waits for the
game thread to acknowledge a resumable boundary, then pauses audio and stops
hardware clock delivery. Host pacing credits are discarded while parked and
rebased on resume. Load followed by save while paused retains the pending
timeline unwind. The menu remains responsive while waiting for a safe point.

The UI is drawn by Dear ImGui's Vulkan backend **after scene presentation**,
in the swapchain render pass. It does not enter original VRAM, game screen-copy
feedback, CRT/bloom effects or temporal history. The launcher uses SDL's
renderer backend and the same keyboard/gamepad navigation. Dear ImGui 1.91.9b
is vendored with MIT notices and file hashes; no downloaded runtime UI assets
or system Tk installation are required.

Input from keyboard, D-pad and stick is combined before publishing it to the
chosen original pad port. Disconnect clears axes/triggers/buttons; focus loss
clears held inputs and requests pause. Switching ports detaches the other pad
without moving memory cards. This exposes the original Psycho Mantis mechanic;
it does not skip the encounter. Full encounter and physical hotplug validation
remain distinct from routing/unit tests.

## Build and packaging

The initial host build required glibc 2.43 because it linked new libm symbols
(`atan2f`, `acosf`, `sqrtf`). A real SDK link via `MGS_ABI_LIBRARY_DIR` selects
the SDK's math/C++ library versions. The candidate's measured native requirement
is glibc 2.38. No symbol metadata is rewritten. This is not equivalent to a
complete clean rebuild/test on every target distribution.

AppImage packaging audits **every shipped ELF** against a glibc 2.39 ceiling.
It bundles Python 3.13, its standard library, SDL and the dependency closure
from an explicit Steam Runtime SDK root. glibc and hardware driver implementations
remain system-provided. Distro copyright notices for selected dependencies are
included. The appimagetool and type-2 runtime hashes enter the assembly report.

Flatpak uses Freedesktop 25.08, which provides Python, SDL and Vulkan. The
bundle exports only the audited native package and desktop metadata. Permissions
cover GPU, controller input, audio, Wayland/X11 and the two read-only import
locations. No network permission is requested. Flatpak with `--device=input`
support is required; test the exact shipped SteamOS Flatpak version.

After building from the private original discs (see `docs/mgs-alpha-plan.md`
in the accompanying source archive):

```bash
cmake -S engine -B build -G Ninja \
  -DMGS_ABI_LIBRARY_DIR=/path/to/steam-runtime-sdk/usr/lib/x86_64-linux-gnu
cmake --build build --target ps1Runtime mgsFrontend ps1Runtime_tests --parallel 4
python3 scripts/package_alpha.py data/releases/steamos-candidate

python3 scripts/package_portable.py appimage \
  data/releases/steamos-candidate/mgs-recomp-0.1.0-alpha.1-dev.3-linux-x86_64 \
  data/releases/steamos-appimage \
  --sdk-root /path/to/steam-runtime-sdk \
  --appimagetool /path/to/appimagetool --appimage-runtime /path/to/runtime-x86_64

python3 scripts/package_portable.py flatpak \
  data/releases/steamos-candidate/mgs-recomp-0.1.0-alpha.1-dev.3-linux-x86_64 \
  data/releases/steamos-flatpak
```

`flatpak --stage-only` prepares the payload, permissions and metadata without
exporting it. Run the generated `export-flatpak.sh` in that output directory
on a machine where Flatpak's normal sandbox works to produce the bundle;
the staging report explicitly records that export and runtime tests are pending.

Use new output directories; existing artifacts are never overwritten. The
portable packager rejects a binary above the ABI ceiling before producing an
artifact. It never copies the user's installed game library. Source archives
from `package_alpha.py` accompany both formats. Public CI builds the native
launcher and source tests without retail inputs; playable binaries still need
the private disc-based build step.

## Clean host shutdown (2026-10-07)

The BIOS cooperative polling boundaries now observe a host-only cancellation
request, including callback-blocked polling, HLE waits and VSync clock queries.
A distinct exception unwinds the active native fiber back through its root to
the game-thread owner; it is not a guest CPU exception or savestate load. Quit
wakes the state coordinator, TAS rendezvous and interruptible pacing timer,
then joins both threads before final diagnostics and SDL/Vulkan teardown.
Paused sessions process SIGTERM/SIGINT before the pause-loop early return.
Quit remains accessible even when a safe save boundary cannot be reached.
No memory-card flush of arbitrary restored RAM or automatic game save is added.
The Python launcher forwards SIGTERM/SIGINT to the native child and waits for
its shutdown before releasing the profile lock or dismissing the frontend.
The last-session record retains the native exit code and requested signal.
The child inherits the lock descriptor: even SIGKILL of the parent cannot
unlock the profile while the child is still running. SIGKILL itself cannot be
handled or made graceful. Process tests verify forwarding, cleanup/reaping,
lock retention through a delayed flush and parent death; a real native run
terminated through the launcher also closes cleanly.
When the launcher's actual parent executable is the AppImage itself, Linux
[`PR_SET_PDEATHSIG`](https://man7.org/linux/man-pages/man2/PR_SET_PDEATHSIG.2const.html)
connects that wrapper's death to the same SIGTERM cleanup. This addresses the
extract-and-run wrapper observed terminating without forwarding the signal.
The guard checks executable identity and the parent-registration race, and
restores the previous setting on exit. Ordinary shell/desktop/Steam parents
are not armed by this guard. A subprocess regression test covers wrapper death;
packaged acceptance is recorded separately under the validation directory.

The isolated native matrix covers early boot/loading, intro, pause/save/load/
resume, SDL window-close plus confirmation, menu quit, and SIGTERM/SIGINT while
running and while parked. Confirmation is driven through the shared host quit
action headlessly; this is not a physical Vulkan/Steam interaction test.
The two-second `_Exit` containment remains solely for a non-cooperative hang,
returns failure **45**, and never detaches into destroyed hardware or reports
success. Guest exceptions now return **46**. The verified cases join normally;
see [evidence](mgs-shutdown-saves-evidence.json).

```bash
python3 scripts/mgs_host_lifecycle_check.py \
  --base-config /path/to/private/runtime.toml \
  --previous-runtime /path/to/previous/bin/mgs-runtime \
  --output data/validation/host-lifecycle-new-run
```

The command creates isolated card/state directories and refuses an existing
output directory. `--previous-runtime` adds an actual cross-build save/load
cycle. It does not modify the player profile or the configured retail media.

## Acceptance before publication

- Install/import/update in SteamOS Desktop; start from Big Picture/Gaming Mode
  with only a controller, including first-run file selection and error recovery.
- Test Steam Input, physical disconnect/reconnect, original port switching,
  suspend/resume, focus changes, mixed keyboard/controller and external monitors.
- Confirm Vulkan menu placement, notification readability, resize/fullscreen,
  original effects and Codec text at 720p and 4K on physical hardware.
- Verify the same original save across application updates in each format;
  complete the remaining mandatory story-path checks, particularly Mantis,
  Hind roof exit and jeep combat. Host-menu tests do not certify these fights.
- Validate AppImage through both FUSE and extract-and-run; install/run the
  Flatpak bundle in a real Flatpak sandbox. The current execution environment
  denies a NETLINK_ROUTE operation during Flatpak launch and the icon validation
  step of `build-export`. Staging/metadata can be prepared here, but exporting
  and running a `.flatpak` bundle require an environment where the normal
  Flatpak sandbox works. The validation is not disabled to manufacture a bundle.

## Primary references

[Valve's input/display guidance](https://partner.steamgames.com/doc/steamhardware/recommendations)
motivates controller access throughout the launch flow and readable UI.
[AppImage best practices](https://docs.appimage.org/reference/best-practices.html)
require a compatible ABI and relocatable resources; merely wrapping a new-host
binary is insufficient. [Flatpak permissions](https://docs.flatpak.org/en/latest/sandbox-permissions.html)
and [runtime documentation](https://docs.flatpak.org/en/latest/available-runtimes.html)
inform device access, separate XDG storage and the fixed runtime branch.
[Steam Runtime](https://github.com/ValveSoftware/steam-runtime) is the source of
the SDK dependency baseline; actual ABI and physical acceptance are recorded
separately from its name.
