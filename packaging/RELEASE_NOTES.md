# Midnight Ghost Stone — alpha.1 — UNTESTED

**UNTESTED experimental prerelease for Linux and Windows.**
These packages are published for early testing. They have not been qualified
for complete gameplay or supported-platform compatibility. Source CI and
package integrity checks are not a certification of the playable builds.

You must supply **both original Metal Gear Solid USA v1.0 discs**
(**SLUS-00594 / SLUS-00776**) as BIN images or single-track MODE2/2352 CUE/BIN
pairs. Other revisions, regions, Integral and modified images are unsupported.
No original disc images, extracted game assets, BIOS or user saves are bundled.

## Downloads

| File | Purpose |
| --- | --- |
| `mgsone-recomp-alpha.1-linux-x86_64.AppImage` | Linux / SteamOS portable application |
| `mgsone-recomp-alpha.1-linux-x86_64.flatpak` | Linux / SteamOS Flatpak bundle |
| `mgsone-recomp-alpha.1-windows-x86_64.zip` | Windows x64 application and dependencies |
| `mgs-recomp-0.1.0-alpha.1-source.tar.gz` | Source snapshot accompanying the Linux packages |
| `mgs-recomp-0.1.0-alpha.1-windows-x86_64-source.tar.gz` | Source snapshot accompanying the Windows package; same source commit |
| `SHA256SUMS` | SHA-256 checksums for the attached packages |
| `release-manifest.json` | Exact commit, package identities and scope of packaging checks |

Use the application package for your platform. GitHub's automatic
"Source code" downloads do not contain a playable runtime.

## Quick start

**Windows:** extract the complete ZIP into a new folder and run
`MGSRecomp.exe`. Requires Windows 10 1903+ / Windows 11 x64 and a working
Vulkan graphics driver. Python and SDL are included.

**Linux AppImage:** make the AppImage executable, then open it. The package
bundles its ordinary runtime dependencies; glibc and GPU drivers come from
the system. Shipped ELF files are audited against a glibc 2.39 ceiling.
If FUSE is unavailable, launch with `--appimage-extract-and-run`.

**Linux Flatpak:** install the bundle with
`flatpak install --user ./mgsone-recomp-alpha.1-linux-x86_64.flatpak`,
then open MGS Recomp. It uses the Freedesktop 25.08 runtime. Put the original
BIN/CUE pairs in Downloads or on SD storage for the import browser.

In the launcher, choose both original discs, select **Verify and import both
discs**, then **PLAY**. Game data is prepared locally. No compiler or separate
Python installation is required by the application packages.

For SteamOS, prepare in Desktop Mode, add the application as a non-Steam game,
select Steam Input's **Gamepad** layout and use `--big-picture`.
This is not a Steam Deck compatibility certification. Detailed instructions:
[Linux / installation](https://github.com/coccofresco/mgsone-recomp/blob/alpha.1/docs/mgs-alpha-install.md),
[SteamOS](https://github.com/coccofresco/mgsone-recomp/blob/alpha.1/docs/mgs-steamos.md),
[Windows](https://github.com/coccofresco/mgsone-recomp/blob/alpha.1/docs/mgs-windows.md).

## Included

- Native recompilation of the USA v1.0 game, with selected decompiled C bridges.
- Original-asset Vulkan presentation, fixed 16:9 and selectable 720p–4K output.
- Floating-point poses, per-pixel lighting, distance-aware ambient occlusion,
  modern water and particles, CRT/RGB display effects and temporal effects.
- Native launcher and host pause menu, SDL controller support, Steam Input
  routing, device selection, deadzone settings and input diagnostics.
- Fast loading and automatic disc 1 to disc 2 transition.
- Persistent memory cards, quick-state slots, save backups and diagnostic export.
- The latest shared source-asset rendering fixes on Linux and Windows.

**F5 / F9** save/load a quick state, **F6 / F7** select a slot,
**Esc / R3** open the host menu and **F11** toggles fullscreen.
The default presentation is 4K and starts windowed; lower the resolution in
Settings for your GPU.

Experimental 60 FPS presentation interpolation is in the runtime source but
is currently a developer opt-in, not enabled by the normal launcher.
Gameplay retains its original 30/20 Hz timing.

## Known limits and reporting

**UNTESTED applies to these released application packages.** Previous isolated
development checks and the passing source-only GitHub CI do not establish a
complete playthrough, final-package visual correctness or platform acceptance.
Complete boss fights, jeep combat, continuous campaign completion, physical
Steam Deck/Steam Input behavior and native Windows hardware acceptance remain
open. Graphics, audio, input, loading or save/restore regressions may occur.

Keep **ordinary in-game Codec saves and backups**, not just quick states.
Alpha savestate compatibility may change. Do not overwrite an existing
portable user-data folder when extracting a newer build. No automatic updater
or telemetry is included.

[Report a reproducible issue](https://github.com/coccofresco/mgsone-recomp/issues)
with `alpha.1`, OS, GPU/driver, settings, disc, difficulty, location and steps.
Inspect diagnostic exports before sharing them. Do not attach discs, extracted
assets or full memory savestates to public issues.

The matching source archives include the build and import scripts, notices
and dependency provenance; regenerate game translations from your own discs.
Third-party license scopes remain as documented in
[THIRD_PARTY_NOTICES](https://github.com/coccofresco/mgsone-recomp/blob/alpha.1/packaging/THIRD_PARTY_NOTICES.md).
This is an independent, unofficial project.
