// Original PCX indices and native palette words, resident for the session.
#define MGS_ORIGINAL_TEXTURE_AVAILABLE
#include "mgs_asset_storage.glsl"
uint nativeWord(uint x, uint y);
uvec3 mgs_thermal_emission(uvec3 color, vec3 shade) {
  if (u_draw.mgs_projection.w < 3.0) return color;
  // Reserve a saturated red response for source-mesh IR emitters.
  // The screen sensor remaps this signal to yellow/white heat.
  // Keep modest form shading above an emissive floor, even in darkness.
  float form = dot(clamp(shade,vec3(0),vec3(255)),vec3(0.2126,0.7152,0.0722));
  float detail = dot(vec3(color),vec3(0.2126,0.7152,0.0722))/255.0;
  return uvec3(uint(96.0+min(110.0,65.0*sqrt(detail)+form*0.20)),5u,2u);
}
bool mgs_original_texel(uvec2 uv, out uint texel) {
  if (u_draw.mgs_texture_origin.z == 0) return false;
  ivec2 local = ivec2(uv) - u_draw.mgs_texture_origin.xy;
  if (any(lessThan(local, ivec2(0))) ||
      any(greaterThanEqual(local, ivec2(u_draw.mgs_texture_data.zw)))) return false;
  uint scroll = uint(u_draw.mgs_texture_origin.w);
  uint period = scroll >> 16u;
  if (period != 0u && uint(local.y) < period)
    local.y = int((uint(local.y) + (scroll & 65535u)) % period);
  uint offset = u_draw.mgs_texture_data.x + uint(local.y) * u_draw.mgs_texture_data.z + uint(local.x);
  uint index = (u_mgs.words[offset / 4u] >> ((offset & 3u) * 8u)) & 255u;
  uint clutRow=(u_draw.texture_state.y>>6u)&511u;
  if(u_draw.mgs_surface_effect[3].w!=0.0 && clutRow>=226u && clutRow<256u) {
    // Sepia/monotone transforms the original material palette once, in float
    // at presentation time. STP and transparent zero remain asset properties.
    texel=mgs_word_at_byte(u_draw.mgs_texture_data.y+index*4u)&65535u;
    return true;
  }
  if (u_draw.mgs_texture_origin.z == 2) {
    uint word = u_draw.mgs_palette[index / 8u][(index / 2u) & 3u];
    texel = (word >> ((index & 1u) * 16u)) & 65535u;
    return true;
  }
  if (u_draw.mgs_texture_origin.z == 3) {
    uint clut = u_draw.texture_state.y;
    texel = nativeWord((clut & 63u) * 16u + index, (clut >> 6u) & 511u);
    return true;
  }
  texel = mgs_word_at_byte(u_draw.mgs_texture_data.y + index * 4u) & 65535u;
  return true;
}

// Continuous reconstruction of authored effect artwork, in RGB8. Filtering
// includes transparent black in premultiplied color, avoiding bright fringes.
bool mgs_original_filtered(vec2 uv, out vec4 sampleColor, out bool stp) {
  sampleColor=vec4(0);
  stp=false;
  if(u_draw.mgs_texture_origin.z==0) return false;
  vec2 base=floor(uv),f=fract(uv);
  float masked=0.0;
  for(int y=0;y<2;++y) for(int x=0;x<2;++x) {
    uint texel=0u;
    if(!mgs_original_texel(uvec2(ivec2(base)+ivec2(x,y)) & uvec2(255u),texel)) texel=0u;
    uvec3 c=uvec3(texel,texel>>5u,texel>>10u)&uvec3(31u);
    float weight=(x==0 ? 1.0-f.x : f.x)*(y==0 ? 1.0-f.y : f.y);
    vec3 rgb=vec3((c<<3u)|(c>>2u));
    uint row=(u_draw.texture_state.y>>6u)&511u;
    float mode=u_draw.mgs_surface_effect[3].w;
    if(mode!=0.0 && row>=226u && row<256u)
      rgb=vec3(dot(rgb,vec3(1.0/3.0))*(mode==1.0 ? 0.5 : 1.0));
    sampleColor+=vec4(rgb,texel==0u ? 0.0 : 1.0)*weight;
    if((texel&0x8000u)!=0u) masked+=weight;
  }
  stp=masked>sampleColor.a*0.5;
  return true;
}
