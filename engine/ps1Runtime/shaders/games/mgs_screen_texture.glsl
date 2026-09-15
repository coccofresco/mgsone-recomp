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

// Sensor noise has a perceptual size in the view, independent of render scale.
// A cell is about six UHD pixels: visible on a desktop, not one-pixel dithering.
float mgs_sensor_hash(ivec2 cell, uint frame) {
  uvec2 p = uvec2(cell);
  uint h = p.x * 1973u + p.y * 9277u + frame * 26699u + 89173u;
  h = (h ^ (h >> 13u)) * 1274126177u;
  return float((h ^ (h >> 16u)) & 65535u) / 65535.0;
}
float mgs_flow_noise(vec2 p) {
  ivec2 cell = ivec2(floor(p));
  vec2 f = fract(p);
  f = f * f * (3.0 - 2.0 * f);
  return mix(mix(mgs_sensor_hash(cell, 0u), mgs_sensor_hash(cell + ivec2(1,0), 0u), f.x),
             mix(mgs_sensor_hash(cell + ivec2(0,1), 0u), mgs_sensor_hash(cell + ivec2(1,1), 0u), f.x), f.y);
}
float mgs_luminance(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }
vec3 mgs_sensor_halo(vec2 p, float scale, float bufferX) {
  vec3 halo = vec3(0);
  // Two optical scales. Scatter is added to a sharp center sample; this is
  // not a low-resolution blur of the entire sensor image.
  for (int i = 0; i < 4; ++i) {
    vec2 direction = vec2(i == 0 ? -1.0 : i == 1 ? 1.0 : 0.0,
                          i == 2 ? -1.0 : i == 3 ? 1.0 : 0.0);
    vec3 nearLight = mgs_screen_sample(p + direction * 1.6, scale, bufferX, 0.0, 224.0);
    vec3 farLight = mgs_screen_sample(p + direction * 4.8, scale, bufferX, 0.0, 224.0);
    halo += max(nearLight - vec3(0.24), vec3(0)) * 0.16;
    halo += max(farLight - vec3(0.34), vec3(0)) * 0.09;
  }
  return halo;
}
vec3 mgs_thermal_palette(float heat) {
  // Cold iron -> indigo -> magenta -> ember -> yellow -> near-white.
  // This remaps the game's thermal palette signal, not physical degrees.
  vec3 c = mix(vec3(0.008,0.012,0.035), vec3(0.07,0.06,0.26), smoothstep(0.0,0.23,heat));
  c = mix(c,vec3(0.44,0.035,0.32),smoothstep(0.18,0.43,heat));
  c = mix(c,vec3(0.98,0.13,0.015),smoothstep(0.38,0.64,heat));
  c = mix(c,vec3(1.0,0.83,0.12),smoothstep(0.60,0.84,heat));
  return mix(c,vec3(1.0,0.98,0.88),smoothstep(0.81,1.0,heat));
}
vec3 mgs_screen_effect(float scale) {
  vec2 p = gl_FragCoord.xy / scale;
  float x = u_draw.mgs_projection.y;
  uint kind = uint(u_draw.mgs_view_rows[3].y);
  vec3 current = mgs_screen_sample(p, scale, x, 0.0, 224.0);
  vec4 parameters = u_draw.mgs_view_rows[0];
  if (kind == 3u || kind == 4u) {
    return mix(current, vec3(kind == 4u ? 1.0 : 0.0), clamp(parameters.x, 0.0, 1.0));
  }
  vec2 centered = vec2(p.x - x - 160.0, p.y - 112.0);
  if (kind == 5u) {
    float previousX = 320.0 - x;
    vec2 previousP = p + vec2(previousX - x, 0.0);
    vec3 last = mgs_screen_sample(previousP, scale, previousX, 24.0, 184.0);
    vec3 left = mgs_screen_sample(previousP - vec2(1,0), scale, previousX, 24.0, 184.0);
    vec3 right = mgs_screen_sample(previousP + vec2(1,0), scale, previousX, 24.0, 184.0);
    vec3 up = mgs_screen_sample(previousP - vec2(0,1), scale, previousX, 24.0, 184.0);
    vec3 down = mgs_screen_sample(previousP + vec2(0,1), scale, previousX, 24.0, 184.0);
    vec2 gradient = vec2(mgs_luminance(right-left), mgs_luminance(down-up));
    float change = mgs_luminance(current-last);
    // A bounded image-gradient estimate supplies direction to the shutter
    // trail. It is not a claim of geometry-derived motion vectors.
    vec2 motion = clamp(-change * gradient / (dot(gradient,gradient)+0.015), vec2(-2), vec2(2));
    vec2 trail = motion * 1.5 + centered * 0.022;
    vec3 history = last * 0.28;
    float weights = 0.28;
    for (int i = 1; i <= 6; ++i) {
      float f = float(i) / 6.0;
      float weight = exp(-2.2 * f);
      vec3 echo = mgs_screen_sample(previousP + trail * f, scale, previousX, 24.0, 184.0);
      // A slight spectral separation toward the tail gives the psychic
      // afterimage character without tinting the current sharp foreground.
      echo *= mix(vec3(1),vec3(0.72,0.93,1.12),f*0.45);
      history += echo * weight;
      weights += weight;
    }
    history /= weights;
    float movement = smoothstep(0.012,0.20,length(current-last));
    float persistence = clamp(parameters.x * 1.55,0.0,0.78) * movement;
    vec3 composed = mix(current,history,persistence);
    // Bright ghosts persist in the dark, instead of becoming a milky overlay.
    composed += max(history-current,vec3(0)) * movement * 0.16;
    return clamp(composed,vec3(0),vec3(1));
  }
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
  float frame = u_draw.mgs_view_rows[2].w;
  uint seed = uint(frame);
  vec2 local = vec2(p.x-x,p.y);
  float grain = mgs_sensor_hash(ivec2(floor(local*2.1)),seed)-0.5;
  float clumps = mgs_flow_noise(local*0.65+vec2(float(seed%251u)*7.13, float(seed%193u)*3.71))-0.5;
  vec3 halo = mgs_sensor_halo(p,scale,x);
  float luma = mgs_luminance(current);
  float falloff = 1.0 - 0.43*smoothstep(0.24,1.6,dot(centered/vec2(160,112),centered/vec2(160,112)));
  if (kind == 1u) {
    float gain = 1.0-exp(-3.8*luma);
    float bloom = mgs_luminance(halo)*1.5;
    float flicker = 0.985+0.03*mgs_sensor_hash(ivec2(0),seed/2u);
    float signal = (0.055+gain+bloom)*flicker;
    signal += grain * (0.23-0.07*gain) + clumps*0.09;
    // Monochrome phosphor, with yellow-green whites blooming at high gain.
    vec3 phosphor = mix(vec3(0.10,0.94,0.24),vec3(0.80,1.0,0.65),smoothstep(0.70,1.25,signal));
    return clamp(phosphor*max(0.0,signal)*falloff,vec3(0),vec3(1));
  }
  float emitter = (1.0-smoothstep(0.05,0.18,max(current.g,current.b)/max(current.r,0.001))) * smoothstep(0.24,0.34,current.r);
  float heat = pow(clamp(current.r*0.85+luma*0.12,0.0,1.0),0.95);
  heat = mix(heat,0.58+current.r*0.44,emitter);
  // Temperature-like contrast follows the already authored IR palette.
  // Keep the sensor noise smaller than in the image-intensifying goggles.
  heat = clamp(heat+grain*0.025+clumps*0.018,0.0,1.0);
  vec3 thermal = mgs_thermal_palette(heat);
  thermal += mgs_thermal_palette(clamp(heat+0.15,0.0,1.0))*mgs_luminance(halo)*0.35;
  return clamp(thermal*mix(1.0,falloff,0.45),vec3(0),vec3(1));
}

vec3 mgs_water_color(vec2 p, float scale, out vec2 refracted) {
  float x = u_draw.mgs_projection.y;
  // The full actor phase avoids a discontinuity every 64 PSX strip updates.
  float time = u_draw.mgs_projection.x / 30.0;
  vec2 local = vec2((p.x-x)/320.0,p.y/224.0);
  vec2 q = local*vec2(1.7777778,1.0);
  vec2 drift = vec2(time*0.065,-time*0.047);
  vec2 eddies = vec2(mgs_flow_noise(q*3.1+drift),mgs_flow_noise(q*3.7-drift+7.2));
  vec2 warped = q+(eddies-0.5)*0.19;
  float a = dot(warped,vec2(8.0,5.5))+time*1.20;
  float b = dot(warped,vec2(-5.2,10.0))-time*0.83;
  float c = dot(warped,vec2(16.0,-8.2))+time*0.49;
  float edge = smoothstep(6.0,18.0,p.y)*(1.0-smoothstep(206.0,218.0,p.y));
  edge *= smoothstep(0.0,9.0,p.x-x)*(1.0-smoothstep(311.0,320.0,p.x-x));
  vec2 current = vec2(2.7*cos(a)-1.8*sin(b)+0.65*cos(c),
                      3.8*sin(a)+2.2*cos(b)+0.75*sin(c));
  refracted = p+current*edge;
  // Dispersion grows in stronger currents, instead of coloring every edge
  // equally. Each channel still samples the actual high-resolution scene.
  vec2 dispersion = current*edge*0.10;
  vec3 color = mgs_screen_sample(refracted,scale,x,6.0,218.0);
  color.r = mgs_screen_sample(refracted+dispersion,scale,x,6.0,218.0).r;
  color.b = mgs_screen_sample(refracted-dispersion,scale,x,6.0,218.0).b;
  float caustic = pow(clamp(0.5+0.5*sin(a+1.3*cos(b)),0.0,1.0),12.0);
  caustic *= 0.45+0.55*pow(0.5+0.5*cos(c-b),3.0);
  color *= vec3(0.95,1.015,1.035)*(1.0+edge*caustic*0.22);
  color += vec3(0.0,0.008,0.016)*edge*(0.35+eddies.x);
  return clamp(color,vec3(0),vec3(1));
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
    vec3 water = mgs_water_color(gl_FragCoord.xy / scale,scale,source);
    color = uvec3(round(water*255.0));
    source = clamp(source,vec2(u_draw.mgs_projection.y,6.0)+vec2(0.5/scale),
                   vec2(u_draw.mgs_projection.y+320.0,218.0)-vec2(0.5/scale));
  } else {
    color = uvec3(round(texture(u_screen_color,
        source * scale / vec2(textureSize(u_screen_color, 0))).rgb * 255.0));
  }
  ivec2 p = ivec2(floor(source * scale));
  p = clamp(p, ivec2(0), textureSize(u_screen_color, 0) - ivec2(1));
  stp = texelFetch(u_screen_mask, p, 0).r != 0u;
  visible = any(notEqual(color, uvec3(0))) || stp;
  if ((u_draw.texture_state.z & (1u << 1)) == 0u)
    color = min(uvec3(255), (color * uvec3(clamp(shade, vec3(0), vec3(255)))) / 128u);
  return true;
}
