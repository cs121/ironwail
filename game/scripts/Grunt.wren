// Grunt.wren
// Ports the grunt (soldier) monster from grunt.qc to run entirely inside the
// Wren gameplay runtime.

import "./Engine" for Engine
import "./Globals" for Channels, Attenuations, SolidTypes, MoveTypes, Effects, CombatStyles
import "./Monsters" for MonstersModule
import "./AI" for AIModule
import "./Fight" for FightModule
import "./Subs" for SubsModule
import "./Weapons" for WeaponsModule
import "./Items" for ItemsModule
import "./Player" for PlayerModule

var _STAND_FRAMES = [
  "stand1",
  "stand2",
  "stand3",
  "stand4",
  "stand5",
  "stand6",
  "stand7",
  "stand8"
]

var _WALK_FRAMES = [
  "prowl_1",
  "prowl_2",
  "prowl_3",
  "prowl_4",
  "prowl_5",
  "prowl_6",
  "prowl_7",
  "prowl_8",
  "prowl_9",
  "prowl_10",
  "prowl_11",
  "prowl_12",
  "prowl_13",
  "prowl_14",
  "prowl_15",
  "prowl_16",
  "prowl_17",
  "prowl_18",
  "prowl_19",
  "prowl_20",
  "prowl_21",
  "prowl_22",
  "prowl_23",
  "prowl_24"
]

var _WALK_SPEEDS = [
  1,
  1,
  1,
  1,
  2,
  3,
  4,
  4,
  2,
  2,
  2,
  1,
  0,
  1,
  1,
  1,
  3,
  3,
  3,
  3,
  2,
  1,
  1,
  1
]

var _RUN_FRAMES = [
  "run1",
  "run2",
  "run3",
  "run4",
  "run5",
  "run6",
  "run7",
  "run8"
]

var _RUN_SPEEDS = [11, 15, 10, 10, 8, 15, 10, 8]

var _ATTACK_FRAMES = [
  "shoot1",
  "shoot2",
  "shoot3",
  "shoot4",
  "shoot5",
  "shoot6",
  "shoot7",
  "shoot8",
  "shoot9"
]

var _PAIN_SHORT_FRAMES = ["pain1", "pain2", "pain3", "pain4", "pain5", "pain6"]

var _PAIN_MEDIUM_FRAMES = [
  "painb1",
  "painb2",
  "painb3",
  "painb4",
  "painb5",
  "painb6",
  "painb7",
  "painb8",
  "painb9",
  "painb10",
  "painb11",
  "painb12",
  "painb13",
  "painb14"
]

var _PAIN_MEDIUM_ACTIONS = [
  null,
  Fn.new { |g, m| AIModule.ai_painforward(g, m, 13) },
  Fn.new { |g, m| AIModule.ai_painforward(g, m, 9) },
  null,
  null,
  null,
  null,
  null,
  null,
  null,
  null,
  Fn.new { |g, m| AIModule.ai_pain(g, m, 2) },
  null,
  null
]

var _PAIN_LONG_FRAMES = [
  "painc1",
  "painc2",
  "painc3",
  "painc4",
  "painc5",
  "painc6",
  "painc7",
  "painc8",
  "painc9",
  "painc10",
  "painc11",
  "painc12",
  "painc13"
]

var _PAIN_LONG_ACTIONS = [
  null,
  Fn.new { |g, m| AIModule.ai_pain(g, m, 1) },
  null,
  null,
  Fn.new { |g, m| AIModule.ai_painforward(g, m, 1) },
  Fn.new { |g, m| AIModule.ai_painforward(g, m, 1) },
  null,
  Fn.new { |g, m| AIModule.ai_pain(g, m, 1) },
  Fn.new { |g, m| AIModule.ai_painforward(g, m, 4) },
  Fn.new { |g, m| AIModule.ai_painforward(g, m, 3) },
  Fn.new { |g, m| AIModule.ai_painforward(g, m, 6) },
  Fn.new { |g, m| AIModule.ai_painforward(g, m, 8) },
  null
]

var _DEATH_A_FRAMES = [
  "death1",
  "death2",
  "death3",
  "death4",
  "death5",
  "death6",
  "death7",
  "death8",
  "death9",
  "death10"
]

var _DEATH_A_ACTIONS = [
  null,
  null,
  Fn.new { |g, m|
    m.set("solid", SolidTypes.NOT)
    m.set("ammo_shells", 5)
    ItemsModule.DropBackpack(g, m)
  },
  null,
  null,
  null,
  null,
  null,
  null,
  null
]

var _DEATH_B_FRAMES = [
  "deathc1",
  "deathc2",
  "deathc3",
  "deathc4",
  "deathc5",
  "deathc6",
  "deathc7",
  "deathc8",
  "deathc9",
  "deathc10",
  "deathc11"
]

var _DEATH_B_ACTIONS = [
  null,
  Fn.new { |g, m| AIModule.ai_back(g, m, 5) },
  Fn.new { |g, m|
    m.set("solid", SolidTypes.NOT)
    m.set("ammo_shells", 5)
    ItemsModule.DropBackpack(g, m)
    AIModule.ai_back(g, m, 4)
  },
  Fn.new { |g, m| AIModule.ai_back(g, m, 13) },
  Fn.new { |g, m| AIModule.ai_back(g, m, 3) },
  Fn.new { |g, m| AIModule.ai_back(g, m, 4) },
  null,
  null,
  null,
  null,
  null
]

class GruntModule {
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
    var length = GruntModule._vectorLength(v)
    if (length == 0) return [0, 0, 0]
    var inv = 1 / length
    return [v[0] * inv, v[1] * inv, v[2] * inv]
  }

  static _enemyYaw(monster) {
    var enemy = monster.get("enemy", null)
    if (enemy == null) return monster.get("angles", [0, 0, 0])[1]
    var delta = GruntModule._vectorSub(enemy.get("origin", [0, 0, 0]), monster.get("origin", [0, 0, 0]))
    return Engine.vectorToAngles(delta)[1]
  }

  static _setFrame(globals, monster, frame, nextThink, delay) {
    MonstersModule.setFrame(globals, monster, frame, nextThink, delay)
  }

  static _loopSequence(globals, monster, frames, indexField, nextFunction, actionFn) {
    if (monster == null) return
    if (frames == null || frames.count == 0) return

    var index = monster.get(indexField, 0)
    if (index < 0 || index >= frames.count) index = 0

    GruntModule._setFrame(globals, monster, frames[index], nextFunction, 0.1)
    if (actionFn != null) actionFn.call(index)

    if (index >= frames.count - 1) {
      monster.set(indexField, 0)
    } else {
      monster.set(indexField, index + 1)
    }
  }

  static _advanceSequence(globals, monster, frames, actions, indexField, advanceFunction, completionFunction) {
    if (monster == null) return
    if (frames == null || frames.count == 0) return

    var index = monster.get(indexField, 0)
    if (index < 0 || index >= frames.count) index = 0

    var nextName = advanceFunction
    if (index >= frames.count - 1) {
      nextName = completionFunction
    }

    var delay = nextName == null ? null : 0.1
    GruntModule._setFrame(globals, monster, frames[index], nextName, delay)

    if (actions != null && index < actions.count) {
      var action = actions[index]
      if (action != null) action.call(globals, monster)
    }

    if (index >= frames.count - 1) {
      monster.set(indexField, 0)
    } else {
      monster.set(indexField, index + 1)
    }
  }

  static _playIdle(globals, monster, index) {
    if (index == 0 && Engine.random() < 0.2) {
      Engine.playSound(monster, Channels.VOICE, "soldier/idle.wav", 1, Attenuations.IDLE)
    }
  }

  static _walkAction(globals, monster, index) {
    GruntModule._playIdle(globals, monster, index)
    if (index < 0 || index >= _WALK_SPEEDS.count) return
    AIModule.ai_walk(globals, monster, _WALK_SPEEDS[index])
  }

  static _runAction(globals, monster, index) {
    GruntModule._playIdle(globals, monster, index)
    if (index < 0 || index >= _RUN_SPEEDS.count) return
    AIModule.ai_run(globals, monster, _RUN_SPEEDS[index])
  }

  static _faceEnemy(globals, monster) {
    FightModule.ai_face(globals, monster, GruntModule._enemyYaw(monster))
  }

  static _armyFire(globals, monster) {
    var enemy = monster.get("enemy", null)
    if (enemy == null) return

    var enemyOrigin = enemy.get("origin", [0, 0, 0])
    var enemyVelocity = enemy.get("velocity", [0, 0, 0])
    var predicted = GruntModule._vectorSub(enemyOrigin, GruntModule._vectorScale(enemyVelocity, 0.2))
    var dir = GruntModule._vectorNormalize(GruntModule._vectorSub(predicted, monster.get("origin", [0, 0, 0])))

    Engine.playSound(monster, Channels.WEAPON, "soldier/sattck1.wav", 1, Attenuations.NORMAL)
    WeaponsModule.FireBullets(globals, monster, 4, dir, [0.1, 0.1, 0.0])

    var effects = monster.get("effects", 0)
    effects = Engine.bitOr(effects, Effects.MUZZLEFLASH)
    monster.set("effects", effects)
  }

  static _resumeRun(globals, monster) {
    monster.set("_gruntRunIndex", 0)
    GruntModule.army_run1(globals, monster)
  }

  static army_stand1(globals, monster) {
    GruntModule._loopSequence(globals, monster, _STAND_FRAMES, "_gruntStandIndex", "GruntModule.army_stand1", Fn.new { |i|
      AIModule.ai_stand(globals, monster)
    })
  }

  static army_walk1(globals, monster) {
    GruntModule._loopSequence(globals, monster, _WALK_FRAMES, "_gruntWalkIndex", "GruntModule.army_walk1", Fn.new { |i|
      GruntModule._walkAction(globals, monster, i)
    })
  }

  static army_run1(globals, monster) {
    GruntModule._loopSequence(globals, monster, _RUN_FRAMES, "_gruntRunIndex", "GruntModule.army_run1", Fn.new { |i|
      GruntModule._runAction(globals, monster, i)
    })
  }

  static army_atk1(globals, monster) {
    monster.set("_gruntAttackIndex", 0)
    GruntModule._army_attackAdvance(globals, monster)
  }

  static _army_attackAdvance(globals, monster) {
    var actions = [
      Fn.new { |g, m| GruntModule._faceEnemy(g, m) },
      Fn.new { |g, m| GruntModule._faceEnemy(g, m) },
      Fn.new { |g, m| GruntModule._faceEnemy(g, m) },
      Fn.new { |g, m| GruntModule._faceEnemy(g, m) },
      Fn.new { |g, m|
        GruntModule._faceEnemy(g, m)
        GruntModule._armyFire(g, m)
      },
      Fn.new { |g, m| GruntModule._faceEnemy(g, m) },
      Fn.new { |g, m|
        GruntModule._faceEnemy(g, m)
        SubsModule.SUB_CheckRefire(g, m, "GruntModule.army_atk1")
      },
      Fn.new { |g, m| GruntModule._faceEnemy(g, m) },
      Fn.new { |g, m| GruntModule._faceEnemy(g, m) }
    ]
    GruntModule._advanceSequence(globals, monster, _ATTACK_FRAMES, actions, "_gruntAttackIndex", "GruntModule._army_attackAdvance", "GruntModule.army_run1")
  }

  static army_pain(globals, monster, attacker, damage) {
    if (monster.get("pain_finished", 0.0) > globals.time) return

    var choice = Engine.random()
    if (choice < 0.2) {
      monster.set("_gruntPainShortIndex", 0)
      monster.set("pain_finished", globals.time + 0.6)
      Engine.playSound(monster, Channels.VOICE, "soldier/pain1.wav", 1, Attenuations.NORMAL)
      GruntModule._army_painShortAdvance(globals, monster)
    } else if (choice < 0.6) {
      monster.set("_gruntPainMediumIndex", 0)
      monster.set("pain_finished", globals.time + 1.1)
      Engine.playSound(monster, Channels.VOICE, "soldier/pain2.wav", 1, Attenuations.NORMAL)
      GruntModule._army_painMediumAdvance(globals, monster)
    } else {
      monster.set("_gruntPainLongIndex", 0)
      monster.set("pain_finished", globals.time + 1.1)
      Engine.playSound(monster, Channels.VOICE, "soldier/pain2.wav", 1, Attenuations.NORMAL)
      GruntModule._army_painLongAdvance(globals, monster)
    }
  }

  static _army_painShortAdvance(globals, monster) {
    var actions = [
      null,
      null,
      null,
      null,
      null,
      Fn.new { |g, m| AIModule.ai_pain(g, m, 1) }
    ]
    GruntModule._advanceSequence(globals, monster, _PAIN_SHORT_FRAMES, actions, "_gruntPainShortIndex", "GruntModule._army_painShortAdvance", "GruntModule.army_run1")
  }

  static _army_painMediumAdvance(globals, monster) {
    GruntModule._advanceSequence(globals, monster, _PAIN_MEDIUM_FRAMES, _PAIN_MEDIUM_ACTIONS, "_gruntPainMediumIndex", "GruntModule._army_painMediumAdvance", "GruntModule.army_run1")
  }

  static _army_painLongAdvance(globals, monster) {
    GruntModule._advanceSequence(globals, monster, _PAIN_LONG_FRAMES, _PAIN_LONG_ACTIONS, "_gruntPainLongIndex", "GruntModule._army_painLongAdvance", "GruntModule.army_run1")
  }

  static army_die(globals, monster) {
    var health = monster.get("health", 0)
    if (health < -35) {
      Engine.playSound(monster, Channels.VOICE, "player/udeath.wav", 1, Attenuations.NORMAL)
      PlayerModule.ThrowHead(globals, monster, "progs/h_guard.mdl", health)
      PlayerModule.ThrowGib(globals, monster, "progs/gib1.mdl", health)
      PlayerModule.ThrowGib(globals, monster, "progs/gib2.mdl", health)
      PlayerModule.ThrowGib(globals, monster, "progs/gib3.mdl", health)
      return
    }

    Engine.playSound(monster, Channels.VOICE, "soldier/death1.wav", 1, Attenuations.NORMAL)
    if (Engine.random() < 0.5) {
      monster.set("_gruntDeathAIndex", 0)
      GruntModule._army_deathAAdvance(globals, monster)
    } else {
      monster.set("_gruntDeathBIndex", 0)
      GruntModule._army_deathBAdvance(globals, monster)
    }
  }

  static _army_deathAAdvance(globals, monster) {
    GruntModule._advanceSequence(globals, monster, _DEATH_A_FRAMES, _DEATH_A_ACTIONS, "_gruntDeathAIndex", "GruntModule._army_deathAAdvance", null)
  }

  static _army_deathBAdvance(globals, monster) {
    GruntModule._advanceSequence(globals, monster, _DEATH_B_FRAMES, _DEATH_B_ACTIONS, "_gruntDeathBIndex", "GruntModule._army_deathBAdvance", null)
  }

  static monster_army(globals, monster) {
    if (globals.deathmatch != 0) {
      Engine.removeEntity(monster)
      return
    }

    Engine.precacheModel("progs/soldier.mdl")
    Engine.precacheModel("progs/h_guard.mdl")
    Engine.precacheModel("progs/gib1.mdl")
    Engine.precacheModel("progs/gib2.mdl")
    Engine.precacheModel("progs/gib3.mdl")

    Engine.precacheSound("soldier/death1.wav")
    Engine.precacheSound("soldier/idle.wav")
    Engine.precacheSound("soldier/pain1.wav")
    Engine.precacheSound("soldier/pain2.wav")
    Engine.precacheSound("soldier/sattck1.wav")
    Engine.precacheSound("soldier/sight1.wav")
    Engine.precacheSound("player/udeath.wav")

    monster.set("solid", SolidTypes.SLIDEBOX)
    monster.set("movetype", MoveTypes.STEP)
    Engine.setModel(monster, "progs/soldier.mdl")

    monster.set("noise", "soldier/sight1.wav")
    monster.set("netname", "$qc_grunt")
    monster.set("killstring", "$qc_ks_grunt")

    Engine.setSize(monster, [-16, -16, -24], [16, 16, 40])
    monster.set("health", 30)
    monster.set("max_health", 30)

    monster.set("th_stand", "GruntModule.army_stand1")
    monster.set("th_walk", "GruntModule.army_walk1")
    monster.set("th_run", "GruntModule.army_run1")
    monster.set("th_missile", "GruntModule.army_atk1")
    monster.set("th_pain", "GruntModule.army_pain")
    monster.set("th_die", "GruntModule.army_die")

    monster.set("allowPathFind", true)
    monster.set("combat_style", CombatStyles.RANGED)

    monster.set("_gruntStandIndex", 0)
    monster.set("_gruntWalkIndex", 0)
    monster.set("_gruntRunIndex", 0)
    monster.set("_gruntAttackIndex", 0)
    monster.set("_gruntPainShortIndex", 0)
    monster.set("_gruntPainMediumIndex", 0)
    monster.set("_gruntPainLongIndex", 0)
    monster.set("_gruntDeathAIndex", 0)
    monster.set("_gruntDeathBIndex", 0)

    MonstersModule.walkmonster_start(globals, monster)
  }
}
