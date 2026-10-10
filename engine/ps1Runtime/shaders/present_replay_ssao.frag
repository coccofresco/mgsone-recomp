#version 450
#extension GL_GOOGLE_include_directive : require
layout(set=0,binding=0) uniform sampler2D u_replay_color;
layout(set=0,binding=1) uniform sampler2D u_ao_depth;
layout(set=0,binding=2) uniform sampler2D u_ao_visibility;
#include "mgs_ssao_common.glsl"
layout(location=0) in vec2 v_uv;
layout(location=0) out vec4 o_color;
void main() {
  ivec2 p=min(ivec2(v_uv*vec2(ao_full_size())),ao_full_size()-1);
  vec3 color=texelFetch(u_replay_color,ao_source(p),0).rgb;
  float z=ao_depth(p);
  // Text, HUD, video, unresolved depth and background are identity operations.
  if(z==0.0) { o_color=vec4(color,1); return; }
  vec2 half_pos=(vec2(p)-1.0)*0.5;
  ivec2 base=ivec2(floor(half_pos));
  vec2 f=fract(half_pos);
  float sum=0.0, weight=0.0;
  // Bilateral upsampling: do not smear a foreground contact onto the sky,
  // through text, or across an unrelated depth discontinuity.
  for(int y=0;y<2;++y) for(int x=0;x<2;++x) {
    ivec2 q=base+ivec2(x,y);
    if(any(lessThan(q,ivec2(0))) || any(greaterThanEqual(q,ao_size()))) continue;
    float neighbor=ao_depth(ao_center(q));
    if(neighbor==0.0) continue;
    float w=(x==0 ? 1.0-f.x : f.x)*(y==0 ? 1.0-f.y : f.y);
    w*=max(0.0,1.0-abs(neighbor-z)/max(1.0,z*0.015));
    sum+=texelFetch(u_ao_visibility,q,0).r*w; weight+=w;
  }
  float visibility=weight>1e-5 ? sum/weight : 1.0;
  // Visibility is applied in linear light (the inherited scanout is RGB8).
  o_color=vec4(color*sqrt(clamp(visibility,0.0,1.0)),1);
}
