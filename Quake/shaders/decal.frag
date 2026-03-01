layout(binding=0) uniform sampler2D Tex;

layout(location=0) in vec2 out_uv;
layout(location=1) in vec4 out_color;

layout(location=0) out vec4 out_fragcolor;
layout(location=1) out vec4 out_velocity;

void main()
{
	vec4 c = texture(Tex, out_uv) * out_color;
	if (c.a <= 0.001)
		discard;
	out_fragcolor = c;
	out_velocity = vec4(0.0, 0.0, 0.0, 2.0);
}
