# Notices

MGS Recomp uses the GPL-3.0 PS1Recomp runtime inherited from the local Crash 3
recompilation project. The included LICENSE is the engine's license; upstream
file notices remain in the accompanying source archive. Engine provenance is
recorded in `docs/engine-provenance.json`.

Included dependencies: ELFIO (MIT), fmt (MIT), toml11 (MIT); their licenses
are included in `licenses/`. GoogleTest is used only to build tests; its
license is in the source archive. The unpacked developer archive uses system
Python, SDL2 and the Vulkan loader. AppImage bundles Python, SDL2, the Vulkan
loader and ordinary SDK dependencies with separate copyright notices; GPU
drivers remain system-provided. Flatpak uses its declared Freedesktop runtime.
The Windows ZIP bundles the official embeddable CPython distribution (its
combined license file is `python/LICENSE.txt`), SDL2 (zlib), and LLVM's
libunwind runtime (Apache-2.0 with LLVM exceptions). LLVM/libc++ and MinGW-w64
runtime notices are under `licenses/`; pinned upstream download URLs and hashes
are in `windows-dependencies.json`. Windows supplies UCRT; the graphics driver
supplies the Vulkan loader. No Vulkan driver or Wine component is distributed.
Tk is a system dependency for the optional legacy editor only and is not
included in the portable formats. The native launcher uses SDL
and Dear ImGui 1.91.9b (MIT); its license is included and vendored source hashes
are recorded in `engine/third_party/imgui/provenance.json`. Vulkan shaders are
compiled from the project's source.

Ambient occlusion adapts the SSAO shaders of Godot 4.7.2 (MIT), derived from
Intel ASSAO (Copyright 2016 Intel Corporation, MIT). Both licenses are included
as `licenses/Godot.txt` and `licenses/Intel-ASSAO.txt`. The unchanged reference
shaders and pinned upstream hashes are in `engine/third_party/godot_ssao/`;
the runtime uses the project's Vulkan adapter, without linking Godot.
Distance-dependent AO radius unprojection is adapted from N8python/N8AO
(CC0-1.0), with project-specific radius bounds and viewport normalization.
The pinned reference shader and provenance are in
`engine/third_party/n8ao_radius/`; `licenses/N8AO-CC0.txt` contains its license.
No MXAO or HBAO+ source is included in this adapter.

SDL_GameControllerDB (zlib) is included unchanged with its license under
`licenses/SDL_GameControllerDB.txt`; the source snapshot's SHA256 provenance
is in `engine/third_party/SDL_GameControllerDB/provenance.json`. Dusklight and
Aurora were consulted for controller ownership, hotplug and preferences;
no source from these projects was copied into the controller implementation.

Research/reference projects include FoxdieTeam/mgs_reversing, SwanStation,
PSXRecomp, RecompOne, Jayveer/Rex and MGS-KMD-Noesis; source attribution and
porting evidence remain in the repository documentation. Xbox 360 release
research used hedge-dev/UnleashedRecomp and sonicnext-dev/MarathonRecomp;
no launcher code or artwork was copied from those projects.

The texture module specifically adapts `source/libdg/text.c` from
FoxdieTeam/mgs_reversing at commit
`217509eae471629b1a53633b816c61adfef9ed5c`. Its provenance and changes are
identified in `engine/ps1Runtime/src/games/mgs_decomp/texture.c` and
`docs/mgs-decomp-native-core.md`; these notices do not assign a new license
to the upstream work. Verify imported-code notices as part of release review.

No original game disc, executable image, BIOS, extracted assets, memory card
or savestate is included. The installer requires the user's original USA
v1.0 discs and prepares those files locally. Metal Gear Solid and its marks
belong to their respective owners. This is an unofficial project.
