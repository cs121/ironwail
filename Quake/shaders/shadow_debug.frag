layout(binding=0) uniform sampler2D ShadowMap;
layout(location=0) in vec2 v_uv;
layout(location=0) out vec4 out_fragcolor;

// FIX: Linearize depth for visualization so the full [0..1] range is visible.
// Raw depth is non-linear (more precision near camera) which makes the debug
// view appear almost entirely white or black.
// Simple contrast stretch: remap [0.9..1.0] -> [0..1] for typical shadow frusta.
// Replace constants with proper near/far uniforms if accessible in this pass.
void main()
{
	float depth = texture(ShadowMap, v_uv).r;

#if REVERSED_Z
	// Reversed-Z: near=1.0, far=0.0 -- invert so near objects appear bright.
	float display = clamp(depth * 10.0, 0.0, 1.0);
#else
	// Forward-Z: remap the [0.9..1.0] far region into full visible range.
	float display = clamp((depth - 0.9) * 10.0, 0.0, 1.0);
#endif

	out_fragcolor = vec4(vec3(display), 1.0);
}
