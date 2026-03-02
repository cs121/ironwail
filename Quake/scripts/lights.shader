// lights.shader - Q3-kompatibel, Ironwail fork
// Revised: 2026-03-01
//
// Aenderungen vs. vorherige Version:
//   - bloom/bloomScale entfernt (engine-spezifisch, bricht Q3-Parser)
//   - Emissive add-Pass auf Basis-Texturen entfernt (keine Selbstleuchtung mehr)
//   - Defekte Klammerstruktur vollstaendig korrigiert
//   - surfaceparm trans / sort decal von soliden Lichtflaechentexturen entfernt
//   - Behalten: envmap-Sheen + animierter Scan-Overlay fuer lebendigen Look
//   - Behalten: separate _glow-Texturen fuer tlight07/tlight11 (geringe Intensitaet)
//

+0light01
{
    qer_editorimage textures/+0light01
    cull none

    // Lightmap
    {
        map $lightmap
        rgbGen identity
    }

    // Base texture
    {
        map textures/+0light01
        blendFunc filter
        rgbGen identity
    }

    // Subtle envmap sheen
    {
        map textures/effects/envmap
        tcGen environment
        blendFunc GL_ONE GL_ONE_MINUS_SRC_COLOR
        rgbGen const ( 0.05 0.05 0.05 )
    }

    // Animated scan overlay for subtle "lebendige" feel
    {
        map textures/+0light01
        blendFunc add
        tcMod scale 1 6
        tcMod scroll 0 0.35
        rgbGen const ( 0.06 0.06 0.06 )
    }
}

+1light01
{
    qer_editorimage textures/+1light01
    cull none

    // Lightmap
    {
        map $lightmap
        rgbGen identity
    }

    // Base texture
    {
        map textures/+1light01
        blendFunc filter
        rgbGen identity
    }

    // Subtle envmap sheen
    {
        map textures/effects/envmap
        tcGen environment
        blendFunc GL_ONE GL_ONE_MINUS_SRC_COLOR
        rgbGen const ( 0.05 0.05 0.05 )
    }

    // Animated scan overlay for subtle "lebendige" feel
    {
        map textures/+1light01
        blendFunc add
        tcMod scale 1 6
        tcMod scroll 0 0.35
        rgbGen const ( 0.06 0.06 0.06 )
    }
}

+2light01
{
    qer_editorimage textures/+2light01
    cull none

    // Lightmap
    {
        map $lightmap
        rgbGen identity
    }

    // Base texture
    {
        map textures/+2light01
        blendFunc filter
        rgbGen identity
    }

    // Subtle envmap sheen
    {
        map textures/effects/envmap
        tcGen environment
        blendFunc GL_ONE GL_ONE_MINUS_SRC_COLOR
        rgbGen const ( 0.05 0.05 0.05 )
    }

    // Animated scan overlay for subtle "lebendige" feel
    {
        map textures/+2light01
        blendFunc add
        tcMod scale 1 6
        tcMod scroll 0 0.35
        rgbGen const ( 0.06 0.06 0.06 )
    }
}

enter01
{
    qer_editorimage textures/enter01

    // Lightmap
    {
        map $lightmap
        rgbGen identity
    }

    // Base texture
    {
        map textures/enter01
        blendFunc filter
        rgbGen identity
    }

    // Subtle envmap sheen
    {
        map textures/effects/envmap
        tcGen environment
        blendFunc GL_ONE GL_ONE_MINUS_SRC_COLOR
        rgbGen const ( 0.05 0.05 0.05 )
    }

    // Animated scan overlay for subtle "lebendige" feel
    {
        map textures/enter01
        blendFunc add
        tcMod scale 1 6
        tcMod scroll 0 0.35
        rgbGen const ( 0.06 0.06 0.06 )
    }
}

exit01
{
    qer_editorimage textures/exit01

    // Lightmap
    {
        map $lightmap
        rgbGen identity
    }

    // Base texture
    {
        map textures/exit01
        blendFunc filter
        rgbGen identity
    }

    // Subtle envmap sheen
    {
        map textures/effects/envmap
        tcGen environment
        blendFunc GL_ONE GL_ONE_MINUS_SRC_COLOR
        rgbGen const ( 0.05 0.05 0.05 )
    }

    // Animated scan overlay for subtle "lebendige" feel
    {
        map textures/exit01
        blendFunc add
        tcMod scale 1 6
        tcMod scroll 0 0.35
        rgbGen const ( 0.06 0.06 0.06 )
    }
}

light1_1
{
    qer_editorimage textures/light1_1

    // Lightmap
    {
        map $lightmap
        rgbGen identity
    }

    // Base texture
    {
        map textures/light1_1
        blendFunc filter
        rgbGen identity
    }

    // Subtle envmap sheen
    {
        map textures/effects/envmap
        tcGen environment
        blendFunc GL_ONE GL_ONE_MINUS_SRC_COLOR
        rgbGen const ( 0.05 0.05 0.05 )
    }

    // Animated scan overlay for subtle "lebendige" feel
    {
        map textures/light1_1
        blendFunc add
        tcMod scale 1 6
        tcMod scroll 0 0.35
        rgbGen const ( 0.06 0.06 0.06 )
    }
}

light1_2
{
    qer_editorimage textures/light1_2

    // Lightmap
    {
        map $lightmap
        rgbGen identity
    }

    // Base texture
    {
        map textures/light1_2
        blendFunc filter
        rgbGen identity
    }

    // Subtle envmap sheen
    {
        map textures/effects/envmap
        tcGen environment
        blendFunc GL_ONE GL_ONE_MINUS_SRC_COLOR
        rgbGen const ( 0.05 0.05 0.05 )
    }

    // Animated scan overlay for subtle "lebendige" feel
    {
        map textures/light1_2
        blendFunc add
        tcMod scale 1 6
        tcMod scroll 0 0.35
        rgbGen const ( 0.06 0.06 0.06 )
    }
}

light1_3
{
    qer_editorimage textures/light1_3

    // Lightmap
    {
        map $lightmap
        rgbGen identity
    }

    // Base texture
    {
        map textures/light1_3
        blendFunc filter
        rgbGen identity
    }

    // Subtle envmap sheen
    {
        map textures/effects/envmap
        tcGen environment
        blendFunc GL_ONE GL_ONE_MINUS_SRC_COLOR
        rgbGen const ( 0.05 0.05 0.05 )
    }

    // Animated scan overlay for subtle "lebendige" feel
    {
        map textures/light1_3
        blendFunc add
        tcMod scale 1 6
        tcMod scroll 0 0.35
        rgbGen const ( 0.06 0.06 0.06 )
    }
}

light1_4
{
    qer_editorimage textures/light1_4

    // Lightmap
    {
        map $lightmap
        rgbGen identity
    }

    // Base texture
    {
        map textures/light1_4
        blendFunc filter
        rgbGen identity
    }

    // Subtle envmap sheen
    {
        map textures/effects/envmap
        tcGen environment
        blendFunc GL_ONE GL_ONE_MINUS_SRC_COLOR
        rgbGen const ( 0.05 0.05 0.05 )
    }

    // Animated scan overlay for subtle "lebendige" feel
    {
        map textures/light1_4
        blendFunc add
        tcMod scale 1 6
        tcMod scroll 0 0.35
        rgbGen const ( 0.06 0.06 0.06 )
    }
}

light1_5
{
    qer_editorimage textures/light1_5

    // Lightmap
    {
        map $lightmap
        rgbGen identity
    }

    // Base texture
    {
        map textures/light1_5
        blendFunc filter
        rgbGen identity
    }

    // Subtle envmap sheen
    {
        map textures/effects/envmap
        tcGen environment
        blendFunc GL_ONE GL_ONE_MINUS_SRC_COLOR
        rgbGen const ( 0.05 0.05 0.05 )
    }

    // Animated scan overlay for subtle "lebendige" feel
    {
        map textures/light1_5
        blendFunc add
        tcMod scale 1 6
        tcMod scroll 0 0.35
        rgbGen const ( 0.06 0.06 0.06 )
    }
}

light1_7
{
    qer_editorimage textures/light1_7

    // Lightmap
    {
        map $lightmap
        rgbGen identity
    }

    // Base texture
    {
        map textures/light1_7
        blendFunc filter
        rgbGen identity
    }

    // Subtle envmap sheen
    {
        map textures/effects/envmap
        tcGen environment
        blendFunc GL_ONE GL_ONE_MINUS_SRC_COLOR
        rgbGen const ( 0.05 0.05 0.05 )
    }

    // Animated scan overlay for subtle "lebendige" feel
    {
        map textures/light1_7
        blendFunc add
        tcMod scale 1 6
        tcMod scroll 0 0.35
        rgbGen const ( 0.06 0.06 0.06 )
    }
}

light1_8
{
    qer_editorimage textures/light1_8

    // Lightmap
    {
        map $lightmap
        rgbGen identity
    }

    // Base texture
    {
        map textures/light1_8
        blendFunc filter
        rgbGen identity
    }

    // Subtle envmap sheen
    {
        map textures/effects/envmap
        tcGen environment
        blendFunc GL_ONE GL_ONE_MINUS_SRC_COLOR
        rgbGen const ( 0.05 0.05 0.05 )
    }

    // Animated scan overlay for subtle "lebendige" feel
    {
        map textures/light1_8
        blendFunc add
        tcMod scale 1 6
        tcMod scroll 0 0.35
        rgbGen const ( 0.06 0.06 0.06 )
    }
}

light3_3
{
    qer_editorimage textures/light3_3

    // Lightmap
    {
        map $lightmap
        rgbGen identity
    }

    // Base texture
    {
        map textures/light3_3
        blendFunc filter
        rgbGen identity
    }

    // Subtle envmap sheen
    {
        map textures/effects/envmap
        tcGen environment
        blendFunc GL_ONE GL_ONE_MINUS_SRC_COLOR
        rgbGen const ( 0.05 0.05 0.05 )
    }

    // Animated scan overlay for subtle "lebendige" feel
    {
        map textures/light3_3
        blendFunc add
        tcMod scale 1 6
        tcMod scroll 0 0.35
        rgbGen const ( 0.06 0.06 0.06 )
    }
}

light3_5
{
    qer_editorimage textures/light3_5

    // Lightmap
    {
        map $lightmap
        rgbGen identity
    }

    // Base texture
    {
        map textures/light3_5
        blendFunc filter
        rgbGen identity
    }

    // Subtle envmap sheen
    {
        map textures/effects/envmap
        tcGen environment
        blendFunc GL_ONE GL_ONE_MINUS_SRC_COLOR
        rgbGen const ( 0.05 0.05 0.05 )
    }

    // Animated scan overlay for subtle "lebendige" feel
    {
        map textures/light3_5
        blendFunc add
        tcMod scale 1 6
        tcMod scroll 0 0.35
        rgbGen const ( 0.06 0.06 0.06 )
    }
}

light3_6
{
    qer_editorimage textures/light3_6

    // Lightmap
    {
        map $lightmap
        rgbGen identity
    }

    // Base texture
    {
        map textures/light3_6
        blendFunc filter
        rgbGen identity
    }

    // Subtle envmap sheen
    {
        map textures/effects/envmap
        tcGen environment
        blendFunc GL_ONE GL_ONE_MINUS_SRC_COLOR
        rgbGen const ( 0.05 0.05 0.05 )
    }

    // Animated scan overlay for subtle "lebendige" feel
    {
        map textures/light3_6
        blendFunc add
        tcMod scale 1 6
        tcMod scroll 0 0.35
        rgbGen const ( 0.06 0.06 0.06 )
    }
}

light3_7
{
    qer_editorimage textures/light3_7

    // Lightmap
    {
        map $lightmap
        rgbGen identity
    }

    // Base texture
    {
        map textures/light3_7
        blendFunc filter
        rgbGen identity
    }

    // Subtle envmap sheen
    {
        map textures/effects/envmap
        tcGen environment
        blendFunc GL_ONE GL_ONE_MINUS_SRC_COLOR
        rgbGen const ( 0.05 0.05 0.05 )
    }

    // Animated scan overlay for subtle "lebendige" feel
    {
        map textures/light3_7
        blendFunc add
        tcMod scale 1 6
        tcMod scroll 0 0.35
        rgbGen const ( 0.06 0.06 0.06 )
    }
}

light3_8
{
    qer_editorimage textures/light3_8

    // Lightmap
    {
        map $lightmap
        rgbGen identity
    }

    // Base texture
    {
        map textures/light3_8
        blendFunc filter
        rgbGen identity
    }

    // Subtle envmap sheen
    {
        map textures/effects/envmap
        tcGen environment
        blendFunc GL_ONE GL_ONE_MINUS_SRC_COLOR
        rgbGen const ( 0.05 0.05 0.05 )
    }

    // Animated scan overlay for subtle "lebendige" feel
    {
        map textures/light3_8
        blendFunc add
        tcMod scale 1 6
        tcMod scroll 0 0.35
        rgbGen const ( 0.06 0.06 0.06 )
    }
}

sliplite
{
    qer_editorimage textures/sliplite

    // Lightmap
    {
        map $lightmap
        rgbGen identity
    }

    // Base texture
    {
        map textures/sliplite
        blendFunc filter
        rgbGen identity
    }

    // Subtle envmap sheen
    {
        map textures/effects/envmap
        tcGen environment
        blendFunc GL_ONE GL_ONE_MINUS_SRC_COLOR
        rgbGen const ( 0.05 0.05 0.05 )
    }

    // Animated scan overlay for subtle "lebendige" feel
    {
        map textures/sliplite
        blendFunc add
        tcMod scale 1 6
        tcMod scroll 0 0.35
        rgbGen const ( 0.06 0.06 0.06 )
    }
}

tele_top
{
    qer_editorimage textures/tele_top

    // Lightmap
    {
        map $lightmap
        rgbGen identity
    }

    // Base texture
    {
        map textures/tele_top
        blendFunc filter
        rgbGen identity
    }

    // Subtle envmap sheen
    {
        map textures/effects/envmap
        tcGen environment
        blendFunc GL_ONE GL_ONE_MINUS_SRC_COLOR
        rgbGen const ( 0.05 0.05 0.05 )
    }

    // Animated scan overlay for subtle "lebendige" feel
    {
        map textures/tele_top
        blendFunc add
        tcMod scale 1 6
        tcMod scroll 0 0.35
        rgbGen const ( 0.06 0.06 0.06 )
    }
}

tlight01
{
    qer_editorimage textures/tlight01

    // Lightmap
    {
        map $lightmap
        rgbGen identity
    }

    // Base texture
    {
        map textures/tlight01
        blendFunc filter
        rgbGen identity
    }

    // Subtle envmap sheen
    {
        map textures/effects/envmap
        tcGen environment
        blendFunc GL_ONE GL_ONE_MINUS_SRC_COLOR
        rgbGen const ( 0.05 0.05 0.05 )
    }

    // Animated scan overlay for subtle "lebendige" feel
    {
        map textures/tlight01
        blendFunc add
        tcMod scale 1 6
        tcMod scroll 0 0.35
        rgbGen const ( 0.06 0.06 0.06 )
    }
}

tlight01_2
{
    qer_editorimage textures/tlight01_2

    // Lightmap
    {
        map $lightmap
        rgbGen identity
    }

    // Base texture
    {
        map textures/tlight01_2
        blendFunc filter
        rgbGen identity
    }

    // Subtle envmap sheen
    {
        map textures/effects/envmap
        tcGen environment
        blendFunc GL_ONE GL_ONE_MINUS_SRC_COLOR
        rgbGen const ( 0.05 0.05 0.05 )
    }

    // Animated scan overlay for subtle "lebendige" feel
    {
        map textures/tlight01_2
        blendFunc add
        tcMod scale 1 6
        tcMod scroll 0 0.35
        rgbGen const ( 0.06 0.06 0.06 )
    }
}

tlight02
{
    qer_editorimage textures/tlight02

    // Lightmap
    {
        map $lightmap
        rgbGen identity
    }

    // Base texture
    {
        map textures/tlight02
        blendFunc filter
        rgbGen identity
    }

    // Subtle envmap sheen
    {
        map textures/effects/envmap
        tcGen environment
        blendFunc GL_ONE GL_ONE_MINUS_SRC_COLOR
        rgbGen const ( 0.05 0.05 0.05 )
    }

    // Animated scan overlay for subtle "lebendige" feel
    {
        map textures/tlight02
        blendFunc add
        tcMod scale 1 6
        tcMod scroll 0 0.35
        rgbGen const ( 0.06 0.06 0.06 )
    }
}

tlight03
{
    qer_editorimage textures/tlight03

    // Lightmap
    {
        map $lightmap
        rgbGen identity
    }

    // Base texture
    {
        map textures/tlight03
        blendFunc filter
        rgbGen identity
    }

    // Subtle envmap sheen
    {
        map textures/effects/envmap
        tcGen environment
        blendFunc GL_ONE GL_ONE_MINUS_SRC_COLOR
        rgbGen const ( 0.05 0.05 0.05 )
    }

    // Animated scan overlay for subtle "lebendige" feel
    {
        map textures/tlight03
        blendFunc add
        tcMod scale 1 6
        tcMod scroll 0 0.35
        rgbGen const ( 0.06 0.06 0.06 )
    }
}

tlight05
{
    qer_editorimage textures/tlight05

    // Lightmap
    {
        map $lightmap
        rgbGen identity
    }

    // Base texture
    {
        map textures/tlight05
        blendFunc filter
        rgbGen identity
    }

    // Subtle envmap sheen
    {
        map textures/effects/envmap
        tcGen environment
        blendFunc GL_ONE GL_ONE_MINUS_SRC_COLOR
        rgbGen const ( 0.05 0.05 0.05 )
    }

    // Animated scan overlay for subtle "lebendige" feel
    {
        map textures/tlight05
        blendFunc add
        tcMod scale 1 6
        tcMod scroll 0 0.35
        rgbGen const ( 0.06 0.06 0.06 )
    }
}

tlight07
{
    qer_editorimage textures/tlight07

    // Lightmap
    {
        map $lightmap
        rgbGen identity
    }

    // Base texture
    {
        map textures/tlight07
        blendFunc filter
        rgbGen identity
    }

    // Glow overlay (separate _glow texture, low intensity)
    {
        map textures/tlight07_glow
        blendFunc add
        rgbGen const ( 0.4 0.4 0.4 )
    }

    // Subtle envmap sheen
    {
        map textures/effects/envmap
        tcGen environment
        blendFunc GL_ONE GL_ONE_MINUS_SRC_COLOR
        rgbGen const ( 0.05 0.05 0.05 )
    }

    // Animated scan overlay for subtle "lebendige" feel
    {
        map textures/tlight07
        blendFunc add
        tcMod scale 1 6
        tcMod scroll 0 0.35
        rgbGen const ( 0.06 0.06 0.06 )
    }
}

tlight08
{
    qer_editorimage textures/tlight08

    // Lightmap
    {
        map $lightmap
        rgbGen identity
    }

    // Base texture
    {
        map textures/tlight08
        blendFunc filter
        rgbGen identity
    }

    // Subtle envmap sheen
    {
        map textures/effects/envmap
        tcGen environment
        blendFunc GL_ONE GL_ONE_MINUS_SRC_COLOR
        rgbGen const ( 0.05 0.05 0.05 )
    }

    // Animated scan overlay for subtle "lebendige" feel
    {
        map textures/tlight08
        blendFunc add
        tcMod scale 1 6
        tcMod scroll 0 0.35
        rgbGen const ( 0.06 0.06 0.06 )
    }
}

tlight09
{
    qer_editorimage textures/tlight09

    // Lightmap
    {
        map $lightmap
        rgbGen identity
    }

    // Base texture
    {
        map textures/tlight09
        blendFunc filter
        rgbGen identity
    }

    // Subtle envmap sheen
    {
        map textures/effects/envmap
        tcGen environment
        blendFunc GL_ONE GL_ONE_MINUS_SRC_COLOR
        rgbGen const ( 0.05 0.05 0.05 )
    }

    // Animated scan overlay for subtle "lebendige" feel
    {
        map textures/tlight09
        blendFunc add
        tcMod scale 1 6
        tcMod scroll 0 0.35
        rgbGen const ( 0.06 0.06 0.06 )
    }
}

tlight10
{
    qer_editorimage textures/tlight10

    // Lightmap
    {
        map $lightmap
        rgbGen identity
    }

    // Base texture
    {
        map textures/tlight10
        blendFunc filter
        rgbGen identity
    }

    // Subtle envmap sheen
    {
        map textures/effects/envmap
        tcGen environment
        blendFunc GL_ONE GL_ONE_MINUS_SRC_COLOR
        rgbGen const ( 0.05 0.05 0.05 )
    }

    // Animated scan overlay for subtle "lebendige" feel
    {
        map textures/tlight10
        blendFunc add
        tcMod scale 1 6
        tcMod scroll 0 0.35
        rgbGen const ( 0.06 0.06 0.06 )
    }
}

tlight11
{
    qer_editorimage textures/tlight11

    // Lightmap
    {
        map $lightmap
        rgbGen identity
    }

    // Base texture
    {
        map textures/tlight11
        blendFunc filter
        rgbGen identity
    }

    // Glow overlay (separate _glow texture, low intensity)
    {
        map textures/tlight11_glow
        blendFunc add
        rgbGen const ( 0.4 0.4 0.4 )
    }

    // Subtle envmap sheen
    {
        map textures/effects/envmap
        tcGen environment
        blendFunc GL_ONE GL_ONE_MINUS_SRC_COLOR
        rgbGen const ( 0.05 0.05 0.05 )
    }

    // Animated scan overlay for subtle "lebendige" feel
    {
        map textures/tlight11
        blendFunc add
        tcMod scale 1 6
        tcMod scroll 0 0.35
        rgbGen const ( 0.06 0.06 0.06 )
    }
}

wenter01
{
    qer_editorimage textures/wenter01

    // Lightmap
    {
        map $lightmap
        rgbGen identity
    }

    // Base texture
    {
        map textures/wenter01
        blendFunc filter
        rgbGen identity
    }

    // Subtle envmap sheen
    {
        map textures/effects/envmap
        tcGen environment
        blendFunc GL_ONE GL_ONE_MINUS_SRC_COLOR
        rgbGen const ( 0.05 0.05 0.05 )
    }

    // Animated scan overlay for subtle "lebendige" feel
    {
        map textures/wenter01
        blendFunc add
        tcMod scale 1 6
        tcMod scroll 0 0.35
        rgbGen const ( 0.06 0.06 0.06 )
    }
}

wexit01
{
    qer_editorimage textures/wexit01

    // Lightmap
    {
        map $lightmap
        rgbGen identity
    }

    // Base texture
    {
        map textures/wexit01
        blendFunc filter
        rgbGen identity
    }

    // Subtle envmap sheen
    {
        map textures/effects/envmap
        tcGen environment
        blendFunc GL_ONE GL_ONE_MINUS_SRC_COLOR
        rgbGen const ( 0.05 0.05 0.05 )
    }

    // Animated scan overlay for subtle "lebendige" feel
    {
        map textures/wexit01
        blendFunc add
        tcMod scale 1 6
        tcMod scroll 0 0.35
        rgbGen const ( 0.06 0.06 0.06 )
    }
}

window01_1
{
    qer_editorimage textures/window01_1

    // Lightmap
    {
        map $lightmap
        rgbGen identity
    }

    // Base texture
    {
        map textures/window01_1
        blendFunc filter
        rgbGen identity
    }

    // Subtle envmap sheen
    {
        map textures/effects/envmap
        tcGen environment
        blendFunc GL_ONE GL_ONE_MINUS_SRC_COLOR
        rgbGen const ( 0.05 0.05 0.05 )
    }

    // Animated scan overlay for subtle "lebendige" feel
    {
        map textures/window01_1
        blendFunc add
        tcMod scale 1 6
        tcMod scroll 0 0.35
        rgbGen const ( 0.06 0.06 0.06 )
    }
}

window01_2
{
    qer_editorimage textures/window01_2

    // Lightmap
    {
        map $lightmap
        rgbGen identity
    }

    // Base texture
    {
        map textures/window01_2
        blendFunc filter
        rgbGen identity
    }

    // Subtle envmap sheen
    {
        map textures/effects/envmap
        tcGen environment
        blendFunc GL_ONE GL_ONE_MINUS_SRC_COLOR
        rgbGen const ( 0.05 0.05 0.05 )
    }

    // Animated scan overlay for subtle "lebendige" feel
    {
        map textures/window01_2
        blendFunc add
        tcMod scale 1 6
        tcMod scroll 0 0.35
        rgbGen const ( 0.06 0.06 0.06 )
    }
}

window01_3
{
    qer_editorimage textures/window01_3

    // Lightmap
    {
        map $lightmap
        rgbGen identity
    }

    // Base texture
    {
        map textures/window01_3
        blendFunc filter
        rgbGen identity
    }

    // Subtle envmap sheen
    {
        map textures/effects/envmap
        tcGen environment
        blendFunc GL_ONE GL_ONE_MINUS_SRC_COLOR
        rgbGen const ( 0.05 0.05 0.05 )
    }

    // Animated scan overlay for subtle "lebendige" feel
    {
        map textures/window01_3
        blendFunc add
        tcMod scale 1 6
        tcMod scroll 0 0.35
        rgbGen const ( 0.06 0.06 0.06 )
    }
}

window01_4
{
    qer_editorimage textures/window01_4

    // Lightmap
    {
        map $lightmap
        rgbGen identity
    }

    // Base texture
    {
        map textures/window01_4
        blendFunc filter
        rgbGen identity
    }

    // Subtle envmap sheen
    {
        map textures/effects/envmap
        tcGen environment
        blendFunc GL_ONE GL_ONE_MINUS_SRC_COLOR
        rgbGen const ( 0.05 0.05 0.05 )
    }

    // Animated scan overlay for subtle "lebendige" feel
    {
        map textures/window01_4
        blendFunc add
        tcMod scale 1 6
        tcMod scroll 0 0.35
        rgbGen const ( 0.06 0.06 0.06 )
    }
}

window02_1
{
    qer_editorimage textures/window02_1

    // Lightmap
    {
        map $lightmap
        rgbGen identity
    }

    // Base texture
    {
        map textures/window02_1
        blendFunc filter
        rgbGen identity
    }

    // Subtle envmap sheen
    {
        map textures/effects/envmap
        tcGen environment
        blendFunc GL_ONE GL_ONE_MINUS_SRC_COLOR
        rgbGen const ( 0.05 0.05 0.05 )
    }

    // Animated scan overlay for subtle "lebendige" feel
    {
        map textures/window02_1
        blendFunc add
        tcMod scale 1 12
        tcMod scroll 0 0.7
        rgbGen const ( 0.06 0.06 0.06 )
    }
}

window03
{
    qer_editorimage textures/window03

    // Lightmap
    {
        map $lightmap
        rgbGen identity
    }

    // Base texture
    {
        map textures/window03
        blendFunc filter
        rgbGen identity
    }

    // Subtle envmap sheen
    {
        map textures/effects/envmap
        tcGen environment
        blendFunc GL_ONE GL_ONE_MINUS_SRC_COLOR
        rgbGen const ( 0.05 0.05 0.05 )
    }

    // Animated scan overlay for subtle "lebendige" feel
    {
        map textures/window03
        blendFunc add
        tcMod scale 1 6
        tcMod scroll 0 0.35
        rgbGen const ( 0.06 0.06 0.06 )
    }
}

window1_2
{
    qer_editorimage textures/window1_2

    // Lightmap
    {
        map $lightmap
        rgbGen identity
    }

    // Base texture
    {
        map textures/window1_2
        blendFunc filter
        rgbGen identity
    }

    // Subtle envmap sheen
    {
        map textures/effects/envmap
        tcGen environment
        blendFunc GL_ONE GL_ONE_MINUS_SRC_COLOR
        rgbGen const ( 0.05 0.05 0.05 )
    }

    // Animated scan overlay for subtle "lebendige" feel
    {
        map textures/window1_2
        blendFunc add
        tcMod scale 1 6
        tcMod scroll 0 0.35
        rgbGen const ( 0.06 0.06 0.06 )
    }
}

window1_3
{
    qer_editorimage textures/window1_3

    // Lightmap
    {
        map $lightmap
        rgbGen identity
    }

    // Base texture
    {
        map textures/window1_3
        blendFunc filter
        rgbGen identity
    }

    // Subtle envmap sheen
    {
        map textures/effects/envmap
        tcGen environment
        blendFunc GL_ONE GL_ONE_MINUS_SRC_COLOR
        rgbGen const ( 0.05 0.05 0.05 )
    }

    // Animated scan overlay for subtle "lebendige" feel
    {
        map textures/window1_3
        blendFunc add
        tcMod scale 1 6
        tcMod scroll 0 0.35
        rgbGen const ( 0.06 0.06 0.06 )
    }
}

window1_4
{
    qer_editorimage textures/window1_4

    // Lightmap
    {
        map $lightmap
        rgbGen identity
    }

    // Base texture
    {
        map textures/window1_4
        blendFunc filter
        rgbGen identity
    }

    // Subtle envmap sheen
    {
        map textures/effects/envmap
        tcGen environment
        blendFunc GL_ONE GL_ONE_MINUS_SRC_COLOR
        rgbGen const ( 0.05 0.05 0.05 )
    }

    // Animated scan overlay for subtle "lebendige" feel
    {
        map textures/window1_4
        blendFunc add
        tcMod scale 1 6
        tcMod scroll 0 0.35
        rgbGen const ( 0.06 0.06 0.06 )
    }
}

wizwin1_2
{
    qer_editorimage textures/wizwin1_2

    // Lightmap
    {
        map $lightmap
        rgbGen identity
    }

    // Base texture
    {
        map textures/wizwin1_2
        blendFunc filter
        rgbGen identity
    }

    // Subtle envmap sheen
    {
        map textures/effects/envmap
        tcGen environment
        blendFunc GL_ONE GL_ONE_MINUS_SRC_COLOR
        rgbGen const ( 0.05 0.05 0.05 )
    }

    // Animated scan overlay for subtle "lebendige" feel
    {
        map textures/wizwin1_2
        blendFunc add
        tcMod scale 1 6
        tcMod scroll 0 0.35
        rgbGen const ( 0.06 0.06 0.06 )
    }
}

wizwin1_8
{
    qer_editorimage textures/wizwin1_8

    // Lightmap
    {
        map $lightmap
        rgbGen identity
    }

    // Base texture
    {
        map textures/wizwin1_8
        blendFunc filter
        rgbGen identity
    }

    // Subtle envmap sheen
    {
        map textures/effects/envmap
        tcGen environment
        blendFunc GL_ONE GL_ONE_MINUS_SRC_COLOR
        rgbGen const ( 0.05 0.05 0.05 )
    }

    // Animated scan overlay for subtle "lebendige" feel
    {
        map textures/wizwin1_8
        blendFunc add
        tcMod scale 1 6
        tcMod scroll 0 0.35
        rgbGen const ( 0.06 0.06 0.06 )
    }
}

z_exit
{
    qer_editorimage z_exit

    // Lightmap
    {
        map $lightmap
        rgbGen identity
    }

    // Base texture
    {
        map z_exit
        blendFunc filter
        rgbGen identity
    }

    // Subtle envmap sheen
    {
        map textures/effects/envmap
        tcGen environment
        blendFunc GL_ONE GL_ONE_MINUS_SRC_COLOR
        rgbGen const ( 0.05 0.05 0.05 )
    }

    // Animated scan overlay for subtle "lebendige" feel
    {
        map z_exit
        blendFunc add
        tcMod scale 1 6
        tcMod scroll 0 0.35
        rgbGen const ( 0.06 0.06 0.06 )
    }
}
