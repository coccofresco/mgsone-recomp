#version 450
#extension GL_GOOGLE_include_directive : require
#include "replay_depth_fragment.glsl"

// Opt-in visibility oracle shared by the painter-authority (ALWAYS) and
// physical-depth (LEQUAL) passes. It duplicates only fragment acceptance:
// texture-zero transparency, texture-window addressing and E6 destination
// mask rejection. Color/blend arithmetic is irrelevant to ownership.
//
// The renderer executes this immediately after the authoritative segment,
// while its native texture and immutable pre-write destination mask are still
// bound. Unsupported/ineligible primitives use the same acceptance path with
// token/evidence zero and force far depth, conservatively ending the local
// proof without erasing unaffected samples.

layout(set = 0, binding = 0) uniform usampler2D u_native_vram;
layout(set = 0, binding = 2) uniform usampler2D u_destination_mask;

layout(set = 1, binding = 0, std140) uniform CrashReplayDrawState {
  vec4 position_scale_bias;
  vec4 depth_scale_bias;
  vec4 uv_domain;
  uvec4 texture_state;
  uvec4 texture_window;
  uvec4 destination_state;
} u_draw;

layout(push_constant) uniform ReplayVisibilityOwnerState {
  // x=one-based owner, y=pass-local evidence (bit 31 requests diagnostic raw
  // W in the vertex stage), z=force far invalidation.
  uvec4 state;
} u_visibility;

layout(location = 0) in vec2 v_uv;
layout(location = 1) noperspective in vec3 v_color_255;

layout(location = 0) out uint o_owner;
layout(location = 1) out uint o_evidence;

const uint DRAW_TEXTURED = 1u << 0;
const uint DRAW_CHECK_MASK = 1u << 4;
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
  return 0u;
}

ivec2 destinationCoordinate() {
  return ivec2(gl_FragCoord.xy) - ivec2(u_draw.destination_state.xy);
}

bool destinationCoordinateValid(ivec2 coordinate) {
  return all(greaterThanEqual(coordinate, ivec2(0))) &&
         all(lessThan(coordinate, ivec2(u_draw.destination_state.zw)));
}

void main() {
  const uint flags = u_draw.texture_state.z;
  if ((flags & DRAW_TEXTURED) != 0u && samplePs1Texel(v_uv) == 0u)
    discard;

  if ((flags & DRAW_CHECK_MASK) != 0u) {
    const ivec2 destination = destinationCoordinate();
    if (!destinationCoordinateValid(destination) ||
        texelFetch(u_destination_mask, destination, 0).r != 0u) {
      discard;
    }
  }

  o_owner = u_visibility.state.x;
  o_evidence = u_visibility.state.y;
  gl_FragDepth = u_visibility.state.z != 0u ? 1.0 : replay_occlusion_depth();
}
