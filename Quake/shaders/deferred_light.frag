layout(binding=0) uniform sampler2D u_Albedo;
layout(binding=1) uniform sampler2D u_NormalSpec;
layout(binding=2) uniform sampler2D u_Depth;

uniform vec3 lightPos;
uniform vec3 lightColor;
uniform float lightRadius;
uniform float specPower;

#include "frame_uniforms.glsl"

layout(location=0) out vec4 out_Color;

vec3 ReconstructWorldPosition(vec2 uv, float depth)
{
        float z = depth * 2.0 - 1.0;
        vec4 clip = vec4(uv * 2.0 - 1.0, z, 1.0);
        mat4 invViewProj = inverse(ViewProj);
        vec4 world = invViewProj * clip;
        return world.xyz / world.w;
}

void main()
{
        vec2 uv = gl_FragCoord.xy / vec2(textureSize(u_Depth, 0));
        float depth = texture(u_Depth, uv).r;
        vec3 worldPos = ReconstructWorldPosition(uv, depth);

        vec3 albedo = texture(u_Albedo, uv).rgb;
        vec4 normalSpec = texture(u_NormalSpec, uv);
        vec3 normal = normalize(normalSpec.xyz);
        float shininess = normalSpec.w * specPower;

        vec3 toLight = lightPos - worldPos;
        float dist = length(toLight);
        float attenuation = clamp(1.0 - dist / lightRadius, 0.0, 1.0);
        vec3 lightDir = dist > 0.0 ? toLight / dist : vec3(0.0, 0.0, 1.0);

        vec3 toEye = EyePos - worldPos;
        float viewLen = length(toEye);
        vec3 viewDir = viewLen > 0.0 ? toEye / viewLen : vec3(0.0, 0.0, 1.0);

        float ndl = max(dot(normal, lightDir), 0.0);
        vec3 diffuse = ndl * albedo * lightColor;

        vec3 halfVec = normalize(lightDir + viewDir);
        float ndh = max(dot(normal, halfVec), 0.0);
        float spec = shininess > 0.0 ? pow(ndh, shininess) : 0.0;
        vec3 specular = spec * lightColor;

        vec3 result = attenuation * (diffuse + specular);
        out_Color = vec4(result, 1.0);
}
