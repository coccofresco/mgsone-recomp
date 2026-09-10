#version 450

// Ordered native-run expansion contract for replay atlas mutations such as
// accepted GP0(A0) upload runs. The source R16_UINT image is the persistent
// guest-authoritative ABGR1555 shadow. A fullscreen triangle uses the full
// atlas viewport and is restricted by a run-sized scissor in a LOAD render
// pass, so gl_FragCoord remains the absolute 4x atlas coordinate and no
// CPU-side scaled raster is required.

layout(set = 0, binding = 0) uniform usampler2D u_native_shadow;

layout(push_constant) uniform ReplayNativeExpandPush {
  // Exact integer atlas/native ratio. The live profile supplies 4; zero is an
  // invalid host contract and must be rejected before command recording.
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
  // Vulkan fragment centers are N+0.5. Conversion to uvec2 truncates to the
  // absolute integer atlas pixel N; dividing by the integer scale maps every
  // 4x4 block to its authoritative native word.
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
  // The A0 pipeline uses ALWAYS + depth write in the LOAD pass. Accepted runs
  // therefore invalidate prior true-depth ownership to the canonical far value
  // without a separate clear or CPU-side depth raster.
  gl_FragDepth = 1.0;
}
