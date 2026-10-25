#version 440
layout(location = 0) in vec4 vertex;
layout(location = 0) out vec2 uv;
void main() { uv = vertex.xy; gl_Position = vertex; }
