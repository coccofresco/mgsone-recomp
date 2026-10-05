// Semantic local effects evaluated at the presentation pixel. Every profile
// is bound by an authenticated actor/allocation or complete animation script.
float mgs_surface_coverage = 1.0;
float mgs_bubble_transmission = 1.0;
vec3 mgs_bubble_scattering = vec3(0);
float mgs_blast_surface_light = 0.0;
#include "mgs_bubble_shapes.glsl"
vec2 mgs_surface_uv(vec2 original) {
  if(int(u_draw.mgs_surface_effect[0].x)==25 && u_draw.mgs_surface_effect[0].w!=0.0)
    return vec2(mod(dot(v_mgs_world_position,u_draw.mgs_surface_effect[2].xyz)+
                    u_draw.mgs_surface_effect[0].z,256.0),original.y);
  if(int(u_draw.mgs_surface_effect[0].x)!=24) return original;
  vec3 normal=normalize(v_mgs_world_normal+vec3(0,0,1e-7));
  // Camera-space reflection, evaluated after normal interpolation.
  float scale=float(textureSize(u_screen_color,0).x)/640.0;
  vec2 xy=(gl_FragCoord.xy/scale-u_draw.mgs_projection.yz)/max(1.0,u_draw.mgs_projection.x);
  vec3 reflected=reflect(normalize(vec3(xy,1)),normal);
  vec4 bounds=u_draw.mgs_surface_effect[1];
  vec2 p=vec2(reflected.x,-reflected.y)*0.48+0.5;
  return mix(bounds.xy,bounds.zw,clamp(p,vec2(0),vec2(1)));
}
uvec3 mgs_surface_color(uvec3 original,vec2 uv) {
  vec3 result=vec3(original);
  if(int(u_draw.mgs_surface_effect[0].x)==27) {
    vec3 normal=normalize(v_mgs_world_normal+vec3(0,0,1e-7));
    float rim=pow(1.0-abs(dot(normal,u_draw.mgs_surface_effect[2].xyz)),2.0);
    result*=0.96+0.10*rim; // preserve njatrans palette and native fade to zero
  }
  if(int(u_draw.mgs_surface_effect[0].x)==28) {
    vec3 normal=normalize(v_mgs_world_normal+vec3(0,0,1e-7));
    result*=0.92+0.08*pow(abs(normal.y),0.6); // broad cloth sheen, no vertex RGB
  }
  if(int(u_draw.mgs_surface_effect[0].x)==35) {
    vec3 n=normalize(v_mgs_world_normal+vec3(0,0,1e-7));
    float rim=pow(1.0-abs(n.z),1.7);
    float flow=0.8+0.2*sin(dot(v_mgs_world_position,vec3(0.012,0.018,0.007))-u_draw.mgs_surface_effect[0].y*6.0);
    result*=0.92+0.12*flow+0.06*rim; // authored laser/plasma hue and energy
  }
  if(int(u_draw.mgs_surface_effect[0].x)==39) {
    vec3 n=normalize(v_mgs_world_normal+vec3(0,0,1e-7));
    float haze=0.85+0.15*mgs_flow_noise(v_mgs_world_position.xz*0.002+u_draw.mgs_surface_effect[0].y*0.1);
    result*=0.94+0.06*haze*(0.65+0.35*abs(n.y));
  }
  if(int(u_draw.mgs_surface_effect[0].x)==40) {
    // Authored screen content/characters remain readable; a continuous
    // emissive response replaces the dim, quantized vertex modulation.
    result*=1.04; // retain authored flicker; do not add an unrelated scan roll
  }
  vec4 fog=u_draw.mgs_surface_effect[3];
  if(fog.z!=0.0) result*=1.0-smoothstep(fog.x,fog.y,1.0/gl_FragCoord.w);
  return uvec3(round(clamp(result,vec3(0),vec3(255))));
}
float mgs_segment_distance(vec2 p, vec2 a, vec2 b) {
  vec2 ab = b-a;
  return length(p-a-ab*clamp(dot(p-a,ab)/max(dot(ab,ab),1e-6),0.0,1.0));
}
float mgs_stroke(float distance, float radius, float aa) {
  return 1.0-smoothstep(radius-aa,radius+aa,distance);
}
float mgs_particle_intersection(int kind,vec2 p,float sceneDepth,float centerDepth) {
  if(kind==4) {
    // Integrate only the part of the expanding blast in front of the actual
    // scene surface. The original animation supplies the image, emitter and
    // dimensions; its OT raise is not a physical translation toward camera.
    // A floor through the volume reveals a partial plume, while a panel in
    // front of the entire volume occludes it even with raise=2000.
    vec2 radii=u_draw.mgs_surface_effect[2].yz;
    float radius=sqrt(max(0.0,radii.x*radii.y));
    float halfDepth=radius*sqrt(max(0.0,1.0-dot(p,p)));
    float front=centerDepth-halfDepth,back=centerDepth+halfDepth;
    if(radius>1.0 && u_draw.mgs_projection.w!=0.0) {
      // Source rectangles use the resident unit quad. Recover its center
      // and camera-space extents from the same float transform used to draw
      // it, including widescreen scaling. Intersect the perspective ray with
      // that ellipsoid rather than treating every pixel as its center plane.
      vec3 center=vec3(u_draw.mgs_view_rows[0].w,u_draw.mgs_view_rows[1].w,
                       u_draw.mgs_view_rows[2].w)+0.5*vec3(
        u_draw.mgs_view_rows[0].x+u_draw.mgs_view_rows[0].y,
        u_draw.mgs_view_rows[1].x+u_draw.mgs_view_rows[1].y,
        u_draw.mgs_view_rows[2].x+u_draw.mgs_view_rows[2].y);
      vec3 extent=vec3(length(vec2(u_draw.mgs_view_rows[0].x,u_draw.mgs_view_rows[1].x))*0.5,
                        length(vec2(u_draw.mgs_view_rows[0].y,u_draw.mgs_view_rows[1].y))*0.5,radius);
      float scale=float(textureSize(u_screen_color,0).x)/640.0;
      vec3 ray=vec3((gl_FragCoord.xy/scale-u_draw.mgs_projection.yz)/
                    max(1.0,u_draw.mgs_projection.x),1.0);
      vec3 origin=-center/max(extent,vec3(1)),direction=ray/max(extent,vec3(1));
      float a=dot(direction,direction),b=dot(origin,direction);
      float discriminant=b*b-a*(dot(origin,origin)-1.0);
      if(discriminant>0.0) {
        float root=sqrt(discriminant);
        front=(-b-root)/a;back=(-b+root)/a;
      }
      // A restrained local light, reconstructed on the actual scene surface.
      // Surface color modulates it below, retaining panel/floor detail.
      // The visible side of a wall wholly in front of the blast receives no
      // light from its hidden back side. No extra render pass or ray march.
      vec3 position=ray*sceneDepth;
      vec3 delta=center-position;
      float distance=length(delta);
      vec3 normal=cross(dFdx(position),dFdy(position));
      float norm=length(normal);
      normal=norm>1e-6 ? -normal/norm : vec3(0);
      float facing=max(0.0,dot(normal,delta/max(1.0,distance))+
                       0.5*radius/max(radius,distance));
      float falloff=pow(max(0.0,1.0-distance/(radius*1.5)),2.0);
      mgs_blast_surface_light=1.8*facing*falloff*(1.0-smoothstep(0.8,1.0,length(p)));
    }
    if(back-front>2.0) {
      float fraction=smoothstep(front,back,sceneDepth);
      // Emission through a dense fire volume saturates continuously with
      // visible path length, while a fully blocked ray contributes zero.
      return (1.0-exp(-3.0*fraction))/(1.0-exp(-3.0));
    }
    return smoothstep(-1.0,max(8.0,centerDepth*0.0025),sceneDepth-centerDepth);
  }
  float particle=max(1.0,centerDepth-u_draw.mgs_surface_effect[2].x);
  return smoothstep(-1.0,max(8.0,particle*0.025),sceneDepth-particle);
}
bool mgs_authored_effect(int kind,vec2 uv,vec3 shade,out uvec3 color,
                         out bool stp,out bool visible) {
#ifdef MGS_ORIGINAL_TEXTURE_AVAILABLE
  // Original animation frames remain the shape/palette authority. A generic
  // round plume, five-lobed flash or boot must not erase authored variants.
  bool supported=(kind>=1 && kind<=14) || kind==16 || kind==17 ||
                 kind==18 || kind==19 || kind==22 || kind==26 || kind==29 || kind==31;
  if(!supported) return false;
  vec4 artwork;
  vec2 bounded=clamp(uv,u_draw.uv_domain.xy,u_draw.uv_domain.zw);
  if(!mgs_original_filtered(bounded,artwork,stp)) return false;
  visible=artwork.a>0.001;
  if(!visible) {color=uvec3(0);return true;}
  vec4 bounds=u_draw.mgs_surface_effect[1];
  vec2 q=(bounded-bounds.xy)/max(bounds.zw-bounds.xy,vec2(1));
  vec2 p=q*2.0-1.0;
  float time=u_draw.mgs_surface_effect[0].y;
  float seed=u_draw.mgs_surface_effect[0].w;
  float response=1.0;
  if(kind==1 || kind==2) {
    float flow=mgs_flow_noise(p*2.8+vec2(seed*1.17,-time*0.65));
    response=mix(0.90,1.10,flow); // modest continuous volume, original atlas phase
  } else if(kind==3 || kind==4 || kind==7)
    response=1.0+0.12*exp(-dot(p,p)*3.0);
  else if(kind==5 || kind==18 || kind==31)
    response=1.0+0.06*exp(-dot(p-vec2(-0.25,-0.30),p-vec2(-0.25,-0.30))*32.0);
  vec3 modulation=(u_draw.texture_state.z & (1u<<1))!=0u ? vec3(1) : shade/128.0;
  vec3 rgb=artwork.rgb*modulation*response;
  if((u_draw.texture_state.z & (1u<<7))!=0u) {
    float scale=float(textureSize(u_screen_color,0).x)/640.0;
    vec2 at=gl_FragCoord.xy/scale;
    float x=floor(at.x/320.0)*320.0;
    float depth=mgs_screen_depth(at,scale);
    float intersection=1.0;
    if((u_draw.texture_state.z & (1u<<6))!=0u && depth>0.0 && kind!=19 && kind!=26 &&
       !(kind==7 && u_draw.mgs_surface_effect[2].z!=0.0))
      intersection=mgs_particle_intersection(kind,p,depth,1.0/gl_FragCoord.w);
    float coverage=artwork.a*intersection;
    visible=coverage>0.001;
    bool blended=stp && (u_draw.texture_state.z & (1u<<2))!=0u;
    if(!blended || (u_draw.texture_state.w & 3u)==0u) {
      vec3 background=mgs_screen_sample(at,scale,x,0.0,224.0)*255.0;
      // Premultiplied reconstruction feathers toward the existing scene.
      // ABR0 subsequently applies its half-light response exactly once.
      rgb=background*(1.0-coverage)+rgb*intersection;
    } else rgb*=intersection;
    if(kind==4 && blended && (u_draw.texture_state.w & 3u)==1u) {
      vec3 surface=mgs_screen_sample(at,scale,x,0.0,224.0)*255.0;
      rgb+=surface*vec3(1.4,0.55,0.10)*mgs_blast_surface_light*
           clamp(max(shade.r,max(shade.g,shade.b))/128.0,0.0,1.0);
      visible=visible || mgs_blast_surface_light>0.002;
    }
  }
  color=uvec3(round(clamp(rgb,vec3(0),vec3(255))));
  return true;
#else
  return false;
#endif
}
bool mgs_surface_texel(vec2 uv, vec3 shade, out uvec3 color,
                       out bool stp, out bool visible) {
  int kind = int(u_draw.mgs_surface_effect[0].x);
#ifdef MGS_ORIGINAL_TEXTURE_AVAILABLE
  // Codec portraits are composed into the full-interface CRT pass.
  if(kind==42) return false;
  if(kind==25) {
    vec4 artwork;
    if(mgs_original_filtered(mgs_surface_uv(uv),artwork,stp)) {
      vec3 reflected=artwork.rgb*mgs_lighting(shade)/128.0;
      // Give the authored light pattern more presence without introducing
      // a different pattern. Only its luminous texels receive the lift.
      float highlight=pow(clamp(mgs_luminance(artwork.rgb)/255.0,0.0,1.0),1.35);
      reflected=reflected*1.65+vec3(18,32,40)*highlight;
      color=uvec3(round(clamp(reflected,vec3(0),vec3(255))));
      // The authored water-light material is itself a translucent layer.
      // Preserve its STP plane, including masked black; otherwise it hides
      // the underlying blue hull instead of adding its reflected light.
      visible=artwork.a>0.001;return true;
    }
  }
#endif
  if(kind==41) {
    // HIDEO is a black input-switch backplate, not another noisy sensor.
    color=uvec3(0);
    visible=true;stp=false;return true;
  }
  if(kind==38) {
    float scale=float(textureSize(u_screen_color,0).x)/640.0;
    vec2 p=gl_FragCoord.xy/scale;
    float frame=floor(u_draw.mgs_surface_effect[0].y*30.0);
    float noise=mgs_flow_noise(floor(p*1.6)+vec2(frame*17.17,frame*7.73));
    float bands=0.5+0.5*sin(p.y*1.9+frame*0.81);
    vec3 rgb=vec3(0.14,0.54,0.36)*(0.25+0.7*noise+0.2*bands);
    color=uvec3(round(rgb*255.0));visible=true;stp=false;return true;
  }
  if(kind==36 || (kind==23 && u_draw.mgs_surface_effect[2].z!=0.0)) {
    vec4 plane=u_draw.mgs_surface_effect[1];
    if(dot(v_mgs_world_position,plane.xyz)>plane.w+0.5) {
      color=uvec3(0);visible=false;stp=false;return true;
    }
    if(kind==36) return false;
  }
  if(kind==33 && (u_draw.texture_state.z & (1u<<7))!=0u) {
    float scale=float(textureSize(u_screen_color,0).x)/640.0;
    vec2 at=gl_FragCoord.xy/scale;
    float x=floor(at.x/320.0)*320.0;
    vec2 origin=u_draw.mgs_surface_effect[0].zw+vec2(x,0);
    vec2 cell=max(vec2(1),u_draw.mgs_surface_effect[2].xy);
    vec2 sampleAt=origin+(floor((at-origin)/cell)+0.5)*cell;
    // Deliberate censorship blocks from RGB8 scene color, never PSX VRAM.
    color=uvec3(round(mgs_screen_sample(sampleAt,scale,x,0.0,224.0)*255.0));
    stp=false;visible=true;return true;
  }
  if((kind==20 || kind==21) && (u_draw.texture_state.z & (1u<<7))!=0u) {
    float scale=float(textureSize(u_screen_color,0).x)/640.0;
    vec2 p=gl_FragCoord.xy/scale;
    float x=floor(p.x/320.0)*320.0;
    float time=u_draw.mgs_surface_effect[0].y;
    vec2 at=v_mgs_world_position.xz*0.004;
    vec2 normal=vec2(sin(at.x*1.6+time*0.71+cos(at.y)),cos(at.y*1.9-time*0.57));
    if(kind==20) normal=vec2(dFdx(1.0/gl_FragCoord.w),dFdy(1.0/gl_FragCoord.w))*0.03;
    vec3 refracted=mgs_screen_sample(p+normal*(kind==21 ? 2.7 : 1.2),scale,x,0.0,224.0);
    float fresnel=pow(1.0-abs(normalize(vec3(normal,2.5)).z),3.0);
    float glint=pow(max(0.0,dot(normalize(vec3(normal,2.5)),normalize(vec3(-0.3,-0.4,1)))),48.0);
    vec3 optical=kind==21 ? refracted*vec3(0.65,0.85,0.94) : refracted*vec3(0.94,0.98,1.0);
    optical+=vec3(0.35,0.48,0.56)*glint*(kind==21 ? 0.35 : 0.6)+fresnel*vec3(0.4,0.65,0.85);
#ifdef MGS_ORIGINAL_TEXTURE_AVAILABLE
    vec4 artwork;bool authoredStp;
    if(mgs_original_filtered(clamp(uv,u_draw.uv_domain.xy,u_draw.uv_domain.zw),artwork,authoredStp)) {
      // Wsurface and Glass animate actual material frames (including broken
      // panes). Keep their image, holes, hue and shade envelope under the
      // continuous refraction; an unmarked cyan mirror loses that information.
      vec3 background=mgs_screen_sample(p,scale,x,0.0,224.0);
      vec3 material=artwork.rgb*shade/(128.0*255.0);
      float coverage=artwork.a;
      bool blended=authoredStp && (u_draw.texture_state.z & (1u<<2))!=0u;
      uint abr=u_draw.texture_state.w & 3u;
      vec3 authored;
      if(!blended) authored=background*(1.0-coverage)+material;
      else if(abr==0u) authored=background*(1.0-0.5*coverage)+material*0.5;
      else if(abr==2u) authored=max(vec3(0),background-material);
      else authored=min(vec3(1),background+material*(abr==3u ? 0.25 : 1.0));
      float envelope=clamp(max(shade.r,max(shade.g,shade.b))/128.0,0.0,1.0);
      optical=authored+((refracted-background)*0.45+vec3(glint*0.09+fresnel*0.04))*
              coverage*envelope*0.35;
      visible=coverage>0.001;
      color=uvec3(round(clamp(optical,vec3(0),vec3(1))*255.0));
      stp=false;return true;
    }
#endif
    color=uvec3(round(clamp(optical,vec3(0),vec3(1))*255.0));
    stp=false;visible=true;return true;
  }
  if(kind==23 && (u_draw.texture_state.z & (1u<<7))!=0u) {
    float scale=float(textureSize(u_screen_color,0).x)/640.0;
    vec2 p=gl_FragCoord.xy/scale;
    float x=floor(p.x/320.0)*320.0;
    vec3 n=normalize(v_mgs_world_normal+vec3(0,0,1e-7));
    float time=u_draw.mgs_surface_effect[0].y;
    float rim=pow(1.0-abs(n.z),2.6);
    vec2 distortion=n.xy*(0.35+1.1*rim)+
        vec2(sin(p.y*0.11+time*1.4),cos(p.x*0.09-time))*0.15;
    // EQ_VisibleUnit2 contracts screen X by one quarter around the view
    // centre. That broad lens distortion makes the moving silhouette legible;
    // a tiny normal offset alone made the Ninja effectively disappear. Sample
    // the continuous modern scene through the actual source mesh, keeping the
    // original lens scale without its clamped byte UVs or vertex snapping.
    distortion.x-=0.25*(p.x-x-160.0);
    vec3 refracted=mgs_screen_sample(p+distortion,scale,x,0.0,224.0);
    // Kogaku2 supplies greenish Snake / neutral Ninja modulation itself.
    vec3 optical=refracted*shade/128.0+vec3(0.035)*rim*shade/128.0;
    color=uvec3(round(clamp(optical,vec3(0),vec3(1))*255.0));
    stp=false;visible=true;return true;
  }
  if(mgs_authored_effect(kind,uv,shade,color,stp,visible)) return true;
  // Source-less packets retain the analytic fallback. Approved V6 bubbles,
  // snow and submarine optics have their separate original-driven profiles.
  if (kind == 0 || kind == 13 || kind == 14 || (kind >= 20 && kind != 22 && kind != 26 && kind != 29 && kind != 30 && kind != 31 && kind != 32 && kind != 34))
    return false;
  vec4 bubbleArtwork=vec4(0);
  bool authoredBubble=false;
#ifdef MGS_ORIGINAL_TEXTURE_AVAILABLE
  if(kind==15) {bool mask;authoredBubble=mgs_original_filtered(uv,bubbleArtwork,mask);}
#endif
  vec4 bounds = u_draw.mgs_surface_effect[1];
  vec2 q = (uv-bounds.xy)/max(bounds.zw-bounds.xy,vec2(1));
  if((kind==16 || kind==17 || kind==29) && u_draw.mgs_surface_effect[2].y!=0.0) q=q.yx;
  vec2 p = q*2.0-1.0;
  if(kind==15) p=mgs_bubble_local(q,int(u_draw.mgs_surface_effect[2].y));
  float time = u_draw.mgs_surface_effect[0].y;
  float phase = u_draw.mgs_surface_effect[0].z;
  float seed = u_draw.mgs_surface_effect[0].w;
  float aa = max(length(fwidth(p)),0.002);
  float r = length(p), density = 0.0;
  vec3 tint = vec3(1);
  if(kind==34) {
    vec2 flow=vec2(p.x*2.0+time*1.4,p.y*8.0+seed);
    density=mgs_flow_noise(flow)*pow(max(0.0,1.0-dot(p,p)*0.5),0.7);
    density*=0.6+0.4*mgs_flow_noise(flow*2.7);
    tint=vec3(0.83,0.92,1.0);
  } else if (kind == 1 || kind == 2) {
    // Smooth density at several spatial scales, advected without changing
    // the original particle's trajectory or advancing its native lifetime.
    vec2 flow = p*2.8 + vec2(seed*1.17,-time*0.65);
    float noise = 0.62*mgs_flow_noise(flow) + 0.26*mgs_flow_noise(flow*2.1) +
                  0.12*mgs_flow_noise(flow*4.3);
    density = pow(max(0.0,1.0-dot(p,p)),1.35)*mix(0.25,1.15,noise);
    if (kind == 2) { density *= 0.65; tint=vec3(0.83,0.91,1.0); }
    else tint = vec3(0.92,0.96,1.0);
  } else if (kind == 3 || kind == 4) {
    float turbulence = mgs_flow_noise(p*5.0+vec2(seed,time*2.0));
    float lobes = 0.78+0.22*cos(atan(p.y,p.x)*5.0+seed);
    float shape = kind==3 ? r/max(0.2,lobes) : r+0.25*(turbulence-0.5);
    density = exp(-shape*shape*(kind==3 ? 4.5 : 2.7))*(1.0-smoothstep(0.75,1.0,r));
    tint = mix(vec3(1.0,0.18,0.025),vec3(1.0,0.92,0.62),exp(-r*r*7.0));
    density *= kind==3 ? 1.9 : 1.6;
  } else if (kind == 5 || kind == 18) {
    float lobes = 0.76+0.13*sin(atan(p.y,p.x)*7.0+seed)+0.07*sin(atan(p.y,p.x)*13.0);
    density = 1.0-smoothstep(lobes-aa,lobes+aa,r);
    tint = kind==18 ? vec3(0.48,0.022,0.018) : vec3(0.9,0.045,0.025);
    tint += vec3(0.25)*exp(-dot(p-vec2(-0.25,-0.3),p-vec2(-0.25,-0.3))*55.0);
  } else if (kind == 6 || kind == 32) {
    density = exp(-pow((r-0.76)/0.055,2.0)) + 0.10*exp(-pow((r-0.60)/0.08,2.0));
    tint = vec3(1);
  } else if (kind == 7) {
    density = exp(-r*r*7.5)+0.25*exp(-r*r*2.1);
    density *= 1.0-smoothstep(0.8,1.0,r);
  } else if (kind == 8) {
    density = max(mgs_stroke(mgs_segment_distance(p,vec2(0,-0.68),vec2(0,0.18)),0.14,aa),
                  mgs_stroke(length(p-vec2(0,0.62)),0.15,aa));
  } else if (kind == 9) {
    vec2 center=vec2(-0.03,-0.33);
    float arc = mgs_stroke(abs(length(p-center)-0.34),0.10,aa);
    arc *= smoothstep(-0.34,-0.26,p.x)* (1.0-smoothstep(-0.03,0.05,p.y));
    density=max(arc,mgs_stroke(mgs_segment_distance(p,vec2(0.28,-0.15),vec2(0,0.20)),0.10,aa));
    density=max(density,mgs_stroke(length(p-vec2(0,0.62)),0.13,aa));
  } else if (kind == 10) {
    vec2 h=vec2(abs(p.x),-p.y);
    float a=h.x*h.x+h.y*h.y-0.55;
    float heart=a*a*a-h.x*h.x*h.y*h.y*h.y;
    density=1.0-smoothstep(-aa,aa,heart);
  } else if (kind == 11) {
    float d=min(mgs_segment_distance(p,vec2(-0.5,-0.55),vec2(0.5,-0.55)),
                min(mgs_segment_distance(p,vec2(0.5,-0.55),vec2(-0.5,0.55)),
                    mgs_segment_distance(p,vec2(-0.5,0.55),vec2(0.5,0.55))));
    density=mgs_stroke(d,0.095,aa);
  } else if (kind == 12) {
    float boundary=0.49+0.28*cos(atan(p.x,-p.y)*5.0);
    density=1.0-smoothstep(boundary-aa,boundary+aa,r);
  } else if (kind == 15) {
    // A thin air/water interface, rather than a uniformly luminous ring.
    // Geometry, population, trajectory and lifetime remain actor-owned.
    float radius=r/0.88;
    density=1.0-smoothstep(1.0-aa,1.0+aa,radius);
    tint=vec3(0.80,0.93,1.0);
  } else if (kind == 19) {
    // Asymmetric boot sole; mirrored original UVs distinguish left/right.
    vec2 toe=(p-vec2(-0.08,-0.32))/vec2(0.62,0.54);
    vec2 heel=(p-vec2(0.08,0.54))/vec2(0.43,0.24);
    float sole=min(length(toe),length(heel));
    float tread=mix(0.55,1.0,smoothstep(-0.3,0.3,cos(p.y*30.0+p.x*4.0)));
    density=(1.0-smoothstep(0.92-aa,1.0+aa,sole))*tread;
  } else if (kind == 26) {
    density=exp(-r*r*3.7)*(1.0-smoothstep(0.78,1.0,r));
  } else if (kind == 22) {
    // Each original wedge spans the two radii of the same wind cone.
    // Angular phase is continuous between wedges; do not draw 32 rings.
    float angle=(seed+q.x)*6.2831853/32.0;
    float noise=mgs_flow_noise(vec2(cos(angle),sin(angle))*4.0+time*0.9);
    density=pow(max(0.0,sin(q.y*3.14159265)),1.4)*(0.25+0.75*noise);
    tint=vec3(1);
  } else if (kind == 30) {
    float angle=seed*2.399963;
    vec2 flake=mat2(cos(angle),-sin(angle),sin(angle),cos(angle))*p;
    density=exp(-dot(flake*vec2(1.0,1.35),flake*vec2(1.0,1.35))*4.2);
    density*=1.0-smoothstep(0.72,1.0,r);
    shade=vec3(max(shade.r,max(shade.g,shade.b)));
    tint=vec3(0.86,0.94,1.0);
  } else if (kind == 31) {
    float shape=length(p/vec2(0.45,0.9));
    density=1.0-smoothstep(0.8-aa,0.95+aa,shape);
    tint=vec3(0.56,0.84,0.95)+vec3(0.45)*exp(-dot(p-vec2(-0.15,-0.25),p-vec2(-0.15,-0.25))*55.0);
  } else if (kind == 16 || kind == 17 || kind == 29) {
    float bend=kind==17 ? 0.13*sin(p.y*17.0+time*11.0+seed)*sin(3.141593*q.y) : 0.0;
    density=exp(-pow((p.x-bend)/(kind==17 ? 0.09 : 0.12),2.0));
    density+=0.24*exp(-pow((p.x-bend)/0.32,2.0));
    density*=1.0-smoothstep(0.7,1.0,abs(p.y));
    tint=vec3(1); // original packet colors distinguish laser, spark and arc
  } else return false;
  if(authoredBubble) density=1.0; // source silhouette, not an enclosing sphere
  // Preserve the native envelope and ABR selection, in full RGB8 precision.
  vec3 rgb=clamp(shade*tint*density,vec3(0),vec3(255));
  if ((u_draw.texture_state.z & (1u<<7)) != 0u) {
    float scale=float(textureSize(u_screen_color,0).x)/640.0;
    vec2 at=gl_FragCoord.xy/scale;
    float x=floor(at.x/320.0)*320.0;
    vec3 background=mgs_screen_sample(at,scale,x,0.0,224.0)*255.0;
    float sceneDepth=mgs_screen_depth(at,scale);
    // Smooth intersections against original world depth. Unpositioned HUD
    // samples have no physical depth and must not invent an occluder.
    if ((u_draw.texture_state.z & (1u<<6))!=0u && sceneDepth>0.0 && kind!=19 && kind!=26 && kind!=22 && kind!=32)
      density*=mgs_particle_intersection(kind,p,sceneDepth,1.0/gl_FragCoord.w);
    if(kind==15) {
      vec3 optical;
      float radius=clamp(r/0.88,0.0,1.0);
      float z=sqrt(max(0.0,1.0-radius*radius));
      float rim=exp(-pow((radius-0.78)/0.16,2.0));
      float glint=exp(-dot(p-vec2(-0.28,-0.37),p-vec2(-0.28,-0.37))*32.0);
      vec3 rounded=vec3(130,138,160)*(0.48+0.35*z+0.40*rim)+vec3(170)*glint;
      if(authoredBubble) {
        // An intermediate treatment: authored silhouette/palette share the
        // plume with a restrained continuous body/rim at UHD. The
        // original alpha gates every added highlight, including small holes.
        float shell=1.0-smoothstep(1.0-aa,1.0+aa,r/0.88);
        float envelope=max(shade.r,max(shade.g,shade.b));
        vec3 detail=rounded*vec3(0.93,1.025,1.06)*mix(shade,vec3(envelope),0.65)/128.0;
        optical=bubbleArtwork.rgb*shade/128.0;
        // Mix the two compositing responses, not just their texel colors:
        // half of the authored additive light plus a half-strength lit body.
        // Apply body scattering after ABR scaling, against the live target
        // in both replay paths (the shared snapshot predates the plume).
        uint abr=u_draw.texture_state.w & 3u;
        if(abr==1u || abr==3u) {
          optical*=1.0-0.50*shell;
          mgs_bubble_scattering=0.50*detail*bubbleArtwork.a*shell;
          mgs_bubble_transmission=1.0-0.50*bubbleArtwork.a*shell*
              clamp(envelope/128.0,0.0,1.0)*clamp(density,0.0,1.0);
        }
      } else {
        // Source-less diagnostic/fallback packets retain a soft luminous body.
        optical=rounded*shade/128.0;
      }
      mgs_surface_coverage=clamp(density,0.0,1.0);
      color=uvec3(round(clamp(optical,vec3(0),vec3(255))));
      visible=density>0.002 && (any(greaterThan(color,uvec3(0))) ||
                                any(greaterThan(mgs_bubble_scattering,vec3(0.5))));
      stp=true;return true;
    }
    rgb=clamp(shade*tint*density,vec3(0),vec3(255));
    uint abr=u_draw.texture_state.w & 3u;
    bool blended=(u_draw.texture_state.z & (1u<<2)) != 0u;
    mgs_surface_coverage=clamp(density,0.0,1.0);
    if (!blended) rgb=mix(background,shade*tint,mgs_surface_coverage);
    else if (abr==0u) rgb=shade*tint;
    // Additive/subtractive layers compose against the LIVE destination, so
    // particles in the same actor still accumulate over one another. Only
    // the depth/optical scene reference is shared across the actor.
    color=uvec3(round(clamp(rgb,vec3(0),vec3(255))));
    visible=density>0.002;
    stp=blended;
    return true;
  }
  color=uvec3(round(rgb));
  visible=density>0.002 && any(greaterThan(rgb,vec3(0.25)));
  stp=true;
  return true;
}
