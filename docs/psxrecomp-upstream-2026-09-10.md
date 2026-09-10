# PSXRecomp upstream review — 2026-09-10

Inspected source at `ed55299be34710a90fc080484a83e8634bd41fa9`
(master, latest commit 2026-09-09), locally in `.local/psxrecomp-upstream`.
Repository: https://github.com/RetroPortingToolKit/psxrecomp
This is a separate architecture from our imported Crash 3 engine.
No upstream implementation was copied into our runtime during this review.

## Relevant updates

- **BIOS/kernel:** v4 links recompiled OpenBIOS and an optional compatible
  retail BIOS. `runtime/src/bios_hle.c` explicitly leaves thread operations,
  WaitEvent and the CD/card stack to the recompiled BIOS. This is particularly
  relevant to MGS's MTS scheduler; it does not provide a drop-in HLE thread fix.
- **CD mounting:** `7c11d57` (2026-09-07, Alexbeav) preserves CUE track metadata
  through the launch adapter. Relevant to retaining disc structure when using
  the Italian image/CUE pairs. Its tests cover parser/adapter behavior, not
  proof that MGS boots.
- **C interoperability:** `d872546` (2026-09-07, Alexbeav) fixes C11 retry-label
  and trace-declaration compatibility. Our generated code is C++, so this is
  not the cause of the current MGS startup stall.
- **Vulkan aspect:** `6ab221a` (2026-09-06) forwards the selected display aspect
  into the Vulkan presentation paths. This corrects output letterboxing;
  it is not an MGS camera/FOV patch.
- **Wide overlays:** `912c44c` and `ae2fc16` extend native-wide HUD/background
  handling and correct textured-dot placement. Useful later when assessing
  original HUD geometry separately from world geometry.
- **Regression history:** `24c6c56` (2026-09-07) reverts an interlaced rendering
  change to restore retail BIOS boot. The recent renderer changes should be
  considered individually, with oracle regression coverage.
- **Host pause:** `568513b` (2026-09-08) suppresses false freeze detection while
  the host is intentionally paused.

## Architectural implications

The current README describes runtime overlay capture/compilation and a
transient interpreter fallback. We must measure native coverage rather than
infer fully native execution from a successful upstream boot. Its current
`psx_cycles` implementation advances guest cycle counts and services device
boundaries, unlike our inherited wall-clock VBlank pacing. Older upstream
notes describe overclock scaling; those historical descriptions alone do not
establish the current behavior of a 165% setting.

The release notes call the optional temporal effect **frame blending**.
It must not be equated with the future 2x interpolation requested for MGS.

## Source provenance

The current upstream LICENSE is PolyForm Noncommercial 1.0.0; our imported
engine retains GPL licensing. Keep the upstream checkout as a reference,
record provenance for any later imports, and do not relabel upstream code
as GPL. This review imports no upstream source into the engine.

## Decision for MGS

Continue the existing SwanStation oracle/native bring-up. Use the upstream
BIOS-first design as an architectural reference if the MTS/kernel surface
outgrows the current HLE runtime. Keep Vulkan renderer feature adoption after
verified game boot, as requested. No upstream commit examined here proves
Italian MGS support or provides a validated Italian widescreen code.
