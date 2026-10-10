# MGS Recomp 0.1.0-alpha.1 — release draft

Status: local candidate, **not published and not approved for public release**.
The candidate version has a `-dev` suffix until the release gates in
`docs/mgs-alpha-plan.md` are satisfied. SteamOS/Linux x86_64: AppImage and Flatpak, desktop and Big Picture.
Windows x64 also has a separate ZIP candidate with embedded Python, SDL2 and
the same Vulkan shaders. See `docs/mgs-windows.md` and the Windows evidence for
cross-platform captures, save/restore, disc transition and launcher checks.

MGS Recomp runs the USA v1.0 game as native code with a modern Vulkan
presentation, default 3840×2160 at fixed 16:9. Original simulation timing
remains separate from presentation settings. You must import both original
USA v1.0 discs; the application includes no game data or BIOS.

## Included in this candidate

- First-run native SDL/controller launcher with verified two-disc import and local
  preparation of the original asset cache.
- Presentation choices of 720p, 1080p, 1440p and 2160p, fullscreen/windowed
  startup, VSync, volume, mute, keyboard mapping and fast-loading preferences.
- Separate user profiles, optional portable mode, save backups and local
  diagnostics export. Updating the application preserves external user data.
- Vulkan pause menu with save/load feedback, slot selection, original controller
  port switching and quit confirmation; acknowledged guest/audio pause and focus-loss handling.
- Shared controller selection for launcher, game and menu; Steam virtual gamepad
  preference, 743 additional Linux mappings, persistent device/deadzone/label
  preferences and live input testing. Automatic neutral rearming after hotplug,
  remapping and host-menu pause prevents held-button leakage.
- Cooperative shutdown during gameplay and host pause; window/menu quit and
  termination signals stop and join the native worker before teardown.
- Synced atomic save publication, three previous savestate versions and
  pre-session memory-card backups, recovery notices, dated/versioned slot
  browser and backup export including history. NSV12 remains unchanged.
- Existing native quick states, original memory-card saves and seamless
  disc change; modern mesh/effect presentation and fast stage loading.
- Checksummed application/source archives, build identity and dependency
  requirements. See README.md for setup, controls and support.

## Known limitations

This is experimental software. Continuous campaign completion, every full
boss fight and jeep combat are not certified. Isolated scenario success is
not a claim that every path is completable. Save-state compatibility can
change between alpha versions; keep original in-game Codec saves and backups.

The host menu and controller routing are implemented and have automated native
checks, but physical Steam Input/hotplug and Vulkan acceptance remain pending.
The native launcher uses English labels and no system Tk installation.

The SDK-linked native executable now requires glibc 2.38; AppImage dependencies
are audited against a 2.39 ceiling and Flatpak uses Freedesktop 25.08. This does
not replace installation and graphics testing on SteamOS. Continuous campaign
completion and mandatory story-path coverage remain separate release gates.

No automatic updater or telemetry is included. Issue templates and public
source-only CI are prepared; GitHub execution awaits repository creation.
