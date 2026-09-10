#version 450
#extension GL_GOOGLE_include_directive : require
#include "crash3_background_depth.glsl"
#extension GL_ARB_gpu_shader_int64 : require
#extension GL_ARB_gpu_shader_fp64 : require
#include "../gte_color_math.glsl"

// Device-local Crash 3 resident-scene path. Immutable packed vertices and
// topology are read from binding 2. Binding 1 contains only this frame's
// selected faces/materials, camera, resource placement and deformation state.

layout(set = 1, binding = 0, std140) uniform CrashReplayDrawState {
  vec4 position_scale_bias;
  vec4 depth_scale_bias;
  vec4 uv_domain;
  uvec4 texture_state;
  uvec4 texture_window;
  uvec4 destination_state;
} u_draw;

layout(set = 1, binding = 1, std430) readonly buffer CrashSidecarFrameWords {
  uint words[];
} u_frame;

layout(set = 1, binding = 2, std430) readonly buffer CrashSidecarStaticWords {
  uint words[];
} u_static;

layout(push_constant) uniform CrashSidecarRawPullState {
  uint material_word_offset;
  uint face_word_offset;
  uint frame_state_word_offset;
  uint resource_word_offset;
  uint fallback_model_vertex_word_offset;
  uint static_topology_word_offset;
  uint static_vertex_word_offset;
  uint previous_frame_state_word_offset;
} u_pull;

layout(location = 0) out vec2 v_uv;
layout(location = 1) noperspective out vec3 v_color_255;
#include "../replay_depth_vertex.glsl"
layout(location = 3) flat out vec4 v_uv_domain;
layout(location = 4) flat out uvec4 v_texture_state;
layout(location = 5) flat out uvec4 v_texture_window;

const uint DRAW_SURFACE_DEPTH_VALID = 1u << 6;
const uint DRAW_TEXTURED = 1u << 0;
const uint DRAW_SEMI_TRANSPARENT = 1u << 2;
const uint DRAW_FORCE_MASK = 1u << 3;
const uint DRAW_CHECK_MASK = 1u << 4;
const uint DRAW_LIMIT_UV_DOMAIN = 1u << 5;
const float MAX_PRESENTED_SURFACE_DEPTH = 0.999999940395355224609375;
// Keep the camera-enclosing sky as a real far surface in both D32 and R32.
// 2^32 normalizes to the 1.0 clear sentinel; 0x4f7fffff is the immediately
// preceding positive binary32 value and matches kCrash3BackgroundDepthClass.
const uint BACKGROUND_PRESENTATION_W_BITS = 0x4f7fffffu;
const uint MATERIAL_WORDS = 16u;
const uint RAW_FACE_WORDS = 8u;
const uint RAW_TOPOLOGY_WORDS = 8u;
const uint RAW_VERTEX_WORDS = 4u;
const uint RAW_RESOURCE_WORDS = 4u;
const uint RAW_FRAME_WORDS = 52u;
const uint MODEL_VERTEX_WORDS = 8u;
const uint RAW_VERTEX_COLOR_VALID = 1u << 31;
const uint RAW_FACE_PROJECTED_FALLBACK = 1u << 31;
const uint RAW_FACE_INTERPOLATE_PREVIOUS = 1u << 31;
const uint RAW_FACE_DIRECT_MATERIAL = 1u << 30;
const uint RAW_FACE_DIRECT_TEXTURED = 1u << 0;
const uint RAW_FACE_DIRECT_BACKGROUND = 1u << 1;
const uint RAW_FACE_DIRECT_GPU_UV = 1u << 2;
const uint RAW_FACE_DIRECT_QUAD_HALF_ONE = 1u << 3;
const uint RAW_FACE_DIRECT_QUAD_HALF_TWO = 1u << 4;
const uint RAW_FACE_DIRECT_SEMI_TRANSPARENT = 1u << 5;
const uint RAW_FACE_DIRECT_ABR_SHIFT = 6u;
const uint RAW_FACE_DIRECT_FORCE_MASK = 1u << 8;
const uint RAW_FACE_DIRECT_CHECK_MASK = 1u << 9;
const uint RAW_FACE_DIRECT_FLOAT_DOMAIN = 1u << 10;
const uint RAW_PULL_STATIC_FACE = 1u << 31;
const uint RAW_PULL_INTERPOLATION_BITS = 1u << 31;
const uint PREVIOUS_FRAME_DISABLED = 0xffffffffu;

out gl_PerVertex {
  vec4 gl_Position;
  float gl_ClipDistance[1];
};

invariant gl_Position;

int signed_low_half(uint value) {
  return int(value << 16u) >> 16;
}

uint face_word(bool device_local, uint index) {
  return device_local ? u_static.words[index] : u_frame.words[index];
}

int wave_sample(uint frame, uint index) {
  return int(u_frame.words[frame + 32u + index]);
}

int wave_amplitude(int sample_value, uint vector_shift) {
  return int(uint(sample_value) << 16u) >> (vector_shift & 31u);
}

void project_raw_vertex(
    uint frame, uint resource_word_offset, uint resource_index, uint raw_xy,
    uint raw_z_flags, out vec2 position_native, out vec2 position_local_native,
    out float projective_w, out bool has_distance_cue, out int distance_cue) {
  const uint instance =
      resource_word_offset + resource_index * RAW_RESOURCE_WORDS;
  uint model_xy = ((((raw_xy & 0xfff0fff0u) >> 2u) +
                    u_frame.words[instance + 0u]) &
                   0xfffeffffu);
  uint model_z_word = ((raw_z_flags & 0xfff0u) >> 2u) +
                      uint(int(u_frame.words[instance + 1u]));
  int model_z = signed_low_half(model_z_word);

  const uint deformation_flags = u_frame.words[frame + 24u];
  const bool deformation_active = (deformation_flags & 1u) != 0u;
  const bool distance_cued = (deformation_flags & 2u) != 0u;
  const bool displace = (raw_xy & (1u << 18u)) != 0u;
  const bool skip_distance_cue = (raw_xy & (1u << 19u)) != 0u;
  has_distance_cue = false;
  distance_cue = 0;
  if (deformation_active &&
      ((distance_cued && (displace || !skip_distance_cue)) ||
       (!distance_cued && displace))) {
    uint phase = uint(model_z);
    phase += u_frame.words[frame + 28u];
    phase += model_xy;
    phase = uint(int(phase) >> 2u);
    const bool reflect_phase = (phase & 0x20u) != 0u;
    phase += uint(int(u_frame.words[frame + 25u]) >>
                  (u_frame.words[frame + 26u] & 31u));
    if (reflect_phase)
      phase = 0u - phase;
    const uint wave_index = phase & 0x1fu;
    const bool quadrant_bit4 = (phase & 0x10u) != 0u;
    const bool quadrant_bit5 = (phase & 0x20u) != 0u;
    int xy_sample;
    int z_sample;
    if (!quadrant_bit4) {
      z_sample = wave_sample(frame, 16u - wave_index);
      xy_sample = wave_sample(frame, wave_index);
    } else {
      xy_sample = wave_sample(frame, 32u - wave_index);
      z_sample = wave_sample(frame, wave_index - 16u);
    }
    if (displace) {
      const int xy_amplitude =
          wave_amplitude(xy_sample, u_frame.words[frame + 27u]);
      const int z_amplitude =
          wave_amplitude(z_sample, u_frame.words[frame + 27u]);
      if (!quadrant_bit4) {
        if (!quadrant_bit5) {
          model_xy += uint(xy_amplitude);
          model_z_word = uint(model_z) + uint(z_amplitude);
        } else {
          model_xy -= uint(xy_amplitude);
          model_z_word = uint(model_z) - uint(z_amplitude);
        }
      } else {
        if (!quadrant_bit5) {
          model_xy += uint(xy_amplitude);
          model_z_word = uint(model_z) - uint(z_amplitude);
        } else {
          model_xy -= uint(xy_amplitude);
          model_z_word = uint(model_z) + uint(z_amplitude);
        }
      }
      model_z = signed_low_half(model_z_word);
    }
    has_distance_cue = distance_cued && !skip_distance_cue;
    distance_cue = signed_low_half(uint(xy_sample));
  }

  const int model_x = signed_low_half(model_xy);
  const int model_y = int(model_xy) >> 16;
  precise int64_t camera_x =
      int64_t(int(u_frame.words[frame + 12u])) * int64_t(4096);
  precise int64_t camera_y =
      int64_t(int(u_frame.words[frame + 13u])) * int64_t(4096);
  precise int64_t camera_z =
      int64_t(int(u_frame.words[frame + 14u])) * int64_t(4096);
  camera_x += int64_t(int(u_frame.words[frame + 0u])) * int64_t(model_x);
  camera_x += int64_t(int(u_frame.words[frame + 1u])) * int64_t(model_y);
  camera_x += int64_t(int(u_frame.words[frame + 2u])) * int64_t(model_z);
  camera_y += int64_t(int(u_frame.words[frame + 4u])) * int64_t(model_x);
  camera_y += int64_t(int(u_frame.words[frame + 5u])) * int64_t(model_y);
  camera_y += int64_t(int(u_frame.words[frame + 6u])) * int64_t(model_z);
  camera_z += int64_t(int(u_frame.words[frame + 8u])) * int64_t(model_x);
  camera_z += int64_t(int(u_frame.words[frame + 9u])) * int64_t(model_y);
  camera_z += int64_t(int(u_frame.words[frame + 10u])) * int64_t(model_z);

  precise double x = double(camera_x);
  precise double y = double(camera_y);
  precise double z = double(camera_z);
  precise double projection_plane =
      double(int(u_frame.words[frame + 15u]));
  precise double horizontal_scale =
      double(uintBitsToFloat(u_frame.words[frame + 16u]));
  precise double projected_x =
      256.0lf + horizontal_scale * projection_plane * x / z;
  precise double projected_y = 128.0lf + projection_plane * y / z;
  const vec2 presentation_origin = vec2(
      uintBitsToFloat(u_frame.words[frame + 17u]),
      uintBitsToFloat(u_frame.words[frame + 18u]));
  position_local_native = vec2(float(projected_x), float(projected_y));
  position_native = position_local_native + presentation_origin;
  projective_w = float(z / 4096.0lf);
}

uvec2 direct_packet_uv(bool device_local_face, uint selected_face,
                       uint packet_corner) {
  const uint packed_pairs = face_word(
      device_local_face, selected_face + 3u + packet_corner / 2u);
  const uint packed_uv =
      (packed_pairs >> ((packet_corner & 1u) * 16u)) & 0xffffu;
  return uvec2(packed_uv & 0xffu, packed_uv >> 8u);
}

vec2 direct_modern_uv_offset(bool device_local_face, uint selected_face,
                             uint direct_flags, uint topology, uint frame,
                             uint resource_index) {
  const bool quad =
      (direct_flags & (RAW_FACE_DIRECT_QUAD_HALF_ONE |
                       RAW_FACE_DIRECT_QUAD_HALF_TWO)) != 0u;
  if (!quad || (direct_flags & RAW_FACE_DIRECT_GPU_UV) == 0u)
    return vec2(0.0);

  precise vec2 projected[3];
  precise float projected_w[3];
  for (uint index = 0u; index < 3u; ++index) {
    const uint raw_vertex_index = u_static.words[topology + 4u + index];
    const uint raw_vertex =
        u_pull.static_vertex_word_offset + raw_vertex_index * RAW_VERTEX_WORDS;
    vec2 absolute_position;
    bool distance_cued;
    int distance_cue;
    project_raw_vertex(
        frame, u_pull.resource_word_offset, resource_index,
        u_static.words[raw_vertex + 0u], u_static.words[raw_vertex + 1u],
        absolute_position, projected[index], projected_w[index],
        distance_cued, distance_cue);
  }

  const uvec2 uv0 = direct_packet_uv(device_local_face, selected_face, 0u);
  const uvec2 uv1 = direct_packet_uv(device_local_face, selected_face, 1u);
  const uvec2 uv2 = direct_packet_uv(device_local_face, selected_face, 2u);
  precise float abx = projected[1].x - projected[0].x;
  precise float aby = projected[1].y - projected[0].y;
  precise float bcx = projected[2].x - projected[1].x;
  precise float bcy = projected[2].y - projected[1].y;
  precise float cax = projected[0].x - projected[2].x;
  precise float cay = projected[0].y - projected[2].y;
  precise float dudx = -aby * float(uv2.x) - bcy * float(uv0.x) -
                        cay * float(uv1.x);
  precise float dvdx = -aby * float(uv2.y) - bcy * float(uv0.y) -
                        cay * float(uv1.y);
  precise float dudy = abx * float(uv2.x) + bcx * float(uv0.x) +
                        cax * float(uv1.x);
  precise float dvdy = abx * float(uv2.y) + bcx * float(uv0.y) +
                        cax * float(uv1.y);
  precise float area = bcx * cay - bcy * cax;
  const int texture_area =
      (int(uv1.x) - int(uv0.x)) * (int(uv2.y) - int(uv0.y)) -
      (int(uv2.x) - int(uv0.x)) * (int(uv1.y) - int(uv0.y));
  const bool is_3d = projected_w[0] != projected_w[1] ||
                     projected_w[0] != projected_w[2];
  if (area == 0.0 || texture_area == 0 || is_3d)
    return vec2(0.0);

  precise float reciprocal_area = 1.0 / area;
  precise float dudx_area = dudx * reciprocal_area;
  precise float dudy_area = dudy * reciprocal_area;
  precise float dvdx_area = dvdx * reciprocal_area;
  precise float dvdy_area = dvdy * reciprocal_area;
  return vec2(
      ((dudx_area < 0.0 && dudy_area == 0.0) ||
       (dudy_area < 0.0 && dudx_area == 0.0))
          ? 1.0
          : 0.0,
      ((dvdx_area < 0.0 && dvdy_area == 0.0) ||
       (dvdy_area < 0.0 && dvdx_area == 0.0))
          ? 1.0
          : 0.0);
}

void main() {
  const uint local_vertex = uint(gl_VertexIndex);
  const uint triangle = local_vertex / 3u;
  const uint corner = local_vertex % 3u;
  const bool device_local_face =
      (u_pull.face_word_offset & RAW_PULL_STATIC_FACE) != 0u;
  const uint selected_face =
      (u_pull.face_word_offset & ~RAW_PULL_STATIC_FACE) +
      triangle * RAW_FACE_WORDS;
  const uint topology_or_fallback =
      face_word(device_local_face, selected_face + 0u);
  const uint material_index_and_flags =
      face_word(device_local_face, selected_face + 1u);
  bool interpolate_previous =
      (material_index_and_flags & RAW_FACE_INTERPOLATE_PREVIOUS) != 0u;
  const bool interpolation_bitset =
      (u_pull.material_word_offset & RAW_PULL_INTERPOLATION_BITS) != 0u;
  if (device_local_face && interpolation_bitset) {
    const uint interpolation_word_offset =
        u_pull.material_word_offset & ~RAW_PULL_INTERPOLATION_BITS;
    const uint interpolation_word =
        u_frame.words[interpolation_word_offset + triangle / 32u];
    interpolate_previous =
        (interpolation_word & (1u << (triangle & 31u))) != 0u;
  }
  const bool direct_material =
      (material_index_and_flags & RAW_FACE_DIRECT_MATERIAL) != 0u;
  const uint material_index =
      material_index_and_flags &
      ~(RAW_FACE_INTERPOLATE_PREVIOUS | RAW_FACE_DIRECT_MATERIAL);
  const uint material =
      (u_pull.material_word_offset & ~RAW_PULL_INTERPOLATION_BITS) +
      material_index * MATERIAL_WORDS;

  vec4 presentation_depth;
  vec2 direct_uv_offset = vec2(0.0);
  if (direct_material) {
    const uint texture_page_clut =
        face_word(device_local_face, selected_face + 2u);
    const bool textured =
        (material_index & RAW_FACE_DIRECT_TEXTURED) != 0u;
    const bool background =
        (material_index & RAW_FACE_DIRECT_BACKGROUND) != 0u;
    const uint texture_page = texture_page_clut & 0xffffu;
    if ((material_index & RAW_FACE_DIRECT_GPU_UV) != 0u) {
      const uint packed_domain =
          face_word(device_local_face, selected_face + 5u);
      v_uv_domain = vec4(
          float(packed_domain & 0xffu),
          float((packed_domain >> 8u) & 0xffu),
          float((packed_domain >> 16u) & 0xffu),
          float(packed_domain >> 24u));
    } else if ((material_index & RAW_FACE_DIRECT_FLOAT_DOMAIN) != 0u) {
      v_uv_domain = vec4(
          uintBitsToFloat(face_word(device_local_face, selected_face + 3u)),
          uintBitsToFloat(face_word(device_local_face, selected_face + 4u)),
          uintBitsToFloat(face_word(device_local_face, selected_face + 5u)),
          uintBitsToFloat(face_word(device_local_face, selected_face + 6u)));
    } else {
      const uint domain_min =
          face_word(device_local_face, selected_face + 6u);
      const uint domain_max =
          face_word(device_local_face, selected_face + 7u);
      v_uv_domain = vec4(
          float(domain_min & 0xffffu), float(domain_min >> 16u),
          float(domain_max & 0xffffu), float(domain_max >> 16u));
    }
    v_texture_state = u_draw.texture_state;
    v_texture_state.x =
        (v_texture_state.x & ~0x01ffu) | (texture_page & 0x01ffu);
    v_texture_state.y = texture_page_clut >> 16u;
    v_texture_state.z =
        (u_draw.texture_state.z & (DRAW_FORCE_MASK | DRAW_CHECK_MASK)) |
        DRAW_SURFACE_DEPTH_VALID;
    if (textured)
      v_texture_state.z |= DRAW_TEXTURED | DRAW_LIMIT_UV_DOMAIN;
    if ((material_index & RAW_FACE_DIRECT_SEMI_TRANSPARENT) != 0u)
      v_texture_state.z |= DRAW_SEMI_TRANSPARENT;
    if ((material_index & RAW_FACE_DIRECT_FORCE_MASK) != 0u)
      v_texture_state.z |= DRAW_FORCE_MASK;
    if ((material_index & RAW_FACE_DIRECT_CHECK_MASK) != 0u)
      v_texture_state.z |= DRAW_CHECK_MASK;
    v_texture_state.w =
        (material_index & RAW_FACE_DIRECT_SEMI_TRANSPARENT) != 0u
            ? ((material_index >> RAW_FACE_DIRECT_ABR_SHIFT) & 3u)
            : (textured ? ((texture_page >> 5u) & 3u) : 0u);
    v_texture_window = u_draw.texture_window;
    presentation_depth =
        background
            ? vec4(0.0, uintBitsToFloat(BACKGROUND_PRESENTATION_W_BITS), 0.0,
                   uintBitsToFloat(BACKGROUND_PRESENTATION_W_BITS))
            : vec4(4.0, 12800.0, 4.0, 12800.0);
  } else {
    v_uv_domain = vec4(
        uintBitsToFloat(u_frame.words[material + 0u]),
        uintBitsToFloat(u_frame.words[material + 1u]),
        uintBitsToFloat(u_frame.words[material + 2u]),
        uintBitsToFloat(u_frame.words[material + 3u]));
    v_texture_state = uvec4(
        u_frame.words[material + 4u], u_frame.words[material + 5u],
        u_frame.words[material + 6u], u_frame.words[material + 7u]);
    v_texture_window = uvec4(
        u_frame.words[material + 8u], u_frame.words[material + 9u],
        u_frame.words[material + 10u], u_frame.words[material + 11u]);
    presentation_depth = vec4(
        uintBitsToFloat(u_frame.words[material + 12u]),
        uintBitsToFloat(u_frame.words[material + 13u]),
        uintBitsToFloat(u_frame.words[material + 14u]),
        uintBitsToFloat(u_frame.words[material + 15u]));
  }
  const uint frame = u_pull.frame_state_word_offset;

  vec2 position_native;
  vec2 position_local_native;
  float projective_w;
  if ((topology_or_fallback & RAW_FACE_PROJECTED_FALLBACK) != 0u) {
    const uint fallback_index =
        (topology_or_fallback & ~RAW_FACE_PROJECTED_FALLBACK) + corner;
    const uint vertex = u_pull.fallback_model_vertex_word_offset +
                        fallback_index * MODEL_VERTEX_WORDS;
    position_native = vec2(
        uintBitsToFloat(u_frame.words[vertex + 0u]),
        uintBitsToFloat(u_frame.words[vertex + 1u]));
    position_local_native = position_native;
    projective_w = uintBitsToFloat(u_frame.words[vertex + 2u]);
    v_uv = vec2(uintBitsToFloat(u_frame.words[vertex + 3u]),
                uintBitsToFloat(u_frame.words[vertex + 4u]));
    v_color_255 = vec3(uintBitsToFloat(u_frame.words[vertex + 5u]),
                       uintBitsToFloat(u_frame.words[vertex + 6u]),
                       uintBitsToFloat(u_frame.words[vertex + 7u]));
    gl_ClipDistance[0] = 1.0;
  } else {
    const uint topology = u_pull.static_topology_word_offset +
                          topology_or_fallback * RAW_TOPOLOGY_WORDS;
    const uint raw_vertex_index = u_static.words[topology + corner];
    const uint resource_index = u_static.words[topology + 3u];
    const uint raw_vertex =
        u_pull.static_vertex_word_offset + raw_vertex_index * RAW_VERTEX_WORDS;
    const uint raw_xy = u_static.words[raw_vertex + 0u];
    const uint raw_z_flags = u_static.words[raw_vertex + 1u];
    const uint packed_color = u_static.words[raw_vertex + 2u];
    bool has_distance_cue = false;
    int distance_cue = 0;
    project_raw_vertex(frame, u_pull.resource_word_offset, resource_index,
                       raw_xy, raw_z_flags, position_native,
                       position_local_native, projective_w, has_distance_cue,
                       distance_cue);
    if (direct_material) {
      if ((material_index & RAW_FACE_DIRECT_GPU_UV) != 0u) {
        direct_uv_offset = direct_modern_uv_offset(
            device_local_face, selected_face, material_index, topology, frame,
            resource_index);
        uint packet_corner = corner;
        if ((material_index & RAW_FACE_DIRECT_QUAD_HALF_TWO) != 0u)
          ++packet_corner;
        v_uv = vec2(direct_packet_uv(device_local_face, selected_face,
                                     packet_corner)) +
               direct_uv_offset;
        v_uv_domain += direct_uv_offset.xyxy;
      } else {
        const uint packed_uv =
            face_word(device_local_face, selected_face + 3u + corner);
        v_uv = vec2(float(packed_uv & 0xffffu),
                    float(packed_uv >> 16u));
      }
    } else {
      v_uv = vec2(
          uintBitsToFloat(face_word(
              device_local_face, selected_face + 2u + corner * 2u)),
          uintBitsToFloat(face_word(
              device_local_face, selected_face + 3u + corner * 2u)));
    }

    ivec3 color = ivec3(int(packed_color & 0xffu),
                        int((packed_color >> 8u) & 0xffu),
                        int((packed_color >> 16u) & 0xffu));
    if ((raw_z_flags & RAW_VERTEX_COLOR_VALID) == 0u)
      color = ivec3(255, 0, 255);
    if (has_distance_cue) {
      for (uint component = 0u; component < 3u; ++component) {
        color[component] = ps1_gte_dpcs_channel(
            color[component], int(u_frame.words[frame + 20u + component]),
            distance_cue);
      }
    }
    v_color_255 = vec3(color);

    if (interpolate_previous &&
        u_pull.previous_frame_state_word_offset != PREVIOUS_FRAME_DISABLED) {
      const uint previous_frame = u_pull.previous_frame_state_word_offset;
      vec2 previous_position_native;
      vec2 previous_position_local_native;
      float previous_w;
      bool previous_distance_cued;
      int previous_distance_cue;
      project_raw_vertex(
          previous_frame, previous_frame + RAW_FRAME_WORDS, resource_index,
          raw_xy, raw_z_flags, previous_position_native,
          previous_position_local_native, previous_w, previous_distance_cued,
          previous_distance_cue);
      const dvec2 previous_origin = dvec2(
          double(uintBitsToFloat(u_frame.words[previous_frame + 17u])),
          double(uintBitsToFloat(u_frame.words[previous_frame + 18u])));
      const dvec2 current_origin = dvec2(
          double(uintBitsToFloat(u_frame.words[frame + 17u])),
          double(uintBitsToFloat(u_frame.words[frame + 18u])));
      precise double old_w = double(previous_w);
      precise double new_w = double(projective_w);
      precise double interpolated_w = 0.5lf * old_w + 0.5lf * new_w;
      precise dvec2 previous_local =
          dvec2(previous_position_native) - previous_origin;
      precise dvec2 current_local =
          dvec2(position_native) - current_origin;
      precise dvec2 interpolated_local =
          (0.5lf * previous_local * old_w +
           0.5lf * current_local * new_w) /
          interpolated_w;
      position_native = vec2(interpolated_local + current_origin);
      projective_w = float(interpolated_w);
    }
    const float near_w =
        0.5 * float(int(u_frame.words[frame + 15u])) + (1.0 / 4096.0);
    gl_ClipDistance[0] = projective_w - near_w;
  }

  const float occlusion_w =
      projective_w * presentation_depth.x + presentation_depth.y;
  const float surface_w =
      projective_w * presentation_depth.z + presentation_depth.w;
  const vec2 ndc = position_native * u_draw.position_scale_bias.xy +
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
#ifdef PS1_DIAGNOSTIC_SOURCE_ID
  // Opt-in shader-only provenance view: unchanged geometry, depth, UV and
  // texture discard. Join topology+1 to this capture's source catalogue;
  // projected fallback is the reserved white sentinel, never a guessed ID.
  const uint source_id = (topology_or_fallback & RAW_FACE_PROJECTED_FALLBACK) != 0u
                             ? 0xffffffu : topology_or_fallback + 1u;
  v_color_255 = vec3(float(source_id & 255u),
                     float((source_id >> 8u) & 255u),
                     float((source_id >> 16u) & 255u));
  v_texture_state.z |= 1u << 31u;
#endif
}
