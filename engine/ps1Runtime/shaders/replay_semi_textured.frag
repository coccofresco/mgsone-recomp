#version 450
#extension GL_GOOGLE_include_directive : require

// Modern replay fast path for textured semi-transparent primitives. The PS1 selects
// semi-transparency per texel: non-STP texels replace the destination while
// STP texels add to it. Vulkan cannot switch fixed-function blending per
// fragment, so the renderer executes two mutually-exclusive variants of this
// shader. No destination-color snapshot is needed.
//
//   TEXEL_FILTER = 0: accept all texels; alpha selects replace vs ABR blend
//   TEXEL_FILTER = 1: accept non-STP texels (ABR2 replacement pass)
//   TEXEL_FILTER = 2: accept STP texels     (ABR2 subtract pass)

layout(constant_id = 0) const uint TEXEL_FILTER = 0u;

layout(set = 0, binding = 0) uniform usampler2D u_native_vram;

layout(set = 1, binding = 0, std140) uniform CrashReplayDrawState {
  vec4 position_scale_bias;
  vec4 depth_scale_bias;
  vec4 uv_domain;
  uvec4 texture_state;
  uvec4 texture_window;
  uvec4 destination_state;
  vec4 mgs_view_rows[12];
  uvec4 mgs_point_bytes;
  vec4 mgs_projection;
  uvec4 mgs_texture_data;
  ivec4 mgs_texture_origin;
  uvec4 mgs_palette[2];
  vec4 mgs_weights[4];
  vec4 mgs_normals[4];
  vec4 mgs_light_directions[3];
  vec4 mgs_light_colors[3];
  vec4 mgs_ambient;
  vec4 mgs_world_positions[4];
  uvec4 mgs_point_lights;
  vec4 mgs_surface_effect[4];
} u_draw;
#include "games/mgs_original_texture.glsl"
#include "games/mgs_lighting.glsl"
#include "games/mgs_screen_texture.glsl"
#include "games/mgs_surface_effects.glsl"

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

  uvec2 uv = uvec2(ivec2(interpolatedUv)) & uvec2(255u);
  uv = applyTextureWindow(uv);
  uint cachedTexel;
  if (mgs_original_texel(uv, cachedTexel)) return cachedTexel;

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

uvec3 texelRgb5(uint texel) {
  return uvec3(texel, texel >> 5u, texel >> 10u) & uvec3(31u);
}

uvec3 expandRgb5(uvec3 rgb5) {
  return uvec3(round(mgs_albedo(rgb5)));
}

uvec3 shadeColor() {
  return uvec3(clamp(mgs_lighting(v_color_255), vec3(0.0), vec3(255.0)));
}

void main() {
  uvec3 color;
  bool sourceStp, visible;
  if (mgs_surface_texel(v_uv, v_color_255, color, sourceStp, visible) ||
      mgs_screen_texel(v_uv, v_color_255, color, sourceStp, visible)) {
    if (!visible) discard;
  } else {
    const uint texel = samplePs1Texel(mgs_surface_uv(v_uv));
    if (texel == 0u) discard;
    sourceStp = (texel & 0x8000u) != 0u;
    const uvec3 rgb5 = texelRgb5(texel);
    color = (u_draw.texture_state.z & DRAW_RAW_TEXTURE) != 0u
                ? expandRgb5(rgb5)
                : min(uvec3(255u), mgs_modulate(rgb5, v_color_255));
  }
  color = mgs_surface_color(color,v_uv);
  color = mgs_thermal_emission(color, mgs_lighting(v_color_255));
  if ((TEXEL_FILTER == 1u && sourceStp) ||
      (TEXEL_FILTER == 2u && !sourceStp)) discard;
  // ABR3 is B + (F >> 2), not B + floor(float(F) / 4). Perform the
  // architectural integer operation before the ONE+ONE blend. Non-STP texels
  // always replace color and therefore skip this transformation.
  if (sourceStp && (u_draw.texture_state.w & 3u) == 3u)
    color >>= 2u;

  float blendSelector = 1.0;
  if (TEXEL_FILTER == 0u) {
    // ABR0 uses alpha=1/2 to select 1/2 F + 1/2 B. ABR1/3 use alpha=1
    // to admit B; alpha=0 makes every non-STP texel an ordinary replacement.
    blendSelector = sourceStp
                        ? ((u_draw.texture_state.w & 3u) == 0u ? 0.5 : 1.0)
                        : 0.0;
    if (sourceStp && (u_draw.texture_state.w & 3u) == 0u &&
        u_draw.mgs_surface_effect[0].x != 0.0)
      blendSelector=1.0-0.5*mgs_surface_coverage;
  }
  if (u_draw.mgs_surface_effect[0].x==15.0 && sourceStp) {
    // Preserve authored light energy, with restrained extinction for dense
    // original-artwork plumes. This also attenuates preceding live bubbles.
    color=uvec3(round(clamp((vec3(color)+mgs_bubble_scattering)*
                          mgs_surface_coverage,vec3(0),vec3(255))));
    blendSelector=mgs_bubble_transmission;
  }
  o_color = vec4(vec3(color) * (1.0 / 255.0), blendSelector);
  const bool forceMask =
      (u_draw.texture_state.z & DRAW_FORCE_MASK) != 0u;
  o_mask = (sourceStp || forceMask) ? 1u : 0u;
  gl_FragDepth = replay_occlusion_depth();
  o_surface_depth = mgs_screen_output_depth(replay_surface_depth());
}
