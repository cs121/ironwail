# Render Backend Readiness Matrix (QA)

## Ziel und Done-Kriterium
Dieses Dokument definiert einen **reproduzierbaren** Ablauf, um Legacy-Renderpfad (`r_backend 0`) und Backend-Renderpfad (`r_backend 1`) pro Szene ohne Interpretationsspielraum zu vergleichen.

**Done-Kriterium:** Ein Reviewer kann mit den unten stehenden Schritten backend `0/1` vergleichen, Mismatch-Logs eindeutig bewerten und pro Szene ein belastbares **PASS/FAIL** vergeben.

---

## 1) Reproduzierbarer Ablauf (Pflicht)

### 1.1 Testvoraussetzungen
1. Gleicher Build-Stand, gleicher GPU-Treiber, gleiche Video-Basisparameter (`vid_*`, Auflösung, VSync, MSAA/AF).
2. Für reproduzierbare Frametimings immer setzen:
   - `host_maxfps 0`
   - `r_vsync 0`
3. Pro Szene immer mit frischem Zustand starten:
   - Engine-Neustart **oder** `disconnect` + `map <szene>`.
4. Framehash-Debug muss aktiv sein:
   - `r_backend_framehash_debug 1`
5. GL-State-Validierung muss aktiv sein:
   - `r_gl_state_validate 1`

### 1.2 Standard-Setup pro Lauf
Diese CVars **vor jedem** Lauf explizit setzen:

```cfg
r_backend <0|1>
r_backend_ui 1
r_backend_postfx 1
r_backend_particles 1
r_backend_alias 1
r_backend_world 1
r_backend_fogvol 1
r_backend_framehash_debug 1
r_backend_framehash_scene <scene_id>
r_backend_framehash_epsilon <0..255>
r_gl_state_validate 1
```

### 1.3 Zwei-Pass-Prozedur je Szene
Für jede Szene exakt diese Reihenfolge verwenden:

1. `r_backend_framehash_scene <scene_id>` setzen.
2. **Pass A (Referenz):** `r_backend 0`, gewünschtes `r_backend_framehash_epsilon`, dann `map <szene>` laden und Framehash-Zeile protokollieren.
3. **Pass B (Vergleich):** identische Konfiguration, nur `r_backend 1`, gleiche Szene erneut laden, Framehash-Zeile protokollieren.
4. Falls Warnung `backend framehash mismatch` erscheint: nach Kapitel „Vergleichsregeln“ entscheiden.

### 1.4 Minimal-Template (copy/paste)

```cfg
host_maxfps 0
r_vsync 0
r_gl_state_validate 1

r_backend_framehash_scene <scene_id>
r_backend_framehash_debug 1
r_backend_framehash_epsilon <eps>

r_backend_ui 1
r_backend_postfx 1
r_backend_particles 1
r_backend_alias 1
r_backend_world 1
r_backend_fogvol 1

r_backend 0
map <mapname>

r_backend 1
map <mapname>
```

---

## 2) Szenenliste (QA-Matrix)

> Konvention: Alle `r_backend_*` Subtoggles stehen auf `1`, sofern in einer Szene nicht anders angegeben.

| Szene-ID (`r_backend_framehash_scene`) | Szene | Abdeckung | `r_backend_framehash_epsilon` | Hinweise |
|---|---|---|---:|---|
| 1 | Startmap Baseline | World + HUD/UI-Basis | 0 | Strikter Hash-Gleichstand erwartet |
| 2 | Open Area / Long View | Viele World-Batches, Sichtweite | 0 | Strikter Hash-Gleichstand erwartet |
| 3 | Partikel-lastig | Alpha/Blend, dichte Partikel | 1 | Kleine Pixel-Abweichungen tolerierbar |
| 4 | Wasser/Warp/Teleport | Warp-/Teleport-Effekte | 1 | Rundungs-/Samplingdifferenzen tolerierbar |
| 5 | Viewmodel + Dlights | Weaponmodel + dynamische Lichter | 0 | Strikter Hash-Gleichstand erwartet |
| 6 | UI/Console Overlay | HUD/Menu/Console-Reihenfolge | 0 | Strikter Hash-Gleichstand erwartet |
| 7 | FogVol/Godray/SSAO | FogVol + PostFX-Pfad | 2 | Nur bei aktiver PostFX-Ausnahme zulässig |

---

## 3) Vergleichsregeln (verbindlich)

### 3.1 Logzeilen, die bewertet werden
- Debug-Ausgabe pro Lauf:
  - `backend framehash: map=<...> scene=<id> backend=<0|1> frame=<n> hash=<...>`
- Mismatch-Ausgabe:
  - `backend framehash mismatch: ... ctx[... eps=<n>]`

### 3.2 Entscheidungslogik PASS/FAIL
Ein Szenenvergleich ist **PASS**, wenn alle Punkte erfüllt sind:
1. **Framehash-Regel**
   - Entweder Hashes backend 0/1 sind identisch.
   - Oder (nur Ausnahmefall): `r_backend_framehash_epsilon > 0` **und** zulässige PostFX-Ausnahme ist aktiv (siehe 3.3) **und** Pixel-Differenz liegt innerhalb Epsilon.
2. **Stabilitätsregel**
   - Keine Leak-Warnings im Log.
   - Keine GL-Validate-Fehler bei `r_gl_state_validate 1`.
3. **Reproduzierbarkeit**
   - Wiederholung derselben Szene mit identischer Konfiguration ergibt dasselbe Urteil.

Sobald einer dieser Punkte verletzt ist, ist das Ergebnis **FAIL**.

### 3.3 Erlaubte PostFX-Epsilon-Ausnahmen (verbindlich)
`r_backend_framehash_epsilon` darf **nur** als Ausnahme gewertet werden, wenn PostFX aktiv ist und mindestens einer der bekannten schwankenden PostFX-Pfade aktiv ist.

| Bedingung | Muss erfüllt sein |
|---|---|
| PostFX global aktiv | `r_postfx > 0` |
| Mindestens eine Ausnahmequelle aktiv | `r_postfx_damage > 0` **oder** `r_postfx_pickup > 0` **oder** `r_postfx_powerup > 0` **oder** `r_postfx_underwater > 0` **oder** `r_motionblur > 0` **oder** `r_dof > 0` **oder** `r_autoexposure > 0` |

Wenn diese Bedingungen **nicht** erfüllt sind, zählt ein Mismatch trotz gesetztem Epsilon als **FAIL**.

### 3.4 Bewertungsregel für Mismatches
- Mismatch mit `eps=0` => **immer FAIL**.
- Mismatch mit `eps>0`, aber ohne gültige PostFX-Ausnahme aus 3.3 => **FAIL**.
- Mismatch mit `eps>0` und gültiger PostFX-Ausnahme => nur dann **PASS**, wenn keine weiteren Regeln verletzt sind und der Fall im QA-Protokoll begründet wird.

---

## 4) QA-Protokollvorlage (pro Szene)

| Feld | Wert |
|---|---|
| Szene-ID / Name | |
| Map / Startpunkt | |
| Lauf A (`r_backend 0`) Hash | |
| Lauf B (`r_backend 1`) Hash | |
| Mismatch-Warnung vorhanden | Ja/Nein |
| `r_backend_framehash_epsilon` | |
| PostFX-Ausnahme aktiv gemäß 3.3 | Ja/Nein |
| Epsilon-Begründung | |
| Leak-Warnings gefunden | Ja/Nein |
| GL-Validate-Fehler gefunden | Ja/Nein |
| Re-Run identisch | Ja/Nein |
| Ergebnis | PASS / FAIL |
| Tester + Datum | |

---

## 5) Abschlussregel für Backend-Readiness

`Render Backend Ready` gilt erst dann, wenn:
1. alle 7 Szenen mit obigem Ablauf mindestens einmal **PASS** sind,
2. kein offener **FAIL** existiert,
3. alle Epsilon-Fälle auf zulässige PostFX-Ausnahmen (3.3) zurückgeführt und dokumentiert sind,
4. in keinem Lauf Leak-Warnings oder GL-Validate-Fehler verbleiben.
