// MGS immutable original-asset storage, shared with the device-local cache.
// Other profiles keep mgs_projection.w == 0 and never read this binding.
layout(set=1, binding=2, std430) readonly buffer MgsOriginalAssets {
  uint words[];
} u_mgs;
uint mgs_word_at_byte(uint offset) {
  uint shift=(offset & 3u)*8u;
  uint value=u_mgs.words[offset/4u];
  if (shift==0u) return value;
  return (value>>shift) | (u_mgs.words[offset/4u+1u]<<(32u-shift));
}
