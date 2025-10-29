// Enforcer.wren
// Ports the enforcer monster from enforcer.qc so that the enemy can run inside
// the Wren gameplay runtime without relying on legacy QuakeC shims.

import "./Engine" for Engine
import "./Globals" for Channels, Attenuations, SolidTypes, MoveTypes, Effects, CombatStyles
import "./Monsters" for MonstersModule
import "./AI" for AIModule
import "./Fight" for FightModule
import "./Combat" for CombatModule
import "./Subs" for SubsModule
import "./Misc" for MiscModule
import "./Items" for ItemsModule
import "./Player" for PlayerModule

var _STAND_FRAMES = ["stand1", "stand2", "stand3", "stand4", "stand5", "stand6", "stand7"]

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
  "walk16"
]

var _WALK_SPEEDS = [2, 4, 4, 3, 1, 2, 2, 1, 2, 4, 4, 1, 2, 3, 4, 2]

var _RUN_FRAMES = ["run1", "run2", "run3", "run4", "run5", "run6", "run7", "run8"]
var _RUN_SPEEDS = [18, 14, 7, 12, 14, 14, 7, 11]

var _ATTACK_FRAMES = [
  "attack1",
  "attack2",
  "attack3",
  "attack4",
  "attack5",
  "attack6",
  "attack7",
  "attack8",
  "attack5",
  "attack6",
  "attack7",
  "attack8",
  "attack9",
  "attack10"
]

var _PAIN_A_FRAMES = ["paina1", "paina2", "paina3", "paina4"]
var _PAIN_B_FRAMES = ["painb1", "painb2", "painb3", "painb4", "painb5"]
var _PAIN_C_FRAMES = ["painc1", "painc2", "painc3", "painc4", "painc5", "painc6", "painc7", "painc8"]
var _PAIN_D_FRAMES = [
  "paind1",
  "paind2",
  "paind3",
  "paind4",
  "paind5",
  "paind6",
  "paind7",
  "paind8",
  "paind9",
  "paind10",
  "paind11",
  "paind12",
  "paind13",
  "paind14",
  "paind15",
  "paind16",
  "paind17",
  "paind18",
  "paind19"
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
  "death10",
  "death11",
  "death12",
  "death13",
  "death14"
]

var _DEATH_B_FRAMES = [
  "fdeath1",
  "fdeath2",
  "fdeath3",
  "fdeath4",
  "fdeath5",
  "fdeath6",
  "fdeath7",
  "fdeath8",
  "fdeath9",
  "fdeath10",
  "fdeath11"
]

class EnforcerModule {
  static _vectorAdd(a, b) {
    return [a[0] + b[0], a[1] + b[1], a[2] + b[2]]
  }

  static _vectorSub(a, b) {
    return [a[0] - b[0], a[1] - b[1], a[2] - b[2]]
  }

  static _vectorScale(v, scalar) {
    return [v[0] * scalar, v[1] * scalar, v[2] * scalar]
  }

  static _makeVectors(angles) {
    var adjusted = [-angles[0], angles[1], angles[2]]
    return Engine.makeVectors(adjusted)
  }

  static _enemyYaw(monster) {
    var enemy = monster.get("enemy", null)
    if (enemy == null) return monster.get("angles", [0, 0, 0])[1]
    return Engine.vectorToAngles(EnforcerModule._vectorSub(enemy.get("origin", [0, 0, 0]), monster.get("origin", [0, 0, 0])))[1]
  }

  static _faceEnemy(globals, monster) {
    FightModule.ai_face(globals, monster, EnforcerModule._enemyYaw(monster))
  }

  static _setFrame(globals, monster, frame, nextFunction, delay) {
    MonstersModule.setFrame(globals, monster, frame, nextFunction, delay)
  }

  static _loopSequence(globals, monster, frames, indexField, nextFunction, actionFn) {
    if (monster == null) return
    if (frames == null || frames.count == 0) return

    var index = monster.get(indexField, 0)
    if (index < 0 || index >= frames.count) index = 0

    EnforcerModule._setFrame(globals, monster, frames[index], nextFunction, 0.1)

    if (actionFn != null) actionFn.call(index)

    index = index + 1
    if (index >= frames.count) index = 0
    monster.set(indexField, index)
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
    EnforcerModule._setFrame(globals, monster, frames[index], nextName, delay)

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

  static _maybeIdle(globals, monster) {
    if (Engine.random() < 0.2) {
      Engine.playSound(monster, Channels.VOICE, "enforcer/idle1.wav", 1, Attenuations.IDLE)
    }
  }

  static _enforcerFire(globals, monster) {
    var enemy = monster.get("enemy", null)
    if (enemy == null) return

    var effects = monster.get("effects", 0)
    effects = Engine.bitOr(effects, Effects.MUZZLEFLASH)
    monster.set("effects", effects)

    var vectors = EnforcerModule._makeVectors(monster.get("angles", [0, 0, 0]))
    var forward = (vectors != null && vectors.containsKey("forward")) ? vectors["forward"] : [1, 0, 0]
    var right = (vectors != null && vectors.containsKey("right")) ? vectors["right"] : [0, 1, 0]
    var origin = monster.get("origin", [0, 0, 0])
    var muzzle = EnforcerModule._vectorAdd(origin, EnforcerModule._vectorAdd(EnforcerModule._vectorScale(forward, 30), EnforcerModule._vectorScale(right, 8.5)))
    muzzle = EnforcerModule._vectorAdd(muzzle, [0, 0, 16])

    var direction = EnforcerModule._vectorSub(enemy.get("origin", [0, 0, 0]), origin)
    MiscModule.launchLaser(globals, monster, muzzle, direction)
  }

  static enf_stand1(globals, monster) {
    EnforcerModule._loopSequence(globals, monster, _STAND_FRAMES, "_enforcerStandIndex", "EnforcerModule.enf_stand1", Fn.new { |_|
      AIModule.ai_stand(globals, monster)
    })
  }

  static enf_walk1(globals, monster) {
    EnforcerModule._loopSequence(globals, monster, _WALK_FRAMES, "_enforcerWalkIndex", "EnforcerModule.enf_walk1", Fn.new { |index|
      if (index == 0) {
        EnforcerModule._maybeIdle(globals, monster)
      }
      AIModule.ai_walk(globals, monster, _WALK_SPEEDS[index])
    })
  }

  static enf_run1(globals, monster) {
    EnforcerModule._loopSequence(globals, monster, _RUN_FRAMES, "_enforcerRunIndex", "EnforcerModule.enf_run1", Fn.new { |index|
      if (index == 0) {
        EnforcerModule._maybeIdle(globals, monster)
      }
      AIModule.ai_run(globals, monster, _RUN_SPEEDS[index])
    })
  }

  static _enf_attackAdvance(globals, monster) {
    var actions = [
      Fn.new { |g, m| EnforcerModule._faceEnemy(g, m) },
      Fn.new { |g, m| EnforcerModule._faceEnemy(g, m) },
      Fn.new { |g, m| EnforcerModule._faceEnemy(g, m) },
      Fn.new { |g, m| EnforcerModule._faceEnemy(g, m) },
      Fn.new { |g, m| EnforcerModule._faceEnemy(g, m) },
      Fn.new { |g, m| EnforcerModule._enforcerFire(g, m) },
      Fn.new { |g, m| EnforcerModule._faceEnemy(g, m) },
      Fn.new { |g, m| EnforcerModule._faceEnemy(g, m) },
      Fn.new { |g, m| EnforcerModule._faceEnemy(g, m) },
      Fn.new { |g, m| EnforcerModule._enforcerFire(g, m) },
      Fn.new { |g, m| EnforcerModule._faceEnemy(g, m) },
      Fn.new { |g, m| EnforcerModule._faceEnemy(g, m) },
      Fn.new { |g, m| EnforcerModule._faceEnemy(g, m) },
      Fn.new { |g, m|
        EnforcerModule._faceEnemy(g, m)
        SubsModule.SUB_CheckRefire(g, m, "EnforcerModule.enf_atk1")
      }
    ]

    EnforcerModule._advanceSequence(globals, monster, _ATTACK_FRAMES, actions, "_enforcerAttackIndex", "EnforcerModule._enf_attackAdvance", "EnforcerModule.enf_run1")
  }

  static enf_atk1(globals, monster) {
    monster.set("_enforcerAttackIndex", 0)
    EnforcerModule._enf_attackAdvance(globals, monster)
  }

  static _enf_painAAdvance(globals, monster) {
    EnforcerModule._advanceSequence(globals, monster, _PAIN_A_FRAMES, null, "_enforcerPainAIndex", "EnforcerModule._enf_painAAdvance", "EnforcerModule.enf_run1")
  }

  static _enf_painBAdvance(globals, monster) {
    EnforcerModule._advanceSequence(globals, monster, _PAIN_B_FRAMES, null, "_enforcerPainBIndex", "EnforcerModule._enf_painBAdvance", "EnforcerModule.enf_run1")
  }

  static _enf_painCAdvance(globals, monster) {
    EnforcerModule._advanceSequence(globals, monster, _PAIN_C_FRAMES, null, "_enforcerPainCIndex", "EnforcerModule._enf_painCAdvance", "EnforcerModule.enf_run1")
  }

  static _enf_painDAdvance(globals, monster) {
    var actions = [
      null,
      null,
      null,
      Fn.new { |g, m| AIModule.ai_painforward(g, m, 2) },
      Fn.new { |g, m| AIModule.ai_painforward(g, m, 1) },
      null,
      null,
      null,
      null,
      null,
      Fn.new { |g, m| AIModule.ai_painforward(g, m, 1) },
      Fn.new { |g, m| AIModule.ai_painforward(g, m, 1) },
      Fn.new { |g, m| AIModule.ai_painforward(g, m, 1) },
      null,
      null,
      Fn.new { |g, m| AIModule.ai_pain(g, m, 1) },
      Fn.new { |g, m| AIModule.ai_pain(g, m, 1) },
      null,
      null
    ]

    EnforcerModule._advanceSequence(globals, monster, _PAIN_D_FRAMES, actions, "_enforcerPainDIndex", "EnforcerModule._enf_painDAdvance", "EnforcerModule.enf_run1")
  }

  static enf_pain(globals, monster, attacker, damage) {
    if (monster.get("pain_finished", 0.0) > globals.time) return

    var r = Engine.random()
    if (r < 0.5) {
      Engine.playSound(monster, Channels.VOICE, "enforcer/pain1.wav", 1, Attenuations.NORMAL)
    } else {
      Engine.playSound(monster, Channels.VOICE, "enforcer/pain2.wav", 1, Attenuations.NORMAL)
    }

    if (r < 0.2) {
      monster.set("pain_finished", globals.time + 1)
      monster.set("_enforcerPainAIndex", 0)
      EnforcerModule._enf_painAAdvance(globals, monster)
    } else if (r < 0.4) {
      monster.set("pain_finished", globals.time + 1)
      monster.set("_enforcerPainBIndex", 0)
      EnforcerModule._enf_painBAdvance(globals, monster)
    } else if (r < 0.7) {
      monster.set("pain_finished", globals.time + 1)
      monster.set("_enforcerPainCIndex", 0)
      EnforcerModule._enf_painCAdvance(globals, monster)
    } else {
      monster.set("pain_finished", globals.time + 2)
      monster.set("_enforcerPainDIndex", 0)
      EnforcerModule._enf_painDAdvance(globals, monster)
    }
  }

  static _enf_deathAAdvance(globals, monster) {
    var actions = [
      null,
      null,
      Fn.new { |g, m|
        m.set("solid", SolidTypes.NOT)
        m.set("ammo_cells", 5)
        ItemsModule.DropBackpack(g, m)
      },
      Fn.new { |g, m| AIModule.ai_forward(g, m, 14) },
      Fn.new { |g, m| AIModule.ai_forward(g, m, 2) },
      null,
      null,
      null,
      Fn.new { |g, m| AIModule.ai_forward(g, m, 3) },
      Fn.new { |g, m| AIModule.ai_forward(g, m, 5) },
      Fn.new { |g, m| AIModule.ai_forward(g, m, 5) },
      Fn.new { |g, m| AIModule.ai_forward(g, m, 5) },
      null,
      null
    ]

    EnforcerModule._advanceSequence(globals, monster, _DEATH_A_FRAMES, actions, "_enforcerDeathAIndex", "EnforcerModule._enf_deathAAdvance", null)
  }

  static _enf_deathBAdvance(globals, monster) {
    var actions = [
      null,
      null,
      Fn.new { |g, m|
        m.set("solid", SolidTypes.NOT)
        m.set("ammo_cells", 5)
        ItemsModule.DropBackpack(g, m)
      }
    ]

    EnforcerModule._advanceSequence(globals, monster, _DEATH_B_FRAMES, actions, "_enforcerDeathBIndex", "EnforcerModule._enf_deathBAdvance", null)
  }

  static enf_die(globals, monster) {
    if (monster == null) return

    var health = monster.get("health", 0)
    if (health < -35) {
      Engine.playSound(monster, Channels.VOICE, "player/udeath.wav", 1, Attenuations.NORMAL)
      PlayerModule.ThrowHead(globals, monster, "progs/h_mega.mdl", health)
      PlayerModule.ThrowGib(globals, monster, "progs/gib1.mdl", health)
      PlayerModule.ThrowGib(globals, monster, "progs/gib2.mdl", health)
      PlayerModule.ThrowGib(globals, monster, "progs/gib3.mdl", health)
      return
    }

    Engine.playSound(monster, Channels.VOICE, "enforcer/death1.wav", 1, Attenuations.NORMAL)
    if (Engine.random() > 0.5) {
      monster.set("_enforcerDeathAIndex", 0)
      EnforcerModule._enf_deathAAdvance(globals, monster)
    } else {
      monster.set("_enforcerDeathBIndex", 0)
      EnforcerModule._enf_deathBAdvance(globals, monster)
    }
  }

  static monster_enforcer(globals, monster) {
    if (monster == null) return

    if (globals.deathmatch != 0) {
      Engine.removeEntity(monster)
      return
    }

    Engine.precacheModel2("progs/enforcer.mdl")
    Engine.precacheModel2("progs/h_mega.mdl")
    Engine.precacheModel2("progs/laser.mdl")

    Engine.precacheSound2("enforcer/death1.wav")
    Engine.precacheSound2("enforcer/enfire.wav")
    Engine.precacheSound2("enforcer/enfstop.wav")
    Engine.precacheSound2("enforcer/idle1.wav")
    Engine.precacheSound2("enforcer/pain1.wav")
    Engine.precacheSound2("enforcer/pain2.wav")
    Engine.precacheSound2("enforcer/sight1.wav")
    Engine.precacheSound2("enforcer/sight2.wav")
    Engine.precacheSound2("enforcer/sight3.wav")
    Engine.precacheSound2("enforcer/sight4.wav")

    monster.set("solid", SolidTypes.SLIDEBOX)
    monster.set("movetype", MoveTypes.STEP)
    Engine.setModel(monster, "progs/enforcer.mdl")

    monster.set("netname", "$qc_enforcer")
    monster.set("killstring", "$qc_ks_enforcer")
    monster.set("noise", "enforcer/sight1.wav")
    monster.set("noise1", "enforcer/sight2.wav")
    monster.set("noise2", "enforcer/sight3.wav")
    monster.set("noise3", "enforcer/sight4.wav")

    Engine.setSize(monster, [-16, -16, -24], [16, 16, 40])
    monster.set("health", 80)
    monster.set("max_health", 80)

    monster.set("th_stand", "EnforcerModule.enf_stand1")
    monster.set("th_walk", "EnforcerModule.enf_walk1")
    monster.set("th_run", "EnforcerModule.enf_run1")
    monster.set("th_missile", "EnforcerModule.enf_atk1")
    monster.set("th_pain", "EnforcerModule.enf_pain")
    monster.set("th_die", "EnforcerModule.enf_die")

    monster.set("allowPathFind", true)
    monster.set("combat_style", CombatStyles.RANGED)

    monster.set("_enforcerStandIndex", 0)
    monster.set("_enforcerWalkIndex", 0)
    monster.set("_enforcerRunIndex", 0)
    monster.set("_enforcerAttackIndex", 0)
    monster.set("_enforcerPainAIndex", 0)
    monster.set("_enforcerPainBIndex", 0)
    monster.set("_enforcerPainCIndex", 0)
    monster.set("_enforcerPainDIndex", 0)
    monster.set("_enforcerDeathAIndex", 0)
    monster.set("_enforcerDeathBIndex", 0)

    MonstersModule.walkmonster_start(globals, monster)
  }
}
