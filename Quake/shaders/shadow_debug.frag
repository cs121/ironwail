
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

// Use this in fragment shaders that declare `out vec4 fragColor;`
#ifndef DBG_EARLY_OUT_COLOR
#define DBG_EARLY_OUT_COLOR(c) { fragColor = vec4((c), 1.0); return; }
#endif

layout(binding=0) uniform sampler2D ShadowMap;
layout(location=0) in vec2 v_uv;
layout(location=0) out vec4 out_fragcolor;

void main()
{
	float depth = texture(ShadowMap, v_uv).r;
	out_fragcolor = vec4(vec3(depth), 1.0);
}
