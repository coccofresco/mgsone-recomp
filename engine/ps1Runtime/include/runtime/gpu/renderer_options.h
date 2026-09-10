#pragma once

#include "runtime/gpu/display_geometry.h"
#include <optional>
#include <string>
#include <string_view>

namespace ps1::gpu {

// Backend-neutral presentation and modern-rendering policy. internalScale is
// consumed by the accelerated backend only: the authoritative software PS1
// raster always remains at the console's native 1024x512 VRAM resolution.
struct RendererOptions {
  DisplayAreaPolicy displayAreaPolicy = DisplayAreaPolicy::Programmed;
  DisplaySignalProfile displaySignalProfile =
      DisplaySignalProfile::SwanStationNoCrop;
  bool preserveAspect = true;
  int presentationAspectWidth = 4;
  int presentationAspectHeight = 3;
  int internalScale = 1;
  bool pgxp = false;
  bool pgxpNearClip = false;
  bool trueDepth = false;
  std::string trueDepthMode = "hybrid";
  std::string wipDepthManifest;
  bool trueColor = false;
};

inline constexpr std::optional<DisplayAreaPolicy>
displayAreaPolicyFromName(std::string_view name) noexcept {
  if (name == "programmed")
    return DisplayAreaPolicy::Programmed;
  if (name == "nominal")
    return DisplayAreaPolicy::Nominal;
  if (name == "donor_canvas")
    return DisplayAreaPolicy::DonorCanvas;
  return std::nullopt;
}

inline constexpr std::optional<DisplaySignalProfile>
displaySignalProfileFromName(std::string_view name) noexcept {
  if (name == "swanstation_no_crop")
    return DisplaySignalProfile::SwanStationNoCrop;
  if (name == "beetle_uncropped")
    return DisplaySignalProfile::BeetleUncropped;
  return std::nullopt;
}

// Normal Vulkan execution keeps the guest-authoritative software GPU at 1x.
// Centralizing this policy prevents res_scale from accidentally allocating
// the legacy CPU companion when the accelerated backend requests 4x or more.
inline void configureVulkanNativeOracle(GPU &gpu,
                                        bool useProgrammedDisplayRange) {
  gpu.setEnhancedRaster(false);
  gpu.setTrueDepthMode(TrueDepthMode::Off);
  gpu.configureEnhancedPresentation(1, false, useProgrammedDisplayRange);
}

inline bool isVulkanNativeOraclePolicyActive(const GPU &gpu) {
  return !gpu.isEnhancedRasterEnabled() && !gpu.isTrueDepthEnabled() &&
         !gpu.isTrueColorEnabled() && gpu.internalResolutionScale() == 1u;
}

} // namespace ps1::gpu
