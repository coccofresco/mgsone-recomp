#include "runtime/gpu/display_geometry.h"
#include <algorithm>
#include <cstdint>

namespace ps1::gpu {
namespace {

constexpr uint32_t VRAM_ROW_BYTES = GPU::VRAM_WIDTH * sizeof(uint16_t);

struct SignalRange {
  uint32_t horizontalStart;
  uint32_t horizontalEnd;
  uint32_t verticalStart;
  uint32_t verticalEnd;
  uint32_t horizontalTotal;
  uint32_t verticalTotal;
};

constexpr SignalRange SWAN_NTSC_SIGNAL{488, 3288, 16, 256, 3413, 263};
constexpr SignalRange SWAN_PAL_SIGNAL{487, 3282, 20, 308, 3406, 314};
constexpr SignalRange BEETLE_NTSC_SIGNAL{488, 3288, 16, 256, 3413, 263};
constexpr SignalRange BEETLE_PAL_SIGNAL{488, 3288, 20, 308, 3406, 314};

SignalRange signalRange(const RawDisplayCrtcInput &input, bool pal) {
  switch (input.signalProfile) {
  case DisplaySignalProfile::BeetleUncropped:
    return pal ? BEETLE_PAL_SIGNAL : BEETLE_NTSC_SIGNAL;
  case DisplaySignalProfile::SwanStationNoCrop:
  default:
    return pal ? SWAN_PAL_SIGNAL : SWAN_NTSC_SIGNAL;
  }
}

uint32_t horizontalDotClockDivider(uint32_t width) {
  switch (width) {
  case 256:
    return 10;
  case 320:
    return 8;
  case 368:
    return 7;
  case 512:
    return 5;
  case 640:
    return 4;
  default:
    return 1;
  }
}

uint32_t nominalDisplayWidth(const RawDisplayCrtcInput &input) {
  if (!input.modeSet)
    return 320;

  if ((input.gpuStat & (1u << 16)) != 0)
    return 368;

  switch ((input.gpuStat >> 17) & 3u) {
  case 0:
    return 256;
  case 1:
    return 320;
  case 2:
    return 512;
  default:
    return 640;
  }
}

bool vertical480(const RawDisplayCrtcInput &input) {
  return input.modeSet && (input.gpuStat & (1u << 19)) != 0;
}

bool interlaced(const RawDisplayCrtcInput &input) {
  return input.modeSet && (input.gpuStat & (1u << 22)) != 0;
}

uint32_t nominalDisplayHeight(const RawDisplayCrtcInput &input) {
  return vertical480(input) && interlaced(input) ? 480u : 240u;
}

uint32_t effectiveSourceWordX(uint32_t sourceWordX) {
  return (sourceWordX & ~1u) % GPU::VRAM_WIDTH;
}

uint32_t sourceBytesPerPixel(DisplayScanoutFormat format) {
  return format == DisplayScanoutFormat::PackedRGB888 ? 3u : 2u;
}

uint32_t wrapSourceByte(uint64_t sourceByte) {
  return static_cast<uint32_t>(sourceByte % VRAM_ROW_BYTES);
}

uint32_t roundedCrtcWidth(uint32_t pixels) {
  if (pixels == 1)
    return 4;
  return (pixels + 2u) & ~3u;
}

void deriveLegacyPolicy(DisplayScanoutPlan &plan,
                        const RawDisplayCrtcInput &input,
                        DisplayAreaPolicy policy, uint32_t nominalWidth,
                        uint32_t nominalHeight, uint32_t divider) {
  plan.canvas = {nominalWidth, nominalHeight};
  plan.activeExtent = plan.canvas;

  if (policy != DisplayAreaPolicy::Programmed)
    return;

  if (input.horizontalRangeSet) {
    const uint32_t programmedWidth =
        input.horizontalEnd > input.horizontalStart
            ? (input.horizontalEnd - input.horizontalStart) / divider
            : 0u;
    plan.canvas.width = programmedWidth;
    plan.activeExtent.width = programmedWidth;
  }

  if (input.verticalRangeSet) {
    uint32_t programmedHeight = input.verticalEnd > input.verticalStart
                                    ? input.verticalEnd - input.verticalStart
                                    : 0u;
    // Preserve the legacy wrapper's direct GPUSTAT interpretation even for a
    // synthetic state whose modeSet flag is false.
    if ((input.gpuStat & (1u << 19)) != 0 && (input.gpuStat & (1u << 22)) != 0)
      programmedHeight *= 2;
    plan.canvas.height = programmedHeight;
    plan.activeExtent.height = programmedHeight;
  }
}

void deriveDonorCanvasPolicy(DisplayScanoutPlan &plan,
                             const RawDisplayCrtcInput &input,
                             uint32_t nominalWidth, uint32_t nominalHeight,
                             uint32_t divider) {
  const bool pal = input.modeSet && (input.gpuStat & (1u << 20)) != 0;
  const SignalRange signal = signalRange(input, pal);
  const uint32_t heightShift = interlaced(input) ? 1u : 0u;
  const uint32_t sourceYShift =
      interlaced(input) && vertical480(input) ? 1u : 0u;

  plan.canvas.width = (signal.horizontalEnd - signal.horizontalStart) / divider;
  plan.canvas.height = (signal.verticalEnd - signal.verticalStart)
                       << heightShift;

  if (input.horizontalRangeSet) {
    if (input.horizontalEnd <= input.horizontalStart) {
      plan.activeExtent.width = 0;
    } else {
      const bool beetleProfile =
          input.signalProfile == DisplaySignalProfile::BeetleUncropped;
      uint32_t displayStart =
          std::min(input.horizontalStart, signal.horizontalTotal);
      uint32_t displayEnd =
          std::min(input.horizontalEnd, signal.horizontalTotal);
      // SwanStation quantizes both guest endpoints to the dot clock first.
      // Beetle derives origin and width from the raw clock ranges.
      if (!beetleProfile) {
        displayStart = (displayStart / divider) * divider;
        displayEnd = (displayEnd / divider) * divider;
      }
      const uint32_t displayTicks =
          displayEnd > displayStart ? displayEnd - displayStart : 0u;
      uint32_t activeWidth = roundedCrtcWidth(displayTicks / divider);
      uint32_t skippedPixels = 0;

      if (displayStart >= signal.horizontalStart) {
        plan.destinationOrigin.x =
            (displayStart - signal.horizontalStart) / divider;
      } else {
        const uint32_t clippedTicks = signal.horizontalStart - displayStart;
        skippedPixels = beetleProfile ? (clippedTicks + divider - 1u) / divider
                                      : clippedTicks / divider;
      }

      activeWidth -= std::min(activeWidth, skippedPixels);
      if (plan.destinationOrigin.x >= plan.canvas.width) {
        activeWidth = 0;
      } else {
        activeWidth =
            std::min(activeWidth, plan.canvas.width - plan.destinationOrigin.x);
      }
      plan.activeExtent.width = activeWidth;
      plan.sourceByteX =
          wrapSourceByte(static_cast<uint64_t>(plan.sourceByteX) +
                         static_cast<uint64_t>(skippedPixels) *
                             sourceBytesPerPixel(plan.format));
    }
  } else {
    plan.activeExtent.width = std::min(nominalWidth, plan.canvas.width);
  }

  if (input.verticalRangeSet) {
    if (input.verticalEnd <= input.verticalStart) {
      plan.activeExtent.height = 0;
    } else {
      const uint32_t displayStart =
          std::min(input.verticalStart, signal.verticalTotal);
      const uint32_t displayEnd =
          std::min(input.verticalEnd, signal.verticalTotal);

      if (displayStart >= signal.verticalStart) {
        plan.destinationOrigin.y = (displayStart - signal.verticalStart)
                                   << sourceYShift;
      } else {
        const uint32_t skippedRows = (signal.verticalStart - displayStart)
                                     << sourceYShift;
        plan.sourceY = (plan.sourceY + skippedRows) % GPU::VRAM_HEIGHT;
      }

      const uint32_t clippedStart =
          std::max(displayStart, signal.verticalStart);
      const uint32_t clippedEnd = std::min(displayEnd, signal.verticalEnd);
      uint32_t activeHeight = clippedEnd > clippedStart
                                  ? (clippedEnd - clippedStart) << heightShift
                                  : 0u;
      if (plan.destinationOrigin.y >= plan.canvas.height) {
        activeHeight = 0;
      } else {
        activeHeight = std::min(activeHeight,
                                plan.canvas.height - plan.destinationOrigin.y);
      }
      plan.activeExtent.height = activeHeight;
    }
  } else {
    plan.activeExtent.height = std::min(nominalHeight, plan.canvas.height);
  }
}

} // namespace

uint32_t nominalDisplayWidth(const GPU::DisplayState &state) {
  if (!state.modeSet)
    return 320;

  if ((state.gpuStat & (1u << 16)) != 0)
    return 368;

  switch ((state.gpuStat >> 17) & 3u) {
  case 0:
    return 256;
  case 1:
    return 320;
  case 2:
    return 512;
  default:
    return 640;
  }
}

uint32_t nominalDisplayHeight(const GPU::DisplayState &state) {
  if (!state.modeSet)
    return 240;

  const bool vertical480 = (state.gpuStat & (1u << 19)) != 0;
  const bool interlaced = (state.gpuStat & (1u << 22)) != 0;
  return vertical480 && interlaced ? 480u : 240u;
}

RawDisplayCrtcInput rawDisplayCrtcInput(const GPU::DisplayState &state) {
  RawDisplayCrtcInput input;
  // GP1(05) bit zero is not wired to the CRTC address latch.
  input.sourceWordX = state.xStart & ~1u;
  input.sourceY = state.yStart;
  input.horizontalStart = state.displayX1;
  input.horizontalEnd = state.displayX2;
  input.verticalStart = state.displayY1;
  input.verticalEnd = state.displayY2;
  input.gpuStat = state.gpuStat;
  input.modeSet = state.modeSet;
  input.areaSet = state.areaSet;
  input.horizontalRangeSet = state.horizontalRangeSet;
  input.verticalRangeSet = state.verticalRangeSet;
  // GPU::DisplayState has no guest-field member.  Do not infer it from the
  // host frame/latch counters; a future CRTC latch can fill this explicitly.
  input.field = DisplayScanoutField::Unknown;
  return input;
}

DisplayScanoutPlan deriveDisplayScanoutPlan(const RawDisplayCrtcInput &input,
                                            DisplayAreaPolicy policy) {
  DisplayScanoutPlan plan;
  const uint32_t nominalWidth = nominalDisplayWidth(input);
  const uint32_t nominalHeight = nominalDisplayHeight(input);
  const uint32_t divider = horizontalDotClockDivider(nominalWidth);

  plan.sourceByteX = effectiveSourceWordX(input.sourceWordX) * 2u;
  plan.sourceY = input.sourceY % GPU::VRAM_HEIGHT;
  plan.format = input.modeSet && (input.gpuStat & (1u << 21)) != 0
                    ? DisplayScanoutFormat::PackedRGB888
                    : DisplayScanoutFormat::RGB555;
  plan.progressive = !interlaced(input);
  plan.field =
      plan.progressive ? DisplayScanoutField::NotApplicable : input.field;
  plan.rowStep = interlaced(input) && vertical480(input) ? 2u : 1u;
  plan.enabled = (input.gpuStat & (1u << 23)) == 0;

  if (policy == DisplayAreaPolicy::DonorCanvas) {
    deriveDonorCanvasPolicy(plan, input, nominalWidth, nominalHeight, divider);
  } else {
    deriveLegacyPolicy(plan, input, policy, nominalWidth, nominalHeight,
                       divider);
  }
  return plan;
}

DisplayAperture deriveDisplayAperture(const GPU::DisplayState &state,
                                      DisplayAreaPolicy policy) {
  const RawDisplayCrtcInput input = rawDisplayCrtcInput(state);
  const DisplayScanoutPlan plan = deriveDisplayScanoutPlan(input, policy);
  DisplayAperture aperture;
  aperture.x = std::min(plan.sourceByteX / 2u, GPU::VRAM_WIDTH - 1);
  aperture.y = std::min(plan.sourceY, GPU::VRAM_HEIGHT - 1);
  aperture.width = plan.activeExtent.width;
  aperture.height = plan.activeExtent.height;
  aperture.enabled = plan.enabled && plan.activeExtent.width != 0 &&
                     plan.activeExtent.height != 0;

  const uint32_t nominalWidth = nominalDisplayWidth(input);
  const uint32_t divider = horizontalDotClockDivider(nominalWidth);
  aperture.usedHorizontalRange =
      policy != DisplayAreaPolicy::Nominal && input.horizontalRangeSet;
  aperture.usedVerticalRange =
      policy != DisplayAreaPolicy::Nominal && input.verticalRangeSet;

  aperture.width = std::clamp(aperture.width, 1u, GPU::VRAM_WIDTH - aperture.x);
  aperture.height =
      std::clamp(aperture.height, 1u, GPU::VRAM_HEIGHT - aperture.y);
  return aperture;
}

OutputViewport fitDisplayViewport(int drawableWidth, int drawableHeight,
                                  int aspectWidth, int aspectHeight) {
  OutputViewport viewport;
  if (drawableWidth <= 0 || drawableHeight <= 0 || aspectWidth <= 0 ||
      aspectHeight <= 0)
    return viewport;

  const int64_t widthLimitedHeight =
      static_cast<int64_t>(drawableWidth) * aspectHeight / aspectWidth;
  if (widthLimitedHeight <= drawableHeight) {
    viewport.width = drawableWidth;
    viewport.height = static_cast<int>(widthLimitedHeight);
  } else {
    viewport.height = drawableHeight;
    viewport.width = static_cast<int>(static_cast<int64_t>(drawableHeight) *
                                      aspectWidth / aspectHeight);
  }

  viewport.x = (drawableWidth - viewport.width) / 2;
  viewport.y = (drawableHeight - viewport.height) / 2;
  return viewport;
}

} // namespace ps1::gpu
