# Model/Entity Lighting Improvement Backlog (Classic Quake-preserving)

Ziel: Beleuchtungsqualität für Items/Waffen/Monster/Moveable Brush-Modelle verbessern, **ohne** das klassische Quake-System (Lightmaps + DLights) zu ersetzen.
Ansatz: opt-in Verbesserungen per CVar, Legacy-Pfad bleibt verfügbar.

## Task 1: Multi-Point Static Lighting Sampling für Alias/Sprite-Entities

**Problem**
Einzelnes Origin-Sample (`R_EntityStaticLight`) führt bei großen oder animierten Modellen zu Helligkeits-Popping.

**Umsetzung**
- In `R_EntityStaticLight` mehrere Samplepunkte pro Entity bilden:
  - Origin
  - +Z (25% und 50% Modellhöhe)
  - optional +/-X/Y Offsets bei großen Bounding-Boxen.
- Pro Punkt Reihenfolge beibehalten:
  1) Lightgrid (falls aktiv)
  2) sonst `R_LightPointNoGrid`
- Gewichtete Mittelung aller gültigen Samples (z. B. `0.5/0.3/0.2`).
- Fallback `r_minlight_models` unverändert lassen.
- CVar: `r_model_light_multisample` (0=legacy, 1=multi-sample).

**Akzeptanzkriterien**
- Legacy-Darstellung bleibt mit `r_model_light_multisample 0` identisch.
- Sichtbar weniger Helligkeitssprünge bei Monstern auf Treppen/Kanten.
- Kein Crash/NaN bei fehlender World oder leerer Lightdata.

---

## Task 2: Zeitliche Glättung für statische Modellbeleuchtung

**Problem**
Bei Bewegung über harte Lichtgrenzen springt der statische Anteil frameweise.

**Umsetzung**
- In `entity_t.lightcache` einen geglätteten statischen Farbwert pflegen.
- Exponentielles Smoothing pro Frame:
  - `smoothed = lerp(current, target, alpha)`
  - `alpha` CVar-gesteuert (z. B. `r_model_light_smooth 0..1`).
- Beim Teleport/Respawn/LERP_RESET harte Rücksetzung auf Zielwert.
- Nur auf statischen Anteil anwenden, DLight-Anteil bleibt reaktiv.

**Akzeptanzkriterien**
- Weniger „Flicker/Popping“ beim Vorbeigehen an Türschwellen.
- Keine sichtbare Nachzieher-Artefakte bei schnellen Bewegungen (bei moderatem alpha).

---

## Task 3: Richtungssensitives DLight-Shading für Modelle (optional)

**Problem**
Aktueller DLight-Anteil auf Modellen kann flach wirken (rein amplitudenbasiert vom Ursprung aus).

**Umsetzung**
- Optionalen Normalfaktor im Alias-Shader hinzufügen:
  - `NdotL` zwischen Welt-Normale und Richtung zum DLight.
  - Soft-Mix statt hartem Lambert (z. B. `mix(1.0, max(NdotL,0), k)`).
- CVar: `r_dlight_models_directional` (0=legacy additiv, 1=gemischt).
- Legacy-Modus bitnah beibehalten.

**Akzeptanzkriterien**
- Bessere Formlesbarkeit bei Monstern/Waffen unter dynamischen Lichtern.
- Kein extremer Helligkeitsverlust bei grazing angles.

---

## Task 4: Robuste Normalmatrix (Inverse-Transpose) für non-uniform Scale

**Problem**
Normalen im Alias-Vertexpfad sind bei nicht-uniformer Skalierung nur näherungsweise korrekt.

**Umsetzung**
- CPU-seitig pro Alias-Instanz echte Normalmatrix berechnen (3x3 inverse-transpose).
- In `InstanceData` an Shader übergeben.
- Vertexshader nutzt Normalmatrix statt normalisierter Achsvektoren.
- Sicherheitsfallback bei nahezu singulärer Matrix.

**Akzeptanzkriterien**
- Konsistente Highlights/Shading bei skalierten Modellen.
- Keine Regression bei unskalierten Standard-Assets.

---

## Task 5: Probe-Assist für Lightgrid-Invalid/Low-Intensity Fälle

**Problem**
Wenn Lightgrid-Probe gültig aber sehr dunkel/instabil ist, kann Modellbeleuchtung unplausibel werden.

**Umsetzung**
- Wenn Lightgrid-Intensität unter Schwellwert liegt:
  - zusätzliche Nachbar-Probes sampeln (kleiner Radius),
  - oder kontrolliert auf LightPoint mischen.
- CVar:
  - `r_model_lightgrid_assist` (0/1)
  - `r_model_lightgrid_assist_threshold`.

**Akzeptanzkriterien**
- Weniger schwarze Ausreißer bei Items in problematischen Grid-Zellen.
- Kein Verlust der klassischen Gesamtcharakteristik.

---

## Task 6: BModel-ReLight Add-On für bewegte Brush-Entities

**Problem**
Türen/Plats tragen gebackene Lightmaps „mit“, obwohl sie den Ort wechseln.

**Umsetzung**
- Additiven Korrekturterm für bewegte bmodels einführen:
  - statischer Korrekturwert aus aktueller Position (LightPoint/Lightgrid),
  - plus vorhandene DLight-Pässe wie bisher.
- Standardmäßig aus (opt-in): `r_bmodel_relight 0/1`.
- Weltspawn/komplette Weltgeometrie unverändert.

**Akzeptanzkriterien**
- Sichtbar plausiblere Türbeleuchtung beim Öffnen in anders beleuchtete Räume.
- Keine Performance-Regression bei deaktivierter CVar.

---

## Task 7: Performance-Guardrails + Budgeting

**Problem**
Mehr Samples und optionale Qualitätsmodi können CPU/GPU-Last erhöhen.

**Umsetzung**
- Zeitmessung für Entity-Lighting-Pfad (pro Frame aggregiert).
- Hard-Limits:
  - max Entity-Samples/Frame,
  - degrade-to-legacy bei Budgetüberschreitung.
- Debug-CVar: `r_model_light_stats` für Timing/Counter-Ausgabe.

**Akzeptanzkriterien**
- Messbare Obergrenzen und reproduzierbares Verhalten bei Stress-Szenen.
- Keine unkontrollierten Framerate-Einbrüche.

---

## Task 8: Lighting-Debug Overlay/Logging vereinheitlichen

**Problem**
Aktuelle Debug-Infos sind nützlich, aber verteilt und schwer vergleichbar.

**Umsetzung**
- Einheitliches Debug-Format für Alias/Sprite/BModel:
  - static source (grid/point/minlight),
  - dynamic dlight contribution,
  - final color.
- Optionales on-screen Overlay (Entity-ID, final RGB, sample source).
- Spam-Schutz: pro Entity throttlen.

**Akzeptanzkriterien**
- Schnellere Ursachenanalyse bei zu dunklen/zu hellen Modellen.
- Debug-Ausgaben bleiben lesbar in großen Szenen.

---

## Task 9: Content-seitige Richtlinien dokumentieren

**Problem**
Viele Lighting-Probleme entstehen durch uneinheitliche Map/Asset-Konventionen.

**Umsetzung**
- Neue Doku-Sektion mit Best Practices:
  - sinnvolle Lightgrid-Dichte,
  - Umgang mit sehr kleinen/großen Modell-Bounding-Boxes,
  - Fullbright/Emissive-Texturen in Kombination mit Modelllicht.
- „Legacy-safe defaults“ und empfohlene CVar-Profile.

**Akzeptanzkriterien**
- Mapper/Modder können reproduzierbar zu konsistenten Ergebnissen kommen.
- Weniger projektspezifische Sonderfälle im Code.
