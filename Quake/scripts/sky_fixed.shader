// Sky: godray + bloom (und optional emissive aus, damit es nicht als "Emissive-Source" doppelt zählt)

textures/sky1
{
    qer_editorimage textures/sky1
    surfaceparm sky

    // godray true  (removed: not supported by current parser)
    // bloom true  (removed: not supported by current parser)
    emissive false

    {
        map textures/sky1
        rgbGen identity
    // godray true  (removed: not supported by current parser)
    }
}

textures/sky4
{
    qer_editorimage textures/sky4
    surfaceparm sky

    // godray true  (removed: not supported by current parser)
    // bloom true  (removed: not supported by current parser)
    emissive false

    {
        map textures/sky4
        rgbGen identity
    // godray true  (removed: not supported by current parser)
    }
}
