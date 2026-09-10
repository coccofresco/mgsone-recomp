// Shared projective depth contract. Clip W is the authentic projection W;
// presentation depth is an independently authenticated affine function of W.
// Passing vertex depths through fixed-function Z would interpolate W in
// SCREEN space, creating different surfaces for different triangulations.
layout(location = 8) flat out vec4 v_depth_affine;

void replay_set_depth(vec4 affine, vec2 normalization, bool positioned,
                      bool background) {
  v_depth_affine.xy = vec2(affine.x * normalization.x,
                           affine.y * normalization.x + normalization.y);
  if (background)
    v_depth_affine.xy = vec2(0.0, 1.0);
  v_depth_affine.zw = positioned ? affine.zw * (1.0 / 4294967296.0)
                                : vec2(0.0, 1.0);
}
