layout(binding=0) uniform sampler2D GammaTexture;
layout(binding=1) uniform usampler3D PaletteLUT;
layout(binding=2) uniform sampler2D DepthTexture;
layout(binding=3) uniform sampler2D BloomTexture;
layout(binding=4) uniform sampler2D GodrayTexture;

layout(std430, binding=0) restrict readonly buffer PaletteBuffer
{
	uint Palette[256];
};

uvec3 UnpackRGB8(uint c)
{
	return uvec3(c, c >> 8, c >> 16) & 255u;
}

// ALU-only 16x16 Bayer matrix
float bayer01(ivec2 coord)
{
	coord &= 15;
	coord.y ^= coord.x;
	uint v = uint(coord.y | (coord.x << 8));	// 0  0  0  0 | x3 x2 x1 x0 |  0  0  0  0 | y3 y2 y1 y0
	v = (v ^ (v << 2)) & 0x3333;				// 0  0 x3 x2 |  0  0 x1 x0 |  0  0 y3 y2 |  0  0 y1 y0
	v = (v ^ (v << 1)) & 0x5555;				// 0 x3  0 x2 |  0 x1  0 x0 |  0 y3  0 y2 |  0 y1  0 y0
	v |= v >> 7;								// 0 x3  0 x2 |  0 x1  0 x0 | x3 y3 x2 y2 | x1 y1 x0 y0
	v = bitfieldReverse(v) >> 24;				// 0  0  0  0 |  0  0  0  0 | y0 x0 y1 x1 | y2 x2 y3 x3
	return float(v) * (1.0/256.0);
}

float bayer(ivec2 coord)
{
	return bayer01(coord) - 0.5;
}

// Hash without Sine
// https://www.shadertoy.com/view/4djSRW 
float whitenoise01(vec2 p)
{
	vec3 p3 = fract(vec3(p.xyx) * .1031);
	p3 += dot(p3, p3.yzx + 33.33);
	return fract((p3.x + p3.y) * p3.z);
}

float whitenoise(vec2 p)
{
	return whitenoise01(p) - 0.5;
}

// Convert uniform distribution to triangle-shaped distribution
// Input in [0..1], output in [-1..1]
// Based on https://www.shadertoy.com/view/4t2SDh 
float tri(float x)
{
	float orig = x * 2.0 - 1.0;
	uint signbit = floatBitsToUint(orig) & 0x80000000u;
	x = sqrt(abs(orig)) - 1.;
	x = uintBitsToFloat(floatBitsToUint(x) ^ signbit);
	return x;
}

vec3 HableTonemap(vec3 x)
{
        const float A = 0.15;
        const float B = 0.50;
        const float C = 0.10;
        const float D = 0.20;
        const float E = 0.02;
        const float F = 0.30;
        const float W = 11.2;
        vec3 numerator = x * (A * x + C * B) + D * E;
        vec3 denominator = x * (A * x + B) + D * F;
        vec3 mapped = numerator / denominator - E / F;
        float white = ((W * (A * W + C * B) + D * E) / (W * (A * W + B) + D * F)) - E / F;
        mapped /= white;
        return clamp(mapped, 0.0, 1.0);
}

#define DITHER_NOISE(uv) tri(bayer01(ivec2(uv)))
#define SCREEN_SPACE_NOISE() DITHER_NOISE(floor(gl_FragCoord.xy)+0.5)
#define SUPPRESS_BANDING() bayer(ivec2(gl_FragCoord.xy))

layout(location=0) uniform vec4 Params;
layout(location=1) uniform vec4 DoFParams0; // x: enabled, y: focus distance, z: focus range, w: max blur radius (pixels)
layout(location=2) uniform vec4 DoFParams1; // x: near plane, y: far plane, z: reversed-Z flag (>0.5 when reversed)
layout(location=3) uniform vec4 ViewRect;   // xy: view min (normalized), zw: view max (normalized)
layout(location=4) uniform vec4 DepthParams; // xy: inverse view scale, zw: unused
layout(location=5) uniform vec4 HDRParams; // x: bloom intensity, y: exposure, z: tonemap enabled, w: godrays enabled

layout(location=0) out vec4 out_fragcolor;

void main()
{
        float gamma = Params.x;
        float contrast = Params.y;
        float scale = Params.z;
        float dither = Params.w;
        ivec2 pixel = ivec2(gl_FragCoord.xy);
        vec4 color = texelFetch(GammaTexture, pixel, 0);
        vec2 texSize = vec2(textureSize(GammaTexture, 0));
        vec2 uv = (vec2(pixel) + 0.5) / texSize;
        vec2 viewMin = ViewRect.xy;
        vec2 viewMax = ViewRect.zw;
        bool inView = all(greaterThanEqual(uv, viewMin)) && all(lessThanEqual(uv, viewMax));
        if (DoFParams0.x > 0.5 && inView)
        {
                vec2 depthTexSize = vec2(textureSize(DepthTexture, 0));
                vec2 viewMinPx = viewMin * depthTexSize;
                vec2 viewMaxPx = viewMax * depthTexSize;
                vec2 viewSizePx = max(viewMaxPx - viewMinPx, vec2(0.0));
                vec2 invViewScale = max(DepthParams.xy, vec2(1e-4));
                vec2 depthSizePx = max(vec2(1.0), floor(viewSizePx * invViewScale + vec2(0.0001)));
                vec2 fragPx = gl_FragCoord.xy;
                vec2 relPx = clamp(fragPx - viewMinPx, vec2(0.0), max(viewSizePx - vec2(1e-4), vec2(0.0)));
                vec2 depthIdx = floor(relPx * invViewScale);
                vec2 maxDepthIdx = max(depthSizePx - vec2(1.0), vec2(0.0));
                depthIdx = clamp(depthIdx, vec2(0.0), maxDepthIdx);
                vec2 depthPx = viewMinPx + depthIdx + vec2(0.5);
                vec2 depthUV = depthPx / depthTexSize;
                float rawDepth = texture(DepthTexture, depthUV).r;
                float nearPlane = DoFParams1.x;
                float farPlane = DoFParams1.y;
                float reversed = DoFParams1.z;
                float linearDepth;
                if (reversed > 0.5)
                {
                        float denom = nearPlane + rawDepth * (farPlane - nearPlane);
                        linearDepth = (nearPlane * farPlane) / max(denom, 1e-6);
                }
                else
                {
                        float ndcDepth = rawDepth * 2.0 - 1.0;
                        float denom = farPlane + nearPlane - ndcDepth * (farPlane - nearPlane);
                        linearDepth = (2.0 * nearPlane * farPlane) / max(denom, 1e-6);
                }
                float focusDistance = DoFParams0.y;
                float focusRange = max(DoFParams0.z, 0.0001);
                float maxBlur = max(DoFParams0.w, 0.0);
                float coc = abs(linearDepth - focusDistance);
                float blurFactor = clamp((coc - focusRange) / focusRange, 0.0, 1.0);
                float blurRadius = blurFactor * maxBlur;
                if (blurRadius > 0.0001)
                {
                        vec2 invRes = 1.0 / texSize;
                        const vec2 kernel[8] = vec2[](
                                vec2(1.0, 0.0),
                                vec2(-1.0, 0.0),
                                vec2(0.0, 1.0),
                                vec2(0.0, -1.0),
                                vec2(0.70710678, 0.70710678),
                                vec2(-0.70710678, 0.70710678),
                                vec2(0.70710678, -0.70710678),
                                vec2(-0.70710678, -0.70710678)
                        );
                        float noise = SCREEN_SPACE_NOISE();
                        float angle = noise * 6.28318530718;
                        float sine = sin(angle);
                        float cosine = cos(angle);
                        mat2 rotation = mat2(cosine, -sine, sine, cosine);
                        vec3 accum = color.rgb;
                        float weight = 1.0;
                        for (int i = 0; i < 8; ++i)
                        {
                                vec2 offset = rotation * kernel[i] * blurRadius * invRes;
                                vec3 sampleColor = texture(GammaTexture, uv + offset).rgb;
                                accum += sampleColor;
                                weight += 1.0;
                        }
                        color.rgb = accum / weight;
                }
        }
        out_fragcolor = color;
#if PALETTIZE == 1
		vec2 noiseuv = floor(gl_FragCoord.xy * scale) + 0.5;
		out_fragcolor.rgb = sqrt(out_fragcolor.rgb);
	out_fragcolor.rgb += DITHER_NOISE(noiseuv) * dither;
	out_fragcolor.rgb *= out_fragcolor.rgb;
#endif // PALETTIZE == 1
#if PALETTIZE
	ivec3 clr = ivec3(clamp(out_fragcolor.rgb, 0., 1.) * 127. + 0.5);
	uint remap = Palette[texelFetch(PaletteLUT, clr, 0).x];
	out_fragcolor.rgb = vec3(UnpackRGB8(remap)) * (1./255.);
#else
        vec3 hdrColor = out_fragcolor.rgb;
        float bloomIntensity = HDRParams.x;
        vec3 bloomColor = vec3(0.0);
        if (bloomIntensity > 0.0)
        {
                bloomColor = texture(BloomTexture, uv).rgb * bloomIntensity;
        }
        float godrayEnabled = HDRParams.w;
        vec3 shaftColor = vec3(0.0);
        if (godrayEnabled > 0.5)
        {
                shaftColor = texture(GodrayTexture, uv).rgb;
        }
        float exposure = max(HDRParams.y, 0.0);
        float tonemapEnabled = HDRParams.z;
        vec3 combined = (hdrColor + bloomColor + shaftColor) * exposure * contrast;
        combined = max(combined, vec3(0.0));
        vec3 mapped = tonemapEnabled > 0.5 ? HableTonemap(combined) : clamp(combined, 0.0, 1.0);
        mapped = clamp(mapped, 0.0, 1.0);
        out_fragcolor = vec4(pow(mapped, vec3(gamma)), 1.0);
#endif // PALETTIZE
}
