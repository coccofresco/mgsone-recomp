#ifndef PS1_GTE_COLOR_MATH_GLSL
#define PS1_GTE_COLOR_MATH_GLSL

// RGB-only DPCS(sf=1), shared by GPU source constructors. The architectural
// FLAG/FIFO are intentionally not presentation state. Requires shaderInt64.
// Signed intermediate saturation happens BEFORE multiplying by IR0; clamping
// only the final RGB silently diverges for valid negative/large far colours.
int ps1_gte_dpcs_channel(int rgb, int far_color, int factor) {
  const int64_t input_fixed = int64_t(rgb) * int64_t(65536);
  const int64_t difference = int64_t(far_color) * int64_t(4096) - input_fixed;
  const int delta = clamp(int(difference >> 12), -32768, 32767);
  const int mac = int((input_fixed + int64_t(factor) * int64_t(delta)) >> 12);
  return clamp(mac >> 4, 0, 255);
}

#endif
