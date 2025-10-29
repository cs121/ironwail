// Knight.wren
// Ports the knight monster's behavior from knight.qc so the enemy can operate
// entirely within the Wren runtime without falling back to QuakeC helpers.

import "./Engine" for Engine
import "./Globals" for Channels, Attenuations, SolidTypes, MoveTypes
import "./Globals" for CombatStyles
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
  "walk8",
  "walk9",
  "walk10",
  "walk11",
  "walk12",
  "walk13",
  "walk14"
]

var _WALK_SPEEDS = [3, 2, 3, 4, 3, 3, 3, 4, 3, 3, 2, 3, 4, 3]

var _RUN_FRAMES = [
  "runb1",
  "runb2",
  "runb3",
  "runb4",
  "runb5",
  "runb6",
  "runb7",
  "runb8"
]

var _RUN_SPEEDS = [16, 20, 13, 7, 16, 20, 14, 6]

var _RUN_ATTACK_FRAMES = [
  "runattack1",
  "runattack2",
  "runattack3",
  "runattack4",
  "runattack5",
  "runattack6",
  "runattack7",
  "runattack8",
  "runattack9",
  "runattack10",
  "runattack11"
]

var _RUN_ATTACK_ACTIONS = [
  Fn.new { |globals, monster|
    var sample = Engine.random() > 0.5 ? "knight/sword2.wav" : "knight/sword1.wav"
    Engine.playSound(monster, Channels.WEAPON, sample, 1, Attenuations.NORMAL)
    FightModule.ai_charge(globals, monster, 20)
  },
  Fn.new { |globals, monster| FightModule.ai_charge_side(globals, monster) },
  Fn.new { |globals, monster| FightModule.ai_charge_side(globals, monster) },
  Fn.new { |globals, monster| FightModule.ai_charge_side(globals, monster) },
  Fn.new { |globals, monster| FightModule.ai_melee_side(globals, monster) },
  Fn.new { |globals, monster| FightModule.ai_melee_side(globals, monster) },
  Fn.new { |globals, monster| FightModule.ai_melee_side(globals, monster) },
  Fn.new { |globals, monster| FightModule.ai_melee_side(globals, monster) },
  Fn.new { |globals, monster| FightModule.ai_melee_side(globals, monster) },
  Fn.new { |globals, monster| FightModule.ai_charge_side(globals, monster) },
  Fn.new { |globals, monster| FightModule.ai_charge(globals, monster, 10) }
]

var _MELEE_FRAMES = [
  "attackb1",
  "attackb2",
  "attackb3",
  "attackb4",
  "attackb5",
  "attackb6",
  "attackb7",
  "attackb8",
  "attackb9",
  "attackb10"
]

var _MELEE_ACTIONS = [
  Fn.new { |globals, monster|
    Engine.playSound(monster, Channels.WEAPON, "knight/sword1.wav", 1, Attenuations.NORMAL)
    FightModule.ai_charge(globals, monster, 0)
  },
  Fn.new { |globals, monster| FightModule.ai_charge(globals, monster, 7) },
  Fn.new { |globals, monster| FightModule.ai_charge(globals, monster, 4) },
  Fn.new { |globals, monster| FightModule.ai_charge(globals, monster, 0) },
  Fn.new { |globals, monster| FightModule.ai_charge(globals, monster, 3) },
  Fn.new { |globals, monster|
    FightModule.ai_charge(globals, monster, 4)
    FightModule.ai_melee(globals, monster)
  },
  Fn.new { |globals, monster|
    FightModule.ai_charge(globals, monster, 1)
    FightModule.ai_melee(globals, monster)
  },
  Fn.new { |globals, monster|
    FightModule.ai_charge(globals, monster, 3)
    FightModule.ai_melee(globals, monster)
  },
  Fn.new { |globals, monster| FightModule.ai_charge(globals, monster, 1) },
  Fn.new { |globals, monster| FightModule.ai_charge(globals, monster, 5) }
]

var _PAIN_SHORT_FRAMES = ["pain1", "pain2", "pain3"]

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
  "painb11"
]

var _PAIN_LONG_ACTIONS = [
  Fn.new { |globals, monster| AIModule.ai_painforward(globals, monster, 0) },
  Fn.new { |globals, monster| AIModule.ai_painforward(globals, monster, 3) },
  null,
  null,
  Fn.new { |globals, monster| AIModule.ai_painforward(globals, monster, 2) },
  Fn.new { |globals, monster| AIModule.ai_painforward(globals, monster, 4) },
  Fn.new { |globals, monster| AIModule.ai_painforward(globals, monster, 2) },
  Fn.new { |globals, monster| AIModule.ai_painforward(globals, monster, 5) },
  Fn.new { |globals, monster| AIModule.ai_painforward(globals, monster, 5) },
  Fn.new { |globals, monster| AIModule.ai_painforward(globals, monster, 0) },
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

var _DEATH_B_FRAMES = [
  "deathb1",
  "deathb2",
  "deathb3",
  "deathb4",
  "deathb5",
  "deathb6",
  "deathb7",
  "deathb8",
  "deathb9",
  "deathb10",
  "deathb11"
]

class KnightModule {
  static _setFrame(globals, monster, frame, nextThink, delay) {
    MonstersModule.setFrame(globals, monster, frame, nextThink, delay)
  }

  static _loopSequence(globals, monster, frames, indexField, nextFunction, actionFn) {
    if (monster == null) return
    if (frames == null || frames.count == 0) return

    var index = monster.get(indexField, 0)
    if (index < 0 || index >= frames.count) index = 0

    var frame = frames[index]
    KnightModule._setFrame(globals, monster, frame, nextFunction, 0.1)

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
    KnightModule._setFrame(globals, monster, frames[index], nextName, delay)

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

  static _runAction(globals, monster, index) {
    if (index == 0 && Engine.random() < 0.2) {
      Engine.playSound(monster, Channels.VOICE, "knight/idle.wav", 1, Attenuations.IDLE)
    }
    if (index < 0 || index >= _RUN_SPEEDS.count) return
    AIModule.ai_run(globals, monster, _RUN_SPEEDS[index])
  }

  static _walkAction(globals, monster, index) {
    if (index == 0 && Engine.random() < 0.2) {
      Engine.playSound(monster, Channels.VOICE, "knight/idle.wav", 1, Attenuations.IDLE)
    }
    if (index < 0 || index >= _WALK_SPEEDS.count) return
    AIModule.ai_walk(globals, monster, _WALK_SPEEDS[index])
  }

  static _resumeRun(globals, monster) {
    if (monster == null) return
    monster.set("_knightRunIndex", 0)
    KnightModule.knight_run1(globals, monster)
  }

  static knight_stand1(globals, monster) {
    KnightModule._loopSequence(globals, monster, _STAND_FRAMES, "_knightStandIndex", "KnightModule.knight_stand1", Fn.new { |i|
      AIModule.ai_stand(globals, monster)
    })
  }

  static knight_walk1(globals, monster) {
    KnightModule._loopSequence(globals, monster, _WALK_FRAMES, "_knightWalkIndex", "KnightModule.knight_walk1", Fn.new { |i| KnightModule._walkAction(globals, monster, i) })
  }

  static knight_run1(globals, monster) {
    KnightModule._loopSequence(globals, monster, _RUN_FRAMES, "_knightRunIndex", "KnightModule.knight_run1", Fn.new { |i| KnightModule._runAction(globals, monster, i) })
  }

  static knight_runatk1(globals, monster) {
    monster.set("_knightRunAttackIndex", 0)
    KnightModule._knight_runatkAdvance(globals, monster)
  }

  static _knight_runatkAdvance(globals, monster) {
    KnightModule._advanceSequence(globals, monster, _RUN_ATTACK_FRAMES, _RUN_ATTACK_ACTIONS, "_knightRunAttackIndex", "KnightModule._knight_runatkAdvance", "KnightModule._resumeRun")
  }

  static knight_atk1(globals, monster) {
    monster.set("_knightMeleeIndex", 0)
    KnightModule._knight_meleeAdvance(globals, monster)
  }

  static _knight_meleeAdvance(globals, monster) {
    KnightModule._advanceSequence(globals, monster, _MELEE_FRAMES, _MELEE_ACTIONS, "_knightMeleeIndex", "KnightModule._knight_meleeAdvance", "KnightModule._resumeRun")
  }

  static knight_pain(globals, monster, attacker, damage) {
    if (monster == null) return
    if (monster.get("pain_finished", 0.0) > globals.time) return

    Engine.playSound(monster, Channels.VOICE, "knight/khurt.wav", 1, Attenuations.NORMAL)

    var choice = Engine.random()
    if (choice < 0.85) {
      monster.set("_knightPainIndex", 0)
      monster.set("pain_finished", globals.time + 1.0)
      KnightModule._knight_painAdvance(globals, monster)
    } else {
      monster.set("_knightPainLongIndex", 0)
      monster.set("pain_finished", globals.time + 1.0)
      KnightModule._knight_painLongAdvance(globals, monster)
    }
  }

  static _knight_painAdvance(globals, monster) {
    KnightModule._advanceSequence(globals, monster, _PAIN_SHORT_FRAMES, null, "_knightPainIndex", "KnightModule._knight_painAdvance", "KnightModule._resumeRun")
  }

  static _knight_painLongAdvance(globals, monster) {
    KnightModule._advanceSequence(globals, monster, _PAIN_LONG_FRAMES, _PAIN_LONG_ACTIONS, "_knightPainLongIndex", "KnightModule._knight_painLongAdvance", "KnightModule._resumeRun")
  }

  static knight_die(globals, monster) {
    if (monster == null) return

    var health = monster.get("health", 0)
    if (health < -40) {
      Engine.playSound(monster, Channels.VOICE, "player/udeath.wav", 1, Attenuations.NORMAL)
      PlayerModule.ThrowHead(globals, monster, "progs/h_knight.mdl", health)
      PlayerModule.ThrowGib(globals, monster, "progs/gib1.mdl", health)
      PlayerModule.ThrowGib(globals, monster, "progs/gib2.mdl", health)
      PlayerModule.ThrowGib(globals, monster, "progs/gib3.mdl", health)
      return
    }

    Engine.playSound(monster, Channels.VOICE, "knight/kdeath.wav", 1, Attenuations.NORMAL)

    var choice = Engine.random()
    if (choice < 0.5) {
      monster.set("_knightDeathIndex", 0)
      KnightModule._knight_deathAdvance(globals, monster)
    } else {
      monster.set("_knightDeathBIndex", 0)
      KnightModule._knight_deathBAdvance(globals, monster)
    }

  }

  static _knight_deathAdvance(globals, monster) {
    var actions = [
      null,
      null,
      Fn.new { |g, m| m.set("solid", SolidTypes.NOT) },
      null,
      null,
      null,
      null,
      null,
      null,
      null
    ]
    KnightModule._advanceSequence(globals, monster, _DEATH_A_FRAMES, actions, "_knightDeathIndex", "KnightModule._knight_deathAdvance", null)
  }

  static _knight_deathBAdvance(globals, monster) {
    var actions = [
      null,
      null,
      Fn.new { |g, m| m.set("solid", SolidTypes.NOT) },
      null,
      null,
      null,
      null,
      null,
      null,
      null,
      null
    ]
    KnightModule._advanceSequence(globals, monster, _DEATH_B_FRAMES, actions, "_knightDeathBIndex", "KnightModule._knight_deathBAdvance", null)
  }

  static monster_knight(globals, monster) {
    if (monster == null) return

    if (globals.deathmatch != 0) {
      Engine.removeEntity(monster)
      return
    }

    Engine.precacheModel("progs/knight.mdl")
    Engine.precacheModel("progs/h_knight.mdl")
    Engine.precacheSound("knight/kdeath.wav")
    Engine.precacheSound("knight/khurt.wav")
    Engine.precacheSound("knight/ksight.wav")
    Engine.precacheSound("knight/sword1.wav")
    Engine.precacheSound("knight/sword2.wav")
    Engine.precacheSound("knight/idle.wav")

    monster.set("solid", SolidTypes.SLIDEBOX)
    monster.set("movetype", MoveTypes.STEP)
    Engine.setModel(monster, "progs/knight.mdl")

    monster.set("noise", "knight/ksight.wav")
    monster.set("netname", "$qc_knight")
    monster.set("killstring", "$qc_ks_knight")

    Engine.setSize(monster, [-16, -16, -24], [16, 16, 40])
    monster.set("health", 75)
    monster.set("max_health", 75)

    monster.set("th_stand", "KnightModule.knight_stand1")
    monster.set("th_walk", "KnightModule.knight_walk1")
    monster.set("th_run", "KnightModule.knight_run1")
    monster.set("th_melee", "KnightModule.knight_atk1")
    monster.set("th_pain", "KnightModule.knight_pain")
    monster.set("th_die", "KnightModule.knight_die")

    monster.set("allowPathFind", true)
    monster.set("combat_style", CombatStyles.MELEE)

    monster.set("_knightStandIndex", 0)
    monster.set("_knightWalkIndex", 0)
    monster.set("_knightRunIndex", 0)
    monster.set("_knightRunAttackIndex", 0)
    monster.set("_knightMeleeIndex", 0)
    monster.set("_knightPainIndex", 0)
    monster.set("_knightPainLongIndex", 0)
    monster.set("_knightDeathIndex", 0)
    monster.set("_knightDeathBIndex", 0)

    MonstersModule.walkmonster_start(globals, monster)
  }
}

