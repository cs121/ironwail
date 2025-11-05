# Particle Effects Overview

Ironwail now supports DarkPlaces-style particle definitions through `Misc/pak/effectinfo.txt`.
This document summarizes how to work with the system from both the asset and QuakeC sides.

## effectinfo.txt

* Each `effect` block defines a named particle preset.
* Supported keys (all optional unless noted):
  * `type` (`smoke`, `spark`, `beam`, `generic`) – influences default texture selection.
  * `count` – number of particles spawned per call.
  * `color r g b` – RGB color in 0–255 that is converted to linear 0–1.
  * `alpha`, `alpha2` – starting and ending alpha.
  * `fade` – exponent that shapes the alpha curve.
  * `size min max` – starting particle size range in Quake units.
  * `sizeincrease min max` – per-second growth (positive) or shrink (negative).
  * `velocity min max` – speed range in units per second.
  * `velocityjitter` – random offset applied per axis.
  * `gravity` – acceleration applied downward (negative values float upwards).
  * `airfriction` – exponential damping factor.
  * `lifetime` – seconds a particle lives.
  * `spawnradius` – random point jitter around the origin.
  * `glow` – bloom intensity multiplier.
  * `texture` (`soft`, `glow`, `smoke`, `streak`) – overrides the automatic texture.
* On startup the engine reports the number of effects loaded:
  * `Loaded N particle effects from effectinfo.txt`
  * Fallback messages explain when the file is missing or empty.

Changes to `effectinfo.txt` are picked up on the next launch.

## QuakeC usage

New builtins (extension `DP_QC_POINTPARTICLES`) expose the particle presets to progs:

```c
float particleeffectnum(string name);
void pointparticles(float effect, vector org, vector dir, float count);
```

Typical pattern:

```c
float fx = particleeffectnum("fx_spark_min");
if (fx >= 0)
    pointparticles(fx, impactorg, dir, 1);
```

The helper module `qc/particles.qc` demonstrates higher-level wrappers that
fall back to classic temp entities when the extension is unavailable.

## Legacy fallbacks

If `effectinfo.txt` is missing or an effect name cannot be resolved, the engine
reverts to original Quake particle behaviors so existing mods keep working.
