// Immutable modern framebuffer read. Color remains RGB8; neither coordinates
// nor color pass through the 320x224 RGB555 mirror. The independent STP plane
// travels with it, including visible masked black.
layout(set = 0, binding = 8) uniform sampler2D u_screen_color;
layout(set = 0, binding = 9) uniform usampler2D u_screen_mask;

bool mgs_screen_texel(vec2 uv, vec3 shade, out uvec3 color, out bool stp,
                      out bool visible) {
  if ((u_draw.texture_state.z & (1u << 7)) == 0u) return false;
  // Preserve the fractional part within every original texel, including the
  // final texel of a sprite's inclusive UV domain.
  uv = clamp(uv, u_draw.uv_domain.xy, u_draw.uv_domain.zw + vec2(0.99999));
  vec2 page = vec2((u_draw.texture_state.x & 15u) * 64u,
                  ((u_draw.texture_state.x >> 4u) & 1u) * 256u);
  float scale = float(textureSize(u_screen_color, 0).x) / 640.0;
  vec2 source = page + uv;
  if ((u_draw.texture_state.z & (1u << 8)) != 0u) {
    source = gl_FragCoord.xy / scale;
    float phase = u_draw.mgs_projection.x * 0.19634954085;
    float edge = smoothstep(6.0, 14.0, source.y) *
                 (1.0 - smoothstep(210.0, 218.0, source.y));
    // A two-dimensional field, independent of the original SPRT scanlines.
    // Horizontal phase modulation bends the wave fronts across the image;
    // the weaker diagonal current adds lateral refraction. Both periods
    // follow the actor, so pause and savestate restore remain coherent.
    float x = source.x - u_draw.mgs_projection.y;
    float wave = source.y * 0.09817477 + phase +
                 0.45 * sin(x * 0.03926990817 + phase);
    float crossCurrent = source.y * 0.049087385 + x * 0.01963495408 -
                         phase * 0.5;
    source += edge * vec2(0.7 * sin(crossCurrent), 3.5 * sin(wave));
    source = clamp(source,
        vec2(u_draw.mgs_projection.y, 6.0) + vec2(0.5 / scale),
        vec2(u_draw.mgs_projection.y + 320.0, 218.0) - vec2(0.5 / scale));
  }
  ivec2 p = ivec2(floor(source * scale));
  p = clamp(p, ivec2(0), textureSize(u_screen_color, 0) - ivec2(1));
  color = uvec3(round(texture(u_screen_color,
      source * scale / vec2(textureSize(u_screen_color, 0))).rgb * 255.0));
  stp = texelFetch(u_screen_mask, p, 0).r != 0u;
  visible = any(notEqual(color, uvec3(0))) || stp;
  if ((u_draw.texture_state.z & (1u << 1)) == 0u)
    color = min(uvec3(255), (color * uvec3(clamp(shade, vec3(0), vec3(255)))) / 128u);
  return true;
}
