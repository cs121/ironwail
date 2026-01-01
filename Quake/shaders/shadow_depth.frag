layout(location=0) in vec4 v_light_clip;

void main()
{
	float ndc_z = v_light_clip.z / v_light_clip.w;
	float depth01 =
#if CLIP_Z_ZERO_TO_ONE
		ndc_z;
#else
		ndc_z * 0.5 + 0.5;
#endif

#if REVERSED_Z
	gl_FragDepth = 1.0 - depth01;
#else
	gl_FragDepth = depth01;
#endif
}
