// The game supplies light direction, color and ambient. Source KMD normals
// follow the float skeleton; no GTE RGB, fixed-point clamp or vertex lighting
// enters this path. Normalize AFTER perspective interpolation so highlights
// and diffuse terminators are evaluated at the actual presentation pixel.
layout(location = 9) in vec3 v_mgs_world_normal;
layout(location = 10) in vec3 v_mgs_world_position;
layout(set = 1, binding = 1, std430) readonly buffer MgsDynamicLightStorage {
  uint words[];
} u_mgs_lights;

vec4 mgs_light_vec4(uint word) {
  return vec4(uintBitsToFloat(u_mgs_lights.words[word]),
               uintBitsToFloat(u_mgs_lights.words[word+1u]),
               uintBitsToFloat(u_mgs_lights.words[word+2u]),
               uintBitsToFloat(u_mgs_lights.words[word+3u]));
}

vec3 mgs_lighting(vec3 authored_color) {
  if (u_draw.mgs_ambient.w == 0.0) return authored_color;
  float length2 = dot(v_mgs_world_normal, v_mgs_world_normal);
  vec3 normal = length2 > 1e-12
                    ? v_mgs_world_normal * inversesqrt(length2) : vec3(0.0);
  vec3 incidence = max(vec3(dot(u_draw.mgs_light_directions[0].xyz, normal),
                            dot(u_draw.mgs_light_directions[1].xyz, normal),
                            dot(u_draw.mgs_light_directions[2].xyz, normal)),
                       vec3(0.0));
  vec3 color = u_draw.mgs_ambient.xyz +
             vec3(dot(u_draw.mgs_light_colors[0].xyz, incidence),
                  dot(u_draw.mgs_light_colors[1].xyz, incidence),
                  dot(u_draw.mgs_light_colors[2].xyz, incidence));
  for (uint i = 0u; i < u_draw.mgs_point_lights.y; ++i) {
    uint word = u_draw.mgs_point_lights.x + i*8u;
    vec4 position = mgs_light_vec4(word);
    vec4 energy = mgs_light_vec4(word+4u);
    // KMD normals and DG_GetLightVector use this incoming-light convention.
    vec3 delta = v_mgs_world_position - position.xyz;
    float distance = length(delta);
    float extent = max(max(abs(delta.x),abs(delta.y)),abs(delta.z));
    float gate = 1.0-smoothstep(energy.w*0.9,energy.w,extent);
    float attenuation = max(0.0, 0.75*(2.0-distance/max(position.w,0.001)));
    color += energy.rgb * max(0.0, dot(normal,delta/max(distance,0.001))) * attenuation * gate;
  }
  return max(color, vec3(0.0));
}

vec3 mgs_albedo(uvec3 texel5) {
  vec3 color=vec3((texel5<<3u)|(texel5>>2u));
  uint row=(u_draw.texture_state.y>>6u)&511u;
  float mode=u_draw.mgs_surface_effect[3].w;
  if(mode!=0.0 && u_draw.mgs_texture_origin.z!=0 && row>=226u && row<256u)
    color=vec3(dot(color,vec3(1.0/3.0))*(mode==1.0 ? 0.5 : 1.0));
  return color;
}
uvec3 mgs_modulate(uvec3 texel5, vec3 authored_color) {
  bool modern_color = (u_draw.texture_state.z & (1u << 10)) != 0u;
  if (!modern_color && u_draw.mgs_ambient.w == 0.0)
    return (texel5 * uvec3(clamp(authored_color, vec3(0), vec3(255)))) >> 4u;
  // Saturate only the final shaded texture, preserving floating-point light
  // above 255 on dark albedo instead of clipping it before modulation.
  vec3 albedo = modern_color ? mgs_albedo(texel5)
                             : vec3(texel5) * 8.0;
  return uvec3(round(clamp(albedo * mgs_lighting(authored_color) / 128.0,
                           vec3(0.0), vec3(255.0))));
}
