in vec2 vUV;
out float FragAO;

uniform sampler2D uDepth;
uniform sampler2D uNoise;

uniform mat4 uProj;
uniform mat4 uInvProj;

uniform vec3 uSamples[16];
uniform int  uKernelSize;
uniform float uRadius;
uniform float uBias;
uniform float uPower;
uniform float uIntensity;
uniform vec2 uNoiseScale;
uniform vec2 uInvResolution;
uniform bool  uReverseZ;
uniform bool  uNDCZeroToOne;

float DepthToNDC(float depth01)
{
    return uNDCZeroToOne ? depth01 : (depth01 * 2.0 - 1.0);
}

bool IsSkyDepth(float d)
{
    return uReverseZ ? (d <= 0.0005) : (d >= 0.9995);
}

vec3 ReconstructPosition(vec2 uv, float depth01)
{
    float z_ndc = DepthToNDC(depth01);
    vec4 clip = vec4(uv * 2.0 - 1.0, z_ndc, 1.0);
    vec4 view = uInvProj * clip;
    return view.xyz / max(view.w, 1e-6);
}

vec3 ComputeNormal(vec3 centerPos, vec2 uv, vec3 viewDir)
{
    vec2 offsetX = vec2(uInvResolution.x, 0.0);
    vec2 offsetY = vec2(0.0, uInvResolution.y);

    float depthX = texture(uDepth, clamp(uv + offsetX, 0.0, 1.0)).r;
    float depthY = texture(uDepth, clamp(uv + offsetY, 0.0, 1.0)).r;

    vec3 posX = IsSkyDepth(depthX) ? centerPos : ReconstructPosition(uv + offsetX, depthX);
    vec3 posY = IsSkyDepth(depthY) ? centerPos : ReconstructPosition(uv + offsetY, depthY);

    vec3 tangent = posX - centerPos;
    vec3 bitangent = posY - centerPos;
    vec3 normal = normalize(cross(tangent, bitangent));
    if (any(isnan(normal)) || length(normal) < 1e-4)
        normal = vec3(0.0, 0.0, 1.0);

    if (dot(normal, viewDir) < 0.0)
        normal = -normal;
    return normal;
}

void main()
{
    float depth = texture(uDepth, vUV).r;
    if (IsSkyDepth(depth))
    {
        FragAO = 1.0;
        return;
    }

    vec3 position = ReconstructPosition(vUV, depth);
    vec3 viewDir = normalize(-position);
    vec3 normal = ComputeNormal(position, vUV, viewDir);

    vec3 noise = texture(uNoise, vUV * uNoiseScale).xyz * 2.0 - 1.0;
    noise.z = 0.0;
    vec3 tangent = normalize(noise - normal * dot(noise, normal));
    vec3 bitangent = cross(normal, tangent);
    mat3 TBN = mat3(tangent, bitangent, normal);

    float occlusion = 0.0;
    int samples = clamp(uKernelSize, 0, 16);

    for (int i = 0; i < samples; ++i)
    {
        vec3 sampleVec = TBN * uSamples[i];
        vec3 samplePos = position + sampleVec * uRadius;

        vec4 clip = uProj * vec4(samplePos, 1.0);
        clip.xyz /= clip.w;
        vec2 sampleUV = clip.xy * 0.5 + 0.5;
        if (sampleUV.x < 0.0 || sampleUV.x > 1.0 || sampleUV.y < 0.0 || sampleUV.y > 1.0)
            continue;

        float sampleDepth = texture(uDepth, sampleUV).r;
        if (IsSkyDepth(sampleDepth))
            continue;

        vec3 depthPos = ReconstructPosition(sampleUV, sampleDepth);
        float range = uRadius / (abs(position.z - depthPos.z) + 1e-4);
        float weight = clamp(range, 0.0, 1.0);
        float deltaForward = dot(depthPos - samplePos, viewDir);
        const float thickness = 0.05;
        float stepSoft = smoothstep(uBias, uBias + thickness, deltaForward);
        occlusion += stepSoft * weight;
    }

    float ao = 1.0 - (samples > 0 ? occlusion / float(samples) : 0.0);
    ao = pow(clamp(ao, 0.0, 1.0), uPower);
    FragAO = mix(1.0, ao, clamp(uIntensity, 0.0, 1.0));
}
