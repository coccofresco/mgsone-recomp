// gl_FragCoord.w interpolates reciprocal clip W. Reconstructing W here keeps
// depth, coverage and perspective UV on the SAME projective surface, including
// clipped triangles and GPU-interpolated presentation frames. Flat coefficients
// also preserve the exact sky and unpositioned sentinels.
layout(location = 8) flat in vec4 v_depth_affine;

float replay_occlusion_depth() {
  return clamp(v_depth_affine.x / gl_FragCoord.w + v_depth_affine.y,
               0.0, 1.0);
}

float replay_surface_depth() {
  return clamp(v_depth_affine.z / gl_FragCoord.w + v_depth_affine.w,
               0.0, v_depth_affine.z == 0.0 && v_depth_affine.w == 1.0
                        ? 1.0 : 0.999999940395355224609375);
}
