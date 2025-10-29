// Vore.wren
// Ports the shalrath (vore) monster logic from vore.qc so the enemy operates
// natively in Wren.

import "./Engine" for Engine
import "./Globals" for SolidTypes, MoveTypes, DamageValues, Channels, Attenuations, Effects, TempEntityCodes, CombatStyles
import "./AI" for AIModule
import "./Fight" for FightModule
import "./Monsters" for MonstersModule
import "./Combat" for CombatModule
import "./Weapons" for WeaponsModule
import "./Player" for PlayerModule
import "./Subs" for SubsModule

var _STAND_FRAME = "walk1"
var _WALK_FRAMES = [
  "walk2",
  "walk3",
  "walk4",
  "walk5",
  "walk6",
  "walk7",
  "walk8",
  "walk9",
  "walk10",
  "walk11",
  "walk12",
  "walk1"
]
var _WALK_SPEEDS = [6, 4, 0, 0, 0, 0, 5, 6, 5, 0, 4, 5]
var _RUN_FRAMES = _WALK_FRAMES
var _RUN_SPEEDS = [6, 4, 0, 0, 0, 0, 5, 6, 5, 0, 4, 5]
var _ATTACK_FRAMES = [
  "attack1",
  "attack2",
  "attack3",
  "attack4",
  "attack5",
  "attack6",
  "attack7",
  "attack8",
  "attack9",
  "attack10",
  "attack11"
]
var _PAIN_FRAMES = ["pain1", "pain2", "pain3", "pain4", "pain5"]
var _DEATH_FRAMES = ["death1", "death2", "death3", "death4", "death5", "death6", "death7"]

class VoreModule {
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
    var length = VoreModule._vectorLength(v)
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

  static _enemyYaw(monster) {
    var enemy = monster.get("enemy", null)
    if (enemy == null) return monster.get("angles", [0, 0, 0])[1]
    var delta = VoreModule._vectorSub(enemy.get("origin", [0, 0, 0]), monster.get("origin", [0, 0, 0]))
    var angles = Engine.vectorToAngles(delta)
    return angles[1]
  }

  static _faceEnemy(globals, monster) {
    FightModule.ai_face(globals, monster, VoreModule._enemyYaw(monster))
  }

  static _setFrame(globals, monster, frame, nextFunction, delay) {
    MonstersModule.setFrame(globals, monster, frame, nextFunction, delay)
  }

  static _advanceSequence(globals, monster, frames, stateField, advanceName, restartName, actionFn) {
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

    VoreModule._setFrame(globals, monster, frame, nextFunction, 0.1)
    if (actionFn != null) actionFn.call(globals, monster, index)
  }

  static shal_stand(globals, monster) {
    VoreModule._setFrame(globals, monster, _STAND_FRAME, "VoreModule.shal_stand", 0.1)
    AIModule.ai_stand(globals, monster)
  }

  static _walkAction(globals, monster, index) {
    if (index == 0) {
      if (Engine.random() < 0.2) {
        Engine.playSound(monster, Channels.VOICE, "shalrath/idle.wav", 1, Attenuations.IDLE)
      }
    }
    var speed = (index < _WALK_SPEEDS.count) ? _WALK_SPEEDS[index] : 0
    AIModule.ai_walk(globals, monster, speed)
  }

  static shal_walk1(globals, monster) {
    monster.set("_shalWalkIndex", 0)
    VoreModule._shalWalkAdvance(globals, monster)
  }

  static _shalWalkAdvance(globals, monster) {
    VoreModule._advanceSequence(globals, monster, _WALK_FRAMES, "_shalWalkIndex", "VoreModule._shalWalkAdvance", "VoreModule.shal_walk1", Fn.new { |g, m, i|
      VoreModule._walkAction(g, m, i)
    })
  }

  static _runAction(globals, monster, index) {
    if (index == 0) {
      if (Engine.random() < 0.2) {
        Engine.playSound(monster, Channels.VOICE, "shalrath/idle.wav", 1, Attenuations.IDLE)
      }
    }
    var speed = (index < _RUN_SPEEDS.count) ? _RUN_SPEEDS[index] : 0
    AIModule.ai_run(globals, monster, speed)
  }

  static shal_run1(globals, monster) {
    monster.set("_shalRunIndex", 0)
    VoreModule._shalRunAdvance(globals, monster)
  }

  static _shalRunAdvance(globals, monster) {
    VoreModule._advanceSequence(globals, monster, _RUN_FRAMES, "_shalRunIndex", "VoreModule._shalRunAdvance", "VoreModule.shal_run1", Fn.new { |g, m, i|
      VoreModule._runAction(g, m, i)
    })
  }

  static _attackAction(globals, monster, index) {
    if (index == 0) {
      Engine.playSound(monster, Channels.VOICE, "shalrath/attack.wav", 1, Attenuations.NORMAL)
    }
    if (index < 8) {
      VoreModule._faceEnemy(globals, monster)
    } else if (index == 8) {
      VoreModule._faceEnemy(globals, monster)
      VoreModule.ShalMissile(globals, monster)
    } else if (index == 9) {
      VoreModule._faceEnemy(globals, monster)
    }
  }

  static shal_attack1(globals, monster) {
    monster.set("_shalAttackIndex", 0)
    VoreModule._shalAttackAdvance(globals, monster)
  }

  static _shalAttackAdvance(globals, monster) {
    VoreModule._advanceSequence(globals, monster, _ATTACK_FRAMES, "_shalAttackIndex", "VoreModule._shalAttackAdvance", "VoreModule.shal_run1", Fn.new { |g, m, i|
      VoreModule._attackAction(g, m, i)
    })
  }

  static _painAction(globals, monster, index) {
    // Pain sequence simply plays through then returns to run.
  }

  static shal_pain1(globals, monster) {
    monster.set("_shalPainIndex", 0)
    VoreModule._shalPainAdvance(globals, monster)
  }

  static _shalPainAdvance(globals, monster) {
    VoreModule._advanceSequence(globals, monster, _PAIN_FRAMES, "_shalPainIndex", "VoreModule._shalPainAdvance", "VoreModule.shal_run1", Fn.new { |g, m, i|
      if (i == 0) {
        // pain frames don't move; just hold pose
      }
    })
  }

  static _deathAction(globals, monster, index) {
    if (index == 0) {
      Engine.playSound(monster, Channels.VOICE, "shalrath/death.wav", 1, Attenuations.NORMAL)
    }
  }

  static shal_death1(globals, monster) {
    monster.set("_shalDeathIndex", 0)
    VoreModule._shalDeathAdvance(globals, monster)
  }

  static _shalDeathAdvance(globals, monster) {
    VoreModule._advanceSequence(globals, monster, _DEATH_FRAMES, "_shalDeathIndex", "VoreModule._shalDeathAdvance", "VoreModule.shal_death7", Fn.new { |g, m, i|
      VoreModule._deathAction(g, m, i)
    })
  }

  static shal_death7(globals, monster) {
    VoreModule._setFrame(globals, monster, "death7", "VoreModule.shal_death7", 0.1)
  }

  static shalrath_pain(globals, monster, attacker, damage) {
    if (monster == null) return
    var painFinished = monster.get("pain_finished", 0.0)
    if (painFinished > globals.time) return

    Engine.playSound(monster, Channels.VOICE, "shalrath/pain.wav", 1, Attenuations.NORMAL)
    monster.set("pain_finished", globals.time + 3.0)
    VoreModule.shal_pain1(globals, monster)
  }

  static shalrath_die(globals, monster) {
    if (monster == null) return

    if (monster.get("health", 0) < -90) {
      Engine.playSound(monster, Channels.VOICE, "player/udeath.wav", 1, Attenuations.NORMAL)
      PlayerModule.ThrowHead(globals, monster, "progs/h_shal.mdl", monster.get("health", 0))
      PlayerModule.ThrowGib(globals, monster, "progs/gib1.mdl", monster.get("health", 0))
      PlayerModule.ThrowGib(globals, monster, "progs/gib2.mdl", monster.get("health", 0))
      PlayerModule.ThrowGib(globals, monster, "progs/gib3.mdl", monster.get("health", 0))
      return
    }

    monster.set("solid", SolidTypes.NOT)
    VoreModule.shal_death1(globals, monster)
  }

  static ShalMissile(globals, monster) {
    if (monster == null) return
    var enemy = monster.get("enemy", null)
    if (enemy == null) return

    var origin = monster.get("origin", [0, 0, 0])
    var enemyOrigin = enemy.get("origin", [0, 0, 0])
    var dir = VoreModule._vectorNormalize(VoreModule._vectorSub(VoreModule._vectorAdd(enemyOrigin, [0, 0, 10]), origin))
    var distance = VoreModule._vectorLength(VoreModule._vectorSub(enemyOrigin, origin))
    var flyTime = distance * 0.002
    if (flyTime < 0.1) flyTime = 0.1

    var effects = monster.get("effects", 0)
    effects = Engine.bitOr(effects, Effects.MUZZLEFLASH)
    monster.set("effects", effects)

    Engine.playSound(monster, Channels.WEAPON, "shalrath/attack2.wav", 1, Attenuations.NORMAL)

    var spawnOrigin = VoreModule._vectorAdd(origin, [0, 0, 10])
    var missile = WeaponsModule.launch_spike(globals, monster, spawnOrigin, dir)
    if (missile == null) return

    missile.set("classname", "vore_ball")
    missile.set("owner", monster)
    missile.set("solid", SolidTypes.BBOX)
    missile.set("movetype", MoveTypes.FLYMISSILE)
    Engine.setModel(missile, "progs/v_spike.mdl")
    Engine.setSize(missile, [0, 0, 0], [0, 0, 0])
    Engine.setOrigin(missile, spawnOrigin)

    missile.set("velocity", VoreModule._vectorScale(dir, 400))
    missile.set("avelocity", [300, 300, 300])
    missile.set("enemy", enemy)
    missile.set("touch", "VoreModule.ShalMissileTouch")

    var thinkTime = globals.time + flyTime
    missile.set("think", "VoreModule.ShalHome")
    missile.set("nextthink", thinkTime)
    Engine.scheduleThink(missile, "VoreModule.ShalHome", flyTime)
  }

  static ShalHome(globals, missile) {
    if (missile == null) return
    var enemy = missile.get("enemy", null)
    if (enemy == null || enemy.get("health", 0) < 1) {
      Engine.removeEntity(missile)
      return
    }

    var target = VoreModule._vectorAdd(enemy.get("origin", [0, 0, 0]), [0, 0, 10])
    var direction = VoreModule._vectorNormalize(VoreModule._vectorSub(target, missile.get("origin", [0, 0, 0])))
    missile.set("velocity", VoreModule._vectorScale(direction, 250))

    var delay = 0.2
    missile.set("think", "VoreModule.ShalHome")
    missile.set("nextthink", Engine.time() + delay)
    Engine.scheduleThink(missile, "VoreModule.ShalHome", delay)
  }

  static ShalMissileTouch(globals, missile, other) {
    if (missile == null) return
    if (other == missile.get("owner", null)) return

    if (other != null && other.get("classname", "") == "monster_zombie") {
      CombatModule.tDamage(globals, other, missile, missile, 110)
    }

    CombatModule.tRadiusDamage(globals, missile, missile.get("owner", missile), 40, globals.world)
    Engine.playSound(missile, Channels.WEAPON, "weapons/r_exp3.wav", 1, Attenuations.NORMAL)

    Engine.emitTempEntity(TempEntityCodes.EXPLOSION, {
      "origin": missile.get("origin", [0, 0, 0])
    })

    missile.set("velocity", [0, 0, 0])
    missile.set("touch", "SubsModule.subNull")
    WeaponsModule.BecomeExplosion(globals, missile)
  }

  static monster_shalrath(globals, monster) {
    if (globals.deathmatch != 0) {
      Engine.removeEntity(monster)
      return
    }

    Engine.precacheModel2("progs/shalrath.mdl")
    Engine.precacheModel2("progs/h_shal.mdl")
    Engine.precacheModel2("progs/v_spike.mdl")

    Engine.precacheSound2("shalrath/attack.wav")
    Engine.precacheSound2("shalrath/attack2.wav")
    Engine.precacheSound2("shalrath/death.wav")
    Engine.precacheSound2("shalrath/idle.wav")
    Engine.precacheSound2("shalrath/pain.wav")
    Engine.precacheSound2("shalrath/sight.wav")

    monster.set("solid", SolidTypes.SLIDEBOX)
    monster.set("movetype", MoveTypes.STEP)
    Engine.setModel(monster, "progs/shalrath.mdl")

    monster.set("noise", "shalrath/sight.wav")
    monster.set("netname", "$qc_vore")
    monster.set("killstring", "$qc_ks_vore")

    Engine.setSize(monster, [-32, -32, -24], [32, 32, 48])
    monster.set("health", 400)
    monster.set("max_health", 400)

    monster.set("th_stand", "VoreModule.shal_stand")
    monster.set("th_walk", "VoreModule.shal_walk1")
    monster.set("th_run", "VoreModule.shal_run1")
    monster.set("th_die", "VoreModule.shalrath_die")
    monster.set("th_pain", "VoreModule.shalrath_pain")
    monster.set("th_missile", "VoreModule.shal_attack1")
    monster.set("allowPathFind", true)
    monster.set("combat_style", CombatStyles.RANGED)

    MonstersModule.walkmonster_start(globals, monster)
  }
}
