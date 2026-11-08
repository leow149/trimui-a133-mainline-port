#version 450
layout(early_fragment_tests) in;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;
void main() { color = vec4(uv, 0.5, 1.0); }
