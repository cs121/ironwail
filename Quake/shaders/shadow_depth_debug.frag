#include "texunits.glsl"

layout(binding=TEXUNIT_SHADOW) uniform sampler2DShadow ShadowMapCmp;
layout(binding=TEXUNIT_SHADOW_DEPTH) uniform sampler2D ShadowMapDepth;
layout(location=0) uniform float InvertDepth;
layout(location=1) uniform float UseCompare;
layout(location=2) uniform float CompareGreater;
layout(location=0) in vec2 v_uv;
layout(location=0) out vec4 out_fragcolor;

void main()
{
	float depth;
	if (UseCompare > 0.5)
	{
		float low = 0.0;
		float high = 1.0;
		for (int i = 0; i < 8; ++i)
		{
			float mid = (low + high) * 0.5;
			float pass = texture(ShadowMapCmp, vec3(v_uv, mid));
			if (CompareGreater > 0.5)
			{
				if (pass > 0.5)
					high = mid;
				else
					low = mid;
			}
			else
			{
				if (pass > 0.5)
					low = mid;
				else
					high = mid;
			}
		}
		depth = (low + high) * 0.5;
	}
	else
	{
		depth = texture(ShadowMapDepth, v_uv).r;
	}
	if (InvertDepth > 0.5)
		depth = 1.0 - depth;
	out_fragcolor = vec4(vec3(depth), 1.0);
}
