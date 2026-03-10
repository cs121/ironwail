// Base decal definitions (override in mods/paks with later searchpath priority)

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

decal blood_splat_small {
  texture "decals/blood/splat_01"
  size 5 10
  alpha 0.45 0.75
  color 1 1 1
  lifetime 22
  fade 6
  blend alpha
  random_rotation 1
  priority 1
  category blood
}

decal blood_splat_large {
  texture "decals/blood/splat_02"
  size 10 16
  alpha 0.35 0.65
  color 1 1 1
  lifetime 26
  fade 8
  blend alpha
  random_rotation 1
  priority 1
  category blood
}

decal scorch_small {
  texture "decals/bullet/bhole_01"
  size 12 20
  alpha 0.45 0.70
  color 1 1 1
  lifetime 36
  fade 10
  blend alpha
  random_rotation 1
  priority 3
  category scorch
}

decal scorch_large {
  texture "decals/bullet/bhole_01"
  size 20 32
  alpha 0.35 0.60
  color 1 1 1
  lifetime 40
  fade 12
  blend alpha
  random_rotation 1
  priority 3
  category scorch
}

decal dirt_hit {
  texture "decals/bullet/bhole_01"
  size 8 14
  alpha 0.40 0.65
  color 1 1 1
  lifetime 24
  fade 6
  blend alpha
  random_rotation 1
  priority 2
  category dirt
}
