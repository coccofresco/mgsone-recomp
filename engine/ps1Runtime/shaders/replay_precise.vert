#version 450
#extension GL_GOOGLE_include_directive : require
#include "games/crash3_background_depth.glsl"

// CrashReplayV1 precise polygon vertex contract.
//
// The recorder supplies final native-VRAM screen coordinates and one finite,
// positive projective W per vertex.  Complete PGXP packets retain their
// genuine W; packet-atomic donor fallbacks supply W=32768 for every vertex.
// Near-plane clipping/saturation is a packet setup operation and must already
// have happened before this shader runs.
//
// The render target itself is res_scale times larger than native VRAM.  No
// scale is applied to the vertex data: position_scale_bias maps native screen
// coordinates to clip space and Vulkan's viewport supplies the higher sample
// density.  For a full 1024x512 target it is normally
// (2/1024, 2/512, -1, -1). Vulkan's positive-height viewport maps NDC -1
// to the top edge, so PS1's increasing-downward Y must keep a positive scale;
// the OpenGL-style negative-Y form would vertically mirror the replay atlas.

layout(location = 0) in vec2 a_position_native;
layout(location = 1) in float a_projective_w;
layout(location = 2) in vec2 a_uv;
layout(location = 3) in vec3 a_color_255;

// One dynamic UBO is shared by the vertex and fragment stages.  Keeping the
// layout identical in both modules makes the replay ABI explicit and avoids a
// push-constant layout which would exceed Vulkan's portable 128-byte floor as
// more packet state is added.
layout(set = 1, binding = 0, std140) uniform CrashReplayDrawState {
  // xy = native-position scale, zw = native-position bias.
  vec4 position_scale_bias;
  // x = W-to-depth scale, y = W-to-depth bias; zw reserved.
  // The resulting value must map the recorder's positive W domain to [0,1]
  // monotonically, with smaller W nearer.
  vec4 depth_scale_bias;
  // minU, minV, maxU, maxV after packet-wide flipped-quad correction.
  vec4 uv_domain;
  // x=tpage, y=clut, z=ReplayDrawFlags, w=packet-effective ABR mode.
  uvec4 texture_state;
  // Expanded E2 values: maskX, maskY, offsetX, offsetY (multiples of 8).
  uvec4 texture_window;
  // xy=scratch snapshot origin in scaled target pixels, zw=snapshot extent.
  uvec4 destination_state;
} u_draw;

// Presentation correction is independent from the D32 occlusion hypothesis.
// Remapping only depth values, rather than homogeneous X/Y/W, makes screen
// coverage and perspective UV bit-for-bit independent from both transforms.
layout(push_constant) uniform ReplayPresentationDepthState {
  uint occlusion_multiplier_bits;
  uint occlusion_bias_bits;
  uint surface_multiplier_bits;
  uint surface_bias_bits;
} u_presentation;

// Default smooth interpolation plus clip W implements
//   sum(lambda * UV/W) / sum(lambda/W).
layout(location = 0) out vec2 v_uv;

// PS1 Gouraud color is affine in screen space, not perspective-correct.
// Flat polygons simply submit the same color at all three vertices.
layout(location = 1) noperspective out vec3 v_color_255;
// Shared coefficients reconstruct both geometric depth planes per fragment.
#include "replay_depth_vertex.glsl"

const uint DRAW_SURFACE_DEPTH_VALID = 1u << 6;
// Reserve exactly 1.0 as an unpositioned/2D/native sentinel. Even a valid W
// near the uint32 domain limit therefore remains distinguishable in R32.
const float MAX_PRESENTED_SURFACE_DEPTH = 0.999999940395355224609375;

invariant gl_Position;

void main() {
  const float w = a_projective_w;
  const float occlusion_w =
      w * uintBitsToFloat(u_presentation.occlusion_multiplier_bits) +
      uintBitsToFloat(u_presentation.occlusion_bias_bits);
  const float surface_w =
      w * uintBitsToFloat(u_presentation.surface_multiplier_bits) +
      uintBitsToFloat(u_presentation.surface_bias_bits);
  const vec2 ndc =
      a_position_native * u_draw.position_scale_bias.xy +
      u_draw.position_scale_bias.zw;

  // Vertex Z supplies homogeneous clipping only. The fragment shader computes
  // projective depth from reciprocal clip W, not fixed-function screen Z.
  const float normalized_depth =
      clamp(occlusion_w * u_draw.depth_scale_bias.x +
                u_draw.depth_scale_bias.y,
            0.0, 1.0);
  replay_set_depth(vec4(uintBitsToFloat(u_presentation.occlusion_multiplier_bits),
                         uintBitsToFloat(u_presentation.occlusion_bias_bits),
                         uintBitsToFloat(u_presentation.surface_multiplier_bits),
                         uintBitsToFloat(u_presentation.surface_bias_bits)),
                   u_draw.depth_scale_bias.xy,
                   (u_draw.texture_state.z & DRAW_SURFACE_DEPTH_VALID) != 0u,
                   crash3_background_depth(
                       uintBitsToFloat(u_presentation.occlusion_multiplier_bits),
                       uintBitsToFloat(u_presentation.occlusion_bias_bits)));
  gl_Position = vec4(ndc * w,
                     crash3_clip_depth(normalized_depth, w,
                         uintBitsToFloat(u_presentation.occlusion_multiplier_bits),
                         uintBitsToFloat(u_presentation.occlusion_bias_bits)), w);

  v_uv = a_uv;
  v_color_255 = a_color_255;
}
