#version 450

// Resolve an RGB555 scanout from the persistent full-VRAM CrashReplay atlas.
// Native source coordinates are scaled here so wrapped rows and columns retain
// their SxS subpixel position.
layout(set = 0, binding = 0) uniform sampler2D u_replay_color;

// Byte-for-byte identical to VulkanScanoutPushConstants.
layout(push_constant) uniform PresentScanoutPush {
  // sourceByteX, sourceY, rowStep, sourceScale.
  uvec4 source;
  // activeWidth, activeHeight, DisplayScanoutFormat, DisplayScanoutField.
  uvec4 extentFormat;
} pc;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;

void main() {
  const uint FORMAT_RGB555 = 0u;
  const uint VRAM_WIDTH_WORDS = 1024u;
  const uint VRAM_HEIGHT = 512u;
  const uint VRAM_ROW_BYTES = VRAM_WIDTH_WORDS * 2u;
  const uint scale = pc.source.w;
  if (pc.extentFormat.x == 0u || pc.extentFormat.y == 0u ||
      pc.extentFormat.z != FORMAT_RGB555 || pc.source.z == 0u || scale == 0u ||
      scale > 12u || pc.source.x >= VRAM_ROW_BYTES ||
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
  const uvec2 source =
      uvec2(sourceWordX * scale + subpixel.x,
            sourceY * scale + subpixel.y);
  o_color = texelFetch(u_replay_color, ivec2(source), 0);
}
