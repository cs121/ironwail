// Q3-style shader demo for Ironwail
// Uses all supported keywords and functions in the current parser.

textures/demo/q3_shader_all
{
    qer_editorimage textures/demo/q3_shader_all

    surfaceparm solid
    surfaceparm nonsolid
    surfaceparm playerclip
    surfaceparm monsterclip
    surfaceparm trans
    surfaceparm alphashadow
    surfaceparm sky
    surfaceparm fog
    surfaceparm nodraw
    surfaceparm stone

    emissive true
    bloom true
    godray true
    emissive_scale 1.25
    bloom_scale 0.85
    godray_scale 1.1

    {
        map textures/demo/q3_shader_all
        rgbGen identity
    }
}
