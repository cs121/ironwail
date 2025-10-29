// Spawn.wren
// Ports the tarbaby (spawn) monster behavior from spawn.qc so the Wren
// gameplay layer can drive it without falling back to QuakeC.

import "./Engine" for Engine
import "./Globals" for SolidTypes, MoveTypes, DamageValues, Channels, Attenuations, PlayerFlags, CombatStyles, TempEntityCodes
import "./AI" for AIModule
import "./Fight" for FightModule
import "./Monsters" for MonstersModule
import "./Combat" for CombatModule
import "./Weapons" for WeaponsModule
import "./Subs" for SubsModule

var _STAND_FRAME = "walk1"
var _WALK_FRAMES = [
  "walk1",
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
  "walk13",
  "walk14",
  "walk15",
  "walk16",
  "walk17",
  "walk18",
  "walk19",
  "walk20",
  "walk21",
  "walk22",
  "walk23",
  "walk24",
  "walk25"
]
var _RUN_FRAMES = [
  "run1",
  "run2",
  "run3",
  "run4",
  "run5",
  "run6",
  "run7",
  "run8",
  "run9",
  "run10",
  "run11",
  "run12",
  "run13",
  "run14",
  "run15",
  "run16",
  "run17",
  "run18",
  "run19",
  "run20",
  "run21",
  "run22",
  "run23",
  "run24",
  "run25"
]
var _FLY_FRAMES = ["fly1", "fly2", "fly3", "fly4"]
var _JUMP_FRAMES = ["jump1", "jump2", "jump3", "jump4", "jump5", "jump6"]

var _WALK_ACTIONS = []
for (i in 0...10) {
  _WALK_ACTIONS.add(Fn.new { |g, m, i| AIModule.ai_turn(g, m) })
}
for (i in 0...15) {
  _WALK_ACTIONS.add(Fn.new { |g, m, i| AIModule.ai_walk(g, m, 2) })
}

var _RUN_ACTIONS = []
for (i in 0...10) {
  _RUN_ACTIONS.add(Fn.new { |g, m, i| SpawnModule._faceEnemy(g, m) })
}
for (i in 0...15) {
  _RUN_ACTIONS.add(Fn.new { |g, m, i| AIModule.ai_run(g, m, 2) })
}

class SpawnModule {
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
    var length = SpawnModule._vectorLength(v)
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
    var delta = SpawnModule._vectorSub(enemy.get("origin", [0, 0, 0]), monster.get("origin", [0, 0, 0]))
    var angles = Engine.vectorToAngles(delta)
    return angles[1]
  }

  static _faceEnemy(globals, monster) {
    FightModule.ai_face(globals, monster, SpawnModule._enemyYaw(monster))
  }

  static _setFrame(globals, monster, frame, nextFunction, delay) {
    MonstersModule.setFrame(globals, monster, frame, nextFunction, delay)
  }

  static _advanceSequence(globals, monster, frames, actions, stateField, advanceName, restartName, delay) {
    if (monster == null) return
    if (frames == null || frames.count == 0) return

    var index = monster.get(stateField, 0)
    if (index < 0 || index >= frames.count) index = 0

    var frame = frames[index]
    var action = null
    if (actions != null && index < actions.count) {
      action = actions[index]
    }

    var nextIndex = index + 1
    var nextFunction = advanceName
    if (nextIndex >= frames.count) {
      nextIndex = 0
      nextFunction = restartName
    }
    monster.set(stateField, nextIndex)

    var thinkDelay = delay == null ? 0.1 : delay
    SpawnModule._setFrame(globals, monster, frame, nextFunction, thinkDelay)

    if (action != null) {
      action.call(globals, monster, index)
    }
  }

  static tbaby_stand1(globals, monster) {
    SpawnModule._setFrame(globals, monster, _STAND_FRAME, "SpawnModule.tbaby_stand1", 0.1)
    AIModule.ai_stand(globals, monster)
  }

  static tbaby_hang1(globals, monster) {
    SpawnModule._setFrame(globals, monster, _STAND_FRAME, "SpawnModule.tbaby_hang1", 0.1)
    AIModule.ai_stand(globals, monster)
  }

  static tbaby_walk1(globals, monster) {
    monster.set("_tbabyWalkIndex", 0)
    SpawnModule._tbabyWalkAdvance(globals, monster)
  }

  static _tbabyWalkAdvance(globals, monster) {
    SpawnModule._advanceSequence(globals, monster, _WALK_FRAMES, _WALK_ACTIONS, "_tbabyWalkIndex", "SpawnModule._tbabyWalkAdvance", "SpawnModule.tbaby_walk1", 0.1)
  }

  static tbaby_run1(globals, monster) {
    monster.set("_tbabyRunIndex", 0)
    SpawnModule._tbabyRunAdvance(globals, monster)
  }

  static _tbabyRunAdvance(globals, monster) {
    SpawnModule._advanceSequence(globals, monster, _RUN_FRAMES, _RUN_ACTIONS, "_tbabyRunIndex", "SpawnModule._tbabyRunAdvance", "SpawnModule.tbaby_run1", 0.1)
  }

  static _jumpFrame(globals, monster, index, nextName, action) {
    if (index < 0 || index >= _JUMP_FRAMES.count) return
    SpawnModule._setFrame(globals, monster, _JUMP_FRAMES[index], nextName, 0.1)
    if (action != null) action.call(globals, monster)
  }

  static tbaby_jump1(globals, monster) {
    SpawnModule._jumpFrame(globals, monster, 0, "SpawnModule.tbaby_jump2", Fn.new { |g, m| SpawnModule._faceEnemy(g, m) })
  }

  static tbaby_jump2(globals, monster) {
    SpawnModule._jumpFrame(globals, monster, 1, "SpawnModule.tbaby_jump3", Fn.new { |g, m| SpawnModule._faceEnemy(g, m) })
  }

  static tbaby_jump3(globals, monster) {
    SpawnModule._jumpFrame(globals, monster, 2, "SpawnModule.tbaby_jump4", Fn.new { |g, m| SpawnModule._faceEnemy(g, m) })
  }

  static tbaby_jump4(globals, monster) {
    SpawnModule._jumpFrame(globals, monster, 3, "SpawnModule.tbaby_jump5", Fn.new { |g, m| SpawnModule._faceEnemy(g, m) })
  }

  static tbaby_jump5(globals, monster) {
    SpawnModule._jumpFrame(globals, monster, 4, "SpawnModule.tbaby_jump6", Fn.new { |g, m|
      m.set("movetype", MoveTypes.BOUNCE)
      m.set("touch", "SpawnModule.tar_jump_touch")

      var origin = m.get("origin", [0, 0, 0])
      origin[2] = origin[2] + 1
      Engine.setOrigin(m, origin)

      var vectors = SpawnModule._makeVectors(m.get("angles", [0, 0, 0]))
      var forward = vectors["forward"]
      var velocity = SpawnModule._vectorAdd(SpawnModule._vectorScale(forward, 600), [0, 0, 200])
      velocity[2] = velocity[2] + Engine.random() * 150
      m.set("velocity", velocity)

      var flags = m.get("flags", 0)
      flags = flags - Engine.bitAnd(flags, PlayerFlags.ONGROUND)
      m.set("flags", flags)
      m.set("cnt", 0)
    })
  }

  static tbaby_jump6(globals, monster) {
    SpawnModule._jumpFrame(globals, monster, 5, "SpawnModule.tbaby_fly1", null)
  }

  static _flyFrame(globals, monster, index, nextName) {
    if (index < 0 || index >= _FLY_FRAMES.count) return
    SpawnModule._setFrame(globals, monster, _FLY_FRAMES[index], nextName, 0.1)
  }

  static tbaby_fly1(globals, monster) {
    SpawnModule._flyFrame(globals, monster, 0, "SpawnModule.tbaby_fly2")
  }

  static tbaby_fly2(globals, monster) {
    SpawnModule._flyFrame(globals, monster, 1, "SpawnModule.tbaby_fly3")
  }

  static tbaby_fly3(globals, monster) {
    SpawnModule._flyFrame(globals, monster, 2, "SpawnModule.tbaby_fly4")
  }

  static tbaby_fly4(globals, monster) {
    SpawnModule._flyFrame(globals, monster, 3, "SpawnModule.tbaby_fly1")
    var count = monster.get("cnt", 0) + 1
    monster.set("cnt", count)
    if (count == 4) {
      SpawnModule.tbaby_jump5(globals, monster)
    }
  }

  static tar_jump_touch(globals, monster, other) {
    if (monster == null) return
    if (other != null && other.get("takedamage", DamageValues.NO) != DamageValues.NO && other.get("classname", "") != monster.get("classname", "")) {
      var velocity = monster.get("velocity", [0, 0, 0])
      if (SpawnModule._vectorLength(velocity) > 400) {
        var damage = 10 + 10 * Engine.random()
        CombatModule.tDamage(globals, other, monster, monster, damage)
        Engine.playSound(monster, Channels.WEAPON, "blob/hit1.wav", 1, Attenuations.NORMAL)
      }
    } else {
      Engine.playSound(monster, Channels.WEAPON, "blob/land1.wav", 1, Attenuations.NORMAL)
    }

    if (!Engine.checkBottom(monster)) {
      var flags = monster.get("flags", 0)
      if (Engine.bitAnd(flags, PlayerFlags.ONGROUND) != 0) {
        monster.set("touch", "SubsModule.subNull")
        monster.set("think", "SpawnModule.tbaby_run1")
        monster.set("nextthink", globals.time + 0.1)
        Engine.scheduleThink(monster, "SpawnModule.tbaby_run1", 0.1)
      }
      return
    }

    monster.set("touch", "SubsModule.subNull")
    monster.set("think", "SpawnModule.tbaby_jump1")
    monster.set("nextthink", globals.time + 0.1)
    Engine.scheduleThink(monster, "SpawnModule.tbaby_jump1", 0.1)
  }

  static tbaby_die1(globals, monster) {
    SpawnModule._setFrame(globals, monster, "exp", "SpawnModule.tbaby_die2", 0.1)
    monster.set("takedamage", DamageValues.NO)
  }

  static tbaby_die2(globals, monster) {
    SpawnModule._setFrame(globals, monster, "exp", "SpawnModule.tbaby_run1", 0.1)
    CombatModule.tRadiusDamage(globals, monster, monster, 120, globals.world)
    Engine.playSound(monster, Channels.VOICE, "blob/death1.wav", 1, Attenuations.NORMAL)

    var origin = monster.get("origin", [0, 0, 0])
    var velocity = monster.get("velocity", [0, 0, 0])
    var offset = SpawnModule._vectorScale(SpawnModule._vectorNormalize(velocity), 8)
    origin = SpawnModule._vectorSub(origin, offset)
    Engine.setOrigin(monster, origin)

    Engine.emitTempEntity(TempEntityCodes.TAREXPLOSION, {
      "origin": origin
    })

    WeaponsModule.BecomeExplosion(globals, monster)
  }

  static monster_tarbaby(globals, monster) {
    if (globals.deathmatch != 0) {
      Engine.removeEntity(monster)
      return
    }

    Engine.precacheModel2("progs/tarbaby.mdl")
    Engine.precacheSound2("blob/death1.wav")
    Engine.precacheSound2("blob/hit1.wav")
    Engine.precacheSound2("blob/land1.wav")
    Engine.precacheSound2("blob/sight1.wav")

    monster.set("solid", SolidTypes.SLIDEBOX)
    monster.set("movetype", MoveTypes.STEP)
    Engine.setModel(monster, "progs/tarbaby.mdl")

    monster.set("noise", "blob/sight1.wav")
    monster.set("netname", "$qc_spawn")
    monster.set("killstring", "$qc_ks_spawn")

    Engine.setSize(monster, [-16, -16, -24], [16, 16, 40])
    monster.set("health", 80)
    monster.set("max_health", 80)

    monster.set("th_stand", "SpawnModule.tbaby_stand1")
    monster.set("th_walk", "SpawnModule.tbaby_walk1")
    monster.set("th_run", "SpawnModule.tbaby_run1")
    monster.set("th_missile", "SpawnModule.tbaby_jump1")
    monster.set("th_melee", "SpawnModule.tbaby_jump1")
    monster.set("th_die", "SpawnModule.tbaby_die1")
    monster.set("allowPathFind", true)
    monster.set("combat_style", CombatStyles.MELEE)

    MonstersModule.walkmonster_start(globals, monster)
  }
}
