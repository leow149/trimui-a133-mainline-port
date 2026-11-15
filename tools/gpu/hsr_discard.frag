#version 450
// Push-constant color, discards where alpha is below 0.5 (alpha 1.0 here: nothing is discarded).
layout(push_constant) uniform PC { vec4 color; } pc;
layout(location = 0) out vec4 color;
void main()
{
	if (pc.color.a < 0.5)
		discard;
	color = pc.color;
}
