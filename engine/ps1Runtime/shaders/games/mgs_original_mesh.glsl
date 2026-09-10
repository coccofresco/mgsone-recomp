// MGS immutable original-asset storage, shared with the device-local cache.
// Other profiles keep mgs_projection.w == 0 and never read this binding.
layout(set=1, binding=2, std430) readonly buffer MgsOriginalAssets {
  uint words[];
} u_mgs;
uint mgs_word_at_byte(uint offset) {
  uint shift=(offset & 3u)*8u;
  uint value=u_mgs.words[offset/4u];
  if (shift==0u) return value;
  return (value>>shift) | (u_mgs.words[offset/4u+1u]<<(32u-shift));
}
vec3 mgs_original_position(vec2 fallback_xy, float fallback_w) {
  if (u_draw.mgs_projection.w == 0.0) return vec3(fallback_xy,fallback_w);
  uint lane=uint(fallback_xy.x);
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
  return vec3(eye.xy*(u_draw.mgs_projection.x/eye.z)+u_draw.mgs_projection.yz,eye.z);
}
