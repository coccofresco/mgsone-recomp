#version 450

layout(location = 0) out vec2 v_uv;

void main() {
  const vec2 positions[3] = vec2[3](
      vec2(-1.0, -1.0),
      vec2(3.0, -1.0),
      vec2(-1.0, 3.0));
  const vec2 texcoords[3] = vec2[3](
      vec2(0.0, 0.0),
      vec2(2.0, 0.0),
      vec2(0.0, 2.0));

  gl_Position = vec4(positions[gl_VertexIndex], 0.0, 1.0);
  v_uv = texcoords[gl_VertexIndex];
}
