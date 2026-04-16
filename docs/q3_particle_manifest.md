# Q3 Particle Manifest

This file is the manifest-style reference for Ironwail q3-style particle support.
It tracks the common effect descriptors used by the particle systems.

## Scope
- Effect families for legacy and q3p particle paths
- Mapping-oriented contract, not a network protocol spec

## Main families
- `svc_particle.explosion`
- `svc_particle.spray`
- `trail.rocket`
- `trail.smoke`
- `trail.blood_heavy`
- `trail.blood_light`
- `trail.voor`

## Common descriptor keys
- `material`
- `lifetime`
- `size`
- `size_ramp`
- `alpha`
- `alpha_ramp`
- `gravity`
- `drag`
- `jitter`
- `collision`

## Rules
- q3p should prefer the effect descriptor when available.
- If no override exists, the legacy fallback stays active.
- Unsupported stage features must be handled deterministically.

## Example
```text
trail.rocket -> material smoke
trail.blood_light -> material blood_light
svc_particle.explosion -> material explosion
```
