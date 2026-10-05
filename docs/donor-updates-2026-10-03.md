# Donor review — 2026-10-03

The GitHub default-branch heads below were fetched and compared with the local
baselines. Review refs are `refs/remotes/review/2026-10-03` in the existing
`.local` reference repositories. Their working trees and the patched SwanStation
core were kept at their established baselines. No upstream implementation was
imported into our runtime by this review.

| Project | Previous revision | Reviewed head | Relevant changes / decision |
| --- | --- | --- | --- |
| [SwanStation](https://github.com/libretro/swanstation/compare/7f69c199ed88d5723f71dd3a6e9c1b7a45b535a6...b6c30a7b270a3f68ac41f268eafdfa678d17dea2) | `7f69c199` | `b6c30a7b` (Sep 17) | Three files: Apple instruction-cache/JIT platform guards and iOS/tvOS CI. No PSX emulation change relevant to our Linux interpreter oracle. Keep the instrumented pin. Its local `origin` points to another local checkout; the review fetched **GitHub directly**, not that potentially stale local mirror. |
| [FoxdieTeam/mgs_reversing](https://github.com/FoxdieTeam/mgs_reversing/compare/217509eae471629b1a53633b816c61adfef9ed5c...2d9b9f824a422425359d980892629495d059de84) | `217509ea` | `2d9b9f82` (Oct 3) | 1,016 changed files. README now marks caves `s09a/s09ar`, tower `s11a`, snowfield `s12b/s12c` complete; `rank` and cargo elevator `s14e` are in progress. New enemy work, hazard/collision cleanup (`near`, `trap`, `online`), effect/animation naming, VR sound overlay. Best source for future native actors and progression fixes. These are **Integral** addresses/structures; USA entry points still need independent verification. |
| [PSXRecomp](https://github.com/RetroPortingToolKit/psxrecomp/compare/ed55299be34710a90fc080484a83e8634bd41fa9...e8e3ee2d415ec4a7aa9ff0a10825b59b3f87354f) | `ed55299b` | `e8e3ee2d` (Oct 2, upstream timezone) | AOT overlay discovery/packaging, PGXP, netplay scheduling/history/digests, memory-card SIO cleanup, snapshot validation. Study the contracts and regression cases; this is a different runtime and its PolyForm Noncommercial license is not a GPL code donation. |
| [RecompOne](https://github.com/BlackLabelHQ/RecompOne/compare/0409bc227ab41149e27985383eafd0492f2e6afc...8d0091cf4877b27c2d526106a78c75f6b0ef97b2) | `0409bc22` | `8d0091cf` (Oct 1) | 47 files. Function/jump-table discovery, intra-function link-return handling, CD callback/result changes, timers/interrupts, interpolation/PGXP stability and crash logging. Keep our already verified PsyQ entries; do not regenerate USA boundaries blindly from updated heuristics. |
| [Alexbeav/MGS](https://github.com/Alexbeav/metal-gear-solid-recomp/compare/ea9b7bc87de1354d032fe3ffd321ed17a0fa4307...94807c835e4c16df255c4c11978cbcb51020734f) | `ea9b7bc8` | `94807c83` (Sep 30) | Framework/UI repins (BIOS setup and CHD picker), configurable debug tools, manifest/setup-name CI and documentation. These changes do not establish full MGS playability. |
| [OpenGTPS1](https://github.com/GTTeancum/OpenGTPS1) | `d9e6af5d` | unchanged | No new default-branch commit. |
| [Rex](https://github.com/Jayveer/Rex) | `42097c20` | unchanged | No new default-branch commit. |
| [mint](https://github.com/virusek20/mint) | `ed1be793` | unchanged | No new default-branch commit. |
| [MGS-KMD-Noesis](https://github.com/Jayveer/MGS-KMD-Noesis) | `42bc9ed2` | unchanged | No new default-branch commit. |

Specific upstream evidence worth revisiting when a TAS finds a matching failure:

- RecompOne [`d81dec8`](https://github.com/BlackLabelHQ/RecompOne/commit/d81dec8):
  `jr ra` can target an internal link return; its emitter now retains those
  labels. CD changes distinguish data-ready from command completion, add read
  completion callbacks and correct `CdInit`'s result. Our MGS CD SDK is mostly
  translated original code; these are diagnostic clues, not interchangeable HLE.
- PSXRecomp [`18e3030`](https://github.com/RetroPortingToolKit/psxrecomp/commit/18e3030):
  validate snapshot sections before mutating live state.
- PSXRecomp [`439423a`](https://github.com/RetroPortingToolKit/psxrecomp/commit/439423a):
  preserve the never-decoded MDEC state across snapshot restoration.
- PSXRecomp [`d4c45ad`](https://github.com/RetroPortingToolKit/psxrecomp/commit/d4c45ad):
  remove title-specific SIO card IRQ repair; follow protocol ownership instead.
- PSXRecomp `runtime/src/netplay_state_digest.c` at the reviewed head hashes
  device snapshots and native VRAM separately. Our first TAS witness covers
  **all main RAM plus native resume PC**, not a claim of complete device equality.

The new [TAS workflow](mgs-tas.md) is implemented in our existing C++ runtime and
libretro harness. SwanStation remains the independent emulator oracle; its
states, movie clock and RAM layout must not be treated as native snapshots.
