#version 450
// Triangle in the left half: corners at pixel fractions (0.05,0.05), (0.5,0.05), (0.05,0.95)
const vec2 pos[3] = vec2[](vec2(-0.9, -0.9), vec2(0.0, -0.9), vec2(-0.9, 0.9));
void main() { gl_Position = vec4(pos[gl_VertexIndex], 0.0, 1.0); }
