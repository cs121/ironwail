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

vec3 ReconstructPosition(vec2 uv, float depth)
{
    float z = depth * 2.0 - 1.0;
    vec4 clip = vec4(uv * 2.0 - 1.0, z, 1.0);
    vec4 view = uInvProj * clip;
    return view.xyz / view.w;
}

vec3 ComputeNormal(vec3 centerPos, vec2 uv)
{
    vec2 offsetX = vec2(uInvResolution.x, 0.0);
    vec2 offsetY = vec2(0.0, uInvResolution.y);

    float depthX = texture(uDepth, clamp(uv + offsetX, 0.0, 1.0)).r;
    float depthY = texture(uDepth, clamp(uv + offsetY, 0.0, 1.0)).r;

    vec3 posX = (depthX >= 1.0) ? centerPos : ReconstructPosition(uv + offsetX, depthX);
    vec3 posY = (depthY >= 1.0) ? centerPos : ReconstructPosition(uv + offsetY, depthY);

    vec3 tangent = posX - centerPos;
    vec3 bitangent = posY - centerPos;
    vec3 normal = normalize(cross(tangent, bitangent));
    if (any(isnan(normal)) || length(normal) < 1e-4)
        normal = vec3(0.0, 0.0, 1.0);

    vec3 viewDir = normalize(-centerPos);
    if (dot(normal, viewDir) < 0.0)
        normal = -normal;
    return normal;
}

void main()
{
    float depth = texture(uDepth, vUV).r;
    if (depth >= 1.0)
    {
        FragAO = 1.0;
        return;
    }

    vec3 position = ReconstructPosition(vUV, depth);
    vec3 normal = ComputeNormal(position, vUV);

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
        if (sampleDepth >= 1.0)
            continue;

        vec3 depthPos = ReconstructPosition(sampleUV, sampleDepth);
        float range = uRadius / (abs(position.z - depthPos.z) + 1e-4);
        float weight = clamp(range, 0.0, 1.0);
        float delta = depthPos.z - samplePos.z;
        occlusion += (delta > uBias ? 1.0 : 0.0) * weight;
    }

    float ao = 1.0 - (samples > 0 ? occlusion / float(samples) : 0.0);
    ao = pow(clamp(ao, 0.0, 1.0), uPower);
    FragAO = mix(1.0, ao, clamp(uIntensity, 0.0, 1.0));
}
