#pragma once

#include "runtime/gpu/gpu.h"
#include <cstdint>

namespace ps1::gpu {

// Controls how the host turns the PS1 display registers into a VRAM aperture.
// Programmed is the hardware-faithful default; Nominal is retained as a
// diagnostic fallback for titles with incomplete display-environment setup.
enum class DisplayAreaPolicy {
  Programmed,
  Nominal,
  DonorCanvas,
};

enum class DisplayScanoutFormat : uint8_t {
  RGB555,
  PackedRGB888,
};

// "Full signal" is not one universally agreed pixel canvas. Keep the two
// pinned, independently observed donor conventions selectable instead of
// silently blessing either set of crop constants as hardware truth.
enum class DisplaySignalProfile : uint8_t {
  SwanStationNoCrop,
  BeetleUncropped,
};

// DisplayState does not currently retain the guest CRTC field.  Keep that
// absence explicit instead of guessing a parity from the host frame number.
enum class DisplayScanoutField : uint8_t {
  NotApplicable,
  Unknown,
  Even,
  Odd,
};

struct DisplayPoint {
  uint32_t x = 0;
  uint32_t y = 0;
};

struct DisplayExtent {
  uint32_t width = 0;
  uint32_t height = 0;
};

// Raw, title-independent CRTC state.  sourceWordX is the hardware-effective
// GP1(05) address in 16-bit VRAM words: bit zero is ignored by the PS1 GPU.
// gpuStat retains the GP1(08) mode and GP1(03) display-disable bits.
struct RawDisplayCrtcInput {
  uint32_t sourceWordX = 0;
  uint32_t sourceY = 0;
  uint32_t horizontalStart = 0;
  uint32_t horizontalEnd = 0;
  uint32_t verticalStart = 0;
  uint32_t verticalEnd = 0;
  uint32_t gpuStat = 0;
  bool modeSet = false;
  bool areaSet = false;
  bool horizontalRangeSet = false;
  bool verticalRangeSet = false;
  DisplayScanoutField field = DisplayScanoutField::Unknown;
  DisplaySignalProfile signalProfile = DisplaySignalProfile::SwanStationNoCrop;
};

struct DisplayScanoutPlan {
  // Complete donor output canvas, including its selected border convention.
  DisplayExtent canvas{};
  // Placement and size of the VRAM-backed image inside the canvas.
  DisplayPoint destinationOrigin{};
  DisplayExtent activeExtent{};

  // Byte-addressed horizontal source preserves packed-24-bit phase.  It wraps
  // at 2048 bytes; sourceY wraps at 512 rows.  rowStep is the number of VRAM
  // rows between source lines (two for an interleaved 480-line field).
  uint32_t sourceByteX = 0;
  uint32_t sourceY = 0;
  uint32_t rowStep = 1;

  DisplayScanoutFormat format = DisplayScanoutFormat::RGB555;
  bool progressive = true;
  DisplayScanoutField field = DisplayScanoutField::NotApplicable;
  bool enabled = true;
};

struct DisplayAperture {
  uint32_t x = 0;
  uint32_t y = 0;
  uint32_t width = 320;
  uint32_t height = 240;
  bool usedHorizontalRange = false;
  bool usedVerticalRange = false;
  // Width/height stay allocation-safe (at least 1x1). A disabled aperture is
  // presented as black and must not sample that placeholder texel.
  bool enabled = true;
};

struct OutputViewport {
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
};

// Decode the nominal pixel dimensions selected by GP1(08)/GPUSTAT.
uint32_t nominalDisplayWidth(const GPU::DisplayState &state);
uint32_t nominalDisplayHeight(const GPU::DisplayState &state);

// Convert the current GPU latch to hardware-effective raw CRTC input.  The
// latch has no guest-field parity, so interlaced inputs remain Unknown.
RawDisplayCrtcInput rawDisplayCrtcInput(const GPU::DisplayState &state);

// Pure scanout geometry.  Programmed and Nominal retain the legacy aperture
// policies; DonorCanvas places the programmed image in the selected pinned
// donor canvas and exposes its borders rather than cropping them away.
DisplayScanoutPlan deriveDisplayScanoutPlan(
    const RawDisplayCrtcInput &input,
    DisplayAreaPolicy policy = DisplayAreaPolicy::Programmed);

// Derive the visible VRAM aperture. Horizontal GP1 ranges are measured in GPU
// dot clocks and therefore use a mode-dependent divider; vertical ranges are
// scanlines (doubled for 480-line interlace).  This legacy, word-addressed
// view is retained as a backward-compatible wrapper around the scanout plan.
DisplayAperture
deriveDisplayAperture(const GPU::DisplayState &state,
                      DisplayAreaPolicy policy = DisplayAreaPolicy::Programmed);

// Fit a physical display aspect into the drawable without stretching or
// cropping. PS1 pixel grids are not square-pixel canvases, so presentation
// defaults to the console's 4:3 output rather than width/height of VRAM.
OutputViewport fitDisplayViewport(int drawableWidth, int drawableHeight,
                                  int aspectWidth = 4, int aspectHeight = 3);

} // namespace ps1::gpu
