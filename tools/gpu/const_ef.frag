#version 450
layout(early_fragment_tests) in;
layout(location = 0) out vec4 color;
void main() { color = vec4(0.25, 0.5, 0.75, 1.0); }
