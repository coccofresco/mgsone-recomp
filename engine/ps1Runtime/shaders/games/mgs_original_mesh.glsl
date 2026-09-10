#include "mgs_asset_storage.glsl"
vec3 mgs_corner_eye(uint lane) {
  uint reference=u_draw.mgs_point_bytes[lane];
  uint offset=reference & 0x7fffffffu;
  vec3 point;
  if ((reference & 0x80000000u)!=0u) {
    uint xy=mgs_word_at_byte(offset), z=mgs_word_at_byte(offset+4u);
    point=vec3(float(bitfieldExtract(int(xy),0,16)),
               float(bitfieldExtract(int(xy),16,16)),
               float(bitfieldExtract(int(z),0,16)));
  } else {
    point=vec3(uintBitsToFloat(mgs_word_at_byte(offset)),
               uintBitsToFloat(mgs_word_at_byte(offset+4u)),
               uintBitsToFloat(mgs_word_at_byte(offset+8u)));
  }
  vec4 p=vec4(point,1.0);
  vec3 eye=vec3(dot(u_draw.mgs_view_rows[lane*3],p),
                dot(u_draw.mgs_view_rows[lane*3+1],p),
                dot(u_draw.mgs_view_rows[lane*3+2],p));
  return eye;
}

vec3 mgs_eye(uint lane) {
  vec4 weights = u_draw.mgs_weights[lane];
  vec3 eye = vec3(0.0);
  for (uint k=0u; k<4u; ++k)
    if (weights[k] != 0.0) eye += weights[k] * mgs_corner_eye(k);
  return eye;
}
vec3 mgs_original_position(vec2 fallback_xy, float fallback_w) {
  if (u_draw.mgs_projection.w == 0.0) return vec3(fallback_xy,fallback_w);
  vec3 eye = mgs_eye(uint(fallback_xy.x));
  return vec3(eye.xy*(u_draw.mgs_projection.x/max(1.0,eye.z))+u_draw.mgs_projection.yz,eye.z);
}
vec4 mgs_original_clip(uint lane) {
  vec3 eye = mgs_eye(lane);
  vec2 clip_xy = eye.xy * u_draw.mgs_projection.x * u_draw.position_scale_bias.xy +
      eye.z * (u_draw.mgs_projection.yz * u_draw.position_scale_bias.xy + u_draw.position_scale_bias.zw);
  // Vulkan clips against 0 <= Z <= W. This linear homogeneous plane preserves
  // signed eye depth, including crossing triangles and vertices exactly at Z=0.
  return vec4(clip_xy, eye.z - 1.0, eye.z);
}
