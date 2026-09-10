# RecompOne reference review — 2026-09-10

Repository: https://github.com/BlackLabelHQ/RecompOne
Inspected revision: `0409bc227ab41149e27985383eafd0492f2e6afc` (2026-09-08).
Local reference checkout: `.local/recompone`. MIT license.
No source imported into the runtime during this review.

RecompOne translates MIPS code to C# and supplies a .NET runtime. It is
not a directly interchangeable SwanStation/libretro donor. Running MGS
would require its own port/configuration and validation.

## Relevant findings

- Latest commit restructures emission per overlay, corrects PAL frame
  pacing and GPU-thread handling, and extends SDK/audio support.
- PsyQ signature auto-configuration is explicitly experimental. This is
  relevant to the missed VSync/interrupt wrappers in our Italian executable.
- `Host/FrameClock.cs` uses 50 Hz for PAL and 60 Hz for NTSC; it reinforces
  the need to distinguish regional timing from CPU execution speed.
- `Gpu/Interp/TransformInterp.cs` implements experimental transform matching
  and pose interpolation, including discontinuity thresholds. Potentially
  useful for later presentation work; not needed for current native boot.
- `Bios/BiosB.cs` routes B0:0E to an OpenTh implementation that allocates a
  host-side used slot; B0:10 ChangeTh simply breaks. The inspected OpenTh
  body does not install its entry/SP/GP arguments into an executable thread
  context. It therefore cannot currently validate MGS's BIOS thread switching.

## Use in this project

Keep SwanStation as the execution oracle. Use RecompOne as a secondary
source reference for PsyQ matching, regional pacing and future transform
interpolation. Do not infer MGS compatibility or a validated Italian 16:9
patch from its general-purpose runtime. We have not built or executed a
RecompOne MGS port, and make no gameplay claims for it.
