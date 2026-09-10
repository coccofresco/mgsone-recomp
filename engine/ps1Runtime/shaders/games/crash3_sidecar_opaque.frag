#version 450
#extension GL_GOOGLE_include_directive : require

// GPU-driven opaque resident material path. Every triangle pulls its exact
// decoded material from the compact scene table, so textured and untextured
// faces can share one draw without changing the D32 visibility contract.
// Texture-zero still discards every attachment before any write. Admission
// rejects destination-mask state, which remains on the ordered fallback path.

layout(set = 0, binding = 0) uniform usampler2D u_native_vram;

layout(location = 0) in vec2 v_uv;
layout(location = 1) noperspective in vec3 v_color_255;
#include "../replay_depth_fragment.glsl"
layout(location = 3) flat in vec4 v_uv_domain;
layout(location = 4) flat in uvec4 v_texture_state;
layout(location = 5) flat in uvec4 v_texture_window;

layout(location = 0) out vec4 o_color;
layout(location = 1) out uint o_mask;
layout(location = 2) out float o_surface_depth;

const uint DRAW_TEXTURED = 1u << 0;
const uint DRAW_RAW_TEXTURE = 1u << 1;
const uint DRAW_FORCE_MASK = 1u << 3;
const uint DRAW_LIMIT_UV_DOMAIN = 1u << 5;
const uint NATIVE_VRAM_WIDTH_MASK = 1023u;
const uint NATIVE_VRAM_HEIGHT_MASK = 511u;

uint nativeWord(uint x, uint y) {
  const ivec2 coordinate =
      ivec2(int(x & NATIVE_VRAM_WIDTH_MASK),
            int(y & NATIVE_VRAM_HEIGHT_MASK));
  return texelFetch(u_native_vram, coordinate, 0).r & 0xffffu;
}

uvec2 applyTextureWindow(uvec2 uv) {
  const uvec2 mask = v_texture_window.xy & uvec2(255u);
  const uvec2 offset = v_texture_window.zw & uvec2(255u);
  return (uv & (~mask & uvec2(255u))) | (offset & mask);
}

uint samplePs1Texel(vec2 interpolatedUv) {
  if ((v_texture_state.z & DRAW_LIMIT_UV_DOMAIN) != 0u)
    interpolatedUv = clamp(interpolatedUv, v_uv_domain.xy, v_uv_domain.zw);

  uvec2 uv = uvec2(ivec2(interpolatedUv)) & uvec2(255u);
  uv = applyTextureWindow(uv);
  const uint tpage = v_texture_state.x;
  const uint clut = v_texture_state.y;
  const uint pageX = (tpage & 0x0fu) * 64u;
  const uint pageY = ((tpage >> 4u) & 1u) * 256u;
  const uint depth = (tpage >> 7u) & 3u;
  const uint clutX = (clut & 0x3fu) * 16u;
  const uint clutY = (clut >> 6u) & 0x1ffu;
  if (depth == 0u) {
    const uint packed = nativeWord(pageX + (uv.x >> 2u), pageY + uv.y);
    const uint index = (packed >> ((uv.x & 3u) * 4u)) & 0x0fu;
    return nativeWord(clutX + index, clutY);
  }
  if (depth == 1u) {
    const uint packed = nativeWord(pageX + (uv.x >> 1u), pageY + uv.y);
    const uint index = (packed >> ((uv.x & 1u) * 8u)) & 0xffu;
    return nativeWord(clutX + index, clutY);
  }
  // Depth values 2 and the reserved value 3 follow the scalar oracle's
  // direct 15-bit path.
  return nativeWord(pageX + uv.x, pageY + uv.y);
}

uvec3 texelRgb5(uint texel) {
  return uvec3(texel, texel >> 5u, texel >> 10u) & uvec3(31u);
}

uvec3 expandRgb5(uvec3 rgb5) { return (rgb5 << 3u) | (rgb5 >> 2u); }

uvec3 shadeColor() {
  return uvec3(clamp(v_color_255, vec3(0.0), vec3(255.0)));
}

void main() {
  const bool textured = (v_texture_state.z & DRAW_TEXTURED) != 0u;
  uint texel = 0u;
  uvec3 color;
  bool sourceStp = false;
  if (textured) {
    texel = samplePs1Texel(v_uv);
    if (texel == 0u)
      discard;
    sourceStp = (texel & 0x8000u) != 0u;
    const uvec3 rgb5 = texelRgb5(texel);
    color = (v_texture_state.z & DRAW_RAW_TEXTURE) != 0u
                ? expandRgb5(rgb5)
                : min(uvec3(255u), (rgb5 * shadeColor()) >> 4u);
  } else {
    color = shadeColor();
  }

#ifdef PS1_DIAGNOSTIC_SOURCE_ID
  // Only the explicitly tagged raw source lane supplies an integer ID.
  // Other lanes remain shaded and cannot impersonate projected fallbacks.
  if ((v_texture_state.z & (1u << 31u)) != 0u)
    color = uvec3(round(v_color_255));
#endif
  o_color = vec4(vec3(color) * (1.0 / 255.0), 1.0);
  o_mask =
      (sourceStp || (v_texture_state.z & DRAW_FORCE_MASK) != 0u) ? 1u : 0u;
  gl_FragDepth = replay_occlusion_depth();
  o_surface_depth = replay_surface_depth();
}
