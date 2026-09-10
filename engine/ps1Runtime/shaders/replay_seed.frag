#version 450

// M0Reanchor seed contract. The source is the complete guest-authoritative
// 1024x512 ABGR1555 snapshot. Vulkan expands it into the persistent true-color
// and independent mask atlases at internal_scale without a CPU-side 4x image.

layout(set = 0, binding = 0) uniform usampler2D u_native_shadow;

layout(push_constant) uniform ReplaySeedPush {
  uint internal_scale;
} pc;

layout(location = 0) out vec4 o_color;
layout(location = 1) out uint o_mask;
layout(location = 2) out float o_surface_depth;

uint expand5To8(uint value) {
  value &= 31u;
  return (value << 3u) | (value >> 2u);
}

void main() {
  const uvec2 atlas_pixel = uvec2(gl_FragCoord.xy);
  const uvec2 native_extent = uvec2(textureSize(u_native_shadow, 0));
  const uvec2 native_pixel =
      min(atlas_pixel / pc.internal_scale, native_extent - uvec2(1u));
  const uint packed =
      texelFetch(u_native_shadow, ivec2(native_pixel), 0).r & 0xffffu;

  const uvec3 rgb5 =
      uvec3(packed, packed >> 5u, packed >> 10u) & uvec3(31u);
  const uvec3 rgb8 =
      uvec3(expand5To8(rgb5.r), expand5To8(rgb5.g), expand5To8(rgb5.b));
  o_color = vec4(vec3(rgb8) * (1.0 / 255.0), 1.0);
  o_mask = (packed >> 15u) & 1u;
  o_surface_depth = 1.0;
}
