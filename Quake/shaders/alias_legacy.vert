#version 120

uniform mat4 uModel;
uniform mat4 uViewProj;
uniform vec3 cameraPos;
uniform vec3 primaryLightDir;

attribute vec3 aPosition;
attribute vec3 aNormal;
attribute vec2 aTexCoord;

varying vec2 vTexCoord;
varying vec3 vNormal;
varying vec3 vViewDir;
varying vec3 vLightDir;
varying vec3 vFogVector;

void main()
{
        vec4 worldPos = uModel * vec4(aPosition, 1.0);
        vec3 worldNormal = normalize((uModel * vec4(aNormal, 0.0)).xyz);

        vTexCoord = aTexCoord;
        vNormal = worldNormal;
        vViewDir = normalize(cameraPos - worldPos.xyz);
        vLightDir = normalize(primaryLightDir);
        vFogVector = worldPos.xyz - cameraPos;

        gl_Position = uViewProj * worldPos;
}
