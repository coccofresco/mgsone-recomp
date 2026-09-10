#include "runtime/gpu/renderer_vulkan.h"

namespace ps1::gpu {

const char *
vulkanReplayCapabilityReasonName(VulkanReplayCapabilityReason reason) {
  switch (reason) {
  case VulkanReplayCapabilityReason::NotProbed:
    return "not-probed";
  case VulkanReplayCapabilityReason::Supported:
    return "supported";
  case VulkanReplayCapabilityReason::NativeShadowFormatFeatures:
    return "native-shadow-format-features";
  case VulkanReplayCapabilityReason::NativeShadowImageUsage:
    return "native-shadow-image-usage";
  case VulkanReplayCapabilityReason::NativeShadowSampleCount:
    return "native-shadow-sample-count";
  case VulkanReplayCapabilityReason::NativeShadowExtent:
    return "native-shadow-extent";
  case VulkanReplayCapabilityReason::ColorFormatFeatures:
    return "color-format-features";
  case VulkanReplayCapabilityReason::ColorImageUsage:
    return "color-image-usage";
  case VulkanReplayCapabilityReason::ColorSampleCount:
    return "color-sample-count";
  case VulkanReplayCapabilityReason::ColorExtent:
    return "color-extent";
  case VulkanReplayCapabilityReason::MaskFormatFeatures:
    return "mask-format-features";
  case VulkanReplayCapabilityReason::MaskImageUsage:
    return "mask-image-usage";
  case VulkanReplayCapabilityReason::MaskSampleCount:
    return "mask-sample-count";
  case VulkanReplayCapabilityReason::MaskExtent:
    return "mask-extent";
  case VulkanReplayCapabilityReason::SurfaceDepthFormatFeatures:
    return "surface-depth-format-features";
  case VulkanReplayCapabilityReason::SurfaceDepthImageUsage:
    return "surface-depth-image-usage";
  case VulkanReplayCapabilityReason::SurfaceDepthSampleCount:
    return "surface-depth-sample-count";
  case VulkanReplayCapabilityReason::SurfaceDepthExtent:
    return "surface-depth-extent";
  case VulkanReplayCapabilityReason::DepthFormatFeatures:
    return "depth-format-features";
  case VulkanReplayCapabilityReason::DepthImageUsage:
    return "depth-image-usage";
  case VulkanReplayCapabilityReason::DepthSampleCount:
    return "depth-sample-count";
  case VulkanReplayCapabilityReason::DepthExtent:
    return "depth-extent";
  case VulkanReplayCapabilityReason::AtlasImageDimension:
    return "atlas-image-dimension";
  case VulkanReplayCapabilityReason::AtlasFramebufferExtent:
    return "atlas-framebuffer-extent";
  }
  return "unknown";
}

VulkanReplayCapabilityAssessment
assessVulkanReplayCapability(const VulkanReplayResourcePlan &plan,
                             const VulkanReplayDeviceSupport &support) {
  VulkanReplayCapabilityAssessment assessment{
      plan, support, VulkanReplayCapabilityReason::Supported};
  const auto reject = [&](VulkanReplayCapabilityReason reason) {
    assessment.reason = reason;
    return assessment;
  };
  const auto exceeds = [](VulkanReplayExtent wanted,
                          VulkanReplayExtent maximum) {
    return wanted.width == 0 || wanted.height == 0 ||
           wanted.width > maximum.width || wanted.height > maximum.height;
  };

  if (!support.nativeShadow.requiredFeaturesSupported)
    return reject(VulkanReplayCapabilityReason::NativeShadowFormatFeatures);
  if (!support.nativeShadow.imageUsageSupported)
    return reject(VulkanReplayCapabilityReason::NativeShadowImageUsage);
  if (!support.nativeShadow.sampleCountOneSupported)
    return reject(VulkanReplayCapabilityReason::NativeShadowSampleCount);
  if (exceeds(plan.nativeShadowExtent, support.nativeShadow.maxExtent))
    return reject(VulkanReplayCapabilityReason::NativeShadowExtent);

  if (!support.color.requiredFeaturesSupported)
    return reject(VulkanReplayCapabilityReason::ColorFormatFeatures);
  if (!support.color.imageUsageSupported)
    return reject(VulkanReplayCapabilityReason::ColorImageUsage);
  if (!support.color.sampleCountOneSupported)
    return reject(VulkanReplayCapabilityReason::ColorSampleCount);
  if (exceeds(plan.atlasExtent, support.color.maxExtent))
    return reject(VulkanReplayCapabilityReason::ColorExtent);

  if (!support.mask.requiredFeaturesSupported)
    return reject(VulkanReplayCapabilityReason::MaskFormatFeatures);
  if (!support.mask.imageUsageSupported)
    return reject(VulkanReplayCapabilityReason::MaskImageUsage);
  if (!support.mask.sampleCountOneSupported)
    return reject(VulkanReplayCapabilityReason::MaskSampleCount);
  if (exceeds(plan.atlasExtent, support.mask.maxExtent))
    return reject(VulkanReplayCapabilityReason::MaskExtent);

  if (!support.surfaceDepth.requiredFeaturesSupported) {
    return reject(VulkanReplayCapabilityReason::SurfaceDepthFormatFeatures);
  }
  if (!support.surfaceDepth.imageUsageSupported)
    return reject(VulkanReplayCapabilityReason::SurfaceDepthImageUsage);
  if (!support.surfaceDepth.sampleCountOneSupported)
    return reject(VulkanReplayCapabilityReason::SurfaceDepthSampleCount);
  if (exceeds(plan.atlasExtent, support.surfaceDepth.maxExtent))
    return reject(VulkanReplayCapabilityReason::SurfaceDepthExtent);

  if (!support.depth.requiredFeaturesSupported)
    return reject(VulkanReplayCapabilityReason::DepthFormatFeatures);
  if (!support.depth.imageUsageSupported)
    return reject(VulkanReplayCapabilityReason::DepthImageUsage);
  if (!support.depth.sampleCountOneSupported)
    return reject(VulkanReplayCapabilityReason::DepthSampleCount);
  if (exceeds(plan.atlasExtent, support.depth.maxExtent))
    return reject(VulkanReplayCapabilityReason::DepthExtent);

  if (plan.nativeShadowExtent.width > support.maxImageDimension2D ||
      plan.nativeShadowExtent.height > support.maxImageDimension2D)
    return reject(VulkanReplayCapabilityReason::NativeShadowExtent);
  if (plan.atlasExtent.width > support.maxImageDimension2D ||
      plan.atlasExtent.height > support.maxImageDimension2D)
    return reject(VulkanReplayCapabilityReason::AtlasImageDimension);
  if (plan.atlasExtent.width > support.maxFramebufferWidth ||
      plan.atlasExtent.height > support.maxFramebufferHeight)
    return reject(VulkanReplayCapabilityReason::AtlasFramebufferExtent);
  return assessment;
}

} // namespace ps1::gpu
