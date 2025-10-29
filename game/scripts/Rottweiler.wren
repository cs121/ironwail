// Rottweiler.wren
// Ports the rottweiler (dog) monster from rottweiler.qc so that the enemy can
// run entirely inside the Wren gameplay runtime without falling back to
// QuakeC shims.

import "./Engine" for Engine
import "./Globals" for Channels, Attenuations, SolidTypes, MoveTypes
import "./Globals" for CombatStyles, PlayerFlags, DamageValues
import "./Monsters" for MonstersModule
import "./AI" for AIModule
import "./Fight" for FightModule
import "./Combat" for CombatModule
import "./Player" for PlayerModule

var _STAND_FRAMES = [
  "stand1",
  "stand2",
  "stand3",
  "stand4",
  "stand5",
  "stand6",
  "stand7",
  "stand8",
  "stand9"
]

var _WALK_FRAMES = [
  "walk1",
  "walk2",
  "walk3",
  "walk4",
  "walk5",
  "walk6",
  "walk7",
  "walk8"
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
  "run12"
]

var _RUN_SPEEDS = [16, 32, 32, 20, 64, 32, 16, 32, 32, 20, 64, 32]
var _WALK_SPEED = 8

var _ATTACK_FRAMES = [
  "attack1",
  "attack2",
  "attack3",
  "attack4",
  "attack5",
  "attack6",
  "attack7",
  "attack8"
]

var _ATTACK_ACTIONS = [
  Fn.new { |globals, monster| FightModule.ai_charge(globals, monster, 10) },
  Fn.new { |globals, monster| FightModule.ai_charge(globals, monster, 10) },
  Fn.new { |globals, monster| FightModule.ai_charge(globals, monster, 10) },
  Fn.new { |globals, monster|
    Engine.playSound(monster, Channels.VOICE, "dog/dattack1.wav", 1, Attenuations.NORMAL)
    RottweilerModule._dogBite(globals, monster)
  },
  Fn.new { |globals, monster| FightModule.ai_charge(globals, monster, 10) },
  Fn.new { |globals, monster| FightModule.ai_charge(globals, monster, 10) },
  Fn.new { |globals, monster| FightModule.ai_charge(globals, monster, 10) },
  Fn.new { |globals, monster| FightModule.ai_charge(globals, monster, 10) }
]

var _LEAP_FRAMES = [
  "leap1",
  "leap2",
  "leap3",
  "leap4",
  "leap5",
  "leap6",
  "leap7",
  "leap8",
  "leap9"
]

var _PAIN_SHORT_FRAMES = ["pain1", "pain2", "pain3", "pain4", "pain5", "pain6"]

var _PAIN_SHORT_ACTIONS = [
  null,
  null,
  null,
  null,
  null,
  Fn.new { |globals, monster| AIModule.ai_pain(globals, monster, 1) }
]

var _PAIN_LONG_FRAMES = [
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
  "painb14",
  "painb15",
  "painb16"
]

var _PAIN_LONG_ACTIONS = [
  null,
  null,
  Fn.new { |globals, monster| AIModule.ai_pain(globals, monster, 4) },
  Fn.new { |globals, monster| AIModule.ai_pain(globals, monster, 12) },
  Fn.new { |globals, monster| AIModule.ai_pain(globals, monster, 12) },
  Fn.new { |globals, monster| AIModule.ai_pain(globals, monster, 2) },
  null,
  Fn.new { |globals, monster| AIModule.ai_pain(globals, monster, 4) },
  null,
  Fn.new { |globals, monster| AIModule.ai_pain(globals, monster, 10) },
  null,
  null,
  null,
  null,
  null,
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
  "death9"
]

var _DEATH_B_FRAMES = [
  "deathb1",
  "deathb2",
  "deathb3",
  "deathb4",
  "deathb5",
  "deathb6",
  "deathb7",
  "deathb8",
  "deathb9"
]

class RottweilerModule {
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

  static _vectorToYaw(vector) {
    var angles = Engine.vectorToAngles(vector)
    return angles[1]
  }

  static _enemyYaw(monster) {
    var enemy = monster.get("enemy", null)
    if (enemy == null) return monster.get("angles", [0, 0, 0])[1]
    return RottweilerModule._vectorToYaw(RottweilerModule._vectorSub(enemy.get("origin", [0, 0, 0]), monster.get("origin", [0, 0, 0])))
  }

  static _forwardVector(monster) {
    var angles = monster.get("angles", [0, 0, 0])
    var adjusted = [-angles[0], angles[1], angles[2]]
    var vectors = Engine.makeVectors(adjusted)
    if (vectors == null) return [1, 0, 0]
    if (!vectors.containsKey("forward")) return [1, 0, 0]
    return vectors["forward"]
  }

  static _setFrame(globals, monster, frame, nextFunction, delay) {
    MonstersModule.setFrame(globals, monster, frame, nextFunction, delay)
  }

  static _loopSequence(globals, monster, frames, indexField, nextFunction, actionFn) {
    if (monster == null) return
    if (frames == null || frames.count == 0) return

    var index = monster.get(indexField, 0)
    if (index < 0 || index >= frames.count) index = 0

    var frame = frames[index]
    RottweilerModule._setFrame(globals, monster, frame, nextFunction, 0.1)

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
    RottweilerModule._setFrame(globals, monster, frames[index], nextName, delay)

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
      Engine.playSound(monster, Channels.VOICE, "dog/idle.wav", 1, Attenuations.IDLE)
    }
  }

  static _walkAction(globals, monster, index) {
    if (index == 0) {
      RottweilerModule._maybeIdle(globals, monster)
    }
    AIModule.ai_walk(globals, monster, _WALK_SPEED)
  }

  static _runAction(globals, monster, index) {
    if (index == 0) {
      RottweilerModule._maybeIdle(globals, monster)
    }
    if (index < 0 || index >= _RUN_SPEEDS.count) return
    AIModule.ai_run(globals, monster, _RUN_SPEEDS[index])
  }

  static _dogBite(globals, monster) {
    if (monster == null) return
    var enemy = monster.get("enemy", null)
    if (enemy == null) return

    FightModule.ai_charge(globals, monster, 10)

    if (!CombatModule.canDamage(globals, enemy, monster)) return

    var delta = RottweilerModule._vectorSub(enemy.get("origin", [0, 0, 0]), monster.get("origin", [0, 0, 0]))
    if (RottweilerModule._vectorLength(delta) > 100) return

    var damage = (Engine.random() + Engine.random() + Engine.random()) * 8
    CombatModule.tDamage(globals, enemy, monster, monster, damage)
  }

  static _dog_face(globals, monster) {
    FightModule.ai_face(globals, monster, RottweilerModule._enemyYaw(monster))
  }

  static _dog_startLeap(globals, monster) {
    RottweilerModule._dog_face(globals, monster)
    monster.set("touch", "RottweilerModule.dog_jump_touch")

    var origin = monster.get("origin", [0, 0, 0])
    origin[2] = origin[2] + 1
    Engine.setOrigin(monster, origin)

    var forward = RottweilerModule._forwardVector(monster)
    var horizontal = RottweilerModule._vectorScale(forward, 300)
    var velocity = RottweilerModule._vectorAdd(horizontal, [0, 0, 200])
    monster.set("velocity", velocity)

    var flags = monster.get("flags", 0)
    flags = flags - Engine.bitAnd(flags, PlayerFlags.ONGROUND)
    monster.set("flags", flags)
  }

  static _dogLeapFrame(globals, monster, frameIndex, nextFunction, action) {
    if (frameIndex < 0 || frameIndex >= _LEAP_FRAMES.count) return
    RottweilerModule._setFrame(globals, monster, _LEAP_FRAMES[frameIndex], nextFunction, 0.1)
    if (action != null) action.call(globals, monster)
  }

  static dog_stand1(globals, monster) {
    RottweilerModule._loopSequence(globals, monster, _STAND_FRAMES, "_dogStandIndex", "RottweilerModule.dog_stand1", Fn.new { |i|
      AIModule.ai_stand(globals, monster)
    })
  }

  static dog_walk1(globals, monster) {
    RottweilerModule._loopSequence(globals, monster, _WALK_FRAMES, "_dogWalkIndex", "RottweilerModule.dog_walk1", Fn.new { |i|
      RottweilerModule._walkAction(globals, monster, i)
    })
  }

  static dog_run1(globals, monster) {
    RottweilerModule._loopSequence(globals, monster, _RUN_FRAMES, "_dogRunIndex", "RottweilerModule.dog_run1", Fn.new { |i|
      RottweilerModule._runAction(globals, monster, i)
    })
  }

  static _dog_attackAdvance(globals, monster) {
    RottweilerModule._advanceSequence(globals, monster, _ATTACK_FRAMES, _ATTACK_ACTIONS, "_dogAttackIndex", "RottweilerModule._dog_attackAdvance", "RottweilerModule.dog_resumeRun")
  }

  static dog_atta1(globals, monster) {
    monster.set("_dogAttackIndex", 0)
    RottweilerModule._dog_attackAdvance(globals, monster)
  }

  static dog_resumeRun(globals, monster) {
    if (monster == null) return
    monster.set("_dogRunIndex", 0)
    RottweilerModule.dog_run1(globals, monster)
  }

  static dog_leap1(globals, monster) {
    RottweilerModule._dogLeapFrame(globals, monster, 0, "RottweilerModule.dog_leap2", Fn.new { |g, m| RottweilerModule._dog_face(g, m) })
  }

  static dog_leap2(globals, monster) {
    RottweilerModule._dogLeapFrame(globals, monster, 1, "RottweilerModule.dog_leap3", Fn.new { |g, m| RottweilerModule._dog_startLeap(g, m) })
  }

  static dog_leap3(globals, monster) {
    RottweilerModule._dogLeapFrame(globals, monster, 2, "RottweilerModule.dog_leap4", null)
  }

  static dog_leap4(globals, monster) {
    RottweilerModule._dogLeapFrame(globals, monster, 3, "RottweilerModule.dog_leap5", null)
  }

  static dog_leap5(globals, monster) {
    RottweilerModule._dogLeapFrame(globals, monster, 4, "RottweilerModule.dog_leap6", null)
  }

  static dog_leap6(globals, monster) {
    RottweilerModule._dogLeapFrame(globals, monster, 5, "RottweilerModule.dog_leap7", null)
  }

  static dog_leap7(globals, monster) {
    RottweilerModule._dogLeapFrame(globals, monster, 6, "RottweilerModule.dog_leap8", null)
  }

  static dog_leap8(globals, monster) {
    RottweilerModule._dogLeapFrame(globals, monster, 7, "RottweilerModule.dog_leap9", null)
  }

  static dog_leap9(globals, monster) {
    RottweilerModule._dogLeapFrame(globals, monster, 8, "RottweilerModule.dog_leap9", null)
  }

  static dog_jump_touch(globals, monster, other) {
    if (monster == null) return
    if (monster.get("health", 0) <= 0) return

    if (other != null && other.get("takedamage", DamageValues.NO) != DamageValues.NO) {
      var velocity = monster.get("velocity", [0, 0, 0])
      if (RottweilerModule._vectorLength(velocity) > 300) {
        var damage = 10 + 10 * Engine.random()
        CombatModule.tDamage(globals, other, monster, monster, damage)
      }
    }

    if (!Engine.checkBottom(monster)) {
      var flags = monster.get("flags", 0)
      if (Engine.bitAnd(flags, PlayerFlags.ONGROUND) != 0) {
        monster.set("touch", "SubsModule.subNull")
        monster.set("think", "RottweilerModule.dog_leap1")
        monster.set("nextthink", globals.time + 0.1)
        Engine.scheduleThink(monster, "RottweilerModule.dog_leap1", 0.1)
      }
      return
    }

    monster.set("touch", "SubsModule.subNull")
    monster.set("think", "RottweilerModule.dog_run1")
    monster.set("nextthink", globals.time + 0.1)
    Engine.scheduleThink(monster, "RottweilerModule.dog_run1", 0.1)
  }

  static dog_pain(globals, monster, attacker, damage) {
    if (monster == null) return
    Engine.playSound(monster, Channels.VOICE, "dog/dpain1.wav", 1, Attenuations.NORMAL)

    if (Engine.random() > 0.5) {
      monster.set("_dogPainShortIndex", 0)
      RottweilerModule._dog_painAdvance(globals, monster)
    } else {
      monster.set("_dogPainLongIndex", 0)
      RottweilerModule._dog_painLongAdvance(globals, monster)
    }
  }

  static _dog_painAdvance(globals, monster) {
    RottweilerModule._advanceSequence(globals, monster, _PAIN_SHORT_FRAMES, _PAIN_SHORT_ACTIONS, "_dogPainShortIndex", "RottweilerModule._dog_painAdvance", "RottweilerModule.dog_resumeRun")
  }

  static _dog_painLongAdvance(globals, monster) {
    RottweilerModule._advanceSequence(globals, monster, _PAIN_LONG_FRAMES, _PAIN_LONG_ACTIONS, "_dogPainLongIndex", "RottweilerModule._dog_painLongAdvance", "RottweilerModule.dog_resumeRun")
  }

  static dog_die(globals, monster) {
    if (monster == null) return

    var health = monster.get("health", 0)
    if (health < -35) {
      Engine.playSound(monster, Channels.VOICE, "player/udeath.wav", 1, Attenuations.NORMAL)
      PlayerModule.ThrowGib(globals, monster, "progs/gib3.mdl", health)
      PlayerModule.ThrowGib(globals, monster, "progs/gib3.mdl", health)
      PlayerModule.ThrowGib(globals, monster, "progs/gib3.mdl", health)
      PlayerModule.ThrowHead(globals, monster, "progs/h_dog.mdl", health)
      return
    }

    Engine.playSound(monster, Channels.VOICE, "dog/ddeath.wav", 1, Attenuations.NORMAL)
    monster.set("solid", SolidTypes.NOT)

    if (Engine.random() > 0.5) {
      monster.set("_dogDeathIndex", 0)
      RottweilerModule._dog_deathAdvance(globals, monster)
    } else {
      monster.set("_dogDeathBIndex", 0)
      RottweilerModule._dog_deathBAdvance(globals, monster)
    }
  }

  static _dog_deathAdvance(globals, monster) {
    RottweilerModule._advanceSequence(globals, monster, _DEATH_A_FRAMES, null, "_dogDeathIndex", "RottweilerModule._dog_deathAdvance", null)
  }

  static _dog_deathBAdvance(globals, monster) {
    RottweilerModule._advanceSequence(globals, monster, _DEATH_B_FRAMES, null, "_dogDeathBIndex", "RottweilerModule._dog_deathBAdvance", null)
  }

  static monster_dog(globals, monster) {
    if (monster == null) return

    if (globals.deathmatch != 0) {
      Engine.removeEntity(monster)
      return
    }

    Engine.precacheModel("progs/h_dog.mdl")
    Engine.precacheModel("progs/dog.mdl")
    Engine.precacheSound("dog/dattack1.wav")
    Engine.precacheSound("dog/ddeath.wav")
    Engine.precacheSound("dog/dpain1.wav")
    Engine.precacheSound("dog/dsight.wav")
    Engine.precacheSound("dog/idle.wav")

    monster.set("solid", SolidTypes.SLIDEBOX)
    monster.set("movetype", MoveTypes.STEP)
    Engine.setModel(monster, "progs/dog.mdl")

    monster.set("noise", "dog/dsight.wav")
    monster.set("netname", "$qc_rottweiler")
    monster.set("killstring", "$qc_ks_rottweiler")

    Engine.setSize(monster, [-32, -32, -24], [32, 32, 40])
    monster.set("health", 25)
    monster.set("max_health", 25)

    monster.set("th_stand", "RottweilerModule.dog_stand1")
    monster.set("th_walk", "RottweilerModule.dog_walk1")
    monster.set("th_run", "RottweilerModule.dog_run1")
    monster.set("th_pain", "RottweilerModule.dog_pain")
    monster.set("th_die", "RottweilerModule.dog_die")
    monster.set("th_melee", "RottweilerModule.dog_atta1")
    monster.set("th_missile", "RottweilerModule.dog_leap1")

    monster.set("allowPathFind", true)
    monster.set("combat_style", CombatStyles.MELEE)

    monster.set("_dogStandIndex", 0)
    monster.set("_dogWalkIndex", 0)
    monster.set("_dogRunIndex", 0)
    monster.set("_dogAttackIndex", 0)
    monster.set("_dogPainShortIndex", 0)
    monster.set("_dogPainLongIndex", 0)
    monster.set("_dogDeathIndex", 0)
    monster.set("_dogDeathBIndex", 0)

    MonstersModule.walkmonster_start(globals, monster)
  }
}
