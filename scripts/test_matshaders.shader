// Test shaders for material shader parsing/debug.

textures/test/test_glow_add
{
    qer_editorimage textures/test/test_glow_add

    {
        map textures/test/test_glow_add
        rgbGen identity
    }
    {
        map textures/test/test_glow_add_glow
        blendFunc add
        emissive true
    }
}

textures/test/test_lightmap_filter
{
    qer_editorimage textures/test/test_lightmap_filter

    {
        map textures/test/test_lightmap_filter
        rgbGen identity
    }
    {
        map $lightmap
        blendFunc filter
        rgbGen identity
    }
}

textures/test/test_scroll_turb_water
{
    qer_editorimage textures/test/test_scroll_turb_water
    surfaceparm trans

    {
        map textures/test/test_scroll_turb_water
        blendFunc blend
        tcMod scroll 0.1 0.05
        tcMod turb 0.0 0.2 0.0 0.3
    }
}

textures/test/test_animmap_console
{
    qer_editorimage textures/test/test_animmap_console

    {
        animMap 8 textures/test/console_01 textures/test/console_02 textures/test/console_03 textures/test/console_04
        rgbGen identity
    }
}

textures/test/test_godray_source_only
{
    qer_editorimage textures/test/test_godray_source_only
    godray true
    emissive false

    {
        map textures/test/test_godray_source_only
        rgbGen identity
        godray true
    }
}
