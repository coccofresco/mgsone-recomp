// Immutable modern framebuffer read. Color remains RGB8; neither coordinates
// nor color pass through the 320x224 RGB555 mirror. The independent STP plane
// travels with it, including visible masked black.
layout(set = 0, binding = 8) uniform sampler2D u_screen_color;
layout(set = 0, binding = 9) uniform usampler2D u_screen_mask;

vec3 mgs_screen_sample(vec2 p, float scale, float bufferX, float top, float bottom) {
  p = clamp(p, vec2(bufferX, top) + vec2(0.5 / scale),
            vec2(bufferX + 320.0, bottom) - vec2(0.5 / scale));
  return texture(u_screen_color, p * scale / vec2(textureSize(u_screen_color, 0))).rgb;
}

vec3 mgs_screen_effect(float scale) {
  vec2 p = gl_FragCoord.xy / scale;
  float x = u_draw.mgs_projection.y;
  uint kind = uint(u_draw.mgs_view_rows[3].y);
  vec3 current = mgs_screen_sample(p, scale, x, 0.0, 224.0);
  vec4 parameters = u_draw.mgs_view_rows[0];
  if (kind == 3u || kind == 4u) {
    // Exposure-preserving fades: no PS1 subtractive/additive channel clipping.
    // The actor's authored envelope supplies exact black/white endpoints.
    return mix(current, vec3(kind == 4u ? 1.0 : 0.0), clamp(parameters.x, 0.0, 1.0));
  }
  if (kind == 5u) {
    // The other modern draw buffer is the game's previous scene. Keep its
    // true-color/subpixel history and soften the trail, not the current frame.
    float previousX = 320.0 - x;
    vec2 previousP = p + vec2(previousX - x, 0.0);
    vec3 history = mgs_screen_sample(previousP, scale, previousX, 24.0, 184.0) * 0.5;
    for (int i = 0; i < 4; ++i) {
      vec2 direction = vec2((i == 0 ? -1.0 : i == 1 ? 1.0 : 0.0),
                            (i == 2 ? -1.0 : i == 3 ? 1.0 : 0.0));
      history += mgs_screen_sample(previousP + direction * 0.4, scale,
                                    previousX, 24.0, 184.0) * 0.125;
    }
    return mix(current, history, clamp(parameters.x, 0.0, 1.0));
  }
  vec2 centered = vec2(p.x - x - 160.0, p.y - 112.0);
  if (kind == 6u) {
    float radius = length(centered / vec2(1.0, parameters.y));
    float density = smoothstep(parameters.x, 230.0, radius);
    vec3 haze = mix(u_draw.mgs_view_rows[1].rgb, u_draw.mgs_view_rows[2].rgb, density) / 255.0;
    haze *= smoothstep(max(0.0, parameters.x - 2.0), parameters.x + 2.0, radius);
    uint blend = uint(parameters.z);
    if (blend == 2u) return max(vec3(0), current - haze);
    if (blend == 1u) return min(vec3(1), current + haze);
    if (blend == 3u) return min(vec3(1), current + haze * 0.25);
    return mix(current, haze, 0.5);
  }
  // A sensor response and a continuous lens falloff replace 112 alternating
  // dark scanlines. Fine fixed-pattern sensor noise is stable while paused
  // and across savestates; it is generated at the actual Vulkan pixel grid.
  float luma = dot(current, vec3(0.2126, 0.7152, 0.0722));
  vec3 graded;
  if (kind == 1u) {
    float gain = 1.0 - exp(-2.2 * luma);
    graded = mix(current, vec3(0.24, 1.0, 0.18) * (0.07 + gain), 0.82);
  } else {
    // Heat eligibility/CLUT remains authored by the game. This is the optics
    // overlay, not a luminance-based invention of hot/cold object identities.
    graded = current * vec3(1.0, 0.7, 0.65) + vec3(0.12, 0.0, 0.0);
  }
  float vignette = 1.0 - 0.18 * smoothstep(0.25, 1.5, dot(centered / vec2(160,112), centered / vec2(160,112)));
  uvec2 pixel = uvec2(gl_FragCoord.xy) - uvec2(uint(x * scale), 0);
  uint hash = pixel.x * 1973u + pixel.y * 9277u + 89173u;
  hash = (hash ^ (hash >> 13u)) * 1274126177u;
  float grain = (float(hash & 1023u) / 1023.0 - 0.5) * 0.009;
  return clamp(graded * vignette + grain, vec3(0), vec3(1));
}

bool mgs_screen_texel(vec2 uv, vec3 shade, out uvec3 color, out bool stp,
                      out bool visible) {
  if ((u_draw.texture_state.z & (1u << 7)) == 0u) return false;
  // Preserve the fractional part within every original texel, including the
  // final texel of a sprite's inclusive UV domain.
  uv = clamp(uv, u_draw.uv_domain.xy, u_draw.uv_domain.zw + vec2(0.99999));
  vec2 page = vec2((u_draw.texture_state.x & 15u) * 64u,
                  ((u_draw.texture_state.x >> 4u) & 1u) * 256u);
  float scale = float(textureSize(u_screen_color, 0).x) / 640.0;
  if ((u_draw.texture_state.z & (1u << 9)) != 0u) {
    color = uvec3(round(mgs_screen_effect(scale) * 255.0));
    // These replace untextured overlays/complete temporal composition. Black
    // is a valid output, never PS1's transparent-texture sentinel.
    stp = false;
    visible = true;
    return true;
  }
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
