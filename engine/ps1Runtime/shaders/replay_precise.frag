#version 450
#extension GL_GOOGLE_include_directive : require

// CrashReplayV1 precise polygon fragment contract.
//
// Required attachment/pipeline state:
//   location 0 -> R8G8B8A8_UNORM modern color, fixed-function blending OFF
//   location 1 -> R8_UINT independent PS1 mask plane
//   location 2 -> R32_SFLOAT final presented-surface depth
//   depth      -> D32_SFLOAT
//
// Depth is selected per packet, not per STP texel:
//   Hybrid opaque      ALWAYS, accepted fragments write genuine W; GP0/OT
//                      remains the cross-packet visibility authority
//   Strict opaque      LEQUAL, accepted fragments write genuine W
//   valid semi-transp  ALWAYS, accepted fragments retain painter composition
//                      in D32 and write the source surface W to location 2
//   opaque invalid-W   ALWAYS, depth write disabled; packet W=32768 remains
//   fallback           a geometry/UV input only
//   Screen2D           ALWAYS, accepted fragments write the explicit 1.0
//                      unresolved/2D sentinel to location 2
// shader discard preserves prior D32 for texture-zero/mask-rejected samples
// Strict CPU-oracle captures may request an invalid-W far write for diagnosis,
// but that state is non-canonical and the replay execution planner rejects the
// batch before any Vulkan commands are recorded.
//
// Do not enable early_fragment_tests.  A zero texture texel or destination
// mask rejection calls discard and must update neither color, mask nor depth.
//
// Exact PS1 ABR blending and mask testing need the destination established by
// all earlier replay samples.  Bindings 1 and 2 are an immutable snapshot of
// the affected destination rectangle, with its scaled-target origin/extent in
// destination_state.  Transparent or mask-tested commands are ordering
// boundaries: the recorder must refresh that snapshot after prior writes.

layout(set = 0, binding = 0) uniform usampler2D u_native_vram;
layout(set = 0, binding = 1) uniform sampler2D u_destination_color;
layout(set = 0, binding = 2) uniform usampler2D u_destination_mask;

layout(set = 1, binding = 0, std140) uniform CrashReplayDrawState {
  vec4 position_scale_bias;
  vec4 depth_scale_bias;
  vec4 uv_domain;
  // x=tpage, y=clut, z=ReplayDrawFlags, w=packet-effective ABR mode.
  uvec4 texture_state;
  // Expanded E2 values: maskX, maskY, offsetX, offsetY (multiples of 8).
  uvec4 texture_window;
  // xy=scratch snapshot origin in scaled target pixels, zw=snapshot extent.
  uvec4 destination_state;
  vec4 mgs_view_rows[12];
  uvec4 mgs_point_bytes;
  vec4 mgs_projection;
  uvec4 mgs_texture_data;
  ivec4 mgs_texture_origin;
  uvec4 mgs_palette[2];
  vec4 mgs_weights[4];
} u_draw;
#include "games/mgs_original_texture.glsl"

layout(location = 0) in vec2 v_uv;
layout(location = 1) noperspective in vec3 v_color_255;
#include "replay_depth_fragment.glsl"

layout(location = 0) out vec4 o_color;
layout(location = 1) out uint o_mask;
layout(location = 2) out float o_surface_depth;

const uint DRAW_TEXTURED = 1u << 0;
const uint DRAW_RAW_TEXTURE = 1u << 1;
const uint DRAW_SEMI_TRANSPARENT = 1u << 2;
const uint DRAW_FORCE_MASK = 1u << 3;
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

uint samplePs1Texel(vec2 interpolated_uv) {
  if ((u_draw.texture_state.z & DRAW_LIMIT_UV_DOMAIN) != 0u) {
    interpolated_uv =
        clamp(interpolated_uv, u_draw.uv_domain.xy, u_draw.uv_domain.zw);
  }

  // GLSL and the scalar oracle both truncate toward zero before the 8-bit
  // architectural wrap.  Conversion through signed int preserves negative
  // fractional behavior before the low-byte mask.
  const ivec2 truncated = ivec2(interpolated_uv);
  uvec2 uv = uvec2(truncated) & uvec2(255u);
  uv = applyTextureWindow(uv);
  uint cachedTexel;
  if (mgs_original_texel(uv, cachedTexel)) return cachedTexel;

  const uint tpage = u_draw.texture_state.x;
  const uint clut = u_draw.texture_state.y;
  const uint page_x = (tpage & 0x0fu) * 64u;
  const uint page_y = ((tpage >> 4u) & 1u) * 256u;
  const uint depth = (tpage >> 7u) & 3u;
  const uint clut_x = (clut & 0x3fu) * 16u;
  const uint clut_y = (clut >> 6u) & 0x1ffu;

  if (depth == 0u) {
    const uint packed = nativeWord(page_x + (uv.x >> 2u), page_y + uv.y);
    const uint index = (packed >> ((uv.x & 3u) * 4u)) & 0x0fu;
    return nativeWord(clut_x + index, clut_y);
  }
  if (depth == 1u) {
    const uint packed = nativeWord(page_x + (uv.x >> 1u), page_y + uv.y);
    const uint index = (packed >> ((uv.x & 1u) * 8u)) & 0xffu;
    return nativeWord(clut_x + index, clut_y);
  }

  // Depth values 2 and the reserved value 3 follow the scalar oracle's
  // direct 15-bit path.
  return nativeWord(page_x + uv.x, page_y + uv.y);
}

uvec3 texelRgb5(uint texel) {
  return uvec3(texel, texel >> 5u, texel >> 10u) & uvec3(31u);
}

uvec3 expandRgb5(uvec3 rgb5) {
  return (rgb5 << 3u) | (rgb5 >> 2u);
}

uvec3 shadeColor() {
  // uint conversion truncates toward zero, matching the software oracle's
  // float-to-int Gouraud conversion before its 0..255 clamp.
  return uvec3(clamp(v_color_255, vec3(0.0), vec3(255.0)));
}

uvec3 blendPs1TrueColor(uvec3 foreground, uvec3 background, uint mode) {
  switch (mode & 3u) {
  case 0u:
    return (background + foreground) >> 1u;
  case 1u:
    return min(uvec3(255u), background + foreground);
  case 2u:
    return uvec3(max(ivec3(0), ivec3(background) - ivec3(foreground)));
  default:
    return min(uvec3(255u), background + (foreground >> 2u));
  }
}

ivec2 destinationCoordinate() {
  const ivec2 absolute_pixel = ivec2(gl_FragCoord.xy);
  return absolute_pixel - ivec2(u_draw.destination_state.xy);
}

bool destinationCoordinateValid(ivec2 coordinate) {
  return all(greaterThanEqual(coordinate, ivec2(0))) &&
         all(lessThan(coordinate, ivec2(u_draw.destination_state.zw)));
}

void main() {
  const uint flags = u_draw.texture_state.z;
  const bool textured = (flags & DRAW_TEXTURED) != 0u;
  const bool raw_texture = (flags & DRAW_RAW_TEXTURE) != 0u;
  const bool primitive_semi = (flags & DRAW_SEMI_TRANSPARENT) != 0u;
  const bool force_mask = (flags & DRAW_FORCE_MASK) != 0u;
  const bool check_mask = (flags & DRAW_CHECK_MASK) != 0u;

  uint texel = 0u;
  bool source_stp = false;
  uvec3 foreground;
  if (textured) {
    texel = samplePs1Texel(v_uv);
    // Only raw zero is transparent.  0x8000 is visible black with STP/mask.
    if (texel == 0u)
      discard;

    source_stp = (texel & 0x8000u) != 0u;
    const uvec3 rgb5 = texelRgb5(texel);
    if (raw_texture) {
      foreground = expandRgb5(rgb5);
    } else {
      // PS1 modulation uses 0x80 as identity in the eventual RGB555 target:
      // min(255, (texel5 * shade8) >> 4).  CrashReplayV1 retains that exact
      // generated 8-bit value instead of dithering/quantizing it back to 5-bit.
      foreground =
          min(uvec3(255u), (rgb5 * shadeColor()) >> 4u);
    }
  } else {
    foreground = shadeColor();
  }

  // Textured semi-transparency is selected per texel by STP.  Non-STP texels
  // remain opaque in color, while the whole packet stays painter ordered in
  // color and writes a far visibility-boundary depth. Untextured
  // semi-transparent packets always blend.
  const bool blend = primitive_semi && (!textured || source_stp);
  const bool needs_destination = check_mask || blend;
  ivec2 destination = ivec2(0);
  if (needs_destination) {
    destination = destinationCoordinate();
    // A missing scratch region is a recorder contract violation.  Discard is
    // the only safe behavior: clamping would read another pixel and corrupt
    // color, mask, surface and occlusion depth deterministically but incorrectly.
    if (!destinationCoordinateValid(destination))
      discard;
  }

  if (check_mask &&
      texelFetch(u_destination_mask, destination, 0).r != 0u) {
    discard;
  }

  if (blend) {
    const vec3 destination_unorm =
        texelFetch(u_destination_color, destination, 0).rgb;
    const uvec3 background =
        uvec3(round(clamp(destination_unorm, vec3(0.0), vec3(1.0)) * 255.0));
    foreground = blendPs1TrueColor(
        foreground, background, u_draw.texture_state.w);
  }

  o_color = vec4(vec3(foreground) * (1.0 / 255.0), 1.0);
  // PS1 bit 15 is an independent plane.  It is deliberately not host alpha.
  o_mask = (source_stp || force_mask) ? 1u : 0u;
  gl_FragDepth = replay_occlusion_depth();
  o_surface_depth = replay_surface_depth();
}
