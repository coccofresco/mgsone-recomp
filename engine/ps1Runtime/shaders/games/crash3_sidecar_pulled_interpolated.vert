#version 450
#extension GL_GOOGLE_include_directive : require
#include "crash3_background_depth.glsl"
#extension GL_ARB_gpu_shader_fp64 : require

// FP64 midpoint variant of crash3_sidecar_pulled.vert. Geometry and current
// material remain the exact endpoint-B stream. One compact prior-endpoint
// record per vertex supplies endpoint A; the shader interpolates homogeneous
// local X/Y/W and reattaches the result to B's alternating VRAM page. This is
// presentation-only and never advances or mutates guest state.

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
} u_draw;

layout(set = 1, binding = 1, std430) readonly buffer CrashSidecarWords {
  uint words[];
} u_scene;

layout(push_constant) uniform CrashSidecarPullState {
  uint material_index_word_offset;
  uint material_word_offset;
  uint first_vertex;
  uint previous_vertex_word_offset;
} u_pull;

layout(location = 0) out vec2 v_uv;
layout(location = 1) noperspective out vec3 v_color_255;
#include "../replay_depth_vertex.glsl"
layout(location = 3) flat out vec4 v_uv_domain;
layout(location = 4) flat out uvec4 v_texture_state;
layout(location = 5) flat out uvec4 v_texture_window;

const uint DRAW_SURFACE_DEPTH_VALID = 1u << 6;
const float MAX_PRESENTED_SURFACE_DEPTH = 0.999999940395355224609375;
const uint MATERIAL_WORDS = 16u;
const uint PREVIOUS_VERTEX_WORDS = 4u;
const uint PREVIOUS_ORIGIN_X_MASK = 0x7ffu;
const uint PREVIOUS_ORIGIN_Y_MASK = 0x3ffu;
const uint PREVIOUS_ORIGIN_Y_SHIFT = 11u;
const uint PREVIOUS_VALID = 1u << 31;

invariant gl_Position;

void main() {
  const uint local_vertex = uint(gl_VertexIndex) - u_pull.first_vertex;
  const uint triangle = local_vertex / 3u;
  const uint material_index =
      u_scene.words[u_pull.material_index_word_offset + triangle];
  const uint material =
      u_pull.material_word_offset + material_index * MATERIAL_WORDS;

  v_uv_domain = vec4(
      uintBitsToFloat(u_scene.words[material + 0u]),
      uintBitsToFloat(u_scene.words[material + 1u]),
      uintBitsToFloat(u_scene.words[material + 2u]),
      uintBitsToFloat(u_scene.words[material + 3u]));
  v_texture_state = uvec4(
      u_scene.words[material + 4u], u_scene.words[material + 5u],
      u_scene.words[material + 6u], u_scene.words[material + 7u]);
  v_texture_window = uvec4(
      u_scene.words[material + 8u], u_scene.words[material + 9u],
      u_scene.words[material + 10u], u_scene.words[material + 11u]);
  const vec4 presentation_depth = vec4(
      uintBitsToFloat(u_scene.words[material + 12u]),
      uintBitsToFloat(u_scene.words[material + 13u]),
      uintBitsToFloat(u_scene.words[material + 14u]),
      uintBitsToFloat(u_scene.words[material + 15u]));

  vec2 position_native = a_position_native;
  float projective_w = a_projective_w;
  const uint previous =
      u_pull.previous_vertex_word_offset + local_vertex *
          PREVIOUS_VERTEX_WORDS;
  const uint previous_origin_flags = u_scene.words[previous + 3u];
  if ((previous_origin_flags & PREVIOUS_VALID) != 0u) {
    const dvec2 previous_position = dvec2(
        double(uintBitsToFloat(u_scene.words[previous + 0u])),
        double(uintBitsToFloat(u_scene.words[previous + 1u])));
    precise double previous_w =
        double(uintBitsToFloat(u_scene.words[previous + 2u]));
    const dvec2 previous_origin = dvec2(
        double(previous_origin_flags & PREVIOUS_ORIGIN_X_MASK),
        double((previous_origin_flags >> PREVIOUS_ORIGIN_Y_SHIFT) &
               PREVIOUS_ORIGIN_Y_MASK));
    const dvec2 current_origin = dvec2(u_draw.destination_state.xy);
    precise double current_w = double(a_projective_w);
    precise double previous_weight = 0.5lf;
    precise double current_weight = 0.5lf;
    precise double interpolated_w =
        previous_weight * previous_w + current_weight * current_w;
    precise dvec2 previous_local = previous_position - previous_origin;
    precise dvec2 current_local =
        dvec2(a_position_native) - current_origin;
    precise dvec2 previous_term =
        previous_weight * previous_local * previous_w;
    precise dvec2 current_term = current_weight * current_local * current_w;
    precise dvec2 interpolated_local =
        (previous_term + current_term) / interpolated_w;
    position_native = vec2(interpolated_local + current_origin);
    projective_w = float(interpolated_w);
  }

  const float occlusion_w =
      projective_w * presentation_depth.x + presentation_depth.y;
  const float surface_w =
      projective_w * presentation_depth.z + presentation_depth.w;
  const vec2 ndc =
      position_native * u_draw.position_scale_bias.xy +
      u_draw.position_scale_bias.zw;
  const float normalized_depth =
      clamp(occlusion_w * u_draw.depth_scale_bias.x +
                u_draw.depth_scale_bias.y,
            0.0, 1.0);
  replay_set_depth(presentation_depth, u_draw.depth_scale_bias.xy,
                   (v_texture_state.z & DRAW_SURFACE_DEPTH_VALID) != 0u,
                   crash3_background_depth(presentation_depth.x,
                                            presentation_depth.y));
  gl_Position = vec4(ndc * projective_w,
                     crash3_clip_depth(normalized_depth, projective_w,
                         presentation_depth.x, presentation_depth.y),
                     projective_w);

  v_uv = a_uv;
  v_color_255 = a_color_255;
}
