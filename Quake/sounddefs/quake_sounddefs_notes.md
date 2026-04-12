# Quake SoundDefs – erzeugte Dateien

## Dateien
- `quake_sounddefs_generated.sndshd`
- `quake_monster_templates.sndshd`

## Prinzipien
- Nur Pfade verwendet, die in der bereitgestellten Soundliste ausdrücklich genannt wurden.
- Mehrere nahe Varianten wurden zu logischen Defs zusammengefasst, z. B.:
  - `quake/player/death`
  - `quake/weapons/ricochet`
  - `quake/ui/menu_move`
- Ambient-Loops laufen über `bus ambient` mit `loop true`.
- UI-Sounds sind `spatialize false` und `bus ui`.
- Bewegliche Weltobjekte wie Türen/Plattformen nutzen oft `doppler true` und moderaten `reverb_send`.

## Hinweise
- Die Liste enthielt `doors/drclos4.wav` doppelt. Es wurde nur einmal verwendet.
- Für viele Monsterordner waren keine konkreten Dateinamen angegeben. Dafür gibt es eine Template-Datei.
- Alle Definitionen halten sich an die Manifest-Regeln: pro `sound`-Block nur impliziter Layer, keine Mischung mit `layer {}`.

## Beispiel-Konsole
- `snd_reload_defs`
- `snd_list_defs`
- `snd_print_def quake/weapons/shotgun_fire`
- `snd_play_def quake/ui/menu_move`
- `snd_play_def quake/ambient/fire 256 128 0`
