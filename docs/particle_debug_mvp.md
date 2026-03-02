# Particle MVP Debug-Overlay und Referenzvergleich

## Neue CVars

- `r_particles_debug 0|1|2`
  - `0`: aus.
  - `1`: Overlay mit reproduzierbaren Metriken (`active`, `buckets`, `q3p stats`, `overdraw`).
  - `2`: wie `1`, zusätzlich Legacy-Partikel-Bounds (`mins/maxs`) im Overlay.

## Debug-Overlay (`r_particles_debug`)

Das Overlay zeigt pro Frame:

- `active legacy` und `active q3p`
- q3p-Zähler: `spawned`, `drop`, `cull`
- `buckets` der Legacy-Partikeltypen (Index 0..7 entspricht internem `ptype_t`)
- Overdraw-Indikator (`low|med|high`) plus numerischer `overdraw`-Score
- Optional (`r_particles_debug >= 2`): `mins/maxs` Bounding-Werte der aktiven Legacy-Partikel

## Vergleichsprozedur für Referenzeffekte (gleiche Kamera/Seed)

> Ziel: reproduzierbarer A/B-Vergleich zwischen Referenz-Branch und aktuellem Branch.

1. **Determinismus vorbereiten**
   - Gleiche Map/Startposition laden (z. B. Savegame).
   - Dieselben CVars setzen (insb. Partikelmodus, Auflösung, PostFX).
   - Falls Demo vorhanden: gleiche Demo starten und identische Tickposition nutzen.
2. **Kamera fixieren**
   - `noclip` + definierte `setpos` / `setang` verwenden.
3. **Seed fixieren**
   - Effekt direkt nacheinander in frischem Zustand auslösen (gleicher Ablauf/Skript).
4. **Screenshots aufnehmen**
   - Pro Effekt ein Bild: `TE_EXPLOSION`, `GUNSHOT`, `BLOOD`, `LIGHTNING`, `TELEPORT`.
   - Dateinamen exakt: `TE_EXPLOSION.png`, `GUNSHOT.png`, `BLOOD.png`, `LIGHTNING.png`, `TELEPORT.png`.
5. **Optional Hash-Diff**
   - `python/particle_hash_diff.py <baseline_dir> <candidate_dir> --ext png`
   - Exitcode `0` = alle identisch, `1` = mindestens ein Unterschied/fehlende Datei.

## MVP-Checklist (Akzeptanz)

- [ ] `r_particles_debug 1` zeigt reproduzierbare Metriken bei identischem Setup.
- [ ] `TE_EXPLOSION` Screenshot erstellt und geprüft.
- [ ] `GUNSHOT` Screenshot erstellt und geprüft.
- [ ] `BLOOD` Screenshot erstellt und geprüft.
- [ ] `LIGHTNING` Screenshot erstellt und geprüft.
- [ ] `TELEPORT` Screenshot erstellt und geprüft.
- [ ] Optionaler Hash-Diff ausgeführt und dokumentiert.

## Mod-Kompatibilität

- Overlay ist rein Client-seitig und greift nicht in Netzwerkprotokoll/QC ein.
- Legacy- und q3p-Metriken werden parallel berichtet; bestehende Mods ohne q3p bleiben lauffähig.
- `r_particles_debug` beeinflusst Partikelsimulation nicht direkt, nur HUD-Ausgabe/Statistikaggregation.

## Bekannte MVP-Einschränkungen

- Bounds beziehen sich aktuell auf Legacy-Partikel; q3p-Bounds werden nicht separat visualisiert.
- Overdraw ist ein heuristischer Score, kein GPU-Per-Pixel-Counter.
- Hash-Diff vergleicht Rohbilder binär; geringe nichtdeterministische Renderabweichungen führen zu "diff".
