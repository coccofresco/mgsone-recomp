#version 450
#extension GL_GOOGLE_include_directive : require
#include "crash3_background_depth.glsl"
#extension GL_ARB_gpu_shader_int64 : require
#extension GL_ARB_gpu_shader_fp64 : require

// First raw-scene migration boundary for Crash 3. Ordinary vertices carry the
// exact signed model input consumed by the GTE; Vulkan performs the 64-bit
// camera transform and FP64 perspective divide. CPU-generated near-plane fan
// vertices retain their projected X/Y/W endpoint as an explicit fallback, so
// the whole opaque layer can still cross the renderer in one draw while the
// geometric clipping implementation is certified independently.

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

layout(push_constant) uniform CrashSidecarModelPullState {
  uint material_index_word_offset;
  uint material_word_offset;
  uint model_vertex_word_offset;
  uint frame_state_word_offset;
  uint first_vertex;
  uint previous_frame_state_word_offset;
  uint previous_model_vertex_word_offset;
  uint flags;
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
const uint MODEL_VERTEX_WORDS = 8u;
const uint MODEL_SPACE_VERTEX = 1u << 31;

invariant gl_Position;

int signed_low_half(uint value) {
  return int(value << 16u) >> 16;
}

void main() {
  const uint local_vertex = uint(gl_VertexIndex) - u_pull.first_vertex;
  const uint triangle = local_vertex / 3u;
  const uint material_index =
      u_scene.words[u_pull.material_index_word_offset + triangle];
  const uint material =
      u_pull.material_word_offset + material_index * MATERIAL_WORDS;
  const uint vertex =
      u_pull.model_vertex_word_offset + local_vertex * MODEL_VERTEX_WORDS;

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

  vec2 position_native;
  float projective_w;
  const uint position_z_or_w = u_scene.words[vertex + 2u];
  if ((position_z_or_w & MODEL_SPACE_VERTEX) != 0u) {
    const uint packed_xy = u_scene.words[vertex + 0u];
    const int model_x = signed_low_half(packed_xy);
    const int model_y = int(packed_xy) >> 16;
    const int model_z = signed_low_half(position_z_or_w);
    const uint frame = u_pull.frame_state_word_offset;

    precise int64_t camera_x =
        int64_t(int(u_scene.words[frame + 12u])) * int64_t(4096);
    precise int64_t camera_y =
        int64_t(int(u_scene.words[frame + 13u])) * int64_t(4096);
    precise int64_t camera_z =
        int64_t(int(u_scene.words[frame + 14u])) * int64_t(4096);
    camera_x += int64_t(int(u_scene.words[frame + 0u])) * int64_t(model_x);
    camera_x += int64_t(int(u_scene.words[frame + 1u])) * int64_t(model_y);
    camera_x += int64_t(int(u_scene.words[frame + 2u])) * int64_t(model_z);
    camera_y += int64_t(int(u_scene.words[frame + 4u])) * int64_t(model_x);
    camera_y += int64_t(int(u_scene.words[frame + 5u])) * int64_t(model_y);
    camera_y += int64_t(int(u_scene.words[frame + 6u])) * int64_t(model_z);
    camera_z += int64_t(int(u_scene.words[frame + 8u])) * int64_t(model_x);
    camera_z += int64_t(int(u_scene.words[frame + 9u])) * int64_t(model_y);
    camera_z += int64_t(int(u_scene.words[frame + 10u])) * int64_t(model_z);

    precise double x = double(camera_x);
    precise double y = double(camera_y);
    precise double z = double(camera_z);
    precise double projection_plane =
        double(int(u_scene.words[frame + 15u]));
    precise double horizontal_scale =
        double(uintBitsToFloat(u_scene.words[frame + 16u]));
    precise double projected_x =
        256.0lf + horizontal_scale * projection_plane * x / z;
    precise double projected_y = 128.0lf + projection_plane * y / z;
    position_native =
        vec2(float(projected_x), float(projected_y)) +
        vec2(u_draw.destination_state.xy);
    projective_w = float(z / 4096.0lf);
  } else {
    position_native = vec2(
        uintBitsToFloat(u_scene.words[vertex + 0u]),
        uintBitsToFloat(u_scene.words[vertex + 1u]));
    projective_w = uintBitsToFloat(position_z_or_w);
  }

  v_uv = vec2(uintBitsToFloat(u_scene.words[vertex + 3u]),
              uintBitsToFloat(u_scene.words[vertex + 4u]));
  v_color_255 = vec3(uintBitsToFloat(u_scene.words[vertex + 5u]),
                     uintBitsToFloat(u_scene.words[vertex + 6u]),
                     uintBitsToFloat(u_scene.words[vertex + 7u]));

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
}
