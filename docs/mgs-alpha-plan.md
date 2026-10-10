# First GitHub alpha: release plan and X360 references

## Public alpha.1 decision — 2026-10-10

The project is now public at https://github.com/coccofresco/mgsone-recomp.
The publisher explicitly chose to release Linux AppImage/Flatpak and Windows
ZIP as **alpha.1 — UNTESTED**, before full platform/gameplay qualification.
The requirements below remain the qualification backlog, not an assertion
that the public alpha satisfies them. Source-only CI has passed on GitHub;
package identity/inventory checks do not change the UNTESTED label.

`packaging/alpha.json` records public distribution readiness separately from
`validation_status: untested`. See the current
[release notes](../packaging/RELEASE_NOTES.md). The preparation notes below
describe the earlier internal candidates and are retained as history.

## Original preparation plan

Prepared 2026-10-06. Target: SteamOS desktop and Big Picture, in **AppImage and Flatpak**, followed
by a public GitHub prerelease after the gates below. The current branch is
`presentation/source-meshes`; no GitHub remote is configured. Preparation does
not imply that the candidate has been published or the campaign certified.

## What the Xbox 360 recompilations teach us

Primary sources, checked on 2026-10-06:

- [Unleashed Recompiled README](https://github.com/hedge-dev/UnleashedRecomp/blob/main/README.md)
  describes validated game import, controller-accessible settings, selectable
  controller icons, shader compilation during asset loading and graphics
  improvements beyond the original GPU. Its optional usability changes are
  useful precedents for separating presentation preferences from game rules.
  The [release history](https://github.com/hedge-dev/UnleashedRecomp/releases)
  also emphasizes preserving user files during updates and improving errors
  and save-related behavior. For MGS, apply these principles to two-disc
  installation, persistent profiles and clear save feedback. Ultrawide and
  unlocked simulation rates are not our target: retain fixed 16:9 and the
  original 30/20 fps logic.
- [Marathon Recomp](https://github.com/sonicnext-dev/MarathonRecomp) currently
  identifies itself as active development, not ready for public use. Its
  documented user directories, `portable.txt` mode, install validation and
  separation between updates and saves are useful design references, not
  proof of a finished release process. The keyboard configuration and Steam
  Deck/controller caveats reinforce the need to test installation and input
  as product features. MGS's first launcher follows the separate-profile and
  optional-portable model; it does not reuse Marathon code or artwork.

The following priorities are project decisions derived from these references
and MGS's gameplay, not claims that every donor implements them.

## Implemented in this preparation

| Area | Current behavior | Verification |
| --- | --- | --- |
| First run | Native SDL/controller launcher; import both USA v1.0 CDs, verify complete track hashes and original executable, prepare the source asset cache locally | Unit tests include rollback, wrong disc order and preserving existing saves; real retail import in an isolated profile |
| User data | XDG data/config, `portable.txt`, explicit isolated profile, profile lock | Path/lock tests; actual launch with spaces in profile path |
| Updates and backup | Application separated from user data; explicit memory-card/state ZIP backup; existing ZIPs never overwritten | Save allowlist, symlink exclusion, concurrent-run refusal |
| Video | 720/1080/1440/2160 presentation heights, fixed 16:9; windowed default, fullscreen and VSync preferences | Config round trips and native boot; modern raster separated from original 1x oracle |
| Audio/input | Volume/mute and keyboard mapping, persisted for next launch | Validation rejects invalid values and reserved host shortcuts |
| Support | Build identity, runtime hash, local diagnostics ZIP containing metadata and log tail with known paths redacted | Tests exclude retail files and saves; no automatic upload |
| Distribution | Explicit package inventory, per-file hashes, checksums, source snapshot and ABI requirements | Audit rejects extra or modified files; generated translations and retail payload excluded from source archive |
| GitHub | Bug/QoL forms and source-only Linux CI | Workflow prepared; not yet executed on GitHub |

Existing capabilities carried into the candidate include native quick states,
original memory cards, fast loading and automatic disc change. Their evidence
remains in the corresponding runtime documents; this work does not recertify
every prior gameplay or Vulkan test.

Preparation evidence: **1,326 runtime tests passed, 31 skipped; 165 Python
tests passed** in the working tree. Rebuilding the four C++ test targets from
the exported source archive produced **1,577 passes, 35 skips, zero failures**;
Python tests without local retail fixtures produced **155 passes, 10 skips**.
That build used the existing cached Vulkan header dependency because system
headers and network DNS were unavailable. This verifies the exported source
on the current host, not the proposed Ubuntu compatibility baseline.

Both retail discs were imported and fully verified in a new profile with
spaces in its path. Native boot reached 1,800 VBlanks at the default 4K
configuration and again using the packaged executable, launched from `/tmp`,
with the 1080p preference. Both exited successfully with native raster 1x.
These were headless tests: configured presentation dimensions are verified,
but the resulting GPU image was not measured. Save backup and diagnostics
ZIPs passed integrity/content checks; personal project saves remained unchanged.
The [machine-readable evidence](mgs-alpha-evidence.json) records hashes and
the unverified acceptance items.

## Required before a public alpha

| Priority | Work | Acceptance criterion |
| --- | --- | --- |
| P0 | Public Linux build baseline | Build privately from both retail discs on the declared baseline; record actual glibc/libstdc++ requirements; install and play on a clean matching system. The SDK-linked candidate now imports glibc 2.38 symbols; physical SteamOS acceptance is still pending. |
| P0 | Host menu and save feedback | Implemented: Esc/R3 menu, save/load feedback, confirmed quit and acknowledged pause with pacing rebase. Native pause/save/load/save/resume probe added; Vulkan/controller physical acceptance pending. |
| P0 | Controller acceptance | Keyboard and gamepad launch, disconnect/reconnect and mapping tested; explicit virtual port selection for Psycho Mantis, with the original encounter behavior preserved. Do not substitute a gameplay bypass. |
| P0 | Mandatory story transitions | Later Mantis/caves, retail Hind roof return and jeep-to-ending transition contracts now pass; see [2026-10-07 evidence and limits](mgs-alpha-gameplay-soak.md). Full fights, alternatives and continuous campaign remain separate acceptance items. |
| P0 | Endurance and resource stability | Single-process headless workload passed 180 segments, then 56. The [original-asset Vulkan matrix](mgs-asset-presentation-validation.md) exposed and fixed PCX loss during fast loading. On 2026-10-08 the RTX 4060 passed eight save/restore cycles at 4K, with no growth in measured RSS, descriptors or tracked Vulkan allocations after warm-up. Longer physical GPU endurance remains pending. |
| P0 | Physical GPU acceptance | Local runtime: eight scenes at true 4K, required Vulkan validation and 21 GPU integration cases pass on RTX 4060 / 610.57.04, with captures and GPU timestamps. See [evidence and limits](mgs-physical-gpu-evidence.json). Still verify the final AppImage/Flatpak on SteamOS, lower-resolution selection, full intro/Codec, disc change and save/reload in a new process; sampled runtime results do not certify packaged delivery or other GPUs. |
| P0 | Release hygiene | Review dirty source snapshot and provenance, commit/tag the reviewed state, verify matching source/binary checksums, run public CI, create an explicit prerelease with known limits. Do not mark stable/latest automatically. |

An alpha may have documented incomplete coverage. A blocker in an obligatory
story path needs a fix or an explicit supported-scope restriction; a successful
isolated fixture must never be presented as a continuous playthrough.

## Next quality-of-life options

| Priority | Option | Boundary and reason |
| --- | --- | --- |
| P1 | Focus-loss pause and optional background mute | Host control only; resume through the same safe pause boundary. Test cutscenes, Codec, loading and save operations. |
| P1 | Controller presets, glyphs, dead zone and remapping | Keep original buttons and guest port semantics; profiles for keyboard/Xbox/PlayStation, tested hotplug. |
| P1 | Save-slot browser with preview and timestamp | Show build/state compatibility; back up before replacement; retain original Codec saves as durable progression. Resume last session must be explicit. |
| P1 | English/Italian launcher and controller navigation | Improve first-run accessibility independently of the separate Japanese translation project. |
| P1 | Presentation accessibility | Adjustable CRT intensity, bloom, motion trails and camera-shake reduction where host presentation can isolate them; approved defaults retained, per-effect visual comparisons documented. |
| P1 | Remember window/monitor and UI scale | Fixed scene aspect remains 16:9; readable host UI and subtitles at 720p–4K without rewriting original layout logic. |
| P1 | Shader/pipeline warm-up and progress display | Measure first-encounter hitches before choosing cache strategy; invalidate by renderer/shader/device identity. Avoid a fake loading bar or broad simulation acceleration. |
| P2 | 2× presentation interpolation | Only after animation/effect history, pause and camera cuts have regression coverage; game logic continues at original cadence. |
| P2 | Windows and Steam Deck packages | Windows needs the native coroutine backend and platform validation; Steam Deck needs a compatible ABI, controller-first UI and performance testing. No compatibility badge before evidence. |

Automatic updates, cloud saves, broad gameplay cheats and a new translation
integration are outside the first candidate. Manual versioned updates and
explicit backups provide a small, inspectable initial release path.

## Build and package procedure

Dependencies: Python 3.11+, CMake, Ninja, a C++20 compiler, SDL2/Vulkan
development packages, `glslc`, Git and binutils (`readelf`, `strip`). Tk is needed only for the optional legacy editor; the default launcher is native SDL. Ubuntu 24.04 is the proposed initial build/test
baseline, not a compatibility claim until a native package has been built and
tested there. No Docker/Podman baseline build was available in this preparation
environment.

From a clean source checkout or source archive, supply private retail inputs:

```bash
bash scripts/build_alpha.sh '/path/disc1.cue' '/path/disc2.cue'
python3 scripts/package_alpha.py data/releases/alpha-candidate
python3 scripts/package_alpha.py --audit data/releases/alpha-candidate/mgs-recomp-0.1.0-alpha.1-dev-linux-x86_64
```

The build script imports into the ignored `data/alpha-build` profile, reuses it
only after verification on subsequent builds, generates the campaign code,
installs verified decompilation bridges, then builds/tests the native runtime.
It does not use or migrate personal saves. The recipe is provided for clean
builds; the preparation run reused the existing generated campaign and rebuilt
the changed runtime/configuration code. A full clean native rebuild remains a
release gate. Packaging requires Git metadata: when starting from the source
archive, initialize a local Git repository first, or build from the eventual
tagged checkout. An initialized repository also needs an initial source commit
before packaging can record its identity. The public CI uses stubs only to test source infrastructure;
it cannot certify the playable binary without private retail inputs.

The output includes application and source archives, `SHA256SUMS`, an unpacked
candidate and `package-report.json`. `build-info.json` records the original
and stripped runtime hashes, current commit, dirty-worktree status, source
snapshot hash and actual dynamic-library ABI requirements. The source archive
excludes retail payload and generated C++ translations; regenerate these from
your discs with the supplied build recipe. Keep binary and source archives
together. Do not upload the whole repository working directory or `data/`.

See [installation guide](mgs-alpha-install.md), the draft
[release notes](../packaging/RELEASE_NOTES.md), and
[GitHub CI](../.github/workflows/ci.yml). Publication is still pending a chosen
GitHub destination and the release gates above.

The preparation environment lacks the Tk shared library required by its
Python installation, so the GUI could not receive a desktop acceptance test.
The launcher reports this dependency with terminal alternatives; the actual
disc import and native boot were exercised through those terminal commands.

The [SteamOS implementation and packaging contract](mgs-steamos.md) supersedes
the initial desktop-only launcher and platform assumptions. The original alpha
evidence remains historical; the next report covers the native UI and both formats.


## 2026-10-07: shutdown and save recovery

Candidate dev.4 adds cooperative guest/ticker shutdown, paused signal handling,
launcher-to-runtime signal forwarding and an inherited profile lock; failures
of the emergency bounded shutdown now return 45 rather than success.
Save publication syncs the temporary and parent directory, rotates three
previous states/pre-session cards, preserves pre-load disk-card progress,
provides dated/versioned slot history and recovery notices, and exports history
in complete exclusive ZIPs. NSV12 is unchanged. The dev.3/dev.4 update/readback
pair and isolated native lifecycle matrix pass. See
[machine-readable evidence](mgs-shutdown-saves-evidence.json) for exact build
identity, fault injection, test counts and remaining physical-hardware limits.
