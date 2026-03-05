# Shadow Mapping (V1)

## Pass-Reihenfolge

Der Framegraph rendert Schatten vor den Haupt-Scene-Paessen:

1. `R_SetupView`
2. `R_RenderShadowMaps`
   - `R_RenderSunShadowMap` (1x 2D-Depthmap)
   - `R_RenderDLightShadowMaps` (Cube-Array, bis zu 4 Lichter x 6 Faces)
3. Normale Scene-/Lighting-Paesse

Damit sind Shadow-Resultate im selben Frame deterministic in World- und Alias-Receivern verfuegbar.

## Ressourcen

- Sun: `GL_TEXTURE_2D` Depth-Map + eigenes FBO
- DLight: `GL_TEXTURE_CUBE_MAP_ARRAY` Depth-Map + eigenes FBO
- Lifecycle: Erstellung/Loeschung ueber Framebuffer-Lifecycle (`vid_restart` etc.)
- Runtime-Reconcile: `r_shadow`/Map-Size-Aenderungen triggern Recreate ohne Neustart

## Caster / Receiver

- Caster V1:
  - World/Brush (opaque)
  - Alias (MDL + IQM/MD5-Pfad), nur opaque
- Explizit keine Caster:
  - Alpha-Test-Geometrie (V1-Limit)
  - Viewmodel, Sprites, Partikel, Wasser/Transparenz
- Receiver:
  - World: Sun + DLight shadow factors im Lighting-Pfad
  - Alias: Sun + per-light DLight-Beitrag im Fragment-Shader, inkl. Shadow-Modulation

## DLight-Selektion

- Budget: `SHADOW_DLIGHT_MAX = 4`
- Auswahl pro Frame aus sichtbaren GPU-Lights per deterministischem Score:
  - Radius
  - Farbluminanz
  - Distanz zur Kamera
- Tie-break: kleinerer Light-Index gewinnt (stabil/reproduzierbar)

## CVars

Feature-Schalter (Default aktiv):

- `r_shadow` = `1`
- `r_shadow_sun` = `1`
- `r_shadow_dlight` = `1`
- `r_shadow_dlight_max` = `4`

Qualitaet/Tuning:

- `r_shadow_sun_size` (Default `2048`)
- `r_shadow_dlight_size` (Default `512`)
- `r_shadow_sun_distance` (Default `1200`)
- `r_shadow_sun_bias` (Default `0.0015`)
- `r_shadow_dlight_bias` (Default `0.02`)
- `r_shadow_sun_pcf` (Default `1.5`)
- `r_shadow_dlight_pcf` (Default `0.75`)

Debug:

- `r_shadow_debug`:
  - `0`: aus
  - `1`: Sun-Receiver-Faktor
  - `2`: DLight-Receiver-Faktor
  - `3`: Sun-Depth-Map-Ansicht (projektiert auf Receiver)
  - `4`: DLight-Cube-Depth-Ansicht (Slot 0, projektiert auf Receiver)

## Fallback / Kompatibilitaet

- Wenn Sun-Shadow-Ressourcen nicht erstellt werden koennen: Sun- und DLight-Shadowing wird deaktiviert.
- Wenn Cube-Array/DLight-Shadow-Ressourcen nicht verfuegbar sind: Sun-Shadowing bleibt aktiv, DLight-Shadowing wird deaktiviert.
- Fallback-Status wird mit klarer Ursache ins Log geschrieben.

## Bekannte V1-Limits

- Kein Alpha-Test-Caster-Support
- Genau eine Sun-Kaskade
- Maximal 4 shadowed DLights
