// 426 x 240 simulated cells across the 16:9 presentation, independent of
// the native framebuffer and host window. Output remains full resolution.
const vec2 MGS_CRT_PITCH=vec2(320.0/426.0,224.0/240.0);
// Independent emitter reconstruction, informed by CRT-Royale's separation of
// linear signal, beam/phosphor emission and optical diffusion. Callers supply
// fixed cell samples, never the already-shaded color at the output fragment.
// The output is the sum of neighboring RGB emitters; there is no image layer
// under a mask. No history, mask texture or additional render target is needed.
vec3 mgs_crt_cell_light(vec3 drive, vec2 distance, vec2 footprint) {
  // Each cell has three spatially distinct phosphors. Bright excitation
  // widens both the spot and the scanline. Distances are in cell units.
  vec3 excitation=clamp(drive,vec3(0),vec3(1));
  vec3 sx=mix(vec3(0.085),vec3(0.125),excitation);
  vec3 sy=mix(vec3(0.17),vec3(0.30),excitation);
  vec3 vx=sx*sx+vec3(footprint.x*footprint.x/12.0);
  vec3 vy=sy*sy+vec3(footprint.y*footprint.y/12.0);
  vec3 dx=vec3(distance.x)-vec3(-1.0/3.0,0.0,1.0/3.0);
  vec3 core=exp2(-0.72134752*(dx*dx/vx+distance.y*distance.y/vy)) /
            (6.28318531*sqrt(vx*vy));
  // A unit-sum tent reconstruction models light diffused by the glass.
  // Its compact support needs only the four enclosing source cells and
  // remains continuous when their neighborhood changes. This is still light
  // from the sampled emitters, never a copy of the continuous source image.
  float glass=max(0.0,1.0-abs(distance.x))*max(0.0,1.0-abs(distance.y));
  return drive*drive*(0.72*core+0.28*glass);
}
vec3 mgs_crt_encode(vec3 light) {
  // Sum and diffuse linear emission before applying the display response.
  // A shoulder leaves bright phosphors visible without hard RGB clipping.
  vec3 encoded=sqrt(max(light,vec3(0)));
  return clamp(encoded/(vec3(1)+max(encoded-vec3(0.70),vec3(0))*0.70),
               vec3(0),vec3(1));
}
