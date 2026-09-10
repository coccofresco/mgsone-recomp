#version 450
#extension GL_ARB_fragment_shader_interlock : require
#extension GL_GOOGLE_include_directive : require

// Exact, ordered Crash 3 host-scene semitransparency. The rasterizer and D32
// decide coverage; sampled D32 decides projective occlusion. Color, PS1 mask and presented-surface
// depth are read and written as storage images while overlapping fragments
// execute in primitive order. One draw can therefore carry textured and
// untextured faces plus every ABR mode without losing painter semantics.

layout(pixel_interlock_ordered) in;

layout(set = 0, binding = 0) uniform usampler2D u_native_vram;
layout(set = 0, binding = 4, rgba8) uniform coherent image2D u_replay_color;
layout(set = 0, binding = 5, r8ui) uniform coherent uimage2D u_replay_mask;
layout(set = 0, binding = 6, r32f) uniform coherent image2D u_surface_depth;
layout(set = 0, binding = 7) uniform sampler2D u_scene_depth;

layout(location = 0) in vec2 v_uv;
layout(location = 1) noperspective in vec3 v_color_255;
#include "../replay_depth_fragment.glsl"
layout(location = 3) flat in vec4 v_uv_domain;
layout(location = 4) flat in uvec4 v_texture_state;
layout(location = 5) flat in uvec4 v_texture_window;

const uint DRAW_TEXTURED = 1u << 0;
const uint DRAW_RAW_TEXTURE = 1u << 1;
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
  if (depth == 2u)
    return nativeWord(pageX + uv.x, pageY + uv.y);
  return 0u;
}

uvec3 texelRgb5(uint texel) {
  return uvec3(texel, texel >> 5u, texel >> 10u) & uvec3(31u);
}

uvec3 expandRgb5(uvec3 rgb5) { return (rgb5 << 3u) | (rgb5 >> 2u); }

uvec3 shadeColor() {
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

void main() {
  // Storage writes cannot be undone by late fixed-function depth rejection.
  // Sample the read-only opaque D32 plane before entering the ordered blend;
  // never compare its projective depth with screen-affine gl_FragCoord.z.
  const ivec2 pixel = ivec2(gl_FragCoord.xy);
  if (replay_occlusion_depth() > texelFetch(u_scene_depth, pixel, 0).r)
    discard;
  const uint flags = v_texture_state.z;
  const bool textured = (flags & DRAW_TEXTURED) != 0u;
  bool sourceStp = false;
  uvec3 foreground;
  if (textured) {
    const uint texel = samplePs1Texel(v_uv);
    if (texel == 0u)
      discard;
    sourceStp = (texel & 0x8000u) != 0u;
    const uvec3 rgb5 = texelRgb5(texel);
    foreground = (flags & DRAW_RAW_TEXTURE) != 0u
                     ? expandRgb5(rgb5)
                     : min(uvec3(255u), (rgb5 * shadeColor()) >> 4u);
  } else {
    foreground = shadeColor();
  }

  beginInvocationInterlockARB();
  const bool rejected =
      (flags & DRAW_CHECK_MASK) != 0u &&
      imageLoad(u_replay_mask, pixel).r != 0u;
  if (!rejected) {
    if (!textured || sourceStp) {
      const vec3 destinationUnorm = imageLoad(u_replay_color, pixel).rgb;
      const uvec3 background = uvec3(
          round(clamp(destinationUnorm, vec3(0.0), vec3(1.0)) * 255.0));
      foreground = blendPs1TrueColor(foreground, background,
                                     v_texture_state.w);
    }
    imageStore(u_replay_color, pixel,
               vec4(vec3(foreground) * (1.0 / 255.0), 1.0));
    imageStore(u_replay_mask, pixel,
               uvec4(sourceStp || (flags & DRAW_FORCE_MASK) != 0u ? 1u : 0u));
    imageStore(u_surface_depth, pixel, vec4(replay_surface_depth()));
  }
  endInvocationInterlockARB();
}
