#version 450

layout(set = 0, binding = 0) uniform sampler2D u_rgba_image;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;

void main() {
  o_color = texture(u_rgba_image, v_uv);
}
