// Original PCX indices and native palette words, resident for the session.
#include "mgs_asset_storage.glsl"
bool mgs_original_texel(uvec2 uv, out uint texel) {
  if (u_draw.mgs_texture_origin.z == 0) return false;
  ivec2 local = ivec2(uv) - u_draw.mgs_texture_origin.xy;
  if (any(lessThan(local, ivec2(0))) ||
      any(greaterThanEqual(local, ivec2(u_draw.mgs_texture_data.zw)))) return false;
  uint offset = u_draw.mgs_texture_data.x + uint(local.y) * u_draw.mgs_texture_data.z + uint(local.x);
  uint index = (u_mgs.words[offset / 4u] >> ((offset & 3u) * 8u)) & 255u;
  if (u_draw.mgs_texture_origin.z == 2) {
    uint word = u_draw.mgs_palette[index / 8u][(index / 2u) & 3u];
    texel = (word >> ((index & 1u) * 16u)) & 65535u;
    return true;
  }
  texel = mgs_word_at_byte(u_draw.mgs_texture_data.y + index * 4u) & 65535u;
  return true;
}
