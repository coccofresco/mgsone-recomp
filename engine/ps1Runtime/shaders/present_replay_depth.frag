#version 450

// Presentation-only view shared by D32 pre-depth and R32 presented-surface
// planes. Both store genuine GTE W as W / 2^32. Crash's useful geometric
// range observed across the checkpoint matrix is concentrated around
// 2^12..2^18, while its presentation-only sky class sits near 2^32. Mapping
// the entire 32-bit domain made a genuinely continuous buffer look like only
// one foreground value plus sky. Expand that scene range smoothly across the
// complete RGB ramp. Earlier quarter-octave isolines looked like periodic
// black corruption, so the primary F8 view deliberately contains no
// synthetic bands. The sky stays explicitly blue and invalid depth remains
// black.
layout(set = 0, binding = 0) uniform sampler2D u_replay_depth;

// 0 = authentic guest/projective W before presentation correction;
// 1 = corrected presented-surface W.  The two planes occupy deliberately
// different numeric ranges in Crash 3, so sharing one transfer function made
// the pre plane look flat even when its D32 values were continuous.
layout(constant_id = 0) const uint DEPTH_DOMAIN = 1u;

// Byte-for-byte identical to VulkanScanoutPushConstants.
layout(push_constant) uniform PresentScanoutPush {
  // sourceByteX, sourceY, rowStep, sourceScale.
  uvec4 source;
  // activeWidth, activeHeight, DisplayScanoutFormat, DisplayScanoutField.
  uvec4 extentFormat;
} pc;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;

vec3 depthFalseColour(float normalizedLogDepth) {
  // HSV hue 0..2/3 without a branch-heavy general HSV conversion: red at the
  // near end, green at the midpoint and blue at the far end.
  const float hue = clamp(normalizedLogDepth, 0.0, 1.0) * (2.0 / 3.0);
  const vec3 phase = abs(mod(hue * 6.0 + vec3(0.0, 4.0, 2.0), 6.0) - 3.0);
  return clamp(phase - 1.0, 0.0, 1.0);
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
  const uvec2 source =
      uvec2(sourceWordX * scale + subpixel.x,
            sourceY * scale + subpixel.y);

  const float encodedDepth = texelFetch(u_replay_depth, ivec2(source), 0).r;
  if (!isnan(encodedDepth) && encodedDepth > 0.0 && encodedDepth < 1.0) {
    const float rawW = max(encodedDepth * 4294967296.0, 1.0);
    if (rawW >= 2147483648.0) {
      o_color = vec4(0.0, 0.0, 1.0, 1.0);
    } else {
      const float logW = log2(rawW);
      const float logMinimum = DEPTH_DOMAIN == 0u ? 0.0 : 12.0;
      const float logRange = DEPTH_DOMAIN == 0u ? 16.0 : 6.0;
      const float normalizedLogDepth =
          clamp((logW - logMinimum) / logRange, 0.0, 1.0);
      o_color = vec4(depthFalseColour(normalizedLogDepth), 1.0);
    }
  } else {
    o_color = vec4(0.0, 0.0, 0.0, 1.0);
  }
}
