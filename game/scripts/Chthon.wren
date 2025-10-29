// Chthon.wren
// Ports the Chthon boss fight logic from chthon.qc so the encounter runs
// entirely under the Wren gameplay layer.

import "./Engine" for Engine
import "./Globals" for SolidTypes, MoveTypes, DamageValues, Channels, Attenuations, Effects, TempEntityCodes
import "./Globals" for MessageTypes, ServiceCodes, MoverStates
import "./AI" for AIModule
import "./Fight" for FightModule
import "./Monsters" for MonstersModule
import "./Combat" for CombatModule
import "./Weapons" for WeaponsModule
import "./Subs" for SubsModule
import "./Doors" for DoorsModule

var _RISE_FRAMES = []
for (i in 1..17) { _RISE_FRAMES.add("rise" + i.toString) }

var _IDLE_FRAMES = []
for (i in 1..31) { _IDLE_FRAMES.add("walk" + i.toString) }

var _MISSILE_FRAMES = []
for (i in 1..23) { _MISSILE_FRAMES.add("attack" + i.toString) }

var _SHOCKA_FRAMES = []
for (i in 1..10) { _SHOCKA_FRAMES.add("shocka" + i.toString) }

var _SHOCKB_FRAMES = ["shockb1", "shockb2", "shockb3", "shockb4", "shockb5", "shockb6", "shockb1", "shockb2", "shockb3", "shockb4"]
var _SHOCKC_FRAMES = []
for (i in 1..10) { _SHOCKC_FRAMES.add("shockc" + i.toString) }

var _DEATH_FRAMES = []
for (i in 1..9) { _DEATH_FRAMES.add("death" + i.toString) }

var _RISE_ACTIONS = []
for (i in 0..._RISE_FRAMES.count) {
  if (i == 0) {
    _RISE_ACTIONS.add(Fn.new { |g, m, idx| Engine.playSound(m, Channels.WEAPON, "boss1/out1.wav", 1, Attenuations.NORMAL) })
  } else if (i == 1) {
    _RISE_ACTIONS.add(Fn.new { |g, m, idx| Engine.playSound(m, Channels.VOICE, "boss1/sight1.wav", 1, Attenuations.NORMAL) })
  } else {
    _RISE_ACTIONS.add(null)
  }
}

var _MISSILE_ACTIONS = []
for (i in 0..._MISSILE_FRAMES.count) {
  if (i == 8) {
    _MISSILE_ACTIONS.add(Fn.new { |g, m, idx| ChthonModule._bossLaunchMissile(g, m, [100, 100, 200]) })
  } else if (i == 19) {
    _MISSILE_ACTIONS.add(Fn.new { |g, m, idx| ChthonModule._bossLaunchMissile(g, m, [100, -100, 200]) })
  } else {
    _MISSILE_ACTIONS.add(Fn.new { |g, m, idx| ChthonModule.boss_face(g, m) })
  }
}

var _DEATH_ACTIONS = []
for (i in 0..._DEATH_FRAMES.count) {
  if (i == 0) {
    _DEATH_ACTIONS.add(Fn.new { |g, m, idx| Engine.playSound(m, Channels.VOICE, "boss1/death.wav", 1, Attenuations.NORMAL) })
  } else if (i == 8) {
    _DEATH_ACTIONS.add(Fn.new { |g, m, idx|
      Engine.playSound(m, Channels.BODY, "boss1/out1.wav", 1, Attenuations.NORMAL)
      Engine.emitTempEntity(TempEntityCodes.LAVASPLASH, { "origin": m.get("origin", [0, 0, 0]) })
    })
  } else {
    _DEATH_ACTIONS.add(null)
  }
}

var _lightningLe1 = null
var _lightningLe2 = null
var _lightningEndTime = 0.0

class ChthonModule {
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
    var length = ChthonModule._vectorLength(v)
    if (length == 0) return [0, 0, 0]
    var inv = 1 / length
    return [v[0] * inv, v[1] * inv, v[2] * inv]
  }

  static _makeVectors(angles) {
    var adjusted = [-angles[0], angles[1], angles[2]]
    var vectors = Engine.makeVectors(adjusted)
    if (vectors == null) return {"forward": [1, 0, 0], "right": [0, 1, 0], "up": [0, 0, 1]}
    if (!vectors.containsKey("forward")) vectors["forward"] = [1, 0, 0]
    if (!vectors.containsKey("right")) vectors["right"] = [0, 1, 0]
    if (!vectors.containsKey("up")) vectors["up"] = [0, 0, 1]
    return vectors
  }

  static _entityCenter(entity) {
    var mins = entity.get("mins", [0, 0, 0])
    var maxs = entity.get("maxs", [0, 0, 0])
    return [(mins[0] + maxs[0]) * 0.5, (mins[1] + maxs[1]) * 0.5, (mins[2] + maxs[2]) * 0.5]
  }

  static _enemyYaw(monster) {
    var enemy = monster.get("enemy", null)
    if (enemy == null) return monster.get("angles", [0, 0, 0])[1]
    var delta = ChthonModule._vectorSub(enemy.get("origin", [0, 0, 0]), monster.get("origin", [0, 0, 0]))
    var angles = Engine.vectorToAngles(delta)
    return angles[1]
  }

  static _setFrame(globals, monster, frame, nextFunction, delay) {
    MonstersModule.setFrame(globals, monster, frame, nextFunction, delay)
  }

  static _advanceSequence(globals, monster, frames, stateField, advanceName, restartName, actions, delay) {
    if (monster == null) return
    if (frames == null || frames.count == 0) return

    var index = monster.get(stateField, 0)
    if (index < 0 || index >= frames.count) index = 0

    var frame = frames[index]
    var nextIndex = index + 1
    var nextFunction = advanceName
    if (nextIndex >= frames.count) {
      nextIndex = 0
      nextFunction = restartName
    }
    monster.set(stateField, nextIndex)

    var thinkDelay = delay == null ? 0.1 : delay
    ChthonModule._setFrame(globals, monster, frame, nextFunction, thinkDelay)

    if (actions != null && index < actions.count) {
      var action = actions[index]
      if (action != null) action.call(globals, monster, index)
    }
  }

  static boss_face(globals, monster) {
    if (monster == null) return
    var enemy = monster.get("enemy", null)
    if (enemy != null && (enemy.get("health", 0) <= 0 || Engine.random() < 0.02)) {
      var target = enemy
      var attempts = 0
      while (attempts < 4) {
        target = Engine.find(target, "classname", "player")
        if (target != null && target.get("health", 0) > 0) {
          monster.set("enemy", target)
          break
        }
        attempts = attempts + 1
      }
    }
    FightModule.ai_face(globals, monster, ChthonModule._enemyYaw(monster))
  }

  static _bossLaunchMissile(globals, monster, offset) {
    if (monster == null) return
    ChthonModule.boss_face(globals, monster)
    var enemy = monster.get("enemy", null)
    if (enemy == null) {
      ChthonModule.boss_face(globals, monster)
      return
    }

    var origin = monster.get("origin", [0, 0, 0])
    var enemyOrigin = enemy.get("origin", [0, 0, 0])
    var offAngles = Engine.vectorToAngles(ChthonModule._vectorSub(enemyOrigin, origin))
    var vectors = ChthonModule._makeVectors(offAngles)
    var forward = vectors["forward"]
    var right = vectors["right"]
    var up = [0, 0, 1]

    var launchOrigin = ChthonModule._vectorAdd(origin,
      ChthonModule._vectorAdd(ChthonModule._vectorScale(forward, offset[0]),
        ChthonModule._vectorAdd(ChthonModule._vectorScale(right, offset[1]), [0, 0, offset[2]])))

    var predicted = enemyOrigin
    if (globals.skill > 1) {
      var distance = ChthonModule._vectorLength(ChthonModule._vectorSub(enemyOrigin, launchOrigin))
      var t = distance / 300.0
      var enemyVelocity = enemy.get("velocity", [0, 0, 0])
      enemyVelocity[2] = 0
      predicted = ChthonModule._vectorAdd(enemyOrigin, ChthonModule._vectorScale(enemyVelocity, t))
    }

    var direction = ChthonModule._vectorNormalize(ChthonModule._vectorSub(predicted, launchOrigin))
    var missile = WeaponsModule.launch_spike(globals, monster, launchOrigin, direction)
    if (missile == null) return

    missile.set("classname", "chthon_lavaball")
    Engine.setModel(missile, "progs/lavaball.mdl")
    missile.set("avelocity", [200, 100, 300])
    Engine.setSize(missile, [0, 0, 0], [0, 0, 0])
    missile.set("velocity", ChthonModule._vectorScale(direction, 300))
    missile.set("angles", Engine.vectorToAngles(missile.get("velocity", [0, 0, 0])))
    missile.set("touch", "WeaponsModule.tMissileTouch")
    Engine.playSound(monster, Channels.WEAPON, "boss1/throw.wav", 1, Attenuations.NORMAL)

    if (enemy.get("health", 0) <= 0) {
      ChthonModule.boss_idle1(globals, monster)
    }
  }

  static boss_rise1(globals, monster) {
    monster.set("_chthonRiseIndex", 0)
    ChthonModule._advanceSequence(globals, monster, _RISE_FRAMES, "_chthonRiseIndex", "ChthonModule._bossRiseAdvance", "ChthonModule.boss_missile1", _RISE_ACTIONS, 0.1)
  }

  static _bossRiseAdvance(globals, monster) {
    ChthonModule._advanceSequence(globals, monster, _RISE_FRAMES, "_chthonRiseIndex", "ChthonModule._bossRiseAdvance", "ChthonModule.boss_missile1", _RISE_ACTIONS, 0.1)
  }

  static boss_idle1(globals, monster) {
    var enemy = monster.get("enemy", null)
    if (enemy != null && enemy.get("health", 0) > 0) {
      ChthonModule.boss_missile1(globals, monster)
      return
    }
    monster.set("_chthonIdleIndex", 0)
    ChthonModule._advanceSequence(globals, monster, _IDLE_FRAMES, "_chthonIdleIndex", "ChthonModule._bossIdleAdvance", "ChthonModule.boss_idle1", null, 0.1)
    ChthonModule.boss_face(globals, monster)
  }

  static _bossIdleAdvance(globals, monster) {
    ChthonModule._advanceSequence(globals, monster, _IDLE_FRAMES, "_chthonIdleIndex", "ChthonModule._bossIdleAdvance", "ChthonModule.boss_idle1", null, 0.1)
    ChthonModule.boss_face(globals, monster)
  }

  static boss_missile1(globals, monster) {
    monster.set("_chthonMissileIndex", 0)
    ChthonModule._advanceSequence(globals, monster, _MISSILE_FRAMES, "_chthonMissileIndex", "ChthonModule._bossMissileAdvance", "ChthonModule.boss_missile1", _MISSILE_ACTIONS, 0.1)
  }

  static _bossMissileAdvance(globals, monster) {
    ChthonModule._advanceSequence(globals, monster, _MISSILE_FRAMES, "_chthonMissileIndex", "ChthonModule._bossMissileAdvance", "ChthonModule.boss_missile1", _MISSILE_ACTIONS, 0.1)
  }

  static boss_shocka1(globals, monster) {
    monster.set("_chthonShockAIndex", 0)
    ChthonModule._advanceSequence(globals, monster, _SHOCKA_FRAMES, "_chthonShockAIndex", "ChthonModule._bossShockAAdvance", "ChthonModule.boss_missile1", null, 0.1)
  }

  static _bossShockAAdvance(globals, monster) {
    ChthonModule._advanceSequence(globals, monster, _SHOCKA_FRAMES, "_chthonShockAIndex", "ChthonModule._bossShockAAdvance", "ChthonModule.boss_missile1", null, 0.1)
  }

  static boss_shockb1(globals, monster) {
    monster.set("_chthonShockBIndex", 0)
    ChthonModule._advanceSequence(globals, monster, _SHOCKB_FRAMES, "_chthonShockBIndex", "ChthonModule._bossShockBAdvance", "ChthonModule.boss_missile1", null, 0.1)
  }

  static _bossShockBAdvance(globals, monster) {
    ChthonModule._advanceSequence(globals, monster, _SHOCKB_FRAMES, "_chthonShockBIndex", "ChthonModule._bossShockBAdvance", "ChthonModule.boss_missile1", null, 0.1)
  }

  static boss_shockc1(globals, monster) {
    monster.set("_chthonShockCIndex", 0)
    ChthonModule._advanceSequence(globals, monster, _SHOCKC_FRAMES, "_chthonShockCIndex", "ChthonModule._bossShockCAdvance", "ChthonModule.boss_death1", null, 0.1)
  }

  static _bossShockCAdvance(globals, monster) {
    ChthonModule._advanceSequence(globals, monster, _SHOCKC_FRAMES, "_chthonShockCIndex", "ChthonModule._bossShockCAdvance", "ChthonModule.boss_death1", null, 0.1)
  }

  static boss_death1(globals, monster) {
    monster.set("_chthonDeathIndex", 0)
    ChthonModule._advanceSequence(globals, monster, _DEATH_FRAMES, "_chthonDeathIndex", "ChthonModule._bossDeathAdvance", "ChthonModule.boss_deathFinal", _DEATH_ACTIONS, 0.1)
  }

  static _bossDeathAdvance(globals, monster) {
    ChthonModule._advanceSequence(globals, monster, _DEATH_FRAMES, "_chthonDeathIndex", "ChthonModule._bossDeathAdvance", "ChthonModule.boss_deathFinal", _DEATH_ACTIONS, 0.1)
  }

  static boss_deathFinal(globals, monster) {
    globals.killedMonsters = globals.killedMonsters + 1
    Engine.writeByte(MessageTypes.ALL, ServiceCodes.KILLEDMONSTER, null)
    SubsModule.useTargets(globals, monster, monster.get("enemy", null))
    Engine.removeEntity(monster)
  }

  static boss_awake(globals, monster) {
    if (monster == null) return

    monster.set("solid", SolidTypes.SLIDEBOX)
    monster.set("movetype", MoveTypes.STEP)
    monster.set("takedamage", DamageValues.NO)

    Engine.setModel(monster, "progs/boss.mdl")
    monster.set("netname", "$qc_chthon")
    monster.set("killstring", "$qc_ks_chthon")
    Engine.setSize(monster, [-128, -128, -24], [128, 128, 256])

    monster.set("health", globals.skill == 0 ? 1 : 3)
    monster.set("max_health", monster.get("health", 3))
    monster.set("enemy", globals.activator)

    Engine.emitTempEntity(TempEntityCodes.LAVASPLASH, { "origin": monster.get("origin", [0, 0, 0]) })
    monster.set("yaw_speed", 20)
    ChthonModule.boss_rise1(globals, monster)
  }

  static monster_boss(globals, monster) {
    if (globals.deathmatch != 0) {
      Engine.removeEntity(monster)
      return
    }

    Engine.precacheModel("progs/boss.mdl")
    Engine.precacheModel("progs/lavaball.mdl")
    Engine.precacheSound("weapons/rocket1i.wav")
    Engine.precacheSound("boss1/out1.wav")
    Engine.precacheSound("boss1/sight1.wav")
    Engine.precacheSound("misc/power.wav")
    Engine.precacheSound("boss1/throw.wav")
    Engine.precacheSound("boss1/pain.wav")
    Engine.precacheSound("boss1/death.wav")

    globals.totalMonsters = globals.totalMonsters + 1
    monster.set("use", "ChthonModule.boss_awake")
  }

  static event_lightning(globals, trigger) {
    if (trigger == null) return
    trigger.set("use", "ChthonModule.lightning_use")
  }

  static lightning_fire(globals, trigger) {
    if (globals.time >= _lightningEndTime) {
      if (_lightningLe1 != null) {
        DoorsModule.doorGoDown(globals, _lightningLe1)
      }
      if (_lightningLe2 != null) {
        DoorsModule.doorGoDown(globals, _lightningLe2)
      }
      return
    }

    if (_lightningLe1 == null || _lightningLe2 == null) return

    var p1 = ChthonModule._entityCenter(_lightningLe1)
    var absmin1 = _lightningLe1.get("absmin", [0, 0, 0])
    p1[2] = absmin1[2] - 16

    var p2 = ChthonModule._entityCenter(_lightningLe2)
    var absmin2 = _lightningLe2.get("absmin", [0, 0, 0])
    p2[2] = absmin2[2] - 16

    var delta = ChthonModule._vectorNormalize(ChthonModule._vectorSub(p2, p1))
    p2 = ChthonModule._vectorSub(p2, ChthonModule._vectorScale(delta, 100))

    var delay = 0.1
    trigger.set("think", "ChthonModule.lightning_fire")
    trigger.set("nextthink", globals.time + delay)
    Engine.scheduleThink(trigger, "ChthonModule.lightning_fire", delay)

    Engine.emitTempEntity(TempEntityCodes.LIGHTNING3, {
      "owner": globals.world,
      "start": p1,
      "end": p2
    })
  }

  static lightning_use(globals, trigger, activator) {
    if (_lightningEndTime >= Engine.time() + 1) {
      return
    }

    _lightningLe1 = Engine.find(globals.world, "target", "lightning")
    _lightningLe2 = Engine.find(_lightningLe1, "target", "lightning")
    if (_lightningLe1 == null || _lightningLe2 == null) {
      Engine.log("missing lightning targets")
      return
    }

    var state1 = _lightningLe1.get("state", MoverStates.BOTTOM)
    var state2 = _lightningLe2.get("state", MoverStates.BOTTOM)
    if (!((state1 == MoverStates.TOP || state1 == MoverStates.BOTTOM) &&
        (state2 == MoverStates.TOP || state2 == MoverStates.BOTTOM) && state1 == state2)) {
      return
    }

    _lightningLe1.set("nextthink", -1.0)
    _lightningLe2.set("nextthink", -1.0)
    _lightningEndTime = Engine.time() + 1.0

    Engine.playSound(trigger, Channels.VOICE, "misc/power.wav", 1, Attenuations.NORMAL)
    ChthonModule.lightning_fire(globals, trigger)

    var boss = Engine.find(globals.world, "classname", "monster_boss")
    if (boss == null) return

    boss.set("enemy", activator)
    if (state1 == MoverStates.TOP && boss.get("health", 0) > 0) {
      Engine.playSound(boss, Channels.VOICE, "boss1/pain.wav", 1, Attenuations.NORMAL)
      var health = boss.get("health", 0) - 1
      boss.set("health", health)
      if (health >= 2) {
        ChthonModule.boss_shocka1(globals, boss)
      } else if (health == 1) {
        ChthonModule.boss_shockb1(globals, boss)
      } else if (health <= 0) {
        ChthonModule.boss_shockc1(globals, boss)
      }
    }
  }
}
