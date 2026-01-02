#include "texunits.glsl"

layout(binding=TEXUNIT_SHADOW_DEPTH) uniform sampler2D ShadowMapDepth;
layout(location=0) uniform float InvertDepth;
layout(location=0) in vec2 v_uv;
layout(location=0) out vec4 out_fragcolor;

void main()
{
	float depth = texture(ShadowMapDepth, v_uv).r;
	if (InvertDepth > 0.5)
		depth = 1.0 - depth;
	out_fragcolor = vec4(vec3(depth), 1.0);
}
