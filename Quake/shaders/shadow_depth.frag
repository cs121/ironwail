layout(location=0) in vec4 v_light_clip;

void main()
{
	vec3 ndc = v_light_clip.xyz / v_light_clip.w;
	float depth01 = ndc.z * 0.5 + 0.5;
	gl_FragDepth = clamp(depth01, 0.0, 1.0);
}
