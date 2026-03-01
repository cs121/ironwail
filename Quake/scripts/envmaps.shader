// ------------------------------------------------------------
// Envmap helper materials generated from your cubemaps folder
// Place this file in: scripts/envmaps.shader (or any *.shader)
//
// Usage:
// - Apply material 'envmaps/<id>' to any surface to preview cubemap reflections.
// - Or copy the Reflection layer stage into your real materials.
//
// Default reflection strength: alphaGen const 0.25
// ------------------------------------------------------------

envmaps/01
{
    // Base layer so this material is visible if applied directly.
    {
        map $whiteimage
        rgbGen identity
    }

    // Reflection layer
    {
        map cubemaps/01
        tcGen environment
        // Optional: tweak reflection 'motion' with tcMod
        // tcMod rotate 1
        // tcMod scale 1 1
        blendFunc blend
        alphaGen const 0.25
        rgbGen identity
    }
}

envmaps/02
{
    // Base layer so this material is visible if applied directly.
    {
        map $whiteimage
        rgbGen identity
    }

    // Reflection layer
    {
        map cubemaps/02
        tcGen environment
        // Optional: tweak reflection 'motion' with tcMod
        // tcMod rotate 1
        // tcMod scale 1 1
        blendFunc blend
        alphaGen const 0.25
        rgbGen identity
    }
}

envmaps/03
{
    // Base layer so this material is visible if applied directly.
    {
        map $whiteimage
        rgbGen identity
    }

    // Reflection layer
    {
        map cubemaps/03
        tcGen environment
        // Optional: tweak reflection 'motion' with tcMod
        // tcMod rotate 1
        // tcMod scale 1 1
        blendFunc blend
        alphaGen const 0.25
        rgbGen identity
    }
}

envmaps/04
{
    // Base layer so this material is visible if applied directly.
    {
        map $whiteimage
        rgbGen identity
    }

    // Reflection layer
    {
        map cubemaps/04
        tcGen environment
        // Optional: tweak reflection 'motion' with tcMod
        // tcMod rotate 1
        // tcMod scale 1 1
        blendFunc blend
        alphaGen const 0.25
        rgbGen identity
    }
}

envmaps/05
{
    // Base layer so this material is visible if applied directly.
    {
        map $whiteimage
        rgbGen identity
    }

    // Reflection layer
    {
        map cubemaps/05
        tcGen environment
        // Optional: tweak reflection 'motion' with tcMod
        // tcMod rotate 1
        // tcMod scale 1 1
        blendFunc blend
        alphaGen const 0.25
        rgbGen identity
    }
}

envmaps/06
{
    // Base layer so this material is visible if applied directly.
    {
        map $whiteimage
        rgbGen identity
    }

    // Reflection layer
    {
        map cubemaps/06
        tcGen environment
        // Optional: tweak reflection 'motion' with tcMod
        // tcMod rotate 1
        // tcMod scale 1 1
        blendFunc blend
        alphaGen const 0.25
        rgbGen identity
    }
}

envmaps/07
{
    // Base layer so this material is visible if applied directly.
    {
        map $whiteimage
        rgbGen identity
    }

    // Reflection layer
    {
        map cubemaps/07
        tcGen environment
        // Optional: tweak reflection 'motion' with tcMod
        // tcMod rotate 1
        // tcMod scale 1 1
        blendFunc blend
        alphaGen const 0.25
        rgbGen identity
    }
}

envmaps/08
{
    // Base layer so this material is visible if applied directly.
    {
        map $whiteimage
        rgbGen identity
    }

    // Reflection layer
    {
        map cubemaps/08
        tcGen environment
        // Optional: tweak reflection 'motion' with tcMod
        // tcMod rotate 1
        // tcMod scale 1 1
        blendFunc blend
        alphaGen const 0.25
        rgbGen identity
    }
}

envmaps/09
{
    // Base layer so this material is visible if applied directly.
    {
        map $whiteimage
        rgbGen identity
    }

    // Reflection layer
    {
        map cubemaps/09
        tcGen environment
        // Optional: tweak reflection 'motion' with tcMod
        // tcMod rotate 1
        // tcMod scale 1 1
        blendFunc blend
        alphaGen const 0.25
        rgbGen identity
    }
}

envmaps/10
{
    // Base layer so this material is visible if applied directly.
    {
        map $whiteimage
        rgbGen identity
    }

    // Reflection layer
    {
        map cubemaps/10
        tcGen environment
        // Optional: tweak reflection 'motion' with tcMod
        // tcMod rotate 1
        // tcMod scale 1 1
        blendFunc blend
        alphaGen const 0.25
        rgbGen identity
    }
}

envmaps/11
{
    // Base layer so this material is visible if applied directly.
    {
        map $whiteimage
        rgbGen identity
    }

    // Reflection layer
    {
        map cubemaps/11
        tcGen environment
        // Optional: tweak reflection 'motion' with tcMod
        // tcMod rotate 1
        // tcMod scale 1 1
        blendFunc blend
        alphaGen const 0.25
        rgbGen identity
    }
}

envmaps/12
{
    // Base layer so this material is visible if applied directly.
    {
        map $whiteimage
        rgbGen identity
    }

    // Reflection layer
    {
        map cubemaps/12
        tcGen environment
        // Optional: tweak reflection 'motion' with tcMod
        // tcMod rotate 1
        // tcMod scale 1 1
        blendFunc blend
        alphaGen const 0.25
        rgbGen identity
    }
}

envmaps/13
{
    // Base layer so this material is visible if applied directly.
    {
        map $whiteimage
        rgbGen identity
    }

    // Reflection layer
    {
        map cubemaps/13
        tcGen environment
        // Optional: tweak reflection 'motion' with tcMod
        // tcMod rotate 1
        // tcMod scale 1 1
        blendFunc blend
        alphaGen const 0.25
        rgbGen identity
    }
}

envmaps/14
{
    // Base layer so this material is visible if applied directly.
    {
        map $whiteimage
        rgbGen identity
    }

    // Reflection layer
    {
        map cubemaps/14
        tcGen environment
        // Optional: tweak reflection 'motion' with tcMod
        // tcMod rotate 1
        // tcMod scale 1 1
        blendFunc blend
        alphaGen const 0.25
        rgbGen identity
    }
}

envmaps/15
{
    // Base layer so this material is visible if applied directly.
    {
        map $whiteimage
        rgbGen identity
    }

    // Reflection layer
    {
        map cubemaps/15
        tcGen environment
        // Optional: tweak reflection 'motion' with tcMod
        // tcMod rotate 1
        // tcMod scale 1 1
        blendFunc blend
        alphaGen const 0.25
        rgbGen identity
    }
}

// ------------------------------------------------------------
// window1_4 naming note:
// Your files are named like window1_4_bk.tga (with underscore).
// Depending on engine lookup, the correct base might be:
//   cubemaps/window1_4     (engine appends _bk/_dn/...)
// or
//   cubemaps/window1_4_    (engine appends bk/dn/... without underscore)
// Keep the one that works in your build.

envmaps/window1_4
{
    // Base layer so this material is visible if applied directly.
    {
        map $whiteimage
        rgbGen identity
    }

    // Reflection layer
    {
        map cubemaps/window1_4
        tcGen environment
        // Optional: tweak reflection 'motion' with tcMod
        // tcMod rotate 1
        // tcMod scale 1 1
        blendFunc blend
        alphaGen const 0.25
        rgbGen identity
    }
}

envmaps/window1_4_alt
{
    // Base layer so this material is visible if applied directly.
    {
        map $whiteimage
        rgbGen identity
    }

    // Reflection layer
    {
        map cubemaps/window1_4_
        tcGen environment
        // Optional: tweak reflection 'motion' with tcMod
        // tcMod rotate 1
        // tcMod scale 1 1
        blendFunc blend
        alphaGen const 0.25
        rgbGen identity
    }
}
