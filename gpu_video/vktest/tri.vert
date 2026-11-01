#version 450
// Triangle in the left half; push constants shift it right and set its depth.
layout(push_constant) uniform PC { vec4 color; float z; float dx; } pc;
const vec2 pos[3] = vec2[](vec2(-0.9, -0.9), vec2(0.0, -0.9), vec2(-0.9, 0.9));
void main() { gl_Position = vec4(pos[gl_VertexIndex] + vec2(pc.dx, 0.0), pc.z, 1.0); }
