layout(location=0) in vec4 v_light_clip;

void main()
{
	vec3 ndc = v_light_clip.xyz / v_light_clip.w;
#if CLIP_Z_ZERO_TO_ONE
	float depth01 = ndc.z;
#else
	float depth01 = ndc.z * 0.5 + 0.5;
#endif
#if REVERSED_Z
	gl_FragDepth = clamp(1.0 - depth01, 0.0, 1.0);
#else
	gl_FragDepth = clamp(depth01, 0.0, 1.0);
#endif
}
