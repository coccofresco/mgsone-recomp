#version 450
#extension GL_GOOGLE_include_directive : require

// Crash 3-specific untextured semi-transparent source. Fixed-function Vulkan
// state implements ABR0/1/2. ABR3 needs its foreground integer-divided before
// the saturated additive blend, so that operation remains explicit here.

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
#include "../replay_depth_fragment.glsl"

layout(location = 0) out vec4 o_color;
layout(location = 1) out uint o_mask;
layout(location = 2) out float o_surface_depth;

const uint DRAW_FORCE_MASK = 1u << 3;

void main() {
  uvec3 color = uvec3(clamp(v_color_255, vec3(0.0), vec3(255.0)));
  if ((u_draw.texture_state.w & 3u) == 3u)
    color >>= 2u;
  o_color = vec4(vec3(color) * (1.0 / 255.0), 1.0);
  o_mask = (u_draw.texture_state.z & DRAW_FORCE_MASK) != 0u ? 1u : 0u;
  gl_FragDepth = replay_occlusion_depth();
  o_surface_depth = replay_surface_depth();
}
