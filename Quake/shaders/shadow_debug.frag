layout(binding=0) uniform sampler2D ShadowMapDepth;
layout(location=0) in vec2 v_uv;
layout(location=0) out vec4 out_fragcolor;

void main()
{
	float depth = texture(ShadowMapDepth, v_uv).r;
	out_fragcolor = vec4(vec3(depth), 1.0);
}
