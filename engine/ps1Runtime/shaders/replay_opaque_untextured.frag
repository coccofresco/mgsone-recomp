#version 450
#extension GL_GOOGLE_include_directive : require

// First live M1 fragment profile. It deliberately has no sampled-image
// bindings: an opaque, untextured packet needs neither texture/CLUT reads nor
// an immutable destination snapshot, so accepting one cannot create an atlas
// feedback loop. Textured, blended and mask-tested profiles remain separate
// capability/pipeline additions.

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
#include "games/mgs_lighting.glsl"
#include "games/mgs_screen_texture.glsl"
#include "games/mgs_surface_effects.glsl"

layout(location = 0) in vec2 v_uv;
layout(location = 1) noperspective in vec3 v_color_255;
#include "replay_depth_fragment.glsl"

layout(location = 0) out vec4 o_color;
layout(location = 1) out uint o_mask;
layout(location = 2) out float o_surface_depth;

const uint DRAW_FORCE_MASK = 1u << 3;

void main() {
  // Conversion truncates toward zero, matching the true-color CPU oracle's
  // Gouraud conversion before its 0..255 clamp.
  uvec3 color =
      uvec3(clamp(v_color_255, vec3(0.0), vec3(255.0)));
  bool visible,stp;uvec3 effectColor;
  if(mgs_surface_texel(v_uv,v_color_255,effectColor,stp,visible)) {
    if(!visible) discard;
    color=effectColor;
  }
  color=mgs_surface_color(color,v_uv);
  o_color = vec4(vec3(color) * (1.0 / 255.0), 1.0);
  o_mask = (u_draw.texture_state.z & DRAW_FORCE_MASK) != 0u ? 1u : 0u;
  gl_FragDepth = replay_occlusion_depth();
  o_surface_depth = replay_surface_depth();
}
