#version 450

// Resolve a Vulkan-generated midpoint without exposing holes left by a
// triangle which exists at endpoint A but has no provable match at endpoint
// B.  The midpoint itself remains authoritative wherever it wrote genuine
// presented-surface depth. Only an otherwise empty pixel may inherit the
// previous endpoint, and only when that endpoint carried valid depth. This is
// a post-depth disocclusion fallback: semitransparent midpoint fragments are
// never blended over endpoint A a second time.
layout(set = 0, binding = 0) uniform sampler2D u_midpoint_color;
layout(set = 1, binding = 0) uniform sampler2D u_midpoint_depth;
layout(set = 2, binding = 0) uniform sampler2D u_previous_color;
layout(set = 3, binding = 0) uniform sampler2D u_previous_depth;

// First 32 bytes are identical to VulkanScanoutPushConstants. The final
// vector identifies endpoint A independently of endpoint B's render page.
layout(push_constant) uniform PresentScanoutPush {
  // sourceByteX, sourceY, rowStep, sourceScale.
  uvec4 source;
  // activeWidth, activeHeight, DisplayScanoutFormat, DisplayScanoutField.
  uvec4 extentFormat;
  // endpoint-A sourceByteX, sourceY, rowStep, sourceScale.
  uvec4 previousSource;
} pc;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;

bool validPresentedDepth(float depth) {
  return !isnan(depth) && depth > 0.0 && depth < 1.0;
}

void main() {
  const uint FORMAT_RGB555 = 0u;
  const uint VRAM_WIDTH_WORDS = 1024u;
  const uint VRAM_HEIGHT = 512u;
  const uint VRAM_ROW_BYTES = VRAM_WIDTH_WORDS * 2u;
  const uint scale = pc.source.w;
  if (pc.extentFormat.x == 0u || pc.extentFormat.y == 0u ||
      pc.extentFormat.z != FORMAT_RGB555 || pc.source.z == 0u || scale == 0u ||
      scale > 8u || pc.source.x >= VRAM_ROW_BYTES ||
      (pc.source.x & 1u) != 0u || pc.source.y >= VRAM_HEIGHT) {
    o_color = vec4(0.0, 0.0, 0.0, 1.0);
    return;
  }

  const uvec2 scaledExtent = pc.extentFormat.xy * scale;
  const uvec2 local = min(uvec2(v_uv * vec2(scaledExtent)),
                          scaledExtent - uvec2(1u));
  const uvec2 nativeLocal = local / scale;
  const uvec2 subpixel = local % scale;
  const uint sourceWordX =
      (pc.source.x / 2u + nativeLocal.x) % VRAM_WIDTH_WORDS;
  const uint sourceY =
      (pc.source.y + nativeLocal.y * pc.source.z) % VRAM_HEIGHT;
  const ivec2 source =
      ivec2(sourceWordX * scale + subpixel.x,
            sourceY * scale + subpixel.y);
  const uint previousWordX =
      (pc.previousSource.x / 2u + nativeLocal.x) % VRAM_WIDTH_WORDS;
  const uint previousY =
      (pc.previousSource.y + nativeLocal.y * pc.previousSource.z) %
      VRAM_HEIGHT;
  const ivec2 previousSource =
      ivec2(previousWordX * pc.previousSource.w + subpixel.x,
            previousY * pc.previousSource.w + subpixel.y);

  const float midpointDepth = texelFetch(u_midpoint_depth, source, 0).r;
  const float previousDepth =
      texelFetch(u_previous_depth, previousSource, 0).r;
  const bool usePrevious =
      !validPresentedDepth(midpointDepth) && validPresentedDepth(previousDepth);
  o_color = usePrevious ? texelFetch(u_previous_color, previousSource, 0)
                        : texelFetch(u_midpoint_color, source, 0);
}
