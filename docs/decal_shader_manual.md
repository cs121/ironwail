# Decal Shader Manual (Ironwail)

Dieses Dokument beschreibt das aktuelle Decal-System in Ironwail praxisnah und
codegenau: Erzeugung, Shader-Syntax, Keywords, "Manifest"-Modell, Debugging.

## 1) Ueberblick

Zweck:
- Decals sind projektierte Treffer-/Spuren-Polygone (z. B. bullet holes, scorch
  marks), die zur Laufzeit auf World-Surfaces gelegt werden.

Begriffe:
- `decal definition`: Ein `decal <name> { ... }` Block aus `decals.shader`.
- `category`: Logische Gruppe (z. B. `bullet`, `scorch`) fuer Spawn-Aufloesung.
- `instance`: Konkretes Laufzeit-Decal mit Geometrie, Lifetime, Blend/Texture.
- `pool`: Fester Instanz-Pool mit Eviction statt unkontrollierter Allokation.

## 2) Wie Decals erzeugt werden (Runtime-Pipeline)

### 2.1 Event -> Category

Aktuell spawnt die Engine Decals bei Temp-Entity Impacts in `cl_tent.c`:
- `TE_SPIKE`, `TE_SUPERSPIKE`, `TE_GUNSHOT` -> `R_SpawnImpactDecal("bullet", ...)`
- `TE_EXPLOSION`, `TE_TAREXPLOSION`, `TE_EXPLOSION2` -> `R_SpawnImpactDecal("scorch", ...)`

### 2.2 Category -> Definition

`R_FindDecalDefByCategory`:
- sammelt alle **validen** Decal-Definitionen mit passender `category`
- waehlt zufaellig eine Definition aus den Treffern

Konsequenz:
- mehrere `decal`-Bloecke mit gleicher `category` sind erlaubt und geben Variation.

### 2.3 Spawn-Pipeline (in Reihenfolge)

`R_SpawnImpactDecal(category, origin, normal)`:
1. Basischecks (`r_decals`, `worldmodel`, `category`, `normal`).
2. Definition finden (`category` -> zufaellige passende Def).
3. Radius/Alpha/Rotation randomisieren (aus Def-Intervallen).
4. Impact-Leaf finden (inkl. kleiner Offsets um quantisierte Impacts robust zu
   behandeln).
5. Candidate-Surfaces im Leaf pruefen (Flags, Plane-Abstand, Gueltigkeit).
6. Impact pro Surface auf Plane projizieren und Polygon clippen/projizieren.
7. Instanz im Pool allozieren (free-list/ring-style, priority-aware eviction).
8. Lifetime/Fade/Bounds setzen, dann in den aktiven Pool uebernehmen.

### 2.4 Warum ein Decal abgelehnt wird

Wichtige `reason=` Werte aus dem Debug-Log:
- `decals_disabled`
- `worldmodel_missing`
- `category_missing`
- `decaldef_missing`
- `normal_invalid`
- `impact_leaf_missing`
- `vertex_pool_exhausted`
- `projection_empty`
- `instance_pool_exhausted`

`reason=ok` bedeutet erfolgreich erzeugt.

## 3) Shader-Datei / Syntax

### 3.1 Grundschema

```shader
decal my_decal_name {
  texture "decals/my/path"
  size 5 8
  alpha 0.6 0.9
  color 1 1 1
  lifetime 32
  fade 8
  blend alpha
  random_rotation 1
  priority 4
  category bullet
  atlas_rect 0.0 0.0 0.5 0.5
}
```

Hinweise:
- Pro Block ist `decal <name>` die Definition-ID.
- Ohne `texture` oder `category` wird die Def nicht als valide nutzbar.

### 3.2 Keyword-Referenz

| Keyword | Typ | Default | Validierung / Clamp | Wirkung |
|---|---|---|---|---|
| `texture` | string | leer | muss auf ein ladbares Bild zeigen | Quelle fuer Decal-Textur |
| `size` | float min max | `8 8` | `size_max < size_min` -> `size_max=size_min`; `size_min<=0` -> `1`; `size_max<=0` -> `size_min` | Radius-Intervall |
| `alpha` | float min max | `1 1` | falls `max<min` -> `max=min`; finale Vertex-Alpha wird auf `[0..1]` geklemmt | Alpha-Intervall |
| `color` | float r g b | `1 1 1` | finale Vertex-Farbe auf `[0..1]` geklemmt | Farbton |
| `lifetime` | float | `15` | `<=0` -> `10` | Lebensdauer in Sekunden |
| `fade` | float | `5` | `<0` -> `0`; `>lifetime` -> `lifetime` | Fade-Out Fenster am Lebensende |
| `blend` | enum | `alpha` | `add`, `mul`, sonst `alpha` | Blend-Mode |
| `random_rotation` | int/bool | `0` | `atoi(token)!=0` | zufaellige Rotation pro Instanz |
| `priority` | int | `0` | int | Wichtigkeit bei Eviction (hoeher = schwerer zu ersetzen) |
| `category` | string | leer | fuer Spawn-Aufloesung erforderlich | Spawn-Gruppe (`bullet`, `scorch`, ...) |
| `atlas_rect` | 4 floats u0 v0 u1 v1 | `0 0 1 1` | alles auf `[0..1]`; `u1<=u0`/`v1<=v0` werden minimal korrigiert | UV-Subrect in Atlas |
| `uvrect` | alias | siehe `atlas_rect` | identisch | Alias fuer `atlas_rect` |

### 3.3 Unbekannte Keywords

Unbekannte Tokens im `decal`-Block werden derzeit toleriert/ignoriert (kein harter
Parse-Abbruch des gesamten Scripts).

## 4) "Manifest"-Modell (explizit)

Es gibt **kein separates Manifest-File**.

Der effektive Manifest-Inhalt ist:
- die geladene Menge aller `decal`-Bloecke in `decal_defs[]`
- inkl. ihrer Kategorien, Texturen und Parameter

Lade-Reihenfolge in der Runtime:
1. `decals.shader`
2. `scripts/decals.shader`

Praktische Konsequenzen:
- Mehrere Definitionen mit gleicher `category` koennen gleichzeitig existieren.
- Die Spawn-Aufloesung waehlt zufaellig aus allen passenden validen Defs.
- Welche physische Datei geladen wird, folgt den ueblichen Quake-Suchpfad-/Pak-
  Prioritaeten (`COM_LoadMallocFile`).

## 5) Atlas, Batching, Performance-Basics

### 5.1 Atlas-Support

Mit `atlas_rect`/`uvrect` kann eine Definition nur einen UV-Teilbereich derselben
Textur nutzen. Damit koennen mehrere Decals dieselbe Atlas-Textur teilen.

Beispiel:
```shader
decal bullet_hole_a {
  texture "decals/atlas/impact_atlas"
  atlas_rect 0.0 0.0 0.25 0.25
  category bullet
}
```

### 5.2 Pool und Eviction

- Instanzen liegen in einem festen Pool (`MAX_DECAL_INSTANCES`).
- Effektives Laufzeitlimit: `r_decals_max` (geklemmt auf Poolgroesse).
- Wenn voll: priority-aware Eviction (niedrigere Prioritaet/alte Instanzen zuerst).

### 5.3 Sichtbarkeits- und Draw-Pfad

Vor dem Rendern:
- aktive Instanzen -> sichtbare Menge (Frustum, Distanz, small-on-screen reject)
- Sortierung/Batches nach `blend` und `texture`
- kompakter Upload in dynamische GPU-Buffer pro Frame
- moeglichst wenige Draw Calls/State Changes je Batch

## 6) Debugging & Troubleshooting

### 6.1 CVars

- `r_decals`:
  - `0` aus
  - `1` an
- `r_decals_max`:
  - max. aktive Decal-Instanzen
- `r_decals_debug`:
  - `0`: aus
  - `1`: Spawn/Reject Logs mit `reason=...`
  - `2`: zusaetzlich periodische Frame-Stats

### 6.2 Beispiel-Logs

Spawn/Reject:
```text
decal rejected reason=projection_empty cat=bullet def=bullet_hole_default ...
decal spawned reason=ok cat=bullet def=bullet_hole_metal ...
```

Frame-Stats (Debug 2):
```text
decal stats active=120 visible=46 culled=74(frustum=33 dist=28 small=13) draws=6 upload=48256B
```

### 6.3 "Wenn X, dann Y" Kurzcheck

- `decaldef_missing`: `category` existiert in Script nicht oder Def ist nicht valid.
- `impact_leaf_missing`: Impact liegt nicht sinnvoll in/nahe einem Leaf mit Marksurfaces.
- `projection_empty`: Surface-Kandidaten finden nichts Projektierbares (Geometrie/Normal/Radius).
- `instance_pool_exhausted`: `r_decals_max` zu klein oder Prioritaet verhindert Ersatz.
- viele `culled_small`: Decals sind zu weit weg/zu klein fuer sinnvolle Darstellung.

## 7) Copy/Paste-Beispiele

### 7.1 Minimal funktionierende Definition

```shader
decal bullet_hole_basic {
  texture "decals/bullet/bhole_01"
  category bullet
}
```

### 7.2 Praxisbeispiel mit Parametern

```shader
decal bullet_hole_default {
  texture "decals/bullet/bhole_01"
  size 5 8
  alpha 0.65 0.90
  color 1 1 1
  lifetime 32
  fade 8
  blend alpha
  random_rotation 1
  priority 4
  category bullet
}
```

### 7.3 Atlas-Beispiel

```shader
decal scorch_atlas_a {
  texture "decals/atlas/scorch_atlas"
  atlas_rect 0.5 0.0 1.0 0.5
  size 14 22
  alpha 0.4 0.7
  lifetime 30
  fade 8
  blend alpha
  random_rotation 1
  priority 3
  category scorch
}
```

