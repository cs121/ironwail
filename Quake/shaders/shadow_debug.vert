
// ------------------------------
// DEBUG / SAFETY HELPERS (softshadows)
// ------------------------------
#ifndef SHADOW_DEBUG_HELPERS_INCLUDED
#define SHADOW_DEBUG_HELPERS_INCLUDED 1

float dbg_nan_guard_f(float v) {
    if (isnan(v) || isinf(v)) return 0.0;
    return v;
}
vec2 dbg_nan_guard(vec2 v) {
    if (any(isnan(v)) || any(isinf(v))) return vec2(0.0);
    return v;
}
vec3 dbg_nan_guard(vec3 v) {
    if (any(isnan(v)) || any(isinf(v))) return vec3(1.0, 0.0, 1.0); // magenta marker
    return v;
}
vec4 dbg_nan_guard(vec4 v) {
    if (any(isnan(v)) || any(isinf(v))) return vec4(1.0, 0.0, 1.0, 1.0);
    return v;
}

float dbg_saturate(float v) { return clamp(v, 0.0, 1.0); }
vec2  dbg_saturate(vec2 v)  { return clamp(v, vec2(0.0), vec2(1.0)); }
vec3  dbg_saturate(vec3 v)  { return clamp(v, vec3(0.0), vec3(1.0)); }

#endif

layout(location=0) out vec2 v_uv;

void main()
{
	vec2 pos = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
	v_uv = pos * 0.5;
	gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
}
