// Weapons.wren
// Port of the weapon management routines from weapons.qc. Critical gameplay
// behavior is implemented here, while the more involved projectile and trace
// logic is stubbed for future work.

import "./Engine" for Engine
import "./Globals" for Items, Channels, Attenuations, PlayerExtraFlags
import "./Globals" for SolidTypes, MoveTypes, DamageValues, Contents, TempEntityCodes
import "./Combat" for CombatModule
import "./Subs" for SubsModule

var _WEAPON_SOUNDS = [
  "weapons/r_exp3.wav",
  "weapons/rocket1i.wav",
  "weapons/sgun1.wav",
  "weapons/guncock.wav",
  "weapons/ric1.wav",
  "weapons/ric2.wav",
  "weapons/ric3.wav",
  "weapons/spike2.wav",
  "weapons/tink1.wav",
  "weapons/grenade.wav",
  "weapons/bounce.wav",
  "weapons/shotgn2.wav"
]

var _AMMO_BITS = null
var _multiDamageTarget = null
var _multiDamageAmount = 0.0
var _multiDamageInflictor = null
var _multiDamageAttacker = null

class WeaponsModule {
  static _vectorAdd(a, b) {
    return [a[0] + b[0], a[1] + b[1], a[2] + b[2]]
  }

  static _vectorSub(a, b) {
    return [a[0] - b[0], a[1] - b[1], a[2] - b[2]]
  }

  static _vectorScale(v, scalar) {
    return [v[0] * scalar, v[1] * scalar, v[2] * scalar]
  }

  static _vectorLength(v) {
    return (v[0] * v[0] + v[1] * v[1] + v[2] * v[2]).sqrt
  }

  static _vectorNormalize(v) {
    var length = WeaponsModule._vectorLength(v)
    if (length == 0) return [0, 0, 0]
    return WeaponsModule._vectorScale(v, 1 / length)
  }

  static _vectorDot(a, b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]
  }

  static _vectorCross(a, b) {
    return [
      a[1] * b[2] - a[2] * b[1],
      a[2] * b[0] - a[0] * b[2],
      a[0] * b[1] - a[1] * b[0]
    ]
  }

  static _cRandom() {
    return Engine.random() * 2 - 1
  }

  static crandom() {
    return WeaponsModule._cRandom()
  }

  static _defaultVectors() {
    return {
      "forward": [1, 0, 0],
      "right": [0, 1, 0],
      "up": [0, 0, 1]
    }
  }

  static _makeVectors(angles) {
    var vectors = Engine.makeVectors(angles)
    if (vectors == null) return WeaponsModule._defaultVectors()
    if (!vectors.containsKey("forward")) vectors["forward"] = [1, 0, 0]
    if (!vectors.containsKey("right")) vectors["right"] = [0, 1, 0]
    if (!vectors.containsKey("up")) vectors["up"] = [0, 0, 1]
    return vectors
  }

  static _eyeOrigin(entity) {
    return WeaponsModule._vectorAdd(entity.get("origin", [0, 0, 0]), [0, 0, 16])
  }

  static _absMinZ(entity) {
    var absmin = entity.get("absmin", null)
    if (absmin != null) return absmin[2]
    var origin = entity.get("origin", [0, 0, 0])
    var mins = entity.get("mins", [0, 0, 0])
    return origin[2] + mins[2]
  }

  static _sizeZ(entity) {
    var size = entity.get("size", null)
    if (size != null) return size[2]
    var mins = entity.get("mins", [0, 0, 0])
    var maxs = entity.get("maxs", [0, 0, 0])
    return maxs[2] - mins[2]
  }

  static _spawnParticles(origin, velocity, color, count) {
    Engine.spawnParticles(origin, velocity, color, count)
  }

  static spawnBlood(origin, velocity, damage) {
    WeaponsModule._spawnParticles(origin, velocity, 73, (damage * 2).ceil)
  }

  static _spawnBlood(origin, velocity, damage) {
    WeaponsModule.spawnBlood(origin, velocity, damage)
  }

  static spawnChunk(origin, velocity) {
    WeaponsModule._spawnParticles(origin, WeaponsModule._vectorScale(velocity, 0.02), 0, 10)
  }

  static _emitGunshot(origin) {
    Engine.emitTempEntity(TempEntityCodes.GUNSHOT, {"origin": origin})
  }

  static _emitSpike(origin, isSuper) {
    var code = isSuper ? TempEntityCodes.SUPERSPIKE : TempEntityCodes.SPIKE
    Engine.emitTempEntity(code, {"origin": origin})
  }

  static _emitExplosion(origin) {
    Engine.emitTempEntity(TempEntityCodes.EXPLOSION, {"origin": origin})
  }

  static _emitLightning(owner, start, end) {
    Engine.emitTempEntity(TempEntityCodes.LIGHTNING2, {
      "owner": owner,
      "start": start,
      "end": end
    })
  }

  static _newDamageMap() {
    return {}
  }

  static _addDamage(damageMap, entity, amount) {
    if (entity == null) return
    if (!damageMap.containsKey(entity)) {
      damageMap[entity] = 0.0
    }
    damageMap[entity] = damageMap[entity] + amount
  }

  static _applyDamage(globals, inflictor, attacker, damageMap) {
    for (entity in damageMap.keys) {
      var amount = damageMap[entity]
      CombatModule.tDamage(globals, entity, inflictor, attacker, amount)
    }
  }

  static clearMultiDamage() {
    _multiDamageTarget = null
    _multiDamageAmount = 0.0
    _multiDamageInflictor = null
    _multiDamageAttacker = null
  }

  static addMultiDamage(globals, target, damage, inflictor, attacker) {
    if (target == null) return
    var inf = inflictor
    var atk = attacker
    if (inf == null && globals != null) inf = globals.self
    if (atk == null && globals != null) atk = globals.self

    if (_multiDamageTarget != null && _multiDamageTarget != target) {
      WeaponsModule.applyMultiDamage(globals, inf, atk)
    }

    if (_multiDamageTarget != target) {
      _multiDamageTarget = target
      _multiDamageAmount = 0.0
      _multiDamageInflictor = inf
      _multiDamageAttacker = atk
    }

    _multiDamageAmount = _multiDamageAmount + (damage == null ? 0.0 : damage)
  }

  static applyMultiDamage(globals, inflictor, attacker) {
    if (_multiDamageTarget == null) return

    var inf = inflictor
    var atk = attacker
    if (inf == null) inf = _multiDamageInflictor
    if (atk == null) atk = _multiDamageAttacker
    if (inf == null && globals != null) inf = globals.self
    if (atk == null && globals != null) atk = globals.self

    CombatModule.tDamage(globals, _multiDamageTarget, inf, atk, _multiDamageAmount)
    WeaponsModule.clearMultiDamage()
  }

  static ClearMultiDamage() {
    WeaponsModule.clearMultiDamage()
  }

  static AddMultiDamage(globals, target, damage, inflictor, attacker) {
    WeaponsModule.addMultiDamage(globals, target, damage, inflictor, attacker)
  }

  static ApplyMultiDamage(globals, inflictor, attacker) {
    WeaponsModule.applyMultiDamage(globals, inflictor, attacker)
  }

  static _perpendicularBasis(direction) {
    var forward = WeaponsModule._vectorNormalize(direction)
    if (WeaponsModule._vectorLength(forward) == 0) {
      forward = [0, 0, 1]
    }

    var temp = [0, 0, 1]
    if (WeaponsModule._vectorLength(WeaponsModule._vectorCross(forward, temp)) == 0) {
      temp = [0, 1, 0]
    }

    var right = WeaponsModule._vectorNormalize(WeaponsModule._vectorCross(forward, temp))
    var up = WeaponsModule._vectorNormalize(WeaponsModule._vectorCross(right, forward))
    return {"forward": forward, "right": right, "up": up}
  }

  static _wallVelocity(entity, planeNormal) {
    var velocity = entity.get("velocity", [0, 0, 0])
    var basis = WeaponsModule._perpendicularBasis(velocity)
    var dir = WeaponsModule._vectorAdd(
      basis["forward"],
      WeaponsModule._vectorAdd(
        WeaponsModule._vectorScale(basis["up"], WeaponsModule._cRandom()),
        WeaponsModule._vectorScale(basis["right"], WeaponsModule._cRandom())
      )
    )
    dir = WeaponsModule._vectorNormalize(dir)

    if (planeNormal != null) {
      dir = WeaponsModule._vectorAdd(dir, WeaponsModule._vectorScale(planeNormal, 2))
    }

    return WeaponsModule._vectorScale(dir, 200)
  }

  static wall_velocity(entity, planeNormal) {
    return WeaponsModule._wallVelocity(entity, planeNormal)
  }

  static spawnTouchBlood(entity, damage) {
    if (entity == null) return
    var origin = entity.get("origin", [0, 0, 0])
    var velocity = entity.get("velocity", [0, 0, 0])
    var end = WeaponsModule._vectorAdd(origin, WeaponsModule._vectorScale(velocity, 0.05))
    var trace = Engine.traceLine(origin, end, false, entity)
    var plane = trace != null && trace.containsKey("planeNormal") ? trace["planeNormal"] : null

    var impactVelocity = WeaponsModule._vectorScale(WeaponsModule._wallVelocity(entity, plane), 0.2)
    var impactOrigin = WeaponsModule._vectorAdd(origin, WeaponsModule._vectorScale(impactVelocity, 0.01))
    WeaponsModule.spawnBlood(impactOrigin, impactVelocity, damage)
  }

  static spawn_touchblood(entity, damage) {
    WeaponsModule.spawnTouchBlood(entity, damage)
  }

  static spawnMeatSpray(globals, owner, origin, velocity) {
    if (origin == null) return

    var spray = Engine.spawnEntity()
    spray.set("owner", owner)
    spray.set("movetype", MoveTypes.BOUNCE)
    spray.set("solid", SolidTypes.NOT)
    spray.set("classname", "meat")

    var vel = velocity == null ? [0, 0, 0] : velocity
    var adjusted = [vel[0], vel[1], vel[2] + 250 + 50 * Engine.random()]
    spray.set("velocity", adjusted)
    spray.set("avelocity", [3000, 1000, 2000])

    spray.set("think", "SubsModule.subRemove")
    var removeTime = Engine.time() + 1
    spray.set("nextthink", removeTime)
    Engine.scheduleThink(spray, "SubsModule.subRemove", 1)

    Engine.setModel(spray, "progs/zom_gib.mdl")
    Engine.setSize(spray, [0, 0, 0], [0, 0, 0])
    Engine.setOrigin(spray, origin)
  }

  static _aimDirection(player, distance) {
    var dir = Engine.aim(player, distance)
    if (dir == null) {
      var vectors = WeaponsModule._makeVectors(player.get("v_angle", [0, 0, 0]))
      dir = vectors["forward"]
    }
    return WeaponsModule._vectorNormalize(dir)
  }

  static _traceAttack(globals, shooter, direction, trace, right, up, damageMap, damage) {
    var randomRight = WeaponsModule._vectorScale(right, WeaponsModule._cRandom())
    var randomUp = WeaponsModule._vectorScale(up, WeaponsModule._cRandom())
    var vel = WeaponsModule._vectorAdd(direction, WeaponsModule._vectorAdd(randomUp, randomRight))
    vel = WeaponsModule._vectorNormalize(vel)
    var plane = trace.containsKey("planeNormal") ? trace["planeNormal"] : [0, 0, 0]
    vel = WeaponsModule._vectorAdd(vel, WeaponsModule._vectorScale(plane, 2))
    vel = WeaponsModule._vectorScale(vel, 200)

    var endpos = trace.containsKey("endpos") ? trace["endpos"] : WeaponsModule._vectorAdd(shooter.get("origin", [0, 0, 0]), WeaponsModule._vectorScale(direction, 64))
    var impact = WeaponsModule._vectorSub(endpos, WeaponsModule._vectorScale(direction, 4))
    var hit = trace.containsKey("entity") ? trace["entity"] : null

    if (hit != null && hit.get("takedamage", DamageValues.NO) != DamageValues.NO) {
      WeaponsModule._spawnBlood(impact, WeaponsModule._vectorScale(vel, 0.2), damage)
      WeaponsModule._addDamage(damageMap, hit, damage)
    } else {
      WeaponsModule._emitGunshot(impact)
    }
  }

  static TraceAttack(globals, shooter, direction, trace, right, up, damageMap, damage) {
    WeaponsModule._traceAttack(globals, shooter, direction, trace, right, up, damageMap, damage)
  }

  static _fireBullets(globals, shooter, shotCount, dir, spread) {
    var vectors = WeaponsModule._makeVectors(shooter.get("v_angle", [0, 0, 0]))
    var forward = vectors["forward"]
    var right = vectors["right"]
    var up = vectors["up"]

    var src = WeaponsModule._vectorAdd(shooter.get("origin", [0, 0, 0]), WeaponsModule._vectorScale(forward, 10))
    var baseZ = WeaponsModule._absMinZ(shooter) + WeaponsModule._sizeZ(shooter) * 0.7
    src[2] = baseZ

    var damageMap = WeaponsModule._newDamageMap()

    while (shotCount > 0) {
      var offset = WeaponsModule._vectorAdd(
        WeaponsModule._vectorScale(right, WeaponsModule._cRandom() * spread[0]),
        WeaponsModule._vectorScale(up, WeaponsModule._cRandom() * spread[1])
      )
      var direction = WeaponsModule._vectorAdd(dir, offset)
      direction = WeaponsModule._vectorNormalize(direction)
      var end = WeaponsModule._vectorAdd(src, WeaponsModule._vectorScale(direction, 2048))
      var trace = Engine.traceLine(src, end, false, shooter)
      if (trace != null && trace.containsKey("fraction") && trace["fraction"] < 1) {
        WeaponsModule._traceAttack(globals, shooter, direction, trace, right, up, damageMap, 4)
      }
      shotCount = shotCount - 1
    }

    WeaponsModule._applyDamage(globals, shooter, shooter, damageMap)
  }

  static fireBullets(globals, shooter, shotCount, dir, spread) {
    WeaponsModule._fireBullets(globals, shooter, shotCount, dir, spread)
  }

  static _fireRegularSpikes(globals, player, offset) {
    Engine.playSound(player, Channels.WEAPON, "weapons/rocket1i.wav", 1, Attenuations.NORMAL)
    player.set("attack_finished", Engine.time() + 0.2)
    var ammo = player.get("ammo_nails", 0) - 1
    player.set("ammo_nails", ammo)
    player.set("currentammo", ammo)

    var vectors = WeaponsModule._makeVectors(player.get("v_angle", [0, 0, 0]))
    var origin = WeaponsModule._vectorAdd(WeaponsModule._eyeOrigin(player), WeaponsModule._vectorScale(vectors["right"], offset))
    var dir = WeaponsModule._aimDirection(player, 1000)
    WeaponsModule._launchSpike(globals, player, origin, dir, false)
    player.set("punchangle", [-2, 0, 0])
  }

  static fireRegularSpikes(globals, player, offset) {
    WeaponsModule._fireRegularSpikes(globals, player, offset)
  }

  static _fireSuperSpikes(globals, player) {
    Engine.playSound(player, Channels.WEAPON, "weapons/spike2.wav", 1, Attenuations.NORMAL)
    player.set("attack_finished", Engine.time() + 0.2)
    var ammo = player.get("ammo_nails", 0) - 2
    player.set("ammo_nails", ammo)
    player.set("currentammo", ammo)

    var dir = WeaponsModule._aimDirection(player, 1000)
    var origin = WeaponsModule._eyeOrigin(player)
    WeaponsModule._launchSpike(globals, player, origin, dir, true)
    player.set("punchangle", [-2, 0, 0])
  }

  static fireSuperSpikes(globals, player) {
    WeaponsModule._fireSuperSpikes(globals, player)
  }

  static _fireSpikes(globals, player) {
    var weapon = player.get("weapon", Items.NAILGUN)
    var ammo = player.get("ammo_nails", 0)

    if (weapon == Items.SUPER_NAILGUN) {
      if (ammo >= 2) {
        WeaponsModule._fireSuperSpikes(globals, player)
        return
      }
      player.set("weapon", WeaponsModule.bestWeapon(globals, player))
      WeaponsModule.setCurrentAmmo(globals, player)
      return
    }

    if (ammo < 1) {
      player.set("weapon", WeaponsModule.bestWeapon(globals, player))
      WeaponsModule.setCurrentAmmo(globals, player)
      return
    }

    var cycle = player.get("nail_cycle", 0)
    var offset = cycle == 0 ? 4 : -4
    player.set("nail_cycle", cycle == 0 ? 1 : 0)
    WeaponsModule._fireRegularSpikes(globals, player, offset)
  }

  static _launchSpike(globals, owner, origin, dir, isSuper) {
    var missile = Engine.spawnEntity()
    missile.set("classname", isSuper ? "super_spike" : "spike")
    missile.set("owner", owner)
    missile.set("movetype", MoveTypes.FLYMISSILE)
    missile.set("solid", SolidTypes.BBOX)

    var velocity = WeaponsModule._vectorScale(dir, 1000)
    missile.set("velocity", velocity)
    missile.set("angles", Engine.vectorToAngles(velocity))

    var touch = isSuper ? "WeaponsModule.superSpikeTouch" : "WeaponsModule.spikeTouch"
    missile.set("touch", touch)

    missile.set("think", "SubsModule.subRemove")
    var removeDelay = 6.0
    missile.set("nextthink", Engine.time() + removeDelay)
    Engine.scheduleThink(missile, "SubsModule.subRemove", removeDelay)

    Engine.setModel(missile, isSuper ? "progs/s_spike.mdl" : "progs/spike.mdl")
    Engine.setSize(missile, [0, 0, 0], [0, 0, 0])
    Engine.setOrigin(missile, origin)
    return missile
  }

  static launch_spike(globals, owner, origin, dir) {
    return WeaponsModule._launchSpike(globals, owner, origin, dir, false)
  }

  static _grenadeExplode(globals, grenade) {
    if (grenade == null) return
    var owner = grenade.get("owner", null)
    CombatModule.tRadiusDamage(globals, grenade, owner, 120, globals.world)
    WeaponsModule._emitExplosion(grenade.get("origin", [0, 0, 0]))
    Engine.removeEntity(grenade)
  }

  static grenadeExplode(globals, grenade) {
    WeaponsModule._grenadeExplode(globals, grenade)
  }

  static grenadeTouch(globals, grenade, other) {
    if (grenade == null) return
    if (other == null) return
    if (other == grenade.get("owner", null)) return

    if (other.get("takedamage", DamageValues.NO) == DamageValues.AIM) {
      WeaponsModule._grenadeExplode(globals, grenade)
      return
    }

    Engine.playSound(grenade, Channels.WEAPON, "weapons/bounce.wav", 1, Attenuations.NORMAL)
    var velocity = grenade.get("velocity", [0, 0, 0])
    if (velocity[0] == 0 && velocity[1] == 0 && velocity[2] == 0) {
      grenade.set("avelocity", [0, 0, 0])
    }
  }

  static _spikeDamage(globals, spike, other, damage, isSuper) {
    var owner = spike.get("owner", null)
    if (other.get("takedamage", DamageValues.NO) != DamageValues.NO) {
      WeaponsModule.spawnTouchBlood(spike, damage)
      CombatModule.tDamage(globals, other, spike, owner == null ? spike : owner, damage)
    } else {
      WeaponsModule._emitSpike(spike.get("origin", [0, 0, 0]), isSuper)
    }
    Engine.removeEntity(spike)
  }

  static spikeTouch(globals, spike, other) {
    if (spike == null) return
    if (other == null) return
    if (other == spike.get("owner", null)) return
    if (other.get("solid", SolidTypes.NOT) == SolidTypes.TRIGGER) return

    var contents = Engine.pointContents(spike.get("origin", [0, 0, 0]))
    if (contents == Contents.SKY) {
      Engine.removeEntity(spike)
      return
    }

    WeaponsModule._spikeDamage(globals, spike, other, 9, false)
  }

  static spike_touch(globals, spike, other) {
    WeaponsModule.spikeTouch(globals, spike, other)
  }

  static superSpikeTouch(globals, spike, other) {
    if (spike == null) return
    if (other == null) return
    if (other == spike.get("owner", null)) return
    if (other.get("solid", SolidTypes.NOT) == SolidTypes.TRIGGER) return

    var contents = Engine.pointContents(spike.get("origin", [0, 0, 0]))
    if (contents == Contents.SKY) {
      Engine.removeEntity(spike)
      return
    }

    WeaponsModule._spikeDamage(globals, spike, other, 18, true)
  }

  static superspike_touch(globals, spike, other) {
    WeaponsModule.superSpikeTouch(globals, spike, other)
  }

  static _handleLightningTrace(globals, attacker, trace, damage, skipA, skipB) {
    if (trace == null) return null
    if (!trace.containsKey("entity")) return null
    var target = trace["entity"]
    if (target == null) return null
    if (target == skipA || target == skipB) return target
    if (target.get("takedamage", DamageValues.NO) == DamageValues.NO) return target

    var endpos = trace.containsKey("endpos") ? trace["endpos"] : target.get("origin", [0, 0, 0])
    WeaponsModule._spawnParticles(endpos, [0, 0, 100], 225, (damage * 4).ceil)
    CombatModule.tDamage(globals, target, attacker, attacker, damage)

    if (attacker != null && attacker.get("classname", "") == "player" && target.get("classname", "") == "player") {
      var velocity = target.get("velocity", [0, 0, 0])
      velocity[2] = velocity[2] + 400
      target.set("velocity", velocity)
    }

    return target
  }

  static _lightningDamage(globals, attacker, start, end, damage) {
    var direction = WeaponsModule._vectorSub(end, start)
    if (WeaponsModule._vectorLength(direction) == 0) return
    direction = WeaponsModule._vectorNormalize(direction)

    var perpendicular = [-direction[1], direction[0], 0]
    if (WeaponsModule._vectorLength(perpendicular) == 0) {
      perpendicular = [0, 0, 0]
    } else {
      perpendicular = WeaponsModule._vectorScale(WeaponsModule._vectorNormalize(perpendicular), 16)
    }

    var centerTrace = Engine.traceLine(start, end, false, attacker)
    var first = WeaponsModule._handleLightningTrace(globals, attacker, centerTrace, damage, null, null)

    if (perpendicular[0] != 0 || perpendicular[1] != 0 || perpendicular[2] != 0) {
      var offsetStart = WeaponsModule._vectorAdd(start, perpendicular)
      var offsetEnd = WeaponsModule._vectorAdd(end, perpendicular)
      var secondTrace = Engine.traceLine(offsetStart, offsetEnd, false, attacker)
      var second = WeaponsModule._handleLightningTrace(globals, attacker, secondTrace, damage, first, null)

      var negStart = WeaponsModule._vectorSub(start, perpendicular)
      var negEnd = WeaponsModule._vectorSub(end, perpendicular)
      var thirdTrace = Engine.traceLine(negStart, negEnd, false, attacker)
      WeaponsModule._handleLightningTrace(globals, attacker, thirdTrace, damage, first, second)
    }
  }

  static LightningDamage(globals, attacker, start, end, damage) {
    WeaponsModule._lightningDamage(globals, attacker, start, end, damage)
  }

  static tMissileTouch(globals, missile, other) {
    if (missile == null) return
    if (other == missile.get("owner", null)) return

    if (Engine.pointContents(missile.get("origin", [0, 0, 0])) == Contents.SKY) {
      Engine.removeEntity(missile)
      return
    }

    var owner = missile.get("owner", null)
    if (other != null && other.get("takedamage", DamageValues.NO) != DamageValues.NO) {
      var damage = 100 + Engine.random() * 20
      if (other.get("classname", "") == "monster_shambler") {
        damage = damage * 0.5
      }
      CombatModule.tDamage(globals, other, missile, owner == null ? missile : owner, damage)
    }

    CombatModule.tRadiusDamage(globals, missile, owner, 120, other)
    WeaponsModule._emitExplosion(missile.get("origin", [0, 0, 0]))
    Engine.removeEntity(missile)
  }

  static T_MissileTouch(globals, missile, other) {
    WeaponsModule.tMissileTouch(globals, missile, other)
  }

  static becomeExplosion(globals, entity) {
    if (entity == null) return
    entity.set("movetype", MoveTypes.NONE)
    entity.set("velocity", [0, 0, 0])
    entity.set("touch", null)
    Engine.setModel(entity, "progs/s_explod.spr")
    entity.set("solid", SolidTypes.NOT)
    WeaponsModule.s_explode1(globals, entity)
  }

  static BecomeExplosion(globals, entity) {
    WeaponsModule.becomeExplosion(globals, entity)
  }

  static s_explode1(globals, entity) {
    WeaponsModule._setExplosionFrame(entity, 0, "WeaponsModule.s_explode2")
  }

  static s_explode2(globals, entity) {
    WeaponsModule._setExplosionFrame(entity, 1, "WeaponsModule.s_explode3")
  }

  static s_explode3(globals, entity) {
    WeaponsModule._setExplosionFrame(entity, 2, "WeaponsModule.s_explode4")
  }

  static s_explode4(globals, entity) {
    WeaponsModule._setExplosionFrame(entity, 3, "WeaponsModule.s_explode5")
  }

  static s_explode5(globals, entity) {
    WeaponsModule._setExplosionFrame(entity, 4, "WeaponsModule.s_explode6")
  }

  static s_explode6(globals, entity) {
    WeaponsModule._setExplosionFrame(entity, 5, null)
  }

  static precache(globals) {
    for (path in _WEAPON_SOUNDS) {
      Engine.precacheSound(path)
    }
  }

  static _setExplosionFrame(entity, frame, nextName) {
    if (entity == null) return
    entity.set("frame", frame)
    var delay = 0.1
    if (nextName == null) {
      entity.set("think", "SubsModule.subRemove")
      entity.set("nextthink", Engine.time() + delay)
      Engine.scheduleThink(entity, "SubsModule.subRemove", delay)
    } else {
      entity.set("think", nextName)
      entity.set("nextthink", Engine.time() + delay)
      Engine.scheduleThink(entity, nextName, delay)
    }
  }

  static _ammoMask() {
    if (_AMMO_BITS == null) {
      _AMMO_BITS = Engine.bitOrMany([Items.SHELLS, Items.NAILS, Items.ROCKETS, Items.CELLS])
    }
    return _AMMO_BITS
  }

  static _callPlayerAnimation(globals, player, animation) {
    if (animation == null || animation == "") return

    var functionName = animation
    if (!functionName.contains(".")) {
      functionName = "PlayerModule." + functionName
    }

    var previousSelf = globals.self
    var previousOther = globals.other
    globals.self = player
    Engine.callEntityFunction(player, functionName, [])
    globals.self = previousSelf
    globals.other = previousOther
  }

  static _clearAmmoBits(player) {
    var items = player.get("items", 0)
    items = items - Engine.bitAnd(items, WeaponsModule._ammoMask())
    player.set("items", items)
    return items
  }

  static setCurrentAmmo(globals, player) {
    player.set("weaponframe", 0)
    var items = WeaponsModule._clearAmmoBits(player)
    var weapon = player.get("weapon", Items.AXE)

    if (weapon == Items.AXE) {
      player.set("currentammo", 0)
      player.set("weaponmodel", "progs/v_axe.mdl")
    } else if (weapon == Items.SHOTGUN) {
      player.set("currentammo", player.get("ammo_shells", 0))
      player.set("weaponmodel", "progs/v_shot.mdl")
      items = Engine.bitOr(items, Items.SHELLS)
    } else if (weapon == Items.SUPER_SHOTGUN) {
      player.set("currentammo", player.get("ammo_shells", 0))
      player.set("weaponmodel", "progs/v_shot2.mdl")
      items = Engine.bitOr(items, Items.SHELLS)
    } else if (weapon == Items.NAILGUN) {
      player.set("currentammo", player.get("ammo_nails", 0))
      player.set("weaponmodel", "progs/v_nail.mdl")
      items = Engine.bitOr(items, Items.NAILS)
    } else if (weapon == Items.SUPER_NAILGUN) {
      player.set("currentammo", player.get("ammo_nails", 0))
      player.set("weaponmodel", "progs/v_nail2.mdl")
      items = Engine.bitOr(items, Items.NAILS)
    } else if (weapon == Items.GRENADE_LAUNCHER) {
      player.set("currentammo", player.get("ammo_rockets", 0))
      player.set("weaponmodel", "progs/v_rock.mdl")
      items = Engine.bitOr(items, Items.ROCKETS)
    } else if (weapon == Items.ROCKET_LAUNCHER) {
      player.set("currentammo", player.get("ammo_rockets", 0))
      player.set("weaponmodel", "progs/v_rock2.mdl")
      items = Engine.bitOr(items, Items.ROCKETS)
    } else if (weapon == Items.LIGHTNING) {
      player.set("currentammo", player.get("ammo_cells", 0))
      player.set("weaponmodel", "progs/v_light.mdl")
      items = Engine.bitOr(items, Items.CELLS)
    } else {
      player.set("currentammo", 0)
      player.set("weaponmodel", "")
    }

    player.set("items", items)
  }

  static bestWeapon(globals, player) {
    var items = player.get("items", 0)

    if (player.get("waterlevel", 0) <= 1 && player.get("ammo_cells", 0) >= 1 && Engine.bitAnd(items, Items.LIGHTNING) != 0) {
      return Items.LIGHTNING
    }
    if (player.get("ammo_nails", 0) >= 2 && Engine.bitAnd(items, Items.SUPER_NAILGUN) != 0) {
      return Items.SUPER_NAILGUN
    }
    if (player.get("ammo_shells", 0) >= 2 && Engine.bitAnd(items, Items.SUPER_SHOTGUN) != 0) {
      return Items.SUPER_SHOTGUN
    }
    if (player.get("ammo_nails", 0) >= 1 && Engine.bitAnd(items, Items.NAILGUN) != 0) {
      return Items.NAILGUN
    }
    if (player.get("ammo_shells", 0) >= 1 && Engine.bitAnd(items, Items.SHOTGUN) != 0) {
      return Items.SHOTGUN
    }

    return Items.AXE
  }

  static wantsToChangeWeapon(globals, player, oldWeapon, newWeapon) {
    var extraFlags = player.get("player_flags_ex", 0)
    if (Engine.bitAnd(extraFlags, PlayerExtraFlags.CHANGE_NEVER) != 0) {
      return false
    }
    if (Engine.bitAnd(extraFlags, PlayerExtraFlags.CHANGE_ONLY_NEW) != 0 && oldWeapon == newWeapon) {
      return false
    }
    return true
  }

  static hasNoAmmo(globals, player) {
    if (player.get("currentammo", 0) != 0) return false
    if (player.get("weapon", Items.AXE) == Items.AXE) return false

    var best = WeaponsModule.bestWeapon(globals, player)
    player.set("weapon", best)
    WeaponsModule.setCurrentAmmo(globals, player)
    return true
  }

  static attack(globals, player) {
    if (WeaponsModule.hasNoAmmo(globals, player)) return

    Engine.makeVectors(player.get("v_angle", [0, 0, 0]))
    player.set("show_hostile", Engine.time() + 1)

    var weapon = player.get("weapon", Items.AXE)
    if (weapon != Items.AXE) {
      player.set("fired_weapon", 1)
    }

    if (weapon == Items.AXE) {
      Engine.playSound(player, Channels.WEAPON, "weapons/ax1.wav", 1, Attenuations.NORMAL)
      var r = Engine.random()
      if (r < 0.25) {
        WeaponsModule._callPlayerAnimation(globals, player, "player_axe1")
      } else if (r < 0.5) {
        WeaponsModule._callPlayerAnimation(globals, player, "player_axeb1")
      } else if (r < 0.75) {
        WeaponsModule._callPlayerAnimation(globals, player, "player_axec1")
      } else {
        WeaponsModule._callPlayerAnimation(globals, player, "player_axed1")
      }
      player.set("attack_finished", Engine.time() + 0.5)
      return
    }

    if (weapon == Items.SHOTGUN) {
      WeaponsModule._callPlayerAnimation(globals, player, "player_shot1")
      WeaponsModule.fireShotgun(globals, player)
      player.set("attack_finished", Engine.time() + 0.5)
      return
    }

    if (weapon == Items.SUPER_SHOTGUN) {
      WeaponsModule._callPlayerAnimation(globals, player, "player_shot1")
      WeaponsModule.fireSuperShotgun(globals, player)
      player.set("attack_finished", Engine.time() + 0.7)
      return
    }

    if (weapon == Items.NAILGUN) {
      WeaponsModule._callPlayerAnimation(globals, player, "player_nail1")
      return
    }

    if (weapon == Items.SUPER_NAILGUN) {
      WeaponsModule._callPlayerAnimation(globals, player, "player_nail1")
      return
    }

    if (weapon == Items.GRENADE_LAUNCHER) {
      WeaponsModule._callPlayerAnimation(globals, player, "player_rocket1")
      WeaponsModule.fireGrenade(globals, player)
      player.set("attack_finished", Engine.time() + 0.6)
      return
    }

    if (weapon == Items.ROCKET_LAUNCHER) {
      WeaponsModule._callPlayerAnimation(globals, player, "player_rocket1")
      WeaponsModule.fireRocket(globals, player)
      player.set("attack_finished", Engine.time() + 0.8)
      return
    }

    if (weapon == Items.LIGHTNING) {
      WeaponsModule._callPlayerAnimation(globals, player, "player_light1")
      player.set("attack_finished", Engine.time() + 0.1)
      Engine.playSound(player, Channels.AUTO, "weapons/lstart.wav", 1, Attenuations.NORMAL)
      return
    }
  }

  static changeWeapon(globals, player) {
    var impulse = player.get("impulse", 0)
    var desired = player.get("weapon", Items.AXE)
    var ammoShortage = false

    if (impulse == 1) {
      desired = Items.AXE
    } else if (impulse == 2) {
      desired = Items.SHOTGUN
      ammoShortage = player.get("ammo_shells", 0) < 1
    } else if (impulse == 3) {
      desired = Items.SUPER_SHOTGUN
      ammoShortage = player.get("ammo_shells", 0) < 2
    } else if (impulse == 4) {
      desired = Items.NAILGUN
      ammoShortage = player.get("ammo_nails", 0) < 1
    } else if (impulse == 5) {
      desired = Items.SUPER_NAILGUN
      ammoShortage = player.get("ammo_nails", 0) < 2
    } else if (impulse == 6) {
      desired = Items.GRENADE_LAUNCHER
      ammoShortage = player.get("ammo_rockets", 0) < 1
    } else if (impulse == 7) {
      desired = Items.ROCKET_LAUNCHER
      ammoShortage = player.get("ammo_rockets", 0) < 1
    } else if (impulse == 8) {
      desired = Items.LIGHTNING
      ammoShortage = player.get("ammo_cells", 0) < 1
    }

    player.set("impulse", 0)

    var items = player.get("items", 0)
    if (Engine.bitAnd(items, desired) == 0) {
      Engine.playerPrint(player, "$qc_no_weapon", [])
      return
    }

    if (ammoShortage) {
      Engine.playerPrint(player, "$qc_not_enough_ammo", [])
      return
    }

    player.set("weapon", desired)
    WeaponsModule.setCurrentAmmo(globals, player)
  }

  static cheatCommand(globals, player) {
    if ((globals.deathmatch > 0 || globals.coop > 0) && globals.cheatsAllowed == 0) {
      return
    }

    player.set("ammo_rockets", 100)
    player.set("ammo_nails", 200)
    player.set("ammo_shells", 100)
    player.set("ammo_cells", 200)

    var items = player.get("items", 0)
    items = Engine.bitOrMany([
      items,
      Items.AXE,
      Items.SHOTGUN,
      Items.SUPER_SHOTGUN,
      Items.NAILGUN,
      Items.SUPER_NAILGUN,
      Items.GRENADE_LAUNCHER,
      Items.ROCKET_LAUNCHER,
      Items.KEY1,
      Items.KEY2,
      Items.LIGHTNING
    ])

    var armorBits = Engine.bitOrMany([Items.ARMOR1, Items.ARMOR2, Items.ARMOR3])
    items = items - Engine.bitAnd(items, armorBits)
    items = Engine.bitOr(items, Items.ARMOR3)

    player.set("items", items)
    player.set("armortype", 0.8)
    player.set("armorvalue", 200)

    player.set("weapon", Items.ROCKET_LAUNCHER)
    player.set("impulse", 0)
    WeaponsModule.setCurrentAmmo(globals, player)
  }

  static cycleWeaponCommand(globals, player) {
    player.set("impulse", 0)
    var items = player.get("items", 0)

    while (true) {
      var weapon = player.get("weapon", Items.AXE)
      var ammoShort = false

      if (weapon == Items.LIGHTNING) {
        weapon = Items.AXE
      } else if (weapon == Items.AXE) {
        weapon = Items.SHOTGUN
        ammoShort = player.get("ammo_shells", 0) < 1
      } else if (weapon == Items.SHOTGUN) {
        weapon = Items.SUPER_SHOTGUN
        ammoShort = player.get("ammo_shells", 0) < 2
      } else if (weapon == Items.SUPER_SHOTGUN) {
        weapon = Items.NAILGUN
        ammoShort = player.get("ammo_nails", 0) < 1
      } else if (weapon == Items.NAILGUN) {
        weapon = Items.SUPER_NAILGUN
        ammoShort = player.get("ammo_nails", 0) < 2
      } else if (weapon == Items.SUPER_NAILGUN) {
        weapon = Items.GRENADE_LAUNCHER
        ammoShort = player.get("ammo_rockets", 0) < 1
      } else if (weapon == Items.GRENADE_LAUNCHER) {
        weapon = Items.ROCKET_LAUNCHER
        ammoShort = player.get("ammo_rockets", 0) < 1
      } else if (weapon == Items.ROCKET_LAUNCHER) {
        weapon = Items.LIGHTNING
        ammoShort = player.get("ammo_cells", 0) < 1
      }

      player.set("weapon", weapon)

      if (Engine.bitAnd(items, weapon) != 0 && !ammoShort) {
        WeaponsModule.setCurrentAmmo(globals, player)
        return
      }
    }
  }

  static cycleWeaponReverseCommand(globals, player) {
    player.set("impulse", 0)
    var items = player.get("items", 0)

    while (true) {
      var weapon = player.get("weapon", Items.AXE)
      var ammoShort = false

      if (weapon == Items.LIGHTNING) {
        weapon = Items.ROCKET_LAUNCHER
        ammoShort = player.get("ammo_rockets", 0) < 1
      } else if (weapon == Items.ROCKET_LAUNCHER) {
        weapon = Items.GRENADE_LAUNCHER
        ammoShort = player.get("ammo_rockets", 0) < 1
      } else if (weapon == Items.GRENADE_LAUNCHER) {
        weapon = Items.SUPER_NAILGUN
        ammoShort = player.get("ammo_nails", 0) < 2
      } else if (weapon == Items.SUPER_NAILGUN) {
        weapon = Items.NAILGUN
        ammoShort = player.get("ammo_nails", 0) < 1
      } else if (weapon == Items.NAILGUN) {
        weapon = Items.SUPER_SHOTGUN
        ammoShort = player.get("ammo_shells", 0) < 2
      } else if (weapon == Items.SUPER_SHOTGUN) {
        weapon = Items.SHOTGUN
        ammoShort = player.get("ammo_shells", 0) < 1
      } else if (weapon == Items.SHOTGUN) {
        weapon = Items.AXE
      } else if (weapon == Items.AXE) {
        weapon = Items.LIGHTNING
        ammoShort = player.get("ammo_cells", 0) < 1
      }

      player.set("weapon", weapon)

      if (Engine.bitAnd(items, weapon) != 0 && !ammoShort) {
        WeaponsModule.setCurrentAmmo(globals, player)
        return
      }
    }
  }

  static serverflagsCommand(globals) {
    globals.serverFlags = globals.serverFlags * 2 + 1
  }

  static quadCheat(globals, player) {
    if (globals.cheatsAllowed == 0) return

    player.set("super_time", 1)
    player.set("super_damage_finished", Engine.time() + 30)
    var items = player.get("items", 0)
    items = Engine.bitOr(items, Items.QUAD)
    player.set("items", items)
  }

  static impulseCommands(globals, player) {
    var impulse = player.get("impulse", 0)
    if (impulse >= 1 && impulse <= 8) {
      WeaponsModule.changeWeapon(globals, player)
    } else if (impulse == 9) {
      WeaponsModule.cheatCommand(globals, player)
    } else if (impulse == 10) {
      WeaponsModule.cycleWeaponCommand(globals, player)
    } else if (impulse == 11) {
      WeaponsModule.serverflagsCommand(globals)
    } else if (impulse == 12) {
      WeaponsModule.cycleWeaponReverseCommand(globals, player)
    } else if (impulse == 255) {
      WeaponsModule.quadCheat(globals, player)
    }

    player.set("impulse", 0)
  }

  static weaponFrame(globals, player) {
    if (Engine.time() < player.get("attack_finished", 0)) {
      return
    }

    if (player.get("impulse", 0) != 0) {
      WeaponsModule.impulseCommands(globals, player)
    }

    if (player.get("button0", false)) {
      WeaponsModule.superDamageSound(globals, player)
      WeaponsModule.attack(globals, player)
    }
  }

  static SpawnBlood(origin, velocity, damage) {
    WeaponsModule.spawnBlood(origin, velocity, damage)
  }

  static SpawnChunk(origin, velocity) {
    WeaponsModule.spawnChunk(origin, velocity)
  }

  static SpawnMeatSpray(globals, origin, velocity) {
    var owner = globals == null ? null : globals.self
    WeaponsModule.spawnMeatSpray(globals, owner, origin, velocity)
  }

  static FireBullets(globals, shooter, shotCount, dir, spread) {
    WeaponsModule.fireBullets(globals, shooter, shotCount, dir, spread)
  }

  static GrenadeExplode(globals, grenade) {
    WeaponsModule.grenadeExplode(globals, grenade)
  }

  static GrenadeTouch(globals, grenade, other) {
    WeaponsModule.grenadeTouch(globals, grenade, other)
  }

  static W_Attack(globals, player) {
    WeaponsModule.attack(globals, player)
  }

  static W_ChangeWeapon(globals, player) {
    WeaponsModule.changeWeapon(globals, player)
  }

  static W_FireAxe(globals, player) {
    WeaponsModule.startAxeAttack(globals, player)
  }

  static W_FireShotgun(globals, player) {
    WeaponsModule.fireShotgun(globals, player)
  }

  static W_FireSuperShotgun(globals, player) {
    WeaponsModule.fireSuperShotgun(globals, player)
  }

  static W_FireGrenade(globals, player) {
    WeaponsModule.fireGrenade(globals, player)
  }

  static W_FireRocket(globals, player) {
    WeaponsModule.fireRocket(globals, player)
  }

  static W_FireLightning(globals, player) {
    WeaponsModule.startLightningAttack(globals, player)
  }

  static W_FireSpikes(globals, player, offset) {
    WeaponsModule._fireSpikes(globals, player)
  }

  static W_FireSuperSpikes(globals, player) {
    WeaponsModule._fireSuperSpikes(globals, player)
  }

  static W_HasNoAmmo(globals, player) {
    return WeaponsModule.hasNoAmmo(globals, player)
  }

  static W_Precache(globals) {
    WeaponsModule.precache(globals)
  }

  static W_SetCurrentAmmo(globals, player) {
    WeaponsModule.setCurrentAmmo(globals, player)
  }

  static W_WantsToChangeWeapon(globals, player, oldWeapon, newWeapon) {
    return WeaponsModule.wantsToChangeWeapon(globals, player, oldWeapon, newWeapon)
  }

  static W_WeaponFrame(globals, player) {
    WeaponsModule.weaponFrame(globals, player)
  }

  static W_BestWeapon(globals, player) {
    return WeaponsModule.bestWeapon(globals, player)
  }

  static SuperDamageSound(globals, player) {
    WeaponsModule.superDamageSound(globals, player)
  }

  static CycleWeaponCommand(globals, player) {
    WeaponsModule.cycleWeaponCommand(globals, player)
  }

  static CycleWeaponReverseCommand(globals, player) {
    WeaponsModule.cycleWeaponReverseCommand(globals, player)
  }

  static ImpulseCommands(globals, player) {
    WeaponsModule.impulseCommands(globals, player)
  }

  static ServerflagsCommand(globals) {
    WeaponsModule.serverflagsCommand(globals)
  }

  static QuadCheat(globals, player) {
    WeaponsModule.quadCheat(globals, player)
  }

  static CheatCommand(globals, player) {
    WeaponsModule.cheatCommand(globals, player)
  }

  static superDamageSound(globals, player) {
    if (player.get("super_damage_finished", 0) > Engine.time()) {
      if (player.get("super_sound", 0) < Engine.time()) {
        player.set("super_sound", Engine.time() + 1)
        Engine.playSound(player, Channels.BODY, "items/damage3.wav", 1, Attenuations.NORMAL)
      }
    }
  }

  static startAxeAttack(globals, player) {
    var origin = WeaponsModule._eyeOrigin(player)
    var vectors = WeaponsModule._makeVectors(player.get("v_angle", [0, 0, 0]))
    var forward = vectors["forward"]
    var end = WeaponsModule._vectorAdd(origin, WeaponsModule._vectorScale(forward, 64))
    var trace = Engine.traceLine(origin, end, false, player)
    if (trace == null || !trace.containsKey("fraction") || trace["fraction"] >= 1) return

    var impact = WeaponsModule._vectorSub(trace.containsKey("endpos") ? trace["endpos"] : end, WeaponsModule._vectorScale(forward, 4))
    var target = trace.containsKey("entity") ? trace["entity"] : null

    if (target != null && target.get("takedamage", DamageValues.NO) != DamageValues.NO) {
      target.set("axhitme", 1)
      WeaponsModule._spawnBlood(impact, [0, 0, 0], 20)
      CombatModule.tDamage(globals, target, player, player, 20)
    } else {
      Engine.playSound(player, Channels.WEAPON, "player/axhit2.wav", 1, Attenuations.NORMAL)
      WeaponsModule._emitGunshot(impact)
    }
  }

  static fireShotgun(globals, player) {
    Engine.playSound(player, Channels.WEAPON, "weapons/guncock.wav", 1, Attenuations.NORMAL)
    player.set("punchangle", [-2, 0, 0])
    var ammo = player.get("ammo_shells", 0) - 1
    if (ammo < 0) ammo = 0
    player.set("ammo_shells", ammo)
    player.set("currentammo", ammo)

    var dir = WeaponsModule._aimDirection(player, 100000)
    WeaponsModule._fireBullets(globals, player, 6, dir, [0.04, 0.04, 0])
  }

  static fireSuperShotgun(globals, player) {
    if (player.get("currentammo", 0) == 1) {
      WeaponsModule.fireShotgun(globals, player)
      return
    }

    Engine.playSound(player, Channels.WEAPON, "weapons/shotgn2.wav", 1, Attenuations.NORMAL)
    player.set("punchangle", [-4, 0, 0])
    var ammo = player.get("ammo_shells", 0) - 2
    if (ammo < 0) ammo = 0
    player.set("ammo_shells", ammo)
    player.set("currentammo", ammo)

    var dir = WeaponsModule._aimDirection(player, 100000)
    WeaponsModule._fireBullets(globals, player, 14, dir, [0.14, 0.08, 0])
  }

  static startNailgunAttack(globals, player) {
    WeaponsModule._fireSpikes(globals, player)
  }

  static startSuperNailgunAttack(globals, player) {
    WeaponsModule._fireSpikes(globals, player)
  }

  static fireGrenade(globals, player) {
    var ammo = player.get("ammo_rockets", 0) - 1
    if (ammo < 0) ammo = 0
    player.set("ammo_rockets", ammo)
    player.set("currentammo", ammo)

    Engine.playSound(player, Channels.WEAPON, "weapons/grenade.wav", 1, Attenuations.NORMAL)
    player.set("punchangle", [-2, 0, 0])

    var grenade = Engine.spawnEntity()
    grenade.set("classname", "grenade")
    grenade.set("owner", player)
    grenade.set("movetype", MoveTypes.BOUNCE)
    grenade.set("solid", SolidTypes.BBOX)

    var angles = player.get("v_angle", [0, 0, 0])
    var vectors = WeaponsModule._makeVectors(angles)

    var velocity
    if (angles[0] != 0) {
      var forward = WeaponsModule._vectorScale(vectors["forward"], 600)
      var up = WeaponsModule._vectorScale(vectors["up"], 200)
      var randomRight = WeaponsModule._vectorScale(vectors["right"], WeaponsModule._cRandom() * 10)
      var randomUp = WeaponsModule._vectorScale(vectors["up"], WeaponsModule._cRandom() * 10)
      velocity = WeaponsModule._vectorAdd(WeaponsModule._vectorAdd(forward, up), WeaponsModule._vectorAdd(randomRight, randomUp))
    } else {
      velocity = WeaponsModule._vectorScale(WeaponsModule._aimDirection(player, 10000), 600)
      velocity[2] = 200
    }

    grenade.set("velocity", velocity)
    grenade.set("avelocity", [300, 300, 300])
    grenade.set("angles", Engine.vectorToAngles(velocity))
    grenade.set("touch", "WeaponsModule.grenadeTouch")
    grenade.set("think", "WeaponsModule.grenadeExplode")
    var detonate = Engine.time() + 2.5
    grenade.set("nextthink", detonate)
    Engine.scheduleThink(grenade, "WeaponsModule.grenadeExplode", 2.5)

    Engine.setModel(grenade, "progs/grenade.mdl")
    Engine.setSize(grenade, [0, 0, 0], [0, 0, 0])
    Engine.setOrigin(grenade, player.get("origin", [0, 0, 0]))
  }

  static fireRocket(globals, player) {
    var ammo = player.get("ammo_rockets", 0) - 1
    if (ammo < 0) ammo = 0
    player.set("ammo_rockets", ammo)
    player.set("currentammo", ammo)

    Engine.playSound(player, Channels.WEAPON, "weapons/sgun1.wav", 1, Attenuations.NORMAL)
    player.set("punchangle", [-2, 0, 0])

    var missile = Engine.spawnEntity()
    missile.set("classname", "missile")
    missile.set("owner", player)
    missile.set("movetype", MoveTypes.FLYMISSILE)
    missile.set("solid", SolidTypes.BBOX)

    var dir = WeaponsModule._aimDirection(player, 1000)
    var velocity = WeaponsModule._vectorScale(dir, 1000)
    missile.set("velocity", velocity)
    missile.set("angles", Engine.vectorToAngles(velocity))

    missile.set("touch", "WeaponsModule.tMissileTouch")
    missile.set("think", "SubsModule.subRemove")
    var removeTime = Engine.time() + 5
    missile.set("nextthink", removeTime)
    Engine.scheduleThink(missile, "SubsModule.subRemove", 5)

    var vectors = WeaponsModule._makeVectors(player.get("v_angle", [0, 0, 0]))
    var forward = vectors["forward"]
    var origin = WeaponsModule._vectorAdd(player.get("origin", [0, 0, 0]), WeaponsModule._vectorAdd(WeaponsModule._vectorScale(forward, 8), [0, 0, 16]))

    Engine.setModel(missile, "progs/missile.mdl")
    Engine.setSize(missile, [0, 0, 0], [0, 0, 0])
    Engine.setOrigin(missile, origin)
  }

  static startLightningAttack(globals, player) {
    if (player.get("ammo_cells", 0) < 1) {
      player.set("weapon", WeaponsModule.bestWeapon(globals, player))
      WeaponsModule.setCurrentAmmo(globals, player)
      return
    }

    if (player.get("waterlevel", 0) > 1) {
      var cells = player.get("ammo_cells", 0)
      player.set("ammo_cells", 0)
      WeaponsModule.setCurrentAmmo(globals, player)
      CombatModule.tRadiusDamage(globals, player, player, 35 * cells, globals.world)
      return
    }

    if (player.get("t_width", 0.0) < Engine.time()) {
      Engine.playSound(player, Channels.WEAPON, "weapons/lhit.wav", 1, Attenuations.NORMAL)
      player.set("t_width", Engine.time() + 0.6)
    }

    player.set("punchangle", [-2, 0, 0])
    var cells = player.get("ammo_cells", 0) - 1
    if (cells < 0) cells = 0
    player.set("ammo_cells", cells)
    player.set("currentammo", cells)

    var start = WeaponsModule._eyeOrigin(player)
    var vectors = WeaponsModule._makeVectors(player.get("v_angle", [0, 0, 0]))
    var forward = vectors["forward"]
    var end = WeaponsModule._vectorAdd(start, WeaponsModule._vectorScale(forward, 600))
    var trace = Engine.traceLine(start, end, true, player)
    var impact = trace != null && trace.containsKey("endpos") ? trace["endpos"] : end

    WeaponsModule._emitLightning(player, start, impact)
    var damageEnd = WeaponsModule._vectorAdd(impact, WeaponsModule._vectorScale(forward, 4))
    WeaponsModule._lightningDamage(globals, player, player.get("origin", [0, 0, 0]), damageEnd, 30)
  }
}

