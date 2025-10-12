#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec4 aColor;

uniform mat4 uModel;
uniform mat4 uView;
uniform mat4 uProj;

// Matrix, die Welt → Licht-Clip (ProjLight * ViewLight)
uniform mat4 uLightViewProj;

out vec4 vColor;
out vec4 vShadowCoord;

void main() {
    mat4 MVP = uProj * uView * uModel;
    gl_Position = MVP * vec4(aPos, 1.0);
    vColor = aColor;

    // ShadowCoord: später mit textureProj() genutzt
    vShadowCoord = uLightViewProj * (uModel * vec4(aPos, 1.0));
}
