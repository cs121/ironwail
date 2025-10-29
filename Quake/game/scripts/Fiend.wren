// Fiend.wren
// Ports the fiend (demon) monster so it can run completely inside the Wren
// gameplay runtime without falling back to the legacy QuakeC implementation.

import "./Engine" for Engine
import "./Globals" for Channels, Attenuations, SolidTypes, MoveTypes, PlayerFlags
import "./Globals" for CombatStyles, DamageValues, AttackStates, Ranges
import "./Monsters" for MonstersModule
import "./AI" for AIModule
import "./Fight" for FightModule
import "./Combat" for CombatModule
import "./Subs" for SubsModule
import "./Weapons" for WeaponsModule
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
  "stand9",
  "stand10",
  "stand11",
  "stand12",
  "stand13"
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

var _WALK_SPEEDS = [8, 6, 6, 7, 4, 6, 10, 10]

var _RUN_FRAMES = [
  "run1",
  "run2",
  "run3",
  "run4",
  "run5",
  "run6"
]

var _RUN_SPEEDS = [20, 15, 36, 20, 15, 36]

var _JUMP_FRAMES = [
  "leap1",
  "leap2",
  "leap3",
  "leap4",
  "leap5",
  "leap6",
  "leap7",
  "leap8",
  "leap9",
  "leap10",
  "leap11",
  "leap12"
]

var _ATTACK_FRAMES = [
  "attacka1",
  "attacka2",
  "attacka3",
  "attacka4",
  "attacka5",
  "attacka6",
  "attacka7",
  "attacka8",
  "attacka9",
  "attacka10",
  "attacka11",
  "attacka12",
  "attacka13",
  "attacka14",
  "attacka15"
]

var _PAIN_FRAMES = ["pain1", "pain2", "pain3", "pain4", "pain5", "pain6"]

var _DEATH_FRAMES = [
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

class FiendModule {
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

  static _makeVectors(angles) {
    var adjusted = [-angles[0], angles[1], angles[2]]
    return Engine.makeVectors(adjusted)
  }

  static _forwardVector(monster) {
    var angles = monster.get("angles", [0, 0, 0])
    var vectors = FiendModule._makeVectors(angles)
    if (vectors == null || !vectors.containsKey("forward")) return [1, 0, 0]
    return vectors["forward"]
  }

  static _rightVector(monster) {
    var angles = monster.get("angles", [0, 0, 0])
    var vectors = FiendModule._makeVectors(angles)
    if (vectors == null || !vectors.containsKey("right")) return [0, 1, 0]
    return vectors["right"]
  }

  static _enemyYaw(monster) {
    var enemy = monster.get("enemy", null)
    if (enemy == null) return monster.get("angles", [0, 0, 0])[1]
    return FiendModule._vectorToYaw(FiendModule._vectorSub(enemy.get("origin", [0, 0, 0]), monster.get("origin", [0, 0, 0])))
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
    FiendModule._setFrame(globals, monster, frame, nextFunction, 0.1)

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
    FiendModule._setFrame(globals, monster, frames[index], nextName, delay)

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
      Engine.playSound(monster, Channels.VOICE, "demon/idle1.wav", 1, Attenuations.IDLE)
    }
  }

  static _walkAction(globals, monster, index) {
    if (index == 0) {
      FiendModule._maybeIdle(globals, monster)
    }
    if (index < 0 || index >= _WALK_SPEEDS.count) return
    AIModule.ai_walk(globals, monster, _WALK_SPEEDS[index])
  }

  static _runAction(globals, monster, index) {
    if (index == 0) {
      FiendModule._maybeIdle(globals, monster)
    }
    if (index < 0 || index >= _RUN_SPEEDS.count) return
    AIModule.ai_run(globals, monster, _RUN_SPEEDS[index])
  }

  static _faceEnemy(globals, monster) {
    FightModule.ai_face(globals, monster, FiendModule._enemyYaw(monster))
  }

  static _demonMelee(globals, monster, side) {
    if (monster == null) return
    var enemy = monster.get("enemy", null)
    if (enemy == null) return

    FiendModule._faceEnemy(globals, monster)
    Engine.walkMove(monster, monster.get("ideal_yaw", monster.get("angles", [0, 0, 0])[1]), 12)

    var delta = FiendModule._vectorSub(enemy.get("origin", [0, 0, 0]), monster.get("origin", [0, 0, 0]))
    if (FiendModule._vectorLength(delta) > 100) return

    if (!CombatModule.CanDamage(globals, enemy, monster)) return

    Engine.playSound(monster, Channels.WEAPON, "demon/dhit2.wav", 1, Attenuations.NORMAL)

    var damage = 10 + 5 * Engine.random()
    CombatModule.tDamage(globals, enemy, monster, monster, damage)

    var vectors = FiendModule._makeVectors(monster.get("angles", [0, 0, 0]))
    var forward = vectors != null && vectors.containsKey("forward") ? vectors["forward"] : [1, 0, 0]
    var right = vectors != null && vectors.containsKey("right") ? vectors["right"] : [0, 1, 0]
    var origin = FiendModule._vectorAdd(monster.get("origin", [0, 0, 0]), FiendModule._vectorScale(forward, 16))
    var velocity = FiendModule._vectorScale(right, side)
    WeaponsModule.SpawnMeatSpray(globals, origin, velocity)
  }

  static _attackActions() {
    return [
      Fn.new { |globals, monster| FightModule.ai_charge(globals, monster, 4) },
      Fn.new { |globals, monster| FightModule.ai_charge(globals, monster, 0) },
      Fn.new { |globals, monster| FightModule.ai_charge(globals, monster, 0) },
      Fn.new { |globals, monster| FightModule.ai_charge(globals, monster, 1) },
      Fn.new { |globals, monster| FightModule.ai_charge(globals, monster, 2); FiendModule._demonMelee(globals, monster, 200) },
      Fn.new { |globals, monster| FightModule.ai_charge(globals, monster, 1) },
      Fn.new { |globals, monster| FightModule.ai_charge(globals, monster, 6) },
      Fn.new { |globals, monster| FightModule.ai_charge(globals, monster, 8) },
      Fn.new { |globals, monster| FightModule.ai_charge(globals, monster, 4) },
      Fn.new { |globals, monster| FightModule.ai_charge(globals, monster, 2) },
      Fn.new { |globals, monster| FiendModule._demonMelee(globals, monster, -200) },
      Fn.new { |globals, monster| FightModule.ai_charge(globals, monster, 5) },
      Fn.new { |globals, monster| FightModule.ai_charge(globals, monster, 8) },
      Fn.new { |globals, monster| FightModule.ai_charge(globals, monster, 4) },
      Fn.new { |globals, monster| FightModule.ai_charge(globals, monster, 4) }
    ]
  }

  static _painActions() {
    return [null, null, null, null, null, null]
  }

  static demon_stand1(globals, monster) {
    FiendModule._loopSequence(globals, monster, _STAND_FRAMES, "_fiendStandIndex", "FiendModule.demon_stand1", Fn.new { |index|
      AIModule.ai_stand(globals, monster)
    })
  }

  static demon_walk1(globals, monster) {
    FiendModule._loopSequence(globals, monster, _WALK_FRAMES, "_fiendWalkIndex", "FiendModule.demon_walk1", Fn.new { |index|
      FiendModule._walkAction(globals, monster, index)
    })
  }

  static demon_run1(globals, monster) {
    FiendModule._loopSequence(globals, monster, _RUN_FRAMES, "_fiendRunIndex", "FiendModule.demon_run1", Fn.new { |index|
      FiendModule._runAction(globals, monster, index)
    })
  }

  static _startLeap(globals, monster) {
    FiendModule._faceEnemy(globals, monster)
    monster.set("touch", "FiendModule.demon_jump_touch")

    var origin = monster.get("origin", [0, 0, 0])
    origin[2] = origin[2] + 1
    Engine.setOrigin(monster, origin)

    var forward = FiendModule._forwardVector(monster)
    var velocity = FiendModule._vectorAdd(FiendModule._vectorScale(forward, 600), [0, 0, 250])
    monster.set("velocity", velocity)

    var flags = monster.get("flags", 0)
    flags = flags - Engine.bitAnd(flags, PlayerFlags.ONGROUND)
    monster.set("flags", flags)
  }

  static _jumpFrame(globals, monster, frameIndex, nextFunction, action) {
    if (frameIndex < 0 || frameIndex >= _JUMP_FRAMES.count) return
    var delay = (frameIndex == 9) ? 3.0 : 0.1
    FiendModule._setFrame(globals, monster, _JUMP_FRAMES[frameIndex], nextFunction, delay)
    if (action != null) action.call(globals, monster)
  }

  static demon_jump1(globals, monster) {
    FiendModule._jumpFrame(globals, monster, 0, "FiendModule.demon_jump2", Fn.new { |g, m| FiendModule._faceEnemy(g, m) })
  }

  static demon_jump2(globals, monster) {
    FiendModule._jumpFrame(globals, monster, 1, "FiendModule.demon_jump3", Fn.new { |g, m| FiendModule._faceEnemy(g, m) })
  }

  static demon_jump3(globals, monster) {
    FiendModule._jumpFrame(globals, monster, 2, "FiendModule.demon_jump4", Fn.new { |g, m| FiendModule._faceEnemy(g, m) })
  }

  static demon_jump4(globals, monster) {
    FiendModule._jumpFrame(globals, monster, 3, "FiendModule.demon_jump5", Fn.new { |g, m| FiendModule._startLeap(g, m) })
  }

  static demon_jump5(globals, monster) {
    FiendModule._jumpFrame(globals, monster, 4, "FiendModule.demon_jump6", null)
  }

  static demon_jump6(globals, monster) {
    FiendModule._jumpFrame(globals, monster, 5, "FiendModule.demon_jump7", null)
  }

  static demon_jump7(globals, monster) {
    FiendModule._jumpFrame(globals, monster, 6, "FiendModule.demon_jump8", null)
  }

  static demon_jump8(globals, monster) {
    FiendModule._jumpFrame(globals, monster, 7, "FiendModule.demon_jump9", null)
  }

  static demon_jump9(globals, monster) {
    FiendModule._jumpFrame(globals, monster, 8, "FiendModule.demon_jump10", null)
  }

  static demon_jump10(globals, monster) {
    FiendModule._jumpFrame(globals, monster, 9, "FiendModule.demon_jump1", null)
  }

  static demon_jump11(globals, monster) {
    FiendModule._jumpFrame(globals, monster, 10, "FiendModule.demon_jump12", null)
  }

  static demon_jump12(globals, monster) {
    FiendModule._jumpFrame(globals, monster, 11, "FiendModule.demon_run1", null)
  }

  static _attackAdvance(globals, monster) {
    FiendModule._advanceSequence(globals, monster, _ATTACK_FRAMES, FiendModule._attackActions(), "_fiendAttackIndex", "FiendModule._attackAdvance", "FiendModule.demon_run1")
  }

  static demon_attack(globals, monster) {
    monster.set("_fiendAttackIndex", 0)
    FiendModule._attackAdvance(globals, monster)
  }

  static demon_meleeAttack(globals, monster) {
    FiendModule.demon_attack(globals, monster)
  }

  static demon_pain(globals, monster, attacker, damage) {
    if (monster == null) return
    if (monster.get("touch", "") == "FiendModule.demon_jump_touch") return

    var painFinished = monster.get("pain_finished", 0.0)
    if (painFinished > globals.time) return

    monster.set("pain_finished", globals.time + 1.0)
    Engine.playSound(monster, Channels.VOICE, "demon/dpain1.wav", 1, Attenuations.NORMAL)

    if (Engine.random() * 200 > damage) return

    monster.set("_fiendPainIndex", 0)
    FiendModule._advanceSequence(globals, monster, _PAIN_FRAMES, FiendModule._painActions(), "_fiendPainIndex", "FiendModule._fiendPainAdvance", "FiendModule.demon_run1")
  }

  static _fiendPainAdvance(globals, monster) {
    FiendModule._advanceSequence(globals, monster, _PAIN_FRAMES, FiendModule._painActions(), "_fiendPainIndex", "FiendModule._fiendPainAdvance", "FiendModule.demon_run1")
  }

  static demon_die(globals, monster) {
    if (monster == null) return

    var health = monster.get("health", 0)
    if (health < -80) {
      Engine.playSound(monster, Channels.VOICE, "player/udeath.wav", 1, Attenuations.NORMAL)
      PlayerModule.ThrowHead(globals, monster, "progs/h_demon.mdl", health)
      PlayerModule.ThrowGib(globals, monster, "progs/gib1.mdl", health)
      PlayerModule.ThrowGib(globals, monster, "progs/gib1.mdl", health)
      PlayerModule.ThrowGib(globals, monster, "progs/gib1.mdl", health)
      return
    }

    monster.set("_fiendDeathIndex", 0)
    FiendModule._advanceSequence(globals, monster, _DEATH_FRAMES, [
      Fn.new { |globals, monster| Engine.playSound(monster, Channels.VOICE, "demon/ddeath.wav", 1, Attenuations.NORMAL) },
      null,
      null,
      null,
      null,
      Fn.new { |globals, monster| monster.set("solid", SolidTypes.NOT) },
      null,
      null,
      null
    ], "_fiendDeathIndex", "FiendModule._fiendDeathAdvance", null)
  }

  static _fiendDeathAdvance(globals, monster) {
    FiendModule._advanceSequence(globals, monster, _DEATH_FRAMES, [
      null,
      null,
      null,
      null,
      null,
      Fn.new { |globals, monster| monster.set("solid", SolidTypes.NOT) },
      null,
      null,
      null
    ], "_fiendDeathIndex", "FiendModule._fiendDeathAdvance", null)
  }

  static demon_jump_touch(globals, monster, other) {
    if (monster == null) return
    if (monster.get("health", 0) <= 0) return

    if (other != null && other.get("takedamage", DamageValues.NO) != DamageValues.NO) {
      var velocity = monster.get("velocity", [0, 0, 0])
      if (FiendModule._vectorLength(velocity) > 400) {
        var damage = 40 + 10 * Engine.random()
        CombatModule.tDamage(globals, other, monster, monster, damage)
      }
    }

    if (!Engine.checkBottom(monster)) {
      var flags = monster.get("flags", 0)
      if (Engine.bitAnd(flags, PlayerFlags.ONGROUND) != 0) {
        monster.set("touch", "SubsModule.subNull")
        monster.set("think", "FiendModule.demon_jump1")
        monster.set("nextthink", globals.time + 0.1)
        Engine.scheduleThink(monster, "FiendModule.demon_jump1", 0.1)
      }
      return
    }

    monster.set("touch", "SubsModule.subNull")
    monster.set("think", "FiendModule.demon_jump11")
    monster.set("nextthink", globals.time + 0.1)
    Engine.scheduleThink(monster, "FiendModule.demon_jump11", 0.1)
  }

  static _checkDemonMelee(globals, monster, enemyRange) {
    if (enemyRange == Ranges.MELEE) {
      monster.set("attack_state", AttackStates.MELEE)
      return true
    }
    return false
  }

  static _checkDemonJump(globals, monster) {
    var enemy = monster.get("enemy", null)
    if (enemy == null) return false

    var origin = monster.get("origin", [0, 0, 0])
    var mins = monster.get("mins", [0, 0, 0])
    var maxs = monster.get("maxs", [0, 0, 0])
    var enemyOrigin = enemy.get("origin", [0, 0, 0])
    var enemyMins = enemy.get("mins", [0, 0, 0])
    var enemyMaxs = enemy.get("maxs", [0, 0, 0])
    var enemySizeZ = enemyMaxs[2] - enemyMins[2]

    if (origin[2] + mins[2] > enemyOrigin[2] + enemyMins[2] + 0.75 * enemySizeZ) return false
    if (origin[2] + maxs[2] < enemyOrigin[2] + enemyMins[2] + 0.25 * enemySizeZ) return false

    var dist = FiendModule._vectorSub(enemyOrigin, origin)
    dist[2] = 0
    var distance = FiendModule._vectorLength(dist)

    if (distance < 100) return false
    if (distance > 200) {
      if (Engine.random() < 0.9) return false
    }

    return true
  }

  static demonCheckAttack(globals, monster, enemyRange) {
    if (FiendModule._checkDemonMelee(globals, monster, enemyRange)) return true

    if (FiendModule._checkDemonJump(globals, monster)) {
      monster.set("attack_state", AttackStates.MISSILE)
      Engine.playSound(monster, Channels.VOICE, "demon/djump.wav", 1, Attenuations.NORMAL)
      return true
    }

    return false
  }

  static monster_demon1(globals, monster) {
    if (monster == null) return

    if (globals.deathmatch != 0) {
      Engine.removeEntity(monster)
      return
    }

    Engine.precacheModel("progs/demon.mdl")
    Engine.precacheModel("progs/h_demon.mdl")

    Engine.precacheSound("demon/ddeath.wav")
    Engine.precacheSound("demon/dhit2.wav")
    Engine.precacheSound("demon/djump.wav")
    Engine.precacheSound("demon/dpain1.wav")
    Engine.precacheSound("demon/idle1.wav")
    Engine.precacheSound("demon/sight2.wav")

    monster.set("solid", SolidTypes.SLIDEBOX)
    monster.set("movetype", MoveTypes.STEP)

    Engine.setModel(monster, "progs/demon.mdl")
    monster.set("noise", "demon/sight2.wav")
    monster.set("killstring", "$qc_ks_fiend")
    monster.set("netname", "$qc_fiend")

    Engine.setSize(monster, [-32, -32, -24], [32, 32, 64])
    monster.set("health", 300)
    monster.set("max_health", 300)

    monster.set("th_stand", "FiendModule.demon_stand1")
    monster.set("th_walk", "FiendModule.demon_walk1")
    monster.set("th_run", "FiendModule.demon_run1")
    monster.set("th_pain", "FiendModule.demon_pain")
    monster.set("th_die", "FiendModule.demon_die")
    monster.set("th_melee", "FiendModule.demon_meleeAttack")
    monster.set("th_missile", "FiendModule.demon_jump1")

    monster.set("allowPathFind", true)
    monster.set("combat_style", CombatStyles.MELEE)

    monster.set("_fiendStandIndex", 0)
    monster.set("_fiendWalkIndex", 0)
    monster.set("_fiendRunIndex", 0)
    monster.set("_fiendAttackIndex", 0)
    monster.set("_fiendPainIndex", 0)
    monster.set("_fiendDeathIndex", 0)

    MonstersModule.walkmonster_start(globals, monster)
  }
}
