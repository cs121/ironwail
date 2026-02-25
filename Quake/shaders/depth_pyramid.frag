layout(binding=0) uniform sampler2D DepthInput;
layout(location=0) uniform vec4 u_texel; // xy inv size, zw size
layout(location=1) uniform vec4 u_params; // x level, y reversedZ, z invertRaw
layout(location=0) out vec4 outColor;

float GetDepth(vec2 uv)
{
	float d = texture(DepthInput, uv).r;
	if (u_params.z > 0.5)
		d = 1.0 - d;
	return d;
}

void main()
{
	vec2 uv = gl_FragCoord.xy * u_texel.xy;
	vec2 offs = u_texel.xy;
	float d0 = GetDepth(uv + vec2(-0.5, -0.5) * offs);
	float d1 = GetDepth(uv + vec2( 0.5, -0.5) * offs);
	float d2 = GetDepth(uv + vec2(-0.5,  0.5) * offs);
	float d3 = GetDepth(uv + vec2( 0.5,  0.5) * offs);
	float reduced = (u_params.y > 0.5) ? max(max(d0, d1), max(d2, d3)) : min(min(d0, d1), min(d2, d3));
	outColor = vec4(reduced, 0.0, 0.0, 1.0);
}
