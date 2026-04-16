layout(binding=0) uniform sampler2D DepthTexture;
layout(binding=1) uniform sampler2D NoiseTexture;

#include "frame_uniforms.glsl"

layout(location=0) uniform mat4 u_proj;
layout(location=1) uniform mat4 u_invProj;
layout(location=2) uniform vec4 u_params0; // x: radius, y: bias, z: power, w: min AO
layout(location=3) uniform vec4 u_screenParams; // xy: inv screen size, zw: screen size
layout(location=4) uniform vec4 u_aoParams; // xy: inv AO size, zw: AO size
layout(location=5) uniform vec4 u_noiseParams; // xy: noise scale, z: noise enabled, w: noise seed
layout(location=6) uniform vec4 u_depthParams; // x: near, y: far, z: reversed Z, w: sky depth cutoff
layout(location=7) uniform vec4 u_viewRect; // xy: view min, zw: view max
layout(location=8) uniform int u_samples;
layout(location=9) uniform vec4 u_debugParams; // x: debug mode, y: debug far
layout(location=10) uniform int u_reversedZMode; // 0: default, 1: invert raw, 2: invert ndc
layout(location=11) uniform int u_normalSource; // 0: neighbor, 1: derivatives
layout(location=12) uniform int u_yFlip; // 0: none, 1: flip Y
layout(location=13) uniform int u_noiseMode; // 0: off, 1: IGN, 2: texture
layout(location=14) uniform vec4 u_fogParams; // x: max distance, y: scene fog density

layout(location=0) out vec4 outColor;

const int SSAO_MAX_SAMPLES = 32;
// Hemisphere kernel with guaranteed minimum elevation of 15° above the surface
// (unit-vector z >= sin(15°) = 0.259 for all samples).  The old kernel had 5 samples
// with unit-z < 0.02 (nearly tangent to the surface) which produced regular stripe
// artifacts because those grazing samples alternately hit / miss nearby geometry.
// Samples are length-scaled with accelerating distribution to cluster near the surface
// for local contact occlusion while still reaching full radius for broader occlusion.
const vec3 SSAO_KERNEL[SSAO_MAX_SAMPLES] = vec3[](
	vec3(-0.0620, -0.0745, 0.0280),
	vec3(-0.0147,  0.0926, 0.0439),
	vec3(-0.0060, -0.0698, 0.0821),
	vec3( 0.0841, -0.0677, 0.0369),
	vec3(-0.1033,  0.0552, 0.0343),
	vec3( 0.0199,  0.0999, 0.0834),
	vec3( 0.1289,  0.0217, 0.0581),
	vec3(-0.0688, -0.0946, 0.1035),
	vec3( 0.0227,  0.1209, 0.1191),
	vec3( 0.0661, -0.1688, 0.0495),
	vec3( 0.0447, -0.1222, 0.1602),
	vec3(-0.1129,  0.1772, 0.0847),
	vec3( 0.2063, -0.0568, 0.1263),
	vec3( 0.2146,  0.1414, 0.0900),
	vec3( 0.1212, -0.1725, 0.2103),
	vec3( 0.0685, -0.1827, 0.2599),
	vec3(-0.0685, -0.0159, 0.3469),
	vec3(-0.2069,  0.1979, 0.2570),
	vec3( 0.1391, -0.2553, 0.2993),
	vec3( 0.2119, -0.2507, 0.3101),
	vec3(-0.1313, -0.4474, 0.1428),
	vec3( 0.0641,  0.4584, 0.2487),
	vec3( 0.4470,  0.2450, 0.2437),
	vec3( 0.4323,  0.3182, 0.2818),
	vec3(-0.3626, -0.4148, 0.3436),
	vec3(-0.4330,  0.4601, 0.2874),
	vec3(-0.0239,  0.2230, 0.7059),
	vec3(-0.3320, -0.4453, 0.5605),
	vec3( 0.2398,  0.4437, 0.6707),
	vec3( 0.3882,  0.6416, 0.4812),
	vec3( 0.6410, -0.0423, 0.6926),
	vec3(-0.6019, -0.2250, 0.7662)
);

// SSAO conventions:
// - View space uses +X forward (camera looks down +X).
// - When reverse-Z (clip control) is enabled, NDC depth is [0..1]; otherwise [-1..1].
// - All comparisons are done in view space using +X depth.

vec2 ApplyYFlip(vec2 uv)
{
	if (u_yFlip != 0)
		uv.y = 1.0 - uv.y;
	return uv;
}

vec2 ScreenInvSize()
{
	return u_screenParams.xy;
}

vec2 ScreenSize()
{
	return u_screenParams.zw;
}

vec2 AoInvSize()
{
	return u_aoParams.xy;
}

vec2 AoSize()
{
	return u_aoParams.zw;
}

vec2 AoPixelCoord()
{
	return floor(gl_FragCoord.xy);
}

vec2 AoUvFromPixel(vec2 aoPixel)
{
	return ApplyYFlip((aoPixel + 0.5) * AoInvSize());
}

vec2 UnflipUv(vec2 uv)
{
	if (u_yFlip != 0)
		uv.y = 1.0 - uv.y;
	return uv;
}

vec2 ViewMinPx()
{
	return floor(u_viewRect.xy * ScreenSize());
}

vec2 ViewMaxPx()
{
	vec2 minPx = ViewMinPx();
	vec2 maxPx = floor(u_viewRect.zw * ScreenSize() - vec2(1.0));
	return max(minPx, maxPx);
}

ivec2 ClampScreenPixel(vec2 pixel)
{
	vec2 maxPx = ScreenSize() - vec2(1.0);
	return ivec2(clamp(pixel, vec2(0.0), maxPx));
}

ivec2 ScreenPixelFromAoPixelNearest(vec2 aoPixel)
{
	vec2 screenSize = ScreenSize();
	vec2 aoSize = max(AoSize(), vec2(1.0));
	vec2 screenPixel = floor(aoPixel * screenSize / aoSize);
	return ClampScreenPixel(screenPixel);
}

ivec2 ScreenPixelFromUv(vec2 uv)
{
	vec2 unflipped = UnflipUv(uv);
	vec2 screenPixel = floor(unflipped * ScreenSize());
	return ClampScreenPixel(screenPixel);
}

vec2 ScreenUvFromPixel(ivec2 pixel)
{
	return ApplyYFlip((vec2(pixel) + 0.5) * ScreenInvSize());
}

// Ironwail uses reverse-Z with clip control: near depth ~1, far depth ~0 when reversed is enabled.
float DepthToNdcZ(float depth, float reversed, int mode)
{
	float raw = depth;
	if (mode == 1)
		raw = 1.0 - raw;
	if (reversed > 0.5)
	{
		if (mode == 2)
			raw = 1.0 - raw;
		return raw;
	}
	float ndc = raw * 2.0 - 1.0;
	if (mode == 2)
		ndc = -ndc;
	return ndc;
}

vec3 ReconstructViewPos(vec2 uv, float depth)
{
	float ndcDepth = DepthToNdcZ(depth, u_depthParams.z, u_reversedZMode);
	vec4 clip = vec4(uv * 2.0 - 1.0, ndcDepth, 1.0);
	vec4 view = u_invProj * clip;
	float w = view.w;
	if (abs(w) < 1e-6)
		return vec3(1e30);
	return view.xyz / w;
}

// FIX: ViewZFromDepth now uses ReconstructViewPos consistently; unused reversedZ param removed.
float ViewZFromDepth(vec2 uv, float depth01)
{
	vec3 viewPos = ReconstructViewPos(uv, depth01);
	return viewPos.x;
}

float DepthRawFromPixel(ivec2 pixel)
{
	return texelFetch(DepthTexture, pixel, 0).r;
}

float DepthRawFromUv(vec2 uv)
{
	return DepthRawFromPixel(ScreenPixelFromUv(uv));
}

vec3 ComputeNormalFromViewPos(vec3 viewPos)
{
	vec3 dx = dFdx(viewPos);
	vec3 dy = dFdy(viewPos);
	vec3 normal = normalize(cross(dx, dy));
	if (length(normal) < 1e-4)
		return vec3(0.0, 0.0, 1.0);
	vec3 viewDir = normalize(-viewPos);
	if (dot(normal, viewDir) < 0.0)
		normal = -normal;
	return normal;
}

bool IsSkyDepth(float depth, vec4 depthParams)
{
	float reversed = depthParams.z;
	float cutoff = depthParams.w;
	if (reversed > 0.5)
		return depth <= cutoff;
	return depth >= cutoff;
}

// Scale-invariant depth discontinuity test: true when two raw depth values
// differ by more than 2x, meaning they are on different surfaces.
// Works in both standard and reversed-Z conventions.
#define DEPTH_JUMP(a, b)  ((a) < (b) * 0.5 || (a) > (b) * 2.0)

vec3 ReconstructNormalFromDepth(vec2 uv)
{
	ivec2 centerPixel = ScreenPixelFromUv(uv);
	vec2 viewMinPx = ViewMinPx();
	vec2 viewMaxPx = ViewMaxPx();

	// FIX: Track both pixel and depth together so UV and depth always match the same pixel.
	ivec2 rightPixel = ivec2(clamp(vec2(centerPixel + ivec2(1, 0)), viewMinPx, viewMaxPx));
	ivec2 upPixel    = ivec2(clamp(vec2(centerPixel + ivec2(0, 1)), viewMinPx, viewMaxPx));

	float centerDepth = DepthRawFromPixel(centerPixel);
	if (IsSkyDepth(centerDepth, u_depthParams))
		return vec3(0.0, 0.0, 1.0);

	vec2  uvCenter = ScreenUvFromPixel(centerPixel);
	vec3  p        = ReconstructViewPos(uvCenter, centerDepth);

	// Right neighbor — fall back to left if sky or depth discontinuity, then center.
	float depthRight = DepthRawFromPixel(rightPixel);
	ivec2 usedRightPixel = rightPixel;
	if (IsSkyDepth(depthRight, u_depthParams) || DEPTH_JUMP(depthRight, centerDepth))
	{
		ivec2 leftPixel = ivec2(clamp(vec2(centerPixel - ivec2(1, 0)), viewMinPx, viewMaxPx));
		float depthLeft = DepthRawFromPixel(leftPixel);
		if (!IsSkyDepth(depthLeft, u_depthParams) && !DEPTH_JUMP(depthLeft, centerDepth))
		{
			depthRight     = depthLeft;
			usedRightPixel = leftPixel;
		}
		else
		{
			usedRightPixel = centerPixel;
			depthRight     = centerDepth;
		}
	}
	vec2 uvRight = ScreenUvFromPixel(usedRightPixel);
	vec3 pr      = ReconstructViewPos(uvRight, depthRight);

	// Up neighbor — fall back to down if sky or depth discontinuity, then center.
	float depthUp = DepthRawFromPixel(upPixel);
	ivec2 usedUpPixel = upPixel;
	if (IsSkyDepth(depthUp, u_depthParams) || DEPTH_JUMP(depthUp, centerDepth))
	{
		ivec2 downPixel = ivec2(clamp(vec2(centerPixel - ivec2(0, 1)), viewMinPx, viewMaxPx));
		float depthDown = DepthRawFromPixel(downPixel);
		if (!IsSkyDepth(depthDown, u_depthParams) && !DEPTH_JUMP(depthDown, centerDepth))
		{
			depthUp     = depthDown;
			usedUpPixel = downPixel;
		}
		else
		{
			usedUpPixel = centerPixel;
			depthUp     = centerDepth;
		}
	}
	vec2 uvUp = ScreenUvFromPixel(usedUpPixel);
	vec3 pu   = ReconstructViewPos(uvUp, depthUp);

	vec3 normal = normalize(cross(pr - p, pu - p));
	if (length(normal) < 1e-4)
		return vec3(0.0, 0.0, 1.0);
	vec3 viewDir = normalize(-p);
	if (dot(normal, viewDir) < 0.0)
		normal = -normal;
	return normal;
}

float RandIGN(ivec2 pixel, float seed)
{
	float x = float(pixel.x);
	float y = float(pixel.y);
	float f = fract(0.06711056 * x + 0.00583715 * y + seed);
	return fract(52.9829189 * f);
}

// FIX #1: Use u_fogParams.y (saved scene fog density from gl_rmain.c) instead of
// Fog.w from the frame_uniforms UBO. Fog_DisableGFog() zeroes Fog.w before this
// shader runs (GL_PostProcess is called after R_RenderView), so Fog.w is always 0
// at SSAO generation time, making this function always return 1.0 and disabling
// fog-damped AO entirely.
//
// FIX #2: Use quadratic distance (dist*dist) to match world.frag's fog formula:
//   world.frag: exp2(-Fog.w * dot(p,p))  where dot(p,p) = dist²
// Previous code used linear distance (dist) which underestimates fog falloff and
// produces fog damping that doesn't match the visual fog appearance.
float FogTransmittanceFromViewPos(vec3 viewPos)
{
	float density = abs(u_fogParams.y); // .y = saved scene fog density (NOT Fog.w)
	if (density <= 0.0)
		return 1.0;
	float dist = dot(viewPos, viewPos); // quadratic, matches world.frag dot(p,p)
	float fog = exp2(-density * dist);
	return clamp(fog, 0.0, 1.0);
}

float FogFactorFromViewPos(vec3 viewPos)
{
	return 1.0 - FogTransmittanceFromViewPos(viewPos);
}

bool IsInvalidFloat(float v)
{
	return !(v > -1e20 && v < 1e20);
}

bool IsInvalidVec3(vec3 v)
{
	return IsInvalidFloat(v.x) || IsInvalidFloat(v.y) || IsInvalidFloat(v.z);
}

void main()
{
	vec2 aoPixel = AoPixelCoord();
	vec2 uv      = AoUvFromPixel(aoPixel);

	int debugMode = -1;
	if (u_debugParams.x >= 0.5)
		debugMode = int(u_debugParams.x + 0.5);
	float debugFar = max(u_debugParams.y, 1e-3);

	if (!all(greaterThanEqual(uv, u_viewRect.xy)) || !all(lessThanEqual(uv, u_viewRect.zw)))
	{
		outColor = vec4(1.0);
		return;
	}

	// --- Noise ---
	float noiseSeed    = u_noiseParams.w;
	float noiseEnabled = u_noiseParams.z;
	vec2  noiseVec;
	if (noiseEnabled > 0.5 && u_noiseMode > 0)
	{
		if (u_noiseMode == 2)
		{
			vec2 noiseJitter = vec2(noiseSeed, noiseSeed * 1.37);
			vec2 noiseUV     = fract((aoPixel + noiseJitter + 0.5) * AoInvSize() * u_noiseParams.xy);
			vec2 noiseSample = texture(NoiseTexture, noiseUV).rg * 2.0 - 1.0;
			noiseVec = normalize(noiseSample);
		}
		else
		{
			float angle = RandIGN(ivec2(aoPixel), noiseSeed) * 6.2831853;
			noiseVec = vec2(cos(angle), sin(angle));
		}
	}
	else
	{
		noiseVec = vec2(1.0, 0.0);
	}

	if (debugMode == 8)
	{
		outColor = vec4(noiseVec * 0.5 + 0.5, 0.0, 1.0);
		return;
	}

	// --- Depth / position ---
	ivec2 screenPixel = ScreenPixelFromAoPixelNearest(aoPixel);
	vec2  screenUv    = ScreenUvFromPixel(screenPixel);
	float depth       = DepthRawFromPixel(screenPixel);

	if (debugMode == 4)
	{
		outColor = vec4(vec3(depth), 1.0);
		return;
	}
	if (debugMode == 5)
	{
		if (IsSkyDepth(depth, u_depthParams)) { outColor = vec4(1.0); return; }
		float viewZ = ViewZFromDepth(screenUv, depth);
		if (IsInvalidFloat(viewZ)) { outColor = vec4(1.0, 0.0, 1.0, 1.0); return; }
		float v = clamp(viewZ / debugFar, 0.0, 1.0);
		outColor = vec4(v, v, v, 1.0);
		return;
	}
	if (debugMode == 6)
	{
		if (IsSkyDepth(depth, u_depthParams)) { outColor = vec4(1.0); return; }
		vec3 viewPos = ReconstructViewPos(screenUv, depth);
		if (IsInvalidVec3(viewPos)) { outColor = vec4(1.0, 0.0, 1.0, 1.0); return; }
		float v = clamp(length(viewPos) / debugFar, 0.0, 1.0);
		outColor = vec4(v, v, v, 1.0);
		return;
	}
	if (IsSkyDepth(depth, u_depthParams))
	{
		outColor = vec4(1.0);
		return;
	}

	vec3 viewPos = ReconstructViewPos(screenUv, depth);
	if (IsInvalidVec3(viewPos))
	{
		outColor = (debugMode >= 0) ? vec4(1.0, 0.0, 1.0, 1.0) : vec4(1.0);
		return;
	}

	// Exclude viewmodel/weapon pixels from AO.  Check raw hw depth rather than
	// viewPos.x so it is immune to r_ssao_reversedz_mode remapping.
	// Reversed-Z: near=1.0, viewmodel sits above ~0.85 (within ~5 units at znear=4).
	// Standard Z: near=0.0, viewmodel sits below ~0.15.
	{
		bool nearPlane = (u_depthParams.z > 0.5) ? (depth > 0.85) : (depth < 0.15);
		if (nearPlane) { outColor = vec4(1.0); return; }
	}

	vec3 normal = (u_normalSource != 0) ? ComputeNormalFromViewPos(viewPos) : ReconstructNormalFromDepth(screenUv);
	if (IsInvalidVec3(normal))
	{
		outColor = (debugMode >= 0) ? vec4(1.0, 0.0, 1.0, 1.0) : vec4(1.0);
		return;
	}
	if (debugMode == 7)
	{
		outColor = vec4(normal * 0.5 + 0.5, 1.0);
		return;
	}

	// FIX: Guard against degenerate tangent (noiseVec parallel to normal).
	vec3 noiseDir  = vec3(noiseVec, 0.0);
	float dp       = dot(noiseDir, normal);
	vec3  tangentT = noiseDir - normal * dp;
	float tLen     = length(tangentT);
	// If the noise vector is (nearly) parallel to the normal, fall back to a safe perpendicular.
	if (tLen < 1e-4)
	{
		// Find a component of normal that is smallest to build a robust perpendicular.
		vec3 alt = (abs(normal.x) <= abs(normal.y) && abs(normal.x) <= abs(normal.z))
		           ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 1.0, 0.0);
		tangentT = normalize(alt - normal * dot(alt, normal));
	}
	else
	{
		tangentT = tangentT / tLen;
	}
	vec3 bitangent = cross(normal, tangentT);
	mat3 tbn       = mat3(tangentT, bitangent, normal);

	// --- Sample loop ---
	float radius       = u_params0.x;
	float bias         = u_params0.y;
	float occlusion    = 0.0;
	float validSamples = 0.0;
	// FIX: Loop bound uses the actual sample count so the GPU can unroll/early-out properly.
	int   samples      = clamp(u_samples, 1, SSAO_MAX_SAMPLES);

	for (int i = 0; i < samples; ++i)
	{
		vec3 sampleVec = tbn * SSAO_KERNEL[i];
		vec3 samplePos = viewPos + sampleVec * radius;
		vec4 offset    = u_proj * vec4(samplePos, 1.0);
		if (offset.w <= 1e-6)
			continue;
		vec2 sampleUV = offset.xy / offset.w * 0.5 + 0.5;
		sampleUV = ApplyYFlip(sampleUV);
		if (!all(greaterThanEqual(sampleUV, u_viewRect.xy)) || !all(lessThanEqual(sampleUV, u_viewRect.zw)))
			continue;
		float sampleDepthRaw = DepthRawFromUv(sampleUV);
		if (IsSkyDepth(sampleDepthRaw, u_depthParams))
			continue;
		float sampleViewDepth = ViewZFromDepth(sampleUV, sampleDepthRaw);
		if (IsInvalidFloat(sampleViewDepth))
			continue;
		validSamples += 1.0;
		float depthDelta  = samplePos.x - sampleViewDepth;
		float rangeCheck  = smoothstep(0.0, 1.0, radius / max(abs(depthDelta), 1e-4));
		if (depthDelta > bias)
			occlusion += rangeCheck;
	}

	if (debugMode == 9)
	{
		float ratio = clamp(validSamples / float(samples), 0.0, 1.0);
		outColor = vec4(vec3(ratio), 1.0);
		return;
	}

	// Normalize by actually valid taps; dividing by full kernel size can
	// under-occlude heavily when many taps are rejected (off-screen/sky/behind).
	float denom = max(validSamples, 1.0);
	float ao = 1.0 - occlusion / denom;
	ao = clamp(ao, 0.0, 1.0);
	ao = pow(ao, u_params0.z);
	ao = max(ao, u_params0.w);

	float viewZ       = viewPos.x;
	float maxDistance = max(u_fogParams.x, 1.0);
	if (viewZ > maxDistance)
		ao = 1.0;

	// FIX: Fog damping is handled exclusively by postprocess.frag.
	// ssao.frag previously also applied fog damping (via aoFogWeight / mix), which
	// caused double-damping: postprocess.frag then damped the already-damped AO again.
	// Result: fog_power had to be tuned to ~10 to compensate — completely wrong.
	// ssao.frag now outputs raw AO. postprocess.frag applies fog_strength/fog_power once,
	// correctly, against the raw value.
	//
	// Debug modes 2/3 remain available to inspect the fog factor this pixel would have,
	// using u_fogParams.y (saved scene density) so they reflect the real fog state.
	float fogFactor = FogFactorFromViewPos(viewPos);

	/* Volumetric fog damping is applied in postprocess.frag only. Keep SSAO pass
	 * output as raw AO to avoid double damping. */

	if (debugMode == 3)
	{
		outColor = vec4(vec3(clamp(fogFactor, 0.0, 1.0)), 1.0);
		return;
	}
	if (debugMode == 2)
	{
		// Diagnostic: show what single-damped AO looks like
		float aoFogWeight = 1.0 - clamp(fogFactor, 0.0, 1.0);
		outColor = vec4(vec3(ao * aoFogWeight), 1.0);
		return;
	}
	if (debugMode == 1)
	{
		outColor = vec4(vec3(ao), 1.0);
		return;
	}

	outColor = vec4(ao, ao, ao, 1.0);
}
