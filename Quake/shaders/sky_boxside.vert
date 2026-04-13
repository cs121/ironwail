layout(location=0) uniform mat4 MVP;
layout(location=1) uniform vec3 EyePos;

layout(location=0) in vec3 in_dir;
layout(location=1) in vec2 in_uv;

layout(location=0) out vec3 out_dir;
layout(location=1) out vec2 out_uv;

void main()
{
	gl_Position = MVP * vec4(EyePos + in_dir, 1.0);
	// BUG FIX: with reversed-Z the far plane lives at z/w = 0.0, not 1.0.
	// Setting z = w maps to z/w = 1.0 which is the NEAR plane in reversed-Z —
	// skybox would overdraw everything. Use z = 0 for reversed-Z far plane.
#if REVERSED_Z
	gl_Position.z = 0.0;
#else
	gl_Position.z = gl_Position.w;
#endif
	out_dir = in_dir;
	out_uv = in_uv;
}
