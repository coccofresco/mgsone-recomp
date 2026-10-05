// Immutable modern framebuffer read. Color remains RGB8; neither coordinates
// nor color pass through the 320x224 RGB555 mirror. The independent STP plane
// travels with it, including visible masked black.
layout(set = 0, binding = 8) uniform sampler2D u_screen_color;
layout(set = 0, binding = 9) uniform usampler2D u_screen_mask;
layout(set = 0, binding = 10) uniform sampler2D u_screen_depth;

float mgs_screen_depth(vec2 p, float scale) {
  ivec2 pixel=clamp(ivec2(p*scale),ivec2(0),textureSize(u_screen_depth,0)-ivec2(1));
  float d=texelFetch(u_screen_depth,pixel,0).r;
  return d>=0.9999999 ? -1.0 : d*4294967296.0;
}

float mgs_screen_output_depth(float fallback) {
  // Zoom trails mix history with the current scene without replacing its
  // geometry. Keep that scene's R32 depth for the following underwater pass;
  // the independent operational depth/occlusion policy remains unchanged.
  if ((u_draw.texture_state.z & ((1u<<7)|(1u<<9))) == ((1u<<7)|(1u<<9)) &&
      uint(u_draw.mgs_view_rows[3].y)==8u) {
    float scale=float(textureSize(u_screen_depth,0).x)/640.0;
    float depth=mgs_screen_depth(gl_FragCoord.xy/scale,scale);
    return depth>0.0 ? depth/4294967296.0 : 1.0;
  }
  return fallback;
}

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
#include "mgs_crt.glsl"
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
  // GoggleIR uses a warm red palette (not a rainbow temperature map).
  // Keep that visual language, with a continuous highlight shoulder.
  vec3 c=mix(vec3(0.12,0.012,0.010),vec3(0.72,0.16,0.13),smoothstep(0.0,0.65,heat));
  return mix(c,vec3(1.0,0.66,0.48),smoothstep(0.60,1.0,heat));
}
// Convert a sampled camera cell to electron-gun drive before reconstruction.
// The cell is fixed on the display: noise and exposure belong to its signal.
vec3 mgs_crt_drive(vec3 current,vec2 at,uint seed,bool cinematic,bool red) {
  if(cinematic) {
    float level=red ? max(current.r,mgs_luminance(current)) : mgs_luminance(current);
    float line=floor(at.y*0.5);
    vec3 pedestal=red ? vec3(80,0,0)*0.82/255.0 : vec3(56,120,40)*0.60/255.0;
    vec3 phosphor=red ? vec3(1,0.045,0.020) : vec3(0.22,1,0.25);
    float grain=mgs_sensor_hash(ivec2(floor(at)),seed)-0.5;
    float lineNoise=mgs_sensor_hash(ivec2(0,int(line)),seed)-0.5;
    // Camera spectral response belongs to the signal that excites the guns;
    // adding a red/green veil after reconstruction would bypass the tube.
    vec3 video=mix(current,vec3(level)*phosphor,red ? 0.75 : 0.55);
    vec3 drive=video*(red ? 1.35 : 1.40)+pedestal;
    drive+=phosphor*(grain*0.055*(0.35+level)+lineNoise*0.016);
    vec3 hot=red ? vec3(1,0.27,0.16) : vec3(0.68,1,0.72);
    drive+=hot*smoothstep(0.12,red ? 0.48 : 0.32,level)*0.075;
    return clamp(drive/(vec3(1)+max(drive-vec3(0.70),vec3(0))*0.85),vec3(0),vec3(1));
  }
  float grain=mgs_sensor_hash(ivec2(floor(at*2.1)),seed)-0.5;
  float clumps=mgs_flow_noise(at*0.65+vec2(float(seed%251u)*7.13,float(seed%193u)*3.71))-0.5;
  vec2 centered=at-vec2(160,112);
  float falloff=1.0-0.43*smoothstep(0.24,1.6,dot(centered/vec2(160,112),centered/vec2(160,112)));
  float luma=mgs_luminance(current);
  if(!red) {
    float gain=1.0-exp(-1.65*luma);
    float flicker=0.985+0.03*mgs_sensor_hash(ivec2(0),seed/2u);
    float signal=(0.03+gain)*flicker;
    vec3 phosphor=mix(vec3(0.10,0.94,0.24),vec3(0.72,1.0,0.70),smoothstep(0.48,0.95,signal));
    vec3 authored=current+vec3(56,120,40)*(0.65/255.0);
    vec3 light=mix(authored,phosphor*max(0.0,signal),0.25);
    light=min(light,vec3(0.58))+0.40*(1.0-exp(-max(light-0.58,vec3(0))/0.40));
    return clamp(light*mix(1.0,falloff,0.55)+phosphor*(grain*0.17+clumps*0.07),vec3(0),vec3(1));
  }
  float emitter=(1.0-smoothstep(0.05,0.18,max(current.g,current.b)/max(current.r,0.001)))*smoothstep(0.24,0.34,current.r);
  float heat=pow(clamp(current.r*0.85+luma*0.12,0.0,1.0),0.95);
  heat=mix(heat,0.58+current.r*0.44,emitter);
  heat=clamp(heat+grain*0.025+clumps*0.018,0.0,1.0);
  vec3 authored=clamp(current+vec3(80,0,0)*(0.70/255.0),vec3(0),vec3(1));
  vec3 thermal=mix(authored,mgs_thermal_palette(heat),0.35);
  return clamp(thermal*mix(1.0,falloff,0.45),vec3(0),vec3(1));
}
vec3 mgs_crt_sample_cell(vec2 center,vec2 pitch,float scale,float x,
                         uint seed,bool cinematic,bool red) {
  // Two samples integrate the modern source inside each camera cell. They
  // are fixed for the cell, not moved with the output pixel or RGB aperture.
  vec2 at=center+vec2(x,0.0);
  if(cinematic) {
    float jitter=(mgs_sensor_hash(ivec2(int(floor(center.y*0.5)),0),seed/2u)-0.5)*0.22;
    at.x+=jitter;
  }
  vec2 halfTap=pitch*0.25;
  vec3 a=mgs_screen_sample(at-halfTap,scale,x,0,224);
  vec3 b=mgs_screen_sample(at+halfTap,scale,x,0,224);
  vec3 signal=sqrt(0.5*(a*a+b*b)); // integrate source coverage in linear light
  if(uint(u_draw.mgs_view_rows[3].y)==12u) return signal; // Codec: authored RGB
  return mgs_crt_drive(signal,center,seed,cinematic,red);
}
vec3 mgs_crt_reconstruct(vec2 p,float scale,float x,bool cinematic,bool red) {
  vec2 local=p-vec2(x,0);
  vec2 pitch=MGS_CRT_PITCH;
  // One shared 240-line tube across cameras, sensors and the complete Codec.
  vec2 phase=local/pitch-vec2(0.5);
  vec2 nearest=floor(phase);
  vec2 footprint=fwidth(local)/pitch;
  uint seed=uint(u_draw.mgs_view_rows[2].w);
  vec3 emission=vec3(0);
  for(int row=0;row<=1;++row) for(int col=0;col<=1;++col) {
    vec2 cell=nearest+vec2(col,row);
    vec2 center=(cell+vec2(0.5))*pitch;
    vec3 drive=mgs_crt_sample_cell(center,pitch,scale,x,seed,cinematic,red);
    emission+=mgs_crt_cell_light(drive,phase-cell,footprint);
  }
  // Wide glare is driven by the same cell signals in linear light. There is
  // no unmasked original image or post-CRT noise added to the output.
  const vec2 directions[4]=vec2[4](vec2(-0.79,-0.31),vec2(0.31,-0.79),
                                  vec2(0.79,0.31),vec2(-0.31,0.79));
  vec3 glare=vec3(0);
  float radius=cinematic ? (red ? 6.8 : 11.5) : 3.2;
  for(int i=0;i<4;++i) {
    vec2 cell=nearest+floor(directions[i]*radius/pitch+0.5);
    vec3 drive=mgs_crt_sample_cell((cell+vec2(0.5))*pitch,pitch,scale,x,seed,cinematic,red);
    glare+=drive*drive*0.25;
  }
  return mgs_crt_encode(emission+glare*(cinematic ? (red ? 0.16 : 0.28) : 0.10));
}
vec3 mgs_screen_effect(float scale) {
  vec2 p = gl_FragCoord.xy / scale;
  float x = u_draw.mgs_projection.y;
  uint kind = uint(u_draw.mgs_view_rows[3].y);
  vec4 parameters = u_draw.mgs_view_rows[0];
  if (kind == 7u || (kind == 1u && parameters.y != 0.0))
    return mgs_crt_reconstruct(p,scale,x,true,kind==7u);
  if(kind==12u) return mgs_crt_reconstruct(p,scale,x,false,false);
  if(kind==1u || kind==2u)
    return mgs_crt_reconstruct(p,scale,x,false,kind==2u);
  vec3 current = mgs_screen_sample(p, scale, x, 0.0, 224.0);
  if (kind == 11u) {
    float depth=mgs_screen_depth(p,scale);
    if (depth<=0.0) return current; // HUD and unresolved surfaces stay sharp
    float nearFocus=max(1.0,parameters.x-parameters.y);
    float farFocus=max(nearFocus+1.0,2.0*parameters.x+parameters.y);
    float coc=depth<nearFocus ? (nearFocus-depth)/nearFocus
                             : max(0.0,(depth-farFocus)/farFocus);
    float radius=min(7.5,coc*9.0);
    if (radius<0.12) return current;
    vec3 total=current;
    float weight=1.0;
    // Vogel disk, with depth rejection at silhouettes. Radius is continuous
    // in scene units; taps always sample the full-resolution attachment.
    for (int i=0;i<20;++i) {
      float angle=float(i)*2.399963;
      vec2 tap=p+vec2(cos(angle),sin(angle))*radius*sqrt((float(i)+0.5)/20.0);
      tap=clamp(tap,vec2(x,0.0)+0.5/scale,vec2(x+320.0,224.0)-0.5/scale);
      float neighbor=mgs_screen_depth(tap,scale);
      float accepted=neighbor>0.0 ? smoothstep(0.0,1.0,1.0-abs(depth-neighbor)/max(256.0,depth*0.12)) : 0.0;
      total+=accepted*mgs_screen_sample(tap,scale,x,0.0,224.0);
      weight+=accepted;
    }
    return total/weight;
  }
  if (kind == 3u || kind == 4u) {
    // FadeIO's envelope is a light offset, not an opacity. Multiplicative
    // fading leaves a dark scene visible after the authored cut to black.
    float offset=clamp(parameters.x,0.0,1.0)*(kind==4u ? 1.0 : -1.0);
    return clamp(current+offset,vec3(0),vec3(1));
  }
  vec2 centered = vec2(p.x - x - 160.0, p.y - 112.0);
  if (kind == 9u) {
    // StgfdIo alternates white and yellow flashes before its long recovery.
    // Preserve all three authored envelopes, not just their maximum/white.
    float amount = max(parameters.r, max(parameters.g, parameters.b));
    vec3 authored=min(current+parameters.rgb,vec3(1));
    if(all(greaterThanEqual(parameters.rgb,vec3(0.999)))) return vec3(1);
    vec3 shoulder=vec3(1)-(vec3(1)-current)*exp(-parameters.rgb*1.7);
    vec3 glare=mgs_sensor_halo(p,scale,x)*parameters.rgb*0.12;
    return clamp(mix(authored,shoulder,0.25*amount)+glare,vec3(0),vec3(1));
  }
  if (kind == 10u) {
    // Alarm lighting keeps local scene contrast instead of saturating a
    // PS1 additive TILE. The authored RGB and pulse remain authoritative.
    if (uint(parameters.w) == 2u)
      return mix(max(vec3(0),current-parameters.rgb),current*exp(-parameters.rgb*2.0),0.25);
    if (uint(parameters.w) == 0u) return mix(current, parameters.rgb, 0.5);
    vec3 light=parameters.rgb*(uint(parameters.w)==3u ? 0.25 : 1.0);
    vec3 shoulder=vec3(1)-(vec3(1)-current)*exp(-light*1.8);
    return mix(min(vec3(1),current+light),shoulder,0.25);
  }
  if (kind == 8u) {
    float previousX = 320.0 - x;
    float top = clamp(parameters.z, 0.0, 223.0);
    float bottom = clamp(parameters.w, top + 1.0, 224.0);
    float zoom = uint(parameters.x) == 2u ? -0.065 : 0.065;
    if (uint(parameters.x) == 0u) zoom = 0.024;
    float time = u_draw.mgs_view_rows[2].w / 30.0;
    vec2 flow = parameters.y * vec2(sin(time*2.1+p.y*0.014), cos(time*1.7+p.x*0.009));
    vec3 shutter = vec3(0.0);
    float total = 0.0;
    for (int i = 0; i < 10; ++i) {
      float t = float(i) / 9.0;
      float weight = exp(-2.0*t);
      shutter += weight * mgs_screen_sample(p + vec2(previousX-x,0.0) +
          (centered*zoom + flow)*t, scale, previousX, top, bottom);
      total += weight;
    }
    float amount = uint(parameters.x) == 3u ? 0.25 : 0.5;
    return clamp(mix(current, shutter/total, amount), vec3(0.0), vec3(1.0));
  }
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
    vec2 trail = motion * 1.1;
    vec3 history = last * 0.28;
    float weights = 0.28;
    for (int i = 1; i <= 6; ++i) {
      float f = float(i) / 6.0;
      float weight = exp(-2.2 * f);
      vec3 echo = mgs_screen_sample(previousP + trail * f, scale, previousX, 24.0, 184.0);
      // BlurPure modulates all channels equally. Keep the scene's hue.
      history += echo * weight;
      weights += weight;
    }
    history /= weights;
    float movement = smoothstep(0.012,0.20,length(current-last));
    float persistence = clamp(parameters.x * 1.10,0.0,0.65) * movement;
    vec3 composed = mix(current,history,persistence);
    // Bright ghosts persist in the dark, instead of becoming a milky overlay.
    composed += max(history-current,vec3(0)) * movement * 0.05;
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
  return current;
}

vec3 mgs_water_color(vec2 p, float scale, out vec2 refracted) {
  float x = u_draw.mgs_projection.y;
  // The full actor phase avoids a discontinuity every 64 PSX strip updates.
  float time = u_draw.mgs_projection.x / 30.0;
  vec2 local = vec2((p.x-x)/320.0,p.y/224.0);
  vec2 q = local*vec2(1.7777778,1.0);
  vec2 drift = vec2(time*0.065,-time*0.047);
  if (u_draw.mgs_view_rows[4].x != 0.0) {
    // Hot air rises; no blue grade, water caustics or large underwater swells.
    // A smooth 2D flow field replaces the shared wt_view strip mechanism.
    float envelope = smoothstep(6.0, 22.0, p.y) * (1.0-smoothstep(202.0,218.0,p.y));
    float flow = mgs_flow_noise(q*vec2(14.0,5.0) + vec2(0.0,-time*0.9));
    float detail = mgs_flow_noise(q*vec2(27.0,11.0) + vec2(time*0.12,-time*1.5));
    refracted = p + vec2((flow-0.5)*3.2 + (detail-0.5)*0.8,
                         (detail-0.5)*0.9) * envelope;
    return mgs_screen_sample(refracted, scale, x, 6.0, 218.0);
  }
  vec2 eddies = vec2(mgs_flow_noise(q*3.1+drift),mgs_flow_noise(q*3.7-drift+7.2));
  vec2 warped = q+(eddies-0.5)*0.19;
  float a = dot(warped,vec2(8.0,5.5))+time*1.20;
  float b = dot(warped,vec2(-5.2,10.0))-time*0.83;
  float c = dot(warped,vec2(16.0,-8.2))+time*0.49;
  float edge = smoothstep(6.0,18.0,p.y)*(1.0-smoothstep(206.0,218.0,p.y));
  edge *= smoothstep(0.0,9.0,p.x-x)*(1.0-smoothstep(311.0,320.0,p.x-x));
  // At most 0.70 native pixel of displacement (~7 UHD pixels), with a
  // slow two-dimensional flow. Preserve silhouettes instead of bending them.
  vec2 current = vec2(0.27*cos(a)-0.18*sin(b)+0.065*cos(c),
                      0.38*sin(a)+0.22*cos(b)+0.075*sin(c));
  refracted = p+current*edge;
  // Dispersion grows in stronger currents, instead of coloring every edge
  // equally. Each channel still samples the actual high-resolution scene.
  vec2 dispersion = current*edge*0.10;
  vec3 color = mgs_screen_sample(refracted,scale,x,6.0,218.0);
  color.r = mgs_screen_sample(refracted+dispersion,scale,x,6.0,218.0).r;
  color.b = mgs_screen_sample(refracted-dispersion,scale,x,6.0,218.0).b;
  // Deep-water absorption and shadow contrast, with no additive veil or
  // screen-space caustics lifting the authored blacks. The original intro
  // has a very dark seabed; retain continuous RGB8 gradients in those shadows.
  color=pow(max(color,vec3(0)),vec3(mix(1.0,1.22,edge)));
  color *= mix(vec3(1),vec3(0.80,0.82,0.88),edge);
  // Beer-Lambert extinction in camera-space scene units. The underwater
  // medium is dark blue: distant detail fades without a milky global veil.
  // HUD/unpositioned pixels have no physical depth and remain unaffected.
  float depth=mgs_screen_depth(refracted,scale);
  if(depth>0.0) {
    float distance=max(0.0,depth-2500.0);
    vec3 transmission=exp(-distance*vec3(0.000060,0.000045,0.000033));
    vec3 fogged=color*transmission+vec3(0.004,0.012,0.020)*(vec3(1)-transmission);
    color=mix(color,fogged,edge);
  }
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
