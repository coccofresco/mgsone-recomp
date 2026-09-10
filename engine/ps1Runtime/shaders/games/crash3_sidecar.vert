#version 450
#extension GL_GOOGLE_include_directive : require
#include "crash3_background_depth.glsl"

// Crash 3 host-scene vertex path. Geometry keeps the ordinary replay ABI,
// while locations 4..7 carry flat per-triangle material/depth state. This
// preserves the exact sorted primitive stream while allowing many different
// PS1 materials to share one Vulkan draw.

layout(location = 0) in vec2 a_position_native;
layout(location = 1) in float a_projective_w;
layout(location = 2) in vec2 a_uv;
layout(location = 3) in vec3 a_color_255;
layout(location = 4) in vec4 a_uv_domain;
layout(location = 5) in uvec4 a_texture_state;
layout(location = 6) in uvec4 a_texture_window;
layout(location = 7) in uvec4 a_presentation_depth_bits;

layout(set = 1, binding = 0, std140) uniform CrashReplayDrawState {
  vec4 position_scale_bias;
  vec4 depth_scale_bias;
  vec4 uv_domain;
  uvec4 texture_state;
  uvec4 texture_window;
  uvec4 destination_state;
} u_draw;

layout(location = 0) out vec2 v_uv;
layout(location = 1) noperspective out vec3 v_color_255;
#include "../replay_depth_vertex.glsl"
layout(location = 3) flat out vec4 v_uv_domain;
layout(location = 4) flat out uvec4 v_texture_state;
layout(location = 5) flat out uvec4 v_texture_window;

const uint DRAW_SURFACE_DEPTH_VALID = 1u << 6;
const float MAX_PRESENTED_SURFACE_DEPTH = 0.999999940395355224609375;

invariant gl_Position;

void main() {
  const float w = a_projective_w;
  const float occlusion_w =
      w * uintBitsToFloat(a_presentation_depth_bits.x) +
      uintBitsToFloat(a_presentation_depth_bits.y);
  const float surface_w =
      w * uintBitsToFloat(a_presentation_depth_bits.z) +
      uintBitsToFloat(a_presentation_depth_bits.w);
  const vec2 ndc =
      a_position_native * u_draw.position_scale_bias.xy +
      u_draw.position_scale_bias.zw;
  const float normalized_depth =
      clamp(occlusion_w * u_draw.depth_scale_bias.x +
                u_draw.depth_scale_bias.y,
            0.0, 1.0);
  replay_set_depth(uintBitsToFloat(a_presentation_depth_bits),
                   u_draw.depth_scale_bias.xy,
                   (a_texture_state.z & DRAW_SURFACE_DEPTH_VALID) != 0u,
                   crash3_background_depth(
                       uintBitsToFloat(a_presentation_depth_bits.x),
                       uintBitsToFloat(a_presentation_depth_bits.y)));
  gl_Position = vec4(ndc * w,
                     crash3_clip_depth(normalized_depth, w,
                         uintBitsToFloat(a_presentation_depth_bits.x),
                         uintBitsToFloat(a_presentation_depth_bits.y)), w);

  v_uv = a_uv;
  v_color_255 = a_color_255;
  v_uv_domain = a_uv_domain;
  v_texture_state = a_texture_state;
  v_texture_window = a_texture_window;
}
