// Phantasy-themed decal defaults backed by stock Quake Remaster textures.
// These names are safe for default remastered game VM content.

decal phantasy_explosion_mark {
  texture "decals/scorch/scorch_02"
  size 18 30
  alpha 0.40 0.65
  color 1 1 1
  lifetime 40
  fade 12
  blend alpha
  random_rotation 1
  priority 3
  category scorch
}

decal phantasy_bullet_hole {
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

decal phantasy_blood_mark {
  texture "decals/blood/splat_02"
  size 9 15
  alpha 0.40 0.70
  color 1 1 1
  lifetime 26
  fade 8
  blend alpha
  random_rotation 1
  priority 1
  category blood
}

decal phantasy_energy_mark {
  texture "decals/scorch/scorch_01"
  size 10 18
  alpha 0.35 0.55
  color 0.70 0.90 1.00
  lifetime 30
  fade 10
  blend alpha
  random_rotation 1
  priority 2
  category scorch
}
