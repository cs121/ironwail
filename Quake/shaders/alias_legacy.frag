#version 120

uniform vec3 ambientColor;
uniform vec3 lightColor;
uniform vec4 Fog; // rgb = fog color, w = density

uniform sampler2D baseTexture;
uniform sampler2D fullbrightTexture;

uniform vec3 rimColor;
uniform float rimStrength;
uniform float halfLambertStrength;
uniform vec3 viewmodelRimColor;
uniform float viewmodelRimStrength;
uniform bool isViewmodel;
uniform bool debugViewmodelMagenta;

varying vec2 vTexCoord;
varying vec3 vNormal;
varying vec3 vViewDir;
varying vec3 vLightDir;
varying vec3 vFogVector;

vec3 ApplyFog(vec3 color)
{
        float fog = exp2(-abs(Fog.w) * dot(vFogVector, vFogVector));
        fog = clamp(fog, 0.0, 1.0);
        return mix(Fog.rgb, color, fog);
}

void main()
{
        vec4 albedo = texture2D(baseTexture, vTexCoord);
#ifdef ALPHATEST
        if (albedo.a < 0.666)
                discard;
#endif

        vec3 normal = normalize(vNormal);
        vec3 viewDir = normalize(vViewDir);
        vec3 lightDir = normalize(vLightDir);

        float ndotl = max(dot(normal, lightDir), 0.0);
        float hLambert = pow(ndotl * 0.5 + 0.5, 1.3);
        vec3 diffuseLight = lightColor * (hLambert * halfLambertStrength);

        float rim = pow(1.0 - max(dot(normal, viewDir), 0.0), 3.0);
        vec3 rimLight = rimColor * rim * rimStrength;
        if (isViewmodel)
                rimLight = viewmodelRimColor * rim * viewmodelRimStrength;

        vec3 fullbright = texture2D(fullbrightTexture, vTexCoord).rgb;

        vec3 litColor = (ambientColor + diffuseLight) * albedo.rgb + rimLight;
        litColor += fullbright;
        litColor = ApplyFog(litColor);

        if (debugViewmodelMagenta && isViewmodel)
        {
                gl_FragColor = vec4(1.0, 0.0, 1.0, 1.0);
                return;
        }

        gl_FragColor = vec4(litColor, albedo.a);
}
