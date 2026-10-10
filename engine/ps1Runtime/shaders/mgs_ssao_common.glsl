// Presentation-only coordinates. R32 stores authenticated linear eye W / 2^32;
// 1.0 is deliberately NOT geometry (HUD, text, video, seed and unresolved data).
layout(push_constant) uniform MgsSsaoPush {
  uvec4 source;
  uvec4 extentFormat;
  vec4 camera; // native focal xy (including 16:9), atlas optical center xy
  vec4 tuning; // base world radius, intensity, shadow power, shadow clamp
} ao;

ivec2 ao_full_size() { return ivec2(ao.extentFormat.xy * ao.source.w); }
ivec2 ao_size() { return (ao_full_size() + 1) / 2; }
ivec2 ao_source(ivec2 p) {
  uint scale = ao.source.w;
  uvec2 local = uvec2(p), native = local / scale, sub = local % scale;
  return ivec2(uvec2((ao.source.x / 2u + native.x) % 1024u,
                     (ao.source.y + native.y * ao.source.z) % 512u) * scale + sub);
}
float ao_depth(ivec2 p) {
  if (any(lessThan(p, ivec2(0))) || any(greaterThanEqual(p, ao_full_size()))) return 0.0;
  float d = texelFetch(u_ao_depth, ao_source(p), 0).r;
  return d > 0.0 && d < 0.9999999 ? d * 4294967296.0 : 0.0;
}
vec3 ao_position(ivec2 p, float z) {
  vec2 native = (vec2(p) + 0.5) / float(ao.source.w);
  native.y *= float(ao.source.z);
  native += vec2(ao.source.x / 2u, ao.source.y);
  return vec3((native - ao.camera.zw) * z / ao.camera.xy, z);
}
ivec2 ao_center(ivec2 p) { return min(p * 2 + 1, ao_full_size() - 1); }

// ASSAO edge packing; Intel/Godot MIT, see third_party/godot_ssao.
float ao_pack_edges(vec4 e) {
  return dot(round(clamp(e, 0.0, 1.0) * 3.05), vec4(64,16,4,1) / 255.0);
}
vec4 ao_unpack_edges(float value) {
  uint p = uint(value * 255.5);
  return vec4((p >> 6u) & 3u, (p >> 4u) & 3u, (p >> 2u) & 3u, p & 3u) / 3.0;
}
