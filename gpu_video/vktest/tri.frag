#version 450
layout(push_constant) uniform PC { vec4 color; float z; float dx; } pc;
layout(location = 0) out vec4 color;
void main() { color = pc.color; }
