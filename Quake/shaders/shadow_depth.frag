layout(location=0) in vec4 v_light_clip;
layout(location=0) out float out_depth_debug;
layout(location=1) uniform int uShadowWriteDebug;

void main()
{
	if (uShadowWriteDebug != 0)
	{
		float depth01 = 1.0;
		if (v_light_clip.w > 0.0)
		{
			float ndc = v_light_clip.z / v_light_clip.w;
#if CLIP_Z_ZERO_TO_ONE
			depth01 = ndc;
#else
			depth01 = ndc * 0.5 + 0.5;
#endif
		}
		out_depth_debug = depth01;
	}
	else
	{
		out_depth_debug = 0.0;
	}
}
