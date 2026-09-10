#version 450

layout(set = 0, binding = 0) uniform usampler2D u_native_vram;

// Byte-for-byte identical to VulkanScanoutPushConstants.
layout(push_constant) uniform PresentScanoutPush {
  // sourceByteX, sourceY, rowStep, sourceScale (native R16 requires 1).
  uvec4 source;
  // activeWidth, activeHeight, DisplayScanoutFormat, DisplayScanoutField.
  uvec4 extentFormat;
} pc;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;

uint expand5To8(uint value) {
  return (value << 3u) | (value >> 2u);
}

const uint FORMAT_RGB555 = 0u;
const uint FORMAT_PACKED_RGB888 = 1u;
const uint VRAM_WIDTH_WORDS = 1024u;
const uint VRAM_HEIGHT = 512u;
const uint VRAM_ROW_BYTES = VRAM_WIDTH_WORDS * 2u;

uint sourceRow(uint localY) {
  return (pc.source.y + localY * pc.source.z) % VRAM_HEIGHT;
}

uint nativeWord(uint x, uint y) {
  return texelFetch(u_native_vram,
                    ivec2(x % VRAM_WIDTH_WORDS, y % VRAM_HEIGHT), 0).r;
}

void main() {
  if (pc.extentFormat.x == 0u || pc.extentFormat.y == 0u ||
      pc.source.z == 0u ||
      pc.source.w != 1u || pc.source.x >= VRAM_ROW_BYTES ||
      pc.source.y >= VRAM_HEIGHT) {
    o_color = vec4(0.0, 0.0, 0.0, 1.0);
    return;
  }

  const uvec2 local = min(uvec2(v_uv * vec2(pc.extentFormat.xy)),
                          pc.extentFormat.xy - uvec2(1u));
  const uint y = sourceRow(local.y);

  if (pc.extentFormat.z == FORMAT_PACKED_RGB888) {
    const uint byteX = (pc.source.x + local.x * 3u) % VRAM_ROW_BYTES;
    const uint wordX = byteX >> 1u;
    const uint low = nativeWord(wordX, y);
    const uint high = nativeWord(wordX + 1u, y);
    const uint packed = ((high << 16u) | low) >> ((byteX & 1u) * 8u);
    const uvec3 rgb8 =
        uvec3(packed, packed >> 8u, packed >> 16u) & uvec3(255u);
    o_color = vec4(vec3(rgb8) / 255.0, 1.0);
    return;
  }

  if (pc.extentFormat.z != FORMAT_RGB555 || (pc.source.x & 1u) != 0u) {
    o_color = vec4(0.0, 0.0, 0.0, 1.0);
    return;
  }

  const uint wordX = (pc.source.x / 2u + local.x) % VRAM_WIDTH_WORDS;
  const uint packed = nativeWord(wordX, y);

  const uvec3 rgb5 =
      uvec3(packed, packed >> 5u, packed >> 10u) & uvec3(31u);
  const uvec3 rgb8 = uvec3(expand5To8(rgb5.r), expand5To8(rgb5.g),
                           expand5To8(rgb5.b));
  o_color = vec4(vec3(rgb8) / 255.0, 1.0);
}
