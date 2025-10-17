#version 430 core

in vec3 vWorldPos;

layout(location = 0) out float outDepth;

uniform vec3  uLightPos;
uniform float uLightRadius;

void main()
{
    float dist = length(vWorldPos - uLightPos);
    float nd   = clamp(dist / uLightRadius, 0.0, 1.0);
    outDepth   = nd;
}
