#version 450
#extension GL_GOOGLE_include_directive : require

// Exact opaque-textured M1 profile. The only sampled resource is the native
// R16 VRAM mirror: opaque packets need neither destination-color feedback nor
// destination-mask reads. A zero source word discards color, mask, R32 and D32;
// visible texels retain PS1 STP in the independent mask attachment.

layout(set = 0, binding = 0) uniform usampler2D u_native_vram;

layout(set = 1, binding = 0, std140) uniform CrashReplayDrawState {
  vec4 position_scale_bias;
  vec4 depth_scale_bias;
  vec4 uv_domain;
  uvec4 texture_state;
  uvec4 texture_window;
  uvec4 destination_state;
} u_draw;

layout(location = 0) in vec2 v_uv;
layout(location = 1) noperspective in vec3 v_color_255;
#include "replay_depth_fragment.glsl"

layout(location = 0) out vec4 o_color;
layout(location = 1) out uint o_mask;
layout(location = 2) out float o_surface_depth;

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
  const uvec2 mask = u_draw.texture_window.xy & uvec2(255u);
  const uvec2 offset = u_draw.texture_window.zw & uvec2(255u);
  return (uv & (~mask & uvec2(255u))) | (offset & mask);
}

uint samplePs1Texel(vec2 interpolatedUv) {
  if ((u_draw.texture_state.z & DRAW_LIMIT_UV_DOMAIN) != 0u) {
    interpolatedUv =
        clamp(interpolatedUv, u_draw.uv_domain.xy, u_draw.uv_domain.zw);
  }

  // Signed conversion truncates toward zero before the architectural 8-bit
  // wrap, matching the scalar oracle for negative fractional coordinates.
  uvec2 uv = uvec2(ivec2(interpolatedUv)) & uvec2(255u);
  uv = applyTextureWindow(uv);

  const uint tpage = u_draw.texture_state.x;
  const uint clut = u_draw.texture_state.y;
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
  if (depth == 2u)
    return nativeWord(pageX + uv.x, pageY + uv.y);

  // Reserved texture depth is rejected by the CPU capability/prepare gates.
  // Returning zero is the fragment-side fail-safe if that contract regresses.
  return 0u;
}

uvec3 texelRgb5(uint texel) {
  return uvec3(texel, texel >> 5u, texel >> 10u) & uvec3(31u);
}

uvec3 expandRgb5(uvec3 rgb5) {
  return (rgb5 << 3u) | (rgb5 >> 2u);
}

uvec3 shadeColor() {
  return uvec3(clamp(v_color_255, vec3(0.0), vec3(255.0)));
}

void main() {
  const uint texel = samplePs1Texel(v_uv);
  // 0x0000 alone is transparent; 0x8000 is visible black carrying STP.
  if (texel == 0u)
    discard;

  const uvec3 rgb5 = texelRgb5(texel);
  uvec3 color;
  if ((u_draw.texture_state.z & DRAW_RAW_TEXTURE) != 0u) {
    color = expandRgb5(rgb5);
  } else {
    // 0x80 is the PS1 modulation identity. Preserve the exact generated
    // 8-bit result instead of quantizing it back through RGB555/dithering.
    color = min(uvec3(255u), (rgb5 * shadeColor()) >> 4u);
  }

  o_color = vec4(vec3(color) * (1.0 / 255.0), 1.0);
  const bool sourceStp = (texel & 0x8000u) != 0u;
  const bool forceMask =
      (u_draw.texture_state.z & DRAW_FORCE_MASK) != 0u;
  o_mask = (sourceStp || forceMask) ? 1u : 0u;
  gl_FragDepth = replay_occlusion_depth();
  o_surface_depth = replay_surface_depth();
}
