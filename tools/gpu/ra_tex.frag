#version 450
// Samples the bound texture at the fragment's normalized position.
layout(set = 0, binding = 0) uniform sampler2D tex;
layout(location = 0) out vec4 color;
void main()
{
	color = texture(tex, gl_FragCoord.xy / vec2(256.0));
}
