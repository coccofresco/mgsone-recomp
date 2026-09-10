#version 450

// General GP0(80) CopyRect application pass. Bindings 1/2 hold the immutable
// logical source snapshot; binding 3 independently holds the pre-write
// destination mask used by E6 check-mask. All snapshot copies finish before
// this shader is dispatched for any region, so overlap and VRAM wrap cannot
// expose writes from an earlier destination region.

layout(set = 0, binding = 1) uniform sampler2D u_source_color;
layout(set = 0, binding = 2) uniform usampler2D u_source_mask;
layout(set = 0, binding = 3) uniform usampler2D u_destination_mask;

layout(push_constant) uniform ReplayScratchCopyState {
  // xy = logical source-scratch origin, zw = atlas destination origin.
  uvec4 source_destination;
  // xy = region extent, z = force-mask, w = check-destination-mask.
  uvec4 extent_flags;
} u_copy;

layout(location = 0) out vec4 o_color;
layout(location = 1) out uint o_mask;
layout(location = 2) out float o_surface_depth;

void main() {
  const ivec2 destination = ivec2(u_copy.source_destination.zw);
  const ivec2 relative = ivec2(gl_FragCoord.xy) - destination;
  const ivec2 extent = ivec2(u_copy.extent_flags.xy);
  if (any(lessThan(relative, ivec2(0))) ||
      any(greaterThanEqual(relative, extent))) {
    discard;
  }

  const ivec2 source = ivec2(u_copy.source_destination.xy) + relative;
  if (u_copy.extent_flags.w != 0u &&
      texelFetch(u_destination_mask, source, 0).r != 0u) {
    // Rejected GP0(80) pixels retain color, mask, R32 and D32 together.
    discard;
  }

  o_color = texelFetch(u_source_color, source, 0);
  const uint source_mask = texelFetch(u_source_mask, source, 0).r;
  o_mask = u_copy.extent_flags.z != 0u ? 1u : source_mask;
  o_surface_depth = 1.0;
  gl_FragDepth = 1.0;
}
