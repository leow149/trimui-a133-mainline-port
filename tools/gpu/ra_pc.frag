#version 450
// Solid color from a push constant.
layout(push_constant) uniform PC { vec4 color; } pc;
layout(location = 0) out vec4 color;
void main()
{
	color = pc.color;
}
