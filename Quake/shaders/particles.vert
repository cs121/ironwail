#include "frame_uniforms.glsl"

vec3 ApplyFog(vec3 clr, vec3 p)
{
        float fog = exp2(-Fog.w * dot(p, p));
        fog = clamp(fog, 0.0, 1.0);
        return mix(Fog.rgb, clr, fog);
}

layout(location=0) in vec3 in_pos;
layout(location=1) in vec3 in_vel;
layout(location=2) in vec4 in_color;
layout(location=3) in vec4 in_params;
layout(location=4) in vec4 in_custom;

layout(location=0) out vec2 out_uv;
layout(location=1) out vec4 out_color;
layout(location=2) out vec3 out_pos;
layout(location=3) out vec4 out_params;
layout(location=4) out vec2 out_corner;

layout(location=0) uniform vec3 Params; // x = UV scale
layout(location=1) uniform vec4 AtlasInfo;
layout(location=2) uniform vec4 ViewRight;
layout(location=3) uniform vec4 ViewUp;
layout(location=4) uniform vec4 ViewForward;

const int ORIENT_BILLBOARD = 0;
const int ORIENT_ORIENTED = 1;
const int ORIENT_BEAM = 2;
const int ORIENT_SPARK = 3;

const float GLOW_INV_SCALE = 1.0 / 32.0;

void main()
{
        uvec2 flipsign = uvec2(gl_VertexID, gl_VertexID >> 1) << 31;
        vec2 corner = uintBitsToFloat(floatBitsToUint(-1.0) ^ flipsign);

        float uvScale = Params.x;
        int orient = int(in_params.z + 0.5);

        vec3 viewRight = normalize(ViewRight.xyz);
        vec3 viewUp = normalize(ViewUp.xyz);
        vec3 viewForward = normalize(ViewForward.xyz);

        vec3 worldPos = in_pos;
        vec2 uvCorner;
        vec2 fadeCorner;

        float sizeX = max(in_custom.x, 0.0);
        float sizeY = max(in_custom.y, 0.0);
        float rotation = in_custom.z;
        float extra = max(in_custom.w, 0.0);

        if (orient == ORIENT_BEAM)
        {
                vec3 forward = normalize(in_vel);
                if (length(forward) < 1e-4)
                        forward = viewForward;
                vec3 eyeDir = normalize(EyePos - in_pos);
                vec3 right = normalize(cross(eyeDir, forward));
                if (length(right) < 1e-4)
                        right = normalize(cross(viewUp, forward));
                vec3 up = cross(forward, right);

                float along = clamp(0.5 * (corner.x + 1.0), 0.0, 1.0);
                float halfWidth = sizeY * 0.5;
                vec3 base = in_pos + forward * (extra * along);
                vec3 offset = right * (corner.y * halfWidth);
                worldPos = base + offset;

                uvCorner = vec2(along, 0.5 * (corner.y + 1.0));
                fadeCorner = vec2(corner.y, 0.0);
        }
        else if (orient == ORIENT_SPARK)
        {
            vec3 forward = normalize(in_vel);
            if (length(forward) < 1e-4)
                    forward = viewForward;
            vec3 eyeDir = normalize(EyePos - in_pos);
            vec3 right = normalize(cross(eyeDir, forward));
            if (length(right) < 1e-4)
                    right = normalize(cross(viewUp, forward));
            vec3 up = cross(forward, right);

            float halfLen = extra * 0.5;
            float halfWidth = sizeY * 0.5;
            vec3 center = in_pos + forward * halfLen;
            vec3 offset = forward * (corner.x * halfLen) + right * (corner.y * halfWidth);
            worldPos = center + offset;

            uvCorner = vec2(0.5 * (corner.x + 1.0), 0.5 * (corner.y + 1.0));
            fadeCorner = corner;
        }
        else
        {
                vec2 rotated;
                float s = sin(rotation);
                float c = cos(rotation);
                rotated = vec2(corner.x * c - corner.y * s, corner.x * s + corner.y * c);

                if (orient == ORIENT_ORIENTED)
                {
                        vec3 forward = normalize(in_vel);
                        if (length(forward) < 1e-4)
                                forward = viewForward;
                        vec3 right = normalize(cross(forward, viewUp));
                        if (length(right) < 1e-4)
                                right = normalize(cross(forward, viewRight));
                        vec3 up = normalize(cross(right, forward));
                        vec3 offset = right * (rotated.x * sizeX * 0.5) + up * (rotated.y * sizeY * 0.5);
                        worldPos += offset;
                }
                else
                {
                        vec3 offset = viewRight * (rotated.x * sizeX * 0.5) + viewUp * (rotated.y * sizeY * 0.5);
                        worldPos += offset;
                }

                uvCorner = rotated * 0.5 + 0.5;
                fadeCorner = rotated;
        }

        gl_Position = ViewProj * vec4(worldPos, 1.0);
        out_pos = worldPos - EyePos;

        float textureIndex = in_params.y;
        float columns = AtlasInfo.x > 0.0 ? 1.0 / AtlasInfo.x : 1.0;
        float rows = AtlasInfo.y > 0.0 ? 1.0 / AtlasInfo.y : 1.0;
        float totalTiles = columns * rows;
        textureIndex = clamp(textureIndex, 0.0, max(totalTiles - 1.0, 0.0));

        vec2 uv01 = uvCorner;
        uv01 = (uv01 - 0.5) * uvScale + 0.5;
        vec2 margin = vec2(AtlasInfo.z, AtlasInfo.w);
        uv01 = clamp(uv01, margin, vec2(1.0) - margin);
        float column = columns > 0.0 ? mod(textureIndex, columns) : 0.0;
        float row = columns > 0.0 ? floor(textureIndex / columns) : 0.0;
        row = rows > 0.0 ? clamp(row, 0.0, rows - 1.0) : 0.0;
        vec2 base = vec2(column * AtlasInfo.x, row * AtlasInfo.y);
        vec2 uv = base + uv01 * vec2(AtlasInfo.x, AtlasInfo.y);

        out_uv = uv;
        out_color = in_color;
        out_params = vec4(in_params.x * GLOW_INV_SCALE, uvScale, float(orient), in_params.w);
        out_corner = fadeCorner;
#if OIT
        out_color.a *= 0.9;
#endif
}
