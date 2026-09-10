#version 450
#extension GL_GOOGLE_include_directive : require

// Batched host-scene untextured semitransparency. ABR remains fixed-function
// pipeline state; its matching per-triangle selector is carried flat only for
// the exact ABR3 foreground shift.

layout(location = 0) in vec2 v_uv;
layout(location = 1) noperspective in vec3 v_color_255;
#include "../replay_depth_fragment.glsl"
layout(location = 3) flat in vec4 v_uv_domain;
layout(location = 4) flat in uvec4 v_texture_state;
layout(location = 5) flat in uvec4 v_texture_window;

layout(location = 0) out vec4 o_color;
layout(location = 1) out uint o_mask;
layout(location = 2) out float o_surface_depth;

const uint DRAW_FORCE_MASK = 1u << 3;

void main() {
  uvec3 color = uvec3(clamp(v_color_255, vec3(0.0), vec3(255.0)));
  if ((v_texture_state.w & 3u) == 3u)
    color >>= 2u;
  o_color = vec4(vec3(color) * (1.0 / 255.0), 1.0);
  o_mask = (v_texture_state.z & DRAW_FORCE_MASK) != 0u ? 1u : 0u;
  gl_FragDepth = replay_occlusion_depth();
  o_surface_depth = replay_surface_depth();
}
