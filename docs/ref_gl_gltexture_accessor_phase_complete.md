# gltexture_t Accessor-First Phase Abschluss

## Ziel
Schrittweise Vorbereitung auf `gltexture_t`-Opacity ohne ABI-Bruch, ohne Legacy-Path-Entfernung und ohne Verhaltensänderung.

## Umgesetzte Änderungen
- Neue Accessors in `gl_texmgr`:
  - `TexMgr_GetNativeHandle(const gltexture_t *glt)`
  - `TexMgr_GetTarget(const gltexture_t *glt)`
  - `TexMgr_GetBindlessHandle(const gltexture_t *glt)`
  - `TexMgr_GetInternalFormat(const gltexture_t *glt)`
- Read-Sites umgestellt:
  - `r_sprite.c`: Descriptor `resource_id` nutzt `TexMgr_GetNativeHandle`.
  - `r_world.c`: bindless-Reads nutzen `TexMgr_GetBindlessHandle`.
  - `gl_ktx2.c`: `glBindTexture(..., TexMgr_GetNativeHandle(tex))`.

## Bewusst nicht geändert
- Keine Schreibzugriffe auf `gltexture_t`-Felder ersetzt.
- Keine Struct-Felder entfernt/umbenannt.
- Keine globale API/ABI geändert.
- Kein Entfernen des internen Legacy-GL-Pfads.

## Restbestand direkter Feldzugriffe
- Größtenteils in `gl_texmgr.c` selbst (Owner der Struktur) und damit akzeptiert.
- Übergangsweise weiterhin in GL-privaten Pfaden, wo direkten Zugriff für Upload/Lifecycle gebraucht wird.

## Bewertung
- Accessor-Basis steht und wird genutzt.
- Erste transitional Verbraucher (`r_world`, `r_sprite`) sind entkoppelt.
- Source bleibt logisch kompilierbar (ohne Build-Run validiert).

## Nächste sichere Schritte
1. Weitere reine Read-Zugriffe in transitional Render-Units auf Accessors umstellen (falls vorhanden).
2. Danach Write-Authority explizit dokumentieren (`gl_texmgr.c` only).
3. Erst dann partielle Opaque-Migration (Feldsichtbarkeit reduzieren).
