layout(binding=0) uniform sampler2D MaskTexture;
layout(binding=1) uniform sampler2D DepthTexture;
layout(binding=2) uniform sampler2D DirTexture;

layout(location=0) uniform vec4 LightParams; // xy: light position, z: density, w: weight
layout(location=1) uniform vec4 ScatterParams; // x: decay, y: exposure, z: max radius, w: samples
layout(location=2) uniform mat4 uInvViewProj;
layout(location=6) uniform mat4 uViewProj;
layout(location=10) uniform vec3 uLightDirWS;
layout(location=11) uniform vec3 uEmitterNormalWS;

layout(location=0) out vec4 outColor;

vec3 ReconstructWS(vec2 uv, float depth)
{
	vec4 ndc = vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
	vec4 ws = uInvViewProj * ndc;
	float w = max(abs(ws.w), 1e-6);
	return ws.xyz / w;
}

vec3 ProjectOnPlane(vec3 v, vec3 n)
{
	return v - n * dot(v, n);
}

vec2 ProjectUV(vec3 ws)
{
	vec4 clip = uViewProj * vec4(ws, 1.0);
	float w = max(abs(clip.w), 1e-6);
	vec2 ndc = clip.xy / w;
	return ndc * 0.5 + 0.5;
}

void main()
{
	vec2 texSize = vec2(textureSize(MaskTexture, 0));
	vec2 uv = (gl_FragCoord.xy + 0.5) / max(texSize, vec2(1.0));
	vec2 lightPos = clamp(LightParams.xy, vec2(0.0), vec2(1.0));
        float density = max(LightParams.z, 0.0);
        float weight = max(LightParams.w, 0.0);
        float decay = max(ScatterParams.x, 0.0);
	float exposure = max(ScatterParams.y, 0.0);
	float maxRadius = max(ScatterParams.z, 0.0);
	int samples = int(ScatterParams.w + 0.5);
	samples = clamp(samples, 1, 128);

	vec3 lightDir = normalize(uLightDirWS);
	vec3 emitterNormal = normalize(uEmitterNormalWS);
	vec3 rayWS = ProjectOnPlane(lightDir, emitterNormal);
	float rayLen2 = dot(rayWS, rayWS);
	vec2 dir = vec2(0.0);
	if (rayLen2 > 1e-6)
	{
		vec3 posWS = ReconstructWS(uv, texture(DepthTexture, uv).r);
		vec2 uv2 = ProjectUV(posWS + rayWS * 1.0);
		dir = uv2 - uv;
	}

	vec2 fallback = texture(DirTexture, uv).rg * 2.0 - 1.0;
	float fallbackLen2 = dot(fallback, fallback);
	if (fallbackLen2 < 1e-4)
		fallback = uv - lightPos;
	else
		fallback *= inversesqrt(fallbackLen2);

	float dirLen2 = dot(dir, dir);
	if (dirLen2 < 1e-6)
		dir = fallback;
	else
		dir *= inversesqrt(dirLen2);
	float step_scale = density / float(samples);
	if (maxRadius > 0.0)
		step_scale = min(step_scale, maxRadius / float(samples));
	vec2 step = dir * step_scale;

        vec2 coord = uv;
        vec3 accum = vec3(0.0);
        float illuminationDecay = 1.0;
        for (int i = 0; i < 128; ++i)
        {
                if (i >= samples)
                        break;
                coord -= step;
                if (coord.x < 0.0 || coord.x > 1.0 || coord.y < 0.0 || coord.y > 1.0)
                        break;
                vec4 sampleColor = texture(MaskTexture, coord);
                accum += sampleColor.rgb * sampleColor.a * illuminationDecay * weight;
                illuminationDecay *= decay;
        }

        outColor = vec4(accum * exposure, 1.0);
}
