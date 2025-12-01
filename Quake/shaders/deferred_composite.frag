#version 330 core

in vec2 vTexCoord;

uniform sampler2D u_ForwardColor;
uniform sampler2D u_DeferredLight;

out vec4 FragColor;

void main()
{
    vec3 forwardColor = texture(u_ForwardColor, vTexCoord).rgb;
    vec3 deferredLight = texture(u_DeferredLight, vTexCoord).rgb;
    FragColor = vec4(forwardColor + deferredLight, 1.0);
}
