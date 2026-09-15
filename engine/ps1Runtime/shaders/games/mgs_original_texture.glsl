// Original PCX indices and native palette words, resident for the session.
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
