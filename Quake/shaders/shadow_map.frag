#version 330 core
in vec4 vColor;
in vec4 vShadowCoord;

uniform sampler2DShadow uShadowMap;

// Pixel-Offsets für PCF (in 1/textureSize)
uniform vec2 uShadowTexel;

out vec4 FragColor;

// 8x8 PCF (wie dein Original, aber kompakter)
float pcfShadow(vec4 sc) {
    // textureProj erwartet sc in Licht-Clip (inkl. w) und macht die Division selbst
    float sum = 0.0;
    for (int y = -3; y <= 4; ++y) {
        for (int x = -3; x <= 4; ++x) {
            vec2 off = vec2(x, y) * uShadowTexel;
            // depth bias klein halten; passe 0.0005–0.003 je nach Szene an
            sum += textureProj(uShadowMap, vec4(sc.xy + off * sc.w, sc.z - 0.0015 * sc.w, sc.w));
        }
    }
    return sum / 64.0;
}

void main() {
    float lit = 1.0;
    // Fragments hinter dem Licht-far plane überspringen (wie dein w>1 Guard)
    if (vShadowCoord.w > 0.0) {
        lit = pcfShadow(vShadowCoord);
    }
    // Kleines Ambient-Offset (wie +0.2 in deinem Original)
    FragColor = (lit + 0.2) * vColor;
}
