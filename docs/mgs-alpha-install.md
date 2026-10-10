# MGS Recomp — alpha candidate

This is a local preparation build, not yet a public release. Continuous
campaign completion is not certified. Use the in-game Codec save as well
as quick states; save-state compatibility can change between alpha builds.

## Requirements

Linux x86_64 / SteamOS, SDL2, a Vulkan loader and a working GPU driver.
AppImage bundles Python and the launcher dependencies; Flatpak uses the
Freedesktop 25.08 runtime. The unpacked developer archive needs Python 3.11+.
The default launcher is native SDL and supports controllers; Tk is optional
only for the legacy `gui` editor. Consult `build-info.json` for the actual
GLIBC/GLIBCXX requirements of the binary. The portable candidate is audited against a glibc 2.39 ceiling; physical
SteamOS GPU/controller acceptance is still required. See
[SteamOS packaging and verification](mgs-steamos.md).
Windows x64 has a separate ZIP candidate: see [Windows setup](mgs-windows.md).
This page describes the Linux distributions. macOS binaries are not provided.

Both original **USA v1.0** discs are required, as BIN images or single-track
MODE2/2352 CUE/BIN pairs. Extract ZIP/7z/RAR archives first. Other regions,
revisions and modified images are rejected, because this native code targets
one verified executable. No BIOS download is needed. No game data is bundled.

## First start

For AppImage, mark the file executable and open it. For Flatpak, install the
bundle and open MGS Recomp. For the developer archive, extract the complete
archive into a new directory and launch `mgs-recomp`.
In **Import original discs**, use **Choose disc 1** and **Choose disc 2**,
then **Verify and import both discs**. Wait for verification and asset-cache
preparation, then select **PLAY**. D-pad/left stick and A navigate the native
launcher. Flatpak grants read-only access to Downloads and SD storage for
import; choose BIN/CUE pairs from these locations. Keep approximately 1.6 GiB free for the
import in addition to the application. Original images are copied, never modified.

The importer publishes the installation only after both discs and the cache
are valid. Reimporting preserves existing settings and saves and retains the
previous media directory; it therefore consumes additional disk space.

The native launcher uses English labels. Runtime errors are logged and
shown by the launcher. A controller works through SDL's standard mapping.
The runtime reconnects controllers, combines left-stick/D-pad/keyboard input,
and provides a virtual controller-port selector in its pause menu. Physical
hotplug/Steam Input acceptance on SteamOS and full gamepad remapping remain
on the release work list. **Controller / input test** selects the device,
deadzone and button labels, with shared persistent preferences. See
[controller compatibility and Steam Input](mgs-controllers.md) for setup,
tested mappings and physical acceptance still required.

## Settings and controls

**Settings** provides Vulkan presentation at 720p, 1080p, 1440p or native
3840×2160 output (default). Aspect ratio remains fixed at 16:9, with letterbox
or pillarbox fitting. The original game raster and timing remain unchanged.
Windowed startup is the alpha default. Fullscreen, presentation VSync, volume,
mute, fast loading and keyboard mappings persist for the next launch.

| Host action | Key |
| --- | --- |
| Save / load quick state | F5 / F9 |
| Previous / next state slot | F6 / F7 |
| Toggle fullscreen | F11 |
| Pause / host menu | Esc / right-stick click (R3) |
| Move | Arrow keys |
| Cross / Circle / Square / Triangle | Z / X / A / S |
| L1 / R1 / L2 / R2 | Q / W / E / R |
| Start / Select | Enter / Backspace |

Esc/R3 opens the host menu. The guest parks at a resumable boundary before
audio and guest clocks stop; save/load remains available while paused. Quit
requires confirmation. Focus loss and controller disconnection open the menu;
resume is explicit. Notifications show pending/completed/failed state operations.
The controller-port button moves the original pad to port 1 or 2, leaving
memory cards independent. In-menu audio changes currently affect the session;
use launcher Settings for persistent preferences. Resume-last-session and
automatic checkpointing are not yet implemented.
F5/F9 operate on the selected slot; use Codec saves for durable game progress.

## Data, backups and updates

By default data lives under `$XDG_DATA_HOME/mgs-recomp` (usually
`~/.local/share/mgs-recomp`) and settings under `$XDG_CONFIG_HOME/mgs-recomp`
(usually `~/.config/mgs-recomp`). You can create an empty `portable.txt` beside
the launcher before first start to use `userdata/` beside it instead. Portable
mode selects a different profile; it does not migrate your existing saves.
For AppImage, place `portable.txt` beside the actual `.AppImage` file; that
format uses adjacent `mgs-recomp-userdata/`. Flatpak uses its isolated XDG
directories under `~/.var/app/io.github.mgsrecomp.MGSRecomp/`.

Application updates replace application files, leaving the user profile
separate. Do not delete a portable `userdata/` directory when updating.
**Saves and support** creates an explicit ZIP backup of memory cards and
states in the profile's `exports/` folder, also used for diagnostic ZIPs.
Backup/import and a running game cannot access the same profile concurrently.
No automatic restoration or overwrite of an existing backup is performed.

Support ZIPs contain build metadata, settings and the last log tail, with
known personal paths redacted. They exclude game data, memory cards and states.
Inspect a ZIP before attaching it to an issue. Nothing is uploaded automatically.

## Terminal usage

```bash
./mgs-recomp import '/path/disc1.cue' '/path/disc2.cue'
./mgs-recomp settings --height 1080 --volume 80 --no-fullscreen
./mgs-recomp run
./mgs-recomp --big-picture
./mgs-recomp verify
./mgs-recomp backup '/path/mgs-backup.zip'
./mgs-recomp diagnostics '/path/mgs-diagnostics.zip'
./mgs-recomp --user-dir '/path/separate-test-profile' paths
```

A BIN can replace a CUE. Import verifies full disc hashes; launch verifies
small boot files and sizes to avoid hashing both CDs on every start. `verify`
rehashes all installed data. User-data paths with quotes, backslashes or
control characters are rejected by this first launcher to avoid ambiguity in
the runtime configuration reader. Spaces are supported.

`run --reference` selects the existing original-rendering diagnostic path.
`run --headless-frames N` is for developers and does not verify Vulkan output.
The launcher removes inherited runtime diagnostic overrides for normal play.

## Known gameplay scope

Isolated transitions have native/donor/replay evidence, including both discs,
seamless disc change, PAL terminals, ending branches and credits. Recent checks
cover ketchup prison escape through equipment recovery and room exit, the first
possessed-Meryl encounter into Mantis, and the complete Hind aftermath film.
They do not prove every full fight or a continuous completed campaign. Mantis
later phases, the valid Hind roof-exit entry and jeep combat remain priorities.
