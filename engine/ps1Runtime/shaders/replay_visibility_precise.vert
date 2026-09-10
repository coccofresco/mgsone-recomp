#version 450
#extension GL_GOOGLE_include_directive : require
#include "replay_depth_vertex.glsl"

// Diagnostic-only counterpart of replay_precise.vert. A positive uniform
// multiplier scales clip X/Y/Z/W together for one submitted triangle. Screen
// position and perspective-correct UV are therefore algebraically invariant,
// while D32 receives the candidate presentation gauge. Independent raster
// attachments measure any finite-precision coverage/interpolation divergence.

layout(location = 0) in vec2 a_position_native;
layout(location = 1) in float a_projective_w;
layout(location = 2) in vec2 a_uv;
layout(location = 3) in vec3 a_color_255;

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
} u_draw;
#include "games/mgs_original_mesh.glsl"

layout(push_constant) uniform ReplayVisibilityOwnerState {
  // The high bit of y requests diagnostic raw-W reconstruction; z remains
  // the proof pass's force-far/stack-operation field.  w is
  // floatBits(homogeneous W multiplier).
  uvec4 state;
} u_visibility;

layout(location = 0) out vec2 v_uv;
layout(location = 1) noperspective out vec3 v_color_255;

invariant gl_Position;

void main() {
  const float multiplier = uintBitsToFloat(u_visibility.state.w);
  const vec3 original = mgs_original_position(a_position_native, a_projective_w);
  const float w = original.z * multiplier;
  const vec2 ndc =
      original.xy * u_draw.position_scale_bias.xy +
      u_draw.position_scale_bias.zw;
  // The proof-oriented visibility pass must reproduce the submitted replay
  // policy exactly.  The F8 pre-correction plane has a different purpose: it
  // reconstructs physical depth from every accepted authentic-W triangle,
  // including packets whose painter-compatible replay state deliberately
  // invalidates D32.  Keep that diagnostic contract explicit in the push
  // state instead of silently inheriting depth_scale_bias={0,1}.
  const float normalized_depth =
      (u_visibility.state.y & 0x80000000u) != 0u
          ? clamp(w * (1.0 / 4294967296.0), 0.0,
                  0.999999940395355224609375)
          : clamp(w * u_draw.depth_scale_bias.x + u_draw.depth_scale_bias.y,
                  0.0, 1.0);
  gl_Position = vec4(ndc * w, normalized_depth * w, w);
  const bool raw_depth = (u_visibility.state.y & 0x80000000u) != 0u;
  v_depth_affine = vec4(raw_depth ? (1.0 / 4294967296.0)
                                  : u_draw.depth_scale_bias.x,
                        raw_depth ? 0.0 : u_draw.depth_scale_bias.y, 0.0, 1.0);
  v_uv = a_uv;
  v_color_255 = a_color_255;
}
