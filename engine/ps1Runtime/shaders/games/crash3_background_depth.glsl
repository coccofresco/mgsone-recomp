// Crash's camera-anchored/background class is a layer, not a finite scene
// surface. Keep its genuine clip W for perspective UV, but put clip Z exactly
// on clip W. Multiplying W by nextafter(1,0) instead produces different D32
// values after division and can make equal-depth sky triangles reject one
// another. Z == W is exact through homogeneous clipping as well.
//
// R32 still stores the reserved nextafter(1,0) surface class: it distinguishes
// a positioned background from an untouched/unpositioned pixel (1.0).
bool crash3_background_depth(float occlusion_multiplier, float occlusion_bias) {
  return occlusion_multiplier == 0.0 &&
         floatBitsToUint(occlusion_bias) == 0x4f7fffffu;
}

float crash3_clip_depth(float normalized_depth, float projective_w,
                       float occlusion_multiplier, float occlusion_bias) {
  return crash3_background_depth(occlusion_multiplier, occlusion_bias)
             ? projective_w : normalized_depth * projective_w;
}
